// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/consensus/consensus_peers.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <type_traits>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/common/common.pb.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/consensus.proxy.h"
#include "kudu/consensus/consensus_queue.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/opid_util.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/rpc/periodic.h"
#include "kudu/rpc/response_callback.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/util/fault_injection.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/logging.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/status.h"
#include "kudu/util/threadpool.h"

DEFINE_int32(consensus_rpc_timeout_ms, 30000,
             "Timeout used for all consensus internal RPC communications.");
TAG_FLAG(consensus_rpc_timeout_ms, advanced);

DEFINE_int32(raft_get_node_instance_timeout_ms, 30000,
             "Timeout for retrieving node instance data over RPC.");
TAG_FLAG(raft_get_node_instance_timeout_ms, hidden);

DEFINE_double(fault_crash_on_leader_request_fraction, 0.0,
              "Fraction of the time when the leader will crash just before sending an "
              "UpdateConsensus RPC. (For testing only!)");
TAG_FLAG(fault_crash_on_leader_request_fraction, runtime);
TAG_FLAG(fault_crash_on_leader_request_fraction, unsafe);

DEFINE_double(fault_crash_after_leader_request_fraction, 0.0,
              "Fraction of the time when the leader will crash on getting a response for an "
              "UpdateConsensus RPC. (For testing only!)");
TAG_FLAG(fault_crash_after_leader_request_fraction, runtime);
TAG_FLAG(fault_crash_after_leader_request_fraction, unsafe);


// Allow for disabling Tablet Copy in unit tests where we want to test
// certain scenarios without triggering bootstrap of a remote peer.
DEFINE_bool(enable_tablet_copy, true,
            "Whether Tablet Copy will be initiated by the leader when it "
            "detects that a follower is out of date or does not have a tablet "
            "replica. For testing purposes only.");
TAG_FLAG(enable_tablet_copy, unsafe);

DECLARE_int32(raft_heartbeat_interval_ms);

using kudu::pb_util::SecureShortDebugString;
using kudu::rpc::Messenger;
using kudu::rpc::PeriodicTimer;
using kudu::rpc::RpcController;
using kudu::tserver::TabletServerErrorPB;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using std::weak_ptr;
using strings::Substitute;


namespace kudu {
namespace consensus {

// The number of retries between failed requests whose failure is logged.
constexpr auto kNumRetriesBetweenLoggingFailedRequest = 5;

void Peer::NewRemotePeer(RaftPeerPB peer_pb,
                         string tablet_id,
                         string leader_uuid,
                         PeerMessageQueue* queue,
                         ThreadPoolToken* raft_pool_token,
                         PeerProxyFactory* peer_proxy_factory,
                         shared_ptr<Peer>* peer) {
  auto new_peer(Peer::make_shared(
      std::move(peer_pb),
      std::move(tablet_id),
      std::move(leader_uuid),
      queue,
      raft_pool_token,
      peer_proxy_factory));
  new_peer->Init();
  *peer = std::move(new_peer);
}

Peer::Peer(RaftPeerPB peer_pb,
           string tablet_id,
           string leader_uuid,
           PeerMessageQueue* queue,
           ThreadPoolToken* raft_pool_token,
           PeerProxyFactory* peer_proxy_factory)
    : tablet_id_(std::move(tablet_id)),
      leader_uuid_(std::move(leader_uuid)),
      peer_pb_(std::move(peer_pb)),
      log_prefix_(Substitute("T $0 P $1 -> Peer $2 ($3:$4): ",
                  tablet_id_, leader_uuid_, peer_pb_.permanent_uuid(),
                  peer_pb_.last_known_addr().host(), peer_pb_.last_known_addr().port())),
      peer_proxy_factory_(peer_proxy_factory),
      queue_(queue),
      failed_attempts_(0),
      messenger_(peer_proxy_factory_->messenger()),
      raft_pool_token_(raft_pool_token),
      request_pending_(false),
      closed_(false),
      has_sent_first_request_(false) {
  CreateProxyIfNeeded();
}

void Peer::Init() {
  {
    std::lock_guard l(peer_lock_);
    queue_->TrackPeer(peer_pb_);
  }

  // Capture a weak_ptr reference into the functor so it can safely handle
  // outliving the peer.
  weak_ptr<Peer> w_this = shared_from_this();
  heartbeater_ = PeriodicTimer::Create(
      messenger_,
      [w_this = std::move(w_this)]() {
        if (auto p = w_this.lock()) {
          WARN_NOT_OK(p->SignalRequest(true), "SignalRequest failed");
        }
      },
      MonoDelta::FromMilliseconds(FLAGS_raft_heartbeat_interval_ms));
  heartbeater_->Start();
}

Status Peer::SignalRequest(bool even_if_queue_empty) {
  // This is a best effort logic in checking for 'closed_' and
  // 'request_pending_': it's not necessary to block if some other thread has
  // taken 'peer_lock_' and about to update 'closed_'/'request_pending_' since
  // the implementation of SendNextRequest() checks for 'closed_' and
  // 'request_pending_' on its own.
  if (PREDICT_FALSE(closed_)) {
    return Status::IllegalState("peer closed");
  }

  // Only allow one request at a time. No sense waking up the
  // raft thread pool if the task will just abort anyway.
  if (request_pending_) {
    return Status::OK();
  }

  // Capture a weak_ptr reference into the submitted functor so that we can
  // safely handle the functor outliving its peer.
  weak_ptr<Peer> w_this(shared_from_this());
  return raft_pool_token_->Submit([even_if_queue_empty, w_this = std::move(w_this)]() {
    if (auto p = w_this.lock()) {
      p->SendNextRequest(even_if_queue_empty);
    }
  });
}

void Peer::SendNextRequest(bool even_if_queue_empty) {
  std::unique_lock l(peer_lock_);
  if (PREDICT_FALSE(closed_)) {
    return;
  }

  // Only allow one request at a time.
  if (request_pending_) {
    return;
  }

  // For the first request sent by the peer, we send it even if the queue is empty,
  // which it will always appear to be for the first request, since this is the
  // negotiation round.
  if (!has_sent_first_request_) {
    even_if_queue_empty = true;
  }

  // If our last request generated an error, and this is not a normal
  // heartbeat request, then don't send the "per-op" request. Instead,
  // we'll wait for the heartbeat.
  //
  // TODO(todd): we could consider looking at the number of consecutive failed
  // attempts, and instead of ignoring the signal, ask the heartbeater
  // to "expedite" the next heartbeat in order to achieve something like
  // exponential backoff after an error. As it is implemented today, any
  // transient error will result in a latency blip as long as the heartbeat
  // period.
  if (failed_attempts_ > 0 && !even_if_queue_empty) {
    return;
  }

  // The peer has no pending request nor is sending: send the request.
  bool needs_tablet_copy = false;
  int64_t commit_index_before = request_.has_committed_index() ?
      request_.committed_index() : kMinimumOpIdIndex;
  Status s = queue_->RequestForPeer(peer_pb_.permanent_uuid(), &request_,
                                    &replicate_msg_refs_, &needs_tablet_copy);
  int64_t commit_index_after = request_.has_committed_index() ?
      request_.committed_index() : kMinimumOpIdIndex;

  if (PREDICT_FALSE(!s.ok())) {
    VLOG_WITH_PREFIX_UNLOCKED(1) << s.ToString();
    return;
  }

  // NOTE: we only perform this check after creating the RequestForPeer() call
  // to ensure any peer health updates that happen therein associated with this
  // peer actually happen. E.g. if we haven't been able to create a proxy in a
  // long enough time, the peer should be considered failed.
  if (PREDICT_FALSE(!CreateProxyIfNeeded())) {
    return;
  }

  if (PREDICT_FALSE(needs_tablet_copy)) {
    Status s = PrepareTabletCopyRequest();
    if (s.ok()) {
      controller_.Reset();
      request_pending_ = true;
      l.unlock();
      // Capture a shared_ptr reference into the RPC callback so that we're guaranteed
      // that this object outlives the RPC.
      shared_ptr<Peer> s_this = shared_from_this();
      proxy_->StartTabletCopyAsync(tc_request_, &tc_response_, &controller_,
                                   [s_this]() {
                                     s_this->ProcessTabletCopyResponse();
                                   });
    } else {
      LOG_WITH_PREFIX_UNLOCKED(WARNING) << "Unable to generate Tablet Copy request for peer: "
                                        << s.ToString();
    }
    return;
  }

  bool req_has_ops = request_.ops_size() > 0 || (commit_index_after > commit_index_before);
  // If the queue is empty, check if we were told to send a status-only
  // message, if not just return.
  if (PREDICT_FALSE(!req_has_ops && !even_if_queue_empty)) {
    return;
  }

  if (req_has_ops) {
    // If we're actually sending ops there's no need to heartbeat for a while.
    heartbeater_->Snooze();
  }

  if (!has_sent_first_request_) {
    // Set the 'immutable' fields in the request only once upon first request.
    request_.set_tablet_id(tablet_id_);
    request_.set_caller_uuid(leader_uuid_);
    request_.set_dest_uuid(peer_pb_.permanent_uuid());
    has_sent_first_request_ = true;
  }

  MAYBE_FAULT(FLAGS_fault_crash_on_leader_request_fraction);

  VLOG_WITH_PREFIX_UNLOCKED(2) << "Sending to peer " << peer_pb().permanent_uuid() << ": "
      << SecureShortDebugString(request_);

  controller_.Reset();
  request_pending_ = true;
  l.unlock();

  // Capture a shared_ptr reference into the RPC callback so that we're guaranteed
  // that this object outlives the RPC.
  shared_ptr<Peer> s_this = shared_from_this();
  proxy_->UpdateAsync(request_, &response_, &controller_,
                      [s_this]() {
                        s_this->ProcessResponse();
                      });
}

void Peer::StartElection() {
  if (PREDICT_FALSE(!CreateProxyIfNeeded())) {
    return;
  }
  // The async proxy contract is such that the response and RPC controller must
  // stay in scope until the callback is invoked. Unlike other Peer methods, we
  // can't guarantee that there's only one outstanding StartElection call at a
  // time, so we can't store the response and controller as a class member.
  // Instead, we have to pass them into the callback and free them there.
  RunLeaderElectionRequestPB req;
  unique_ptr<RunLeaderElectionResponsePB> resp_uniq(new RunLeaderElectionResponsePB());
  unique_ptr<RpcController> controller_uniq(new RpcController());
  string peer_uuid = peer_pb().permanent_uuid();
  req.set_dest_uuid(peer_uuid);
  req.set_tablet_id(tablet_id_);

  // TODO(adar): lack of C++14 move capture makes for ugly code.
  RunLeaderElectionResponsePB* resp = resp_uniq.release();
  RpcController* controller = controller_uniq.release();
  auto s_this = shared_from_this();
  proxy_->StartElectionAsync(req, resp, controller, [resp, controller, peer_uuid, s_this]() {
      unique_ptr<RunLeaderElectionResponsePB> r(resp);
      unique_ptr<RpcController> c(controller);
      string error_msg = Substitute("unable to start election on peer $0", peer_uuid);
      if (!c->status().ok()) {
        WARN_NOT_OK(c->status(), error_msg);
      } else if (r->has_error()) {
        WARN_NOT_OK(StatusFromPB(r->error().status()), error_msg);
      }
    });
}

void Peer::ProcessResponse() {
  // Note: This method runs on the reactor thread.
  std::lock_guard lock(peer_lock_);
  if (PREDICT_FALSE(closed_)) {
    return;
  }
  CHECK(request_pending_);

  MAYBE_FAULT(FLAGS_fault_crash_after_leader_request_fraction);

  // Process RpcController errors.
  const auto controller_status = controller_.status();
  if (!controller_status.ok()) {
    auto ps = controller_status.IsRemoteError() ?
        PeerStatus::REMOTE_ERROR : PeerStatus::RPC_LAYER_ERROR;
    queue_->UpdatePeerStatus(peer_pb_.permanent_uuid(), ps, controller_status);
    ProcessResponseErrorUnlocked(controller_status);
    return;
  }

  // Process CANNOT_PREPARE.
  // TODO(todd): there is no integration test coverage of this code path. Likely a bug in
  // this path is responsible for KUDU-1779.
  if (response_.status().has_error() &&
      response_.status().error().code() == consensus::ConsensusErrorPB::CANNOT_PREPARE) {
    Status response_status = StatusFromPB(response_.status().error().status());
    queue_->UpdatePeerStatus(peer_pb_.permanent_uuid(), PeerStatus::CANNOT_PREPARE,
                             response_status);
    ProcessResponseErrorUnlocked(response_status);
    return;
  }

  // Process tserver-level errors.
  if (response_.has_error()) {
    Status response_status = StatusFromPB(response_.error().status());
    PeerStatus ps;
    TabletServerErrorPB resp_error = response_.error();
    switch (response_.error().code()) {
      // We treat WRONG_SERVER_UUID as failed.
      case TabletServerErrorPB::WRONG_SERVER_UUID: [[fallthrough]];
      case TabletServerErrorPB::TABLET_FAILED:
        ps = PeerStatus::TABLET_FAILED;
        break;
      case TabletServerErrorPB::TABLET_NOT_FOUND:
        ps = PeerStatus::TABLET_NOT_FOUND;
        break;
      default:
        // Unknown kind of error.
        ps = PeerStatus::REMOTE_ERROR;
    }
    queue_->UpdatePeerStatus(peer_pb_.permanent_uuid(), ps, response_status);
    ProcessResponseErrorUnlocked(response_status);
    return;
  }

  // The queue's handling of the peer response may generate IO (reads against
  // the WAL) and SendNextRequest() may do the same thing. So we run the rest
  // of the response handling logic on our thread pool and not on the reactor
  // thread.
  //
  // Capture a weak_ptr reference into the submitted functor so that we can
  // safely handle the functor outliving its peer.
  weak_ptr<Peer> w_this = shared_from_this();
  Status s = raft_pool_token_->Submit([w_this]() {
    if (auto p = w_this.lock()) {
      p->DoProcessResponse();
    }
  });
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX_UNLOCKED(WARNING) << Substitute(
        "unable to process peer response: $0: $1",
         s.ToString(), SecureShortDebugString(response_));
    request_pending_ = false;
  }
}

void Peer::DoProcessResponse() {
  VLOG_WITH_PREFIX_UNLOCKED(2) << "Response from peer " << peer_pb().permanent_uuid() << ": "
      << SecureShortDebugString(response_);

  const auto send_more_immediately =
      queue_->ResponseFromPeer(peer_pb_.permanent_uuid(), response_);

  {
    std::lock_guard lock(peer_lock_);
    CHECK(request_pending_);
    failed_attempts_ = 0;
    request_pending_ = false;
  }

  if (send_more_immediately) {
    SendNextRequest(true);
  }
}

Status Peer::PrepareTabletCopyRequest() {
  if (PREDICT_FALSE(!FLAGS_enable_tablet_copy)) {
    failed_attempts_++;
    return Status::NotSupported("Tablet Copy is disabled");
  }

  return queue_->GetTabletCopyRequestForPeer(peer_pb_.permanent_uuid(), &tc_request_);
}

void Peer::ProcessTabletCopyResponse() {
  // If the peer is already closed return.
  std::unique_lock lock(peer_lock_);
  if (PREDICT_FALSE(closed_)) {
    return;
  }
  CHECK(request_pending_);
  request_pending_ = false;

  // If the response is OK, or ALREADY_INPROGRESS, then consider the RPC successful.
  const auto controller_status = controller_.status();
  bool success =
    controller_status.ok() &&
    (!tc_response_.has_error() ||
     tc_response_.error().code() == TabletServerErrorPB::TabletServerErrorPB::ALREADY_INPROGRESS);

  if (success) {
    lock.unlock();
    queue_->UpdatePeerStatus(peer_pb_.permanent_uuid(), PeerStatus::OK, Status::OK());
  } else if (!tc_response_.has_error() ||
              tc_response_.error().code() != TabletServerErrorPB::TabletServerErrorPB::THROTTLED) {
    const auto& response_str = controller_status.ok()
        ? SecureShortDebugString(tc_response_) : controller_status.ToString();
    lock.unlock();
    // THROTTLED is a common response after a tserver with many replicas fails;
    // logging it would generate a great deal of log spam.
    LOG_WITH_PREFIX_UNLOCKED(WARNING) << "Unable to start Tablet Copy on peer: "
                                      << response_str;
  }
}

void Peer::ProcessResponseErrorUnlocked(const Status& status) {
  DCHECK(peer_lock_.is_locked());
  failed_attempts_++;
  string resp_err_info;
  if (response_.has_error()) {
    resp_err_info = Substitute(" Error code: $0 ($1).",
                               TabletServerErrorPB::Code_Name(response_.error().code()),
                               response_.error().code());
  }
  // We log the warning at the first failure, then every
  // 'kNumRetriesBetweenLoggingFailedRequest' retries.
  // TODO(wdberkeley): If a use case comes up elsewhere, consider adding a
  // KLOG_EVERY_N macro that supports an appropriate LogThrottler. For now,
  // this class has 'failed_attempts_' available so it's overkill to add
  // the throttler support.
  if (failed_attempts_ % kNumRetriesBetweenLoggingFailedRequest == 1) {
    LOG_WITH_PREFIX_UNLOCKED(WARNING) <<
      Substitute("Couldn't send request to peer $0.$1 Status: $2. This is "
                 "attempt $3: this message will repeat every $4th retry.",
                 peer_pb_.permanent_uuid(),
                 resp_err_info,
                 status.ToString(),
                 failed_attempts_,
                 kNumRetriesBetweenLoggingFailedRequest);
  }
  request_pending_ = false;
}

bool Peer::CreateProxyIfNeeded() {
  std::lock_guard l(proxy_lock_);
  if (!proxy_) {
    unique_ptr<PeerProxy> peer_proxy;
    Status s = DCHECK_NOTNULL(peer_proxy_factory_)->NewProxy(peer_pb_, &peer_proxy);
    if (!s.ok()) {
      HostPort hostport = HostPortFromPB(peer_pb_.last_known_addr());
      KLOG_EVERY_N_SECS(WARNING, 1)
          << Substitute("Unable to create proxy for $0 ($1)",
                        peer_pb_.permanent_uuid(), hostport.ToString());
      return false;
    }
    proxy_ = std::move(peer_proxy);
  }
  return true;
}

const string& Peer::LogPrefixUnlocked() const {
  return log_prefix_;
}

void Peer::Close() {
  if (closed_) {
    // Do nothing if the peer is already closed.
    return;
  }
  {
    std::lock_guard lock(peer_lock_);
    closed_ = true;
  }
  VLOG_WITH_PREFIX_UNLOCKED(1) << "Closing peer: " << peer_pb_.permanent_uuid();

  queue_->UntrackPeer(peer_pb_.permanent_uuid());
}

Peer::~Peer() {
  Close();
  if (heartbeater_) {
    heartbeater_->Stop();
  }

  // We don't own the ops (the queue does).
  request_.mutable_ops()->UnsafeArenaExtractSubrange(0, request_.ops_size(), nullptr);
}

RpcPeerProxy::RpcPeerProxy(HostPort hostport,
                           unique_ptr<ConsensusServiceProxy> consensus_proxy)
    : hostport_(std::move(hostport)),
      consensus_proxy_(std::move(DCHECK_NOTNULL(consensus_proxy))) {
}

void RpcPeerProxy::UpdateAsync(const ConsensusRequestPB& request,
                               ConsensusResponsePB* response,
                               rpc::RpcController* controller,
                               const rpc::ResponseCallback& callback) {
  controller->set_timeout(MonoDelta::FromMilliseconds(FLAGS_consensus_rpc_timeout_ms));
  consensus_proxy_->UpdateConsensusAsync(request, response, controller, callback);
}

void RpcPeerProxy::StartElectionAsync(const RunLeaderElectionRequestPB& request,
                                      RunLeaderElectionResponsePB* response,
                                      rpc::RpcController* controller,
                                      const rpc::ResponseCallback& callback) {
  controller->set_timeout(MonoDelta::FromMilliseconds(FLAGS_consensus_rpc_timeout_ms));
  consensus_proxy_->RunLeaderElectionAsync(request, response, controller, callback);
}

void RpcPeerProxy::RequestConsensusVoteAsync(const VoteRequestPB& request,
                                             VoteResponsePB* response,
                                             rpc::RpcController* controller,
                                             const rpc::ResponseCallback& callback) {
  consensus_proxy_->RequestConsensusVoteAsync(request, response, controller, callback);
}

void RpcPeerProxy::StartTabletCopyAsync(const StartTabletCopyRequestPB& request,
                                        StartTabletCopyResponsePB* response,
                                        rpc::RpcController* controller,
                                        const rpc::ResponseCallback& callback) {
  controller->set_timeout(MonoDelta::FromMilliseconds(FLAGS_consensus_rpc_timeout_ms));
  consensus_proxy_->StartTabletCopyAsync(request, response, controller, callback);
}

string RpcPeerProxy::PeerName() const {
  return hostport_.ToString();
}

namespace {

Status CreateConsensusServiceProxyForHost(
    const HostPort& hostport,
    const shared_ptr<Messenger>& messenger,
    DnsResolver* dns_resolver,
    unique_ptr<ConsensusServiceProxy>* new_proxy) {
  new_proxy->reset(new ConsensusServiceProxy(messenger, hostport, dns_resolver));
  (*new_proxy)->Init();
  return Status::OK();
}

} // anonymous namespace

RpcPeerProxyFactory::RpcPeerProxyFactory(shared_ptr<Messenger> messenger,
                                         DnsResolver* dns_resolver)
    : messenger_(std::move(messenger)),
      dns_resolver_(dns_resolver) {
}

Status RpcPeerProxyFactory::NewProxy(const RaftPeerPB& peer_pb,
                                     unique_ptr<PeerProxy>* proxy) {
  HostPort hostport = HostPortFromPB(peer_pb.last_known_addr());
  unique_ptr<ConsensusServiceProxy> new_proxy;
  RETURN_NOT_OK(CreateConsensusServiceProxyForHost(
      hostport, messenger_, dns_resolver_, &new_proxy));
  proxy->reset(new RpcPeerProxy(std::move(hostport), std::move(new_proxy)));
  return Status::OK();
}

Status SetPermanentUuidForRemotePeer(
    const shared_ptr<rpc::Messenger>& messenger,
    DnsResolver* resolver,
    RaftPeerPB* remote_peer) {
  DCHECK(!remote_peer->has_permanent_uuid());
  HostPort hostport = HostPortFromPB(remote_peer->last_known_addr());
  unique_ptr<ConsensusServiceProxy> proxy;
  RETURN_NOT_OK(CreateConsensusServiceProxyForHost(
      hostport, messenger, resolver, &proxy));
  GetNodeInstanceRequestPB req;
  GetNodeInstanceResponsePB resp;
  rpc::RpcController controller;

  // TODO generalize this exponential backoff algorithm, as we do the
  // same thing in catalog_manager.cc
  // (AsyncTabletRequestTask::RpcCallBack).
  MonoTime deadline = MonoTime::Now() +
      MonoDelta::FromMilliseconds(FLAGS_raft_get_node_instance_timeout_ms);
  int attempt = 1;
  while (true) {
    VLOG(2) << "Getting uuid from remote peer. Request: " << SecureShortDebugString(req);

    controller.Reset();
    Status s = proxy->GetNodeInstance(req, &resp, &controller);
    if (s.ok()) {
      if (controller.status().ok()) {
        break;
      }
      s = controller.status();
    }

    LOG(WARNING) << "Error getting permanent uuid from config peer " << hostport.ToString() << ": "
                 << s.ToString();
    MonoTime now = MonoTime::Now();
    if (now < deadline) {
      int64_t remaining_ms = (deadline - now).ToMilliseconds();
      int64_t base_delay_ms = 1LL << (attempt + 3); // 1st retry delayed 2^4 ms, 2nd 2^5, etc..
      int64_t jitter_ms = rand() % 50; // Add up to 50ms of additional random delay.
      int64_t delay_ms = std::min<int64_t>(base_delay_ms + jitter_ms, remaining_ms);
      VLOG(1) << "Sleeping " << delay_ms << " ms. before retrying to get uuid from remote peer...";
      SleepFor(MonoDelta::FromMilliseconds(delay_ms));
      LOG(INFO) << "Retrying to get permanent uuid for remote peer: "
          << SecureShortDebugString(*remote_peer) << " attempt: " << attempt++;
    } else {
      s = Status::TimedOut(Substitute("Getting permanent uuid from $0 timed out after $1 ms.",
                                      hostport.ToString(),
                                      FLAGS_raft_get_node_instance_timeout_ms),
                           s.ToString());
      return s;
    }
  }
  remote_peer->set_permanent_uuid(resp.node_instance().permanent_uuid());
  return Status::OK();
}

}  // namespace consensus
}  // namespace kudu
