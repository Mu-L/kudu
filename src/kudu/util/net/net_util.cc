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

#include "kudu/util/net/net_util.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
// IWYU pragma: no_include <bits/local_lim.h>

#include <algorithm>
#include <cerrno>
#include <climits>  // IWYU pragma: keep
#include <cstring>
#include <functional>
#include <memory>
#include <ostream>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/container_hash/hash.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/gutil/endian.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/strip.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/util/debug/trace_event.h"
#include "kudu/util/errno.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/net/socket.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/string_case.h"
#include "kudu/util/subprocess.h"
#include "kudu/util/thread_restrictions.h"
#include "kudu/util/trace.h"

// Mac OS 10.9 does not appear to define HOST_NAME_MAX in unistd.h
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 64
#endif

DEFINE_bool(fail_dns_resolution, false, "Whether to fail dns resolution, for tests.");
TAG_FLAG(fail_dns_resolution, hidden);
DEFINE_string(fail_dns_resolution_hostports, "",
              "Comma-separated list of hostports that fail dns resolution. If empty, fails all "
              "dns resolution attempts. Only takes effect if --fail_dns_resolution is 'true'.");
TAG_FLAG(fail_dns_resolution_hostports, hidden);

DEFINE_string(dns_addr_resolution_override, "",
              "Comma-separated list of '='-separated pairs of hosts to addresses. The left-hand "
              "side of the '=' is taken as a host, and will resolve to the right-hand side which "
              "is expected to be a socket address with no port.");
TAG_FLAG(dns_addr_resolution_override, hidden);

DEFINE_string(host_for_tests, "", "Host to use when resolving a given server's locally bound or "
              "advertised addresses.");

using std::function;
using std::string;
using std::unordered_set;
using std::unique_ptr;
using std::vector;
using strings::Split;
using strings::Substitute;

namespace kudu {

// Allow 18-bit PIDs, max PID up to 262143, for binding in UNIQUE_LOOPBACK mode.
static const int kPidBits = 18;
// The PID and server indices share the same 24-bit space. The 24-bit space
// corresponds to the 127.0.0.0/8 subnet.
static const int kServerIdxBits = 24 - kPidBits;
// The maximum allowed number of 'indexed servers' for binding in UNIQUE_LOOPBACK mode.
const int kServersMaxNum = (1 << kServerIdxBits) - 2;

namespace {

using AddrInfo = unique_ptr<addrinfo, function<void(addrinfo*)>>;

// A utility wrapper around getaddrinfo() call to convert the return code
// of the libc library function into Status.
Status GetAddrInfo(const string& hostname,
                   const addrinfo& hints,
                   const string& op_description,
                   AddrInfo* info) {
  ThreadRestrictions::AssertWaitAllowed();
  addrinfo* res = nullptr;
  const int rc = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
  const int err = errno; // preserving the errno from the getaddrinfo() call
  AddrInfo result(res, ::freeaddrinfo);
  if (rc == 0) {
    if (info != nullptr) {
      info->swap(result);
    }
    return Status::OK();
  }
  const string err_msg = Substitute("unable to $0", op_description);
  if (rc == EAI_SYSTEM) {
    return Status::NetworkError(err_msg, ErrnoToString(err), err);
  }
  return Status::NetworkError(err_msg, gai_strerror(rc));
}

// Converts the given Sockaddr into a HostPort, substituting the FQDN
// in the case that the provided address is the wildcard.
//
// In the case of other addresses, the returned HostPort will contain just the
// stringified form of the IP.
Status HostPortFromSockaddrReplaceWildcard(const Sockaddr& addr, HostPort* hp) {
  string host;
  if (!FLAGS_host_for_tests.empty() || addr.IsWildcard()) {
    RETURN_NOT_OK(GetFQDN(&host));
  } else {
    host = addr.host();
  }
  hp->set_host(host);
  hp->set_port(addr.port());
  return Status::OK();
}

} // anonymous namespace

HostPort::HostPort()
  : host_(""),
    port_(0) {
}

HostPort::HostPort(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

HostPort::HostPort(const Sockaddr& addr)
  : host_(addr.host()),
    port_(addr.port()) {
}

bool operator==(const HostPort& hp1, const HostPort& hp2) {
  return hp1.port() == hp2.port() && hp1.host() == hp2.host();
}

size_t HostPort::HashCode() const {
  size_t seed = 0;
  boost::hash_combine(seed, host_);
  boost::hash_combine(seed, port_);
  return seed;
}

Status HostPort::ParseString(const string& str, uint16_t default_port) {
  std::pair<string, string> p = strings::Split(str, strings::delimiter::Limit(":", 1));

  // Strip any whitespace from the host.
  StripWhiteSpace(&p.first);

  // Parse the port.
  uint32_t port;
  if (p.second.empty() && strcount(str, ':') == 0) {
    // No port specified.
    port = default_port;
  } else if (!SimpleAtoi(p.second, &port) ||
             port > 65535) {
    return Status::InvalidArgument("invalid port", str);
  }

  host_.swap(p.first);
  port_ = port;
  return Status::OK();
}

Status HostPort::ParseStringWithScheme(const string& str, uint16_t default_port) {
  string str_copy(str);
  const string kSchemeSeparator = "://";
  const string kPathSeparator = "/";

  auto scheme_idx = str_copy.find(kSchemeSeparator);

  if (scheme_idx == 0) {
    return Status::InvalidArgument("invalid scheme format", str_copy);
  }

  if (scheme_idx != string::npos) {
    str_copy.erase(0, scheme_idx + kSchemeSeparator.size());
    auto path_idx = str_copy.find(kPathSeparator);
    if (path_idx == 0) {
      return Status::InvalidArgument("invalid address format", str_copy);
    }
    if (path_idx != string::npos) {
      str_copy.erase(path_idx, str_copy.size());
    }
  }

  return ParseString(str_copy, default_port);
}

Status HostPort::ResolveAddresses(vector<Sockaddr>* addresses) const {
  TRACE_EVENT1("net", "HostPort::ResolveAddresses",
               "host", host_);
  TRACE_COUNTER_SCOPE_LATENCY_US("dns_us");
  // NOTE: we use this instead of the FLAGS_... variant because this flag may be
  // changed at runtime in tests and thus needs to be thread-safe.
  const auto dns_addr_resolution_override_flag =
      google::GetCommandLineFlagInfoOrDie("dns_addr_resolution_override");
  if (PREDICT_FALSE(!dns_addr_resolution_override_flag.current_value.empty())) {
    vector<string> hosts_and_addrs = Split(dns_addr_resolution_override_flag.current_value, ",");
    for (const auto& ha : hosts_and_addrs) {
      vector<string> host_and_addr = Split(ha, "=");
      if (host_and_addr.size() != 2) {
        return Status::InvalidArgument("failed to parse injected address override");
      }
      if (iequals(host_and_addr[0], host_)) {
        Sockaddr addr;
        RETURN_NOT_OK_PREPEND(addr.ParseString(host_and_addr[1], port_),
            "failed to parse injected address override");
        *addresses = { addr };
        return Status::OK();
      }
    }
  }
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  AddrInfo result;
  const string op_description = Substitute("resolve address for $0", host_);
  LOG_SLOW_EXECUTION(WARNING, 200, op_description) {
    RETURN_NOT_OK(GetAddrInfo(host_, hints, op_description, &result));
  }

  // DNS may return the same host multiple times. We want to return only the unique
  // addresses, but in the same order as DNS returned them. To do so, we keep track
  // of the already-inserted elements in a set.
  unordered_set<Sockaddr> inserted;
  vector<Sockaddr> result_addresses;
  for (const addrinfo* ai = result.get(); ai != nullptr; ai = ai->ai_next) {
    CHECK_EQ(AF_INET, ai->ai_family);
    sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
    addr->sin_port = htons(port_);
    Sockaddr sockaddr(*addr);
    VLOG(2) << Substitute("resolved address $0 for host/port $1",
                          sockaddr.ToString(), ToString());
    if (InsertIfNotPresent(&inserted, sockaddr)) {
      result_addresses.emplace_back(sockaddr);
    }
  }
  if (PREDICT_FALSE(FLAGS_fail_dns_resolution)) {
    if (FLAGS_fail_dns_resolution_hostports.empty()) {
      return Status::NetworkError("injected DNS resolution failure");
    }
    unordered_set<string> failed_hostports =
        Split(FLAGS_fail_dns_resolution_hostports, ",");
    for (const auto& hp_str : failed_hostports) {
      HostPort hp;
      Status s = hp.ParseString(hp_str, /*default_port=*/0);
      if (!s.ok()) {
        LOG(WARNING) << "Could not parse: " << hp_str;
        continue;
      }
      if (hp == *this) {
        return Status::NetworkError("injected DNS resolution failure", hp_str);
      }
    }
  }
  if (addresses) {
    *addresses = std::move(result_addresses);
  }
  return Status::OK();
}

Status HostPort::ParseStrings(const string& comma_sep_addrs,
                              uint16_t default_port,
                              vector<HostPort>* res) {
  return ParseAddresses(strings::Split(comma_sep_addrs, ",", strings::SkipEmpty()),
                        default_port, res);
}

Status HostPort::ParseAddresses(const vector<string>& addrs, uint16_t default_port,
                                vector<HostPort>* res) {
  res->clear();
  res->reserve(addrs.size());
  for (const string& addr : addrs) {
    HostPort host_port;
    RETURN_NOT_OK(host_port.ParseString(addr, default_port));
    res->emplace_back(std::move(host_port));
  }
  return Status::OK();
}

Status HostPort::ParseStringsWithScheme(const string& comma_sep_addrs,
                                        uint16_t default_port,
                                        vector<HostPort>* res) {
  res->clear();

  vector<string> addr_strings = strings::Split(comma_sep_addrs, ",", strings::SkipEmpty());
  res->reserve(addr_strings.size());
  for (const string& addr_string : addr_strings) {
    HostPort host_port;
    RETURN_NOT_OK(host_port.ParseStringWithScheme(addr_string, default_port));
    res->emplace_back(host_port);
  }
  return Status::OK();
}

string HostPort::ToString() const {
  return Substitute("$0:$1", host_, port_);
}

string HostPort::ToCommaSeparatedString(const vector<HostPort>& hostports) {
  vector<string> hostport_strs;
  for (const HostPort& hostport : hostports) {
    hostport_strs.push_back(hostport.ToString());
  }
  return JoinStrings(hostport_strs, ",");
}

bool HostPort::IsLoopback(uint32_t addr) {
    return (NetworkByteOrder::FromHost32(addr) >> 24) == 127;
}

string HostPort::AddrToString(uint32_t addr) {
  char str[INET_ADDRSTRLEN];
  ::inet_ntop(AF_INET, &addr, str, INET_ADDRSTRLEN);
  return str;
}

Network::Network()
  : addr_(0),
    netmask_(0) {
}

Network::Network(uint32_t addr, uint32_t netmask)
  : addr_(addr), netmask_(netmask) {}

bool Network::WithinNetwork(const Sockaddr& addr) const {
  return ((addr.ipv4_addr().sin_addr.s_addr & netmask_) ==
          (addr_ & netmask_));
}

Status Network::ParseCIDRString(const string& addr) {
  std::pair<string, string> p = strings::Split(addr, strings::delimiter::Limit("/", 1));

  kudu::Sockaddr sockaddr;
  Status s = sockaddr.ParseString(p.first, 0);

  uint32_t bits;
  bool success = SimpleAtoi(p.second, &bits);

  if (!s.ok() || !success || bits > 32) {
    return Status::NetworkError("Unable to parse CIDR address", addr);
  }

  // Netmask in network byte order
  uint32_t netmask = NetworkByteOrder::FromHost32(~(0xffffffff >> bits));
  addr_ = sockaddr.ipv4_addr().sin_addr.s_addr;
  netmask_ = netmask;
  return Status::OK();
}

Status Network::ParseCIDRStrings(const string& comma_sep_addrs,
                                 vector<Network>* res) {
  vector<string> addr_strings = strings::Split(comma_sep_addrs, ",", strings::SkipEmpty());
  for (const string& addr_string : addr_strings) {
    Network network;
    RETURN_NOT_OK(network.ParseCIDRString(addr_string));
    res->push_back(network);
  }
  return Status::OK();
}

bool Network::IsLoopback() const {
  return HostPort::IsLoopback(addr_);
}

string Network::GetAddrAsString() const {
  return HostPort::AddrToString(addr_);
}

bool IsPrivilegedPort(uint16_t port) {
  return port <= 1024 && port != 0;
}

Status ParseAddressList(const std::string& addr_list,
                        uint16_t default_port,
                        std::vector<Sockaddr>* addresses) {
  vector<HostPort> host_ports;
  RETURN_NOT_OK(HostPort::ParseStrings(addr_list, default_port, &host_ports));
  if (host_ports.empty()) {
    return Status::InvalidArgument("No address specified");
  }
  unordered_set<Sockaddr> uniqued;
  for (const HostPort& host_port : host_ports) {
    vector<Sockaddr> this_addresses;
    RETURN_NOT_OK(host_port.ResolveAddresses(&this_addresses));

    // Only add the unique ones -- the user may have specified
    // some IP addresses in multiple ways
    for (const Sockaddr& addr : this_addresses) {
      if (InsertIfNotPresent(&uniqued, addr)) {
        addresses->push_back(addr);
      } else {
        LOG(INFO) << "Address " << addr.ToString() << " for " << host_port.ToString()
                  << " duplicates an earlier resolved entry.";
      }
    }
  }
  return Status::OK();
}

Status GetHostname(string* hostname) {
  TRACE_EVENT0("net", "GetHostname");
  if (!FLAGS_host_for_tests.empty()) {
    *hostname = FLAGS_host_for_tests;
    return Status::OK();
  }
  char name[HOST_NAME_MAX];
  if (gethostname(name, HOST_NAME_MAX) != 0) {
    const int err = errno;
    return Status::NetworkError(
        "Unable to determine local hostname", ErrnoToString(err), err);
  }
  *hostname = name;
  return Status::OK();
}

Status GetLocalNetworks(std::vector<Network>* net) {
  struct ifaddrs* ifap = nullptr;
  SCOPED_CLEANUP({
    if (ifap) {
      freeifaddrs(ifap);
    }
  });

  if (getifaddrs(&ifap) != 0) {
    const int err = errno;
    return Status::NetworkError(
        "Unable to determine local network addresses", ErrnoToString(err), err);
  }

  net->clear();
  for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_netmask == nullptr) {
      continue;
    }
    if (ifa->ifa_addr->sa_family == AF_INET) {
      auto* ifa_address = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
      auto* ifa_netmask = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_netmask);
      if (ifa_netmask->sin_family == AF_UNSPEC) {
        // Tunnel interfaces created by some VPN implementations do not have
        // their network mask's address family (sin_family) properly set. If
        // the address family for the network mask is left as AF_UNSPEC, this
        // code sets the address family of the network mask to be the same as
        // the family of the network address itself. This is to satisfy the
        // constraints in the Sockaddr class.
        ifa_netmask->sin_family = ifa_address->sin_family;
      }
      net->emplace_back(Sockaddr(*ifa_address).ipv4_addr().sin_addr.s_addr,
                        Sockaddr(*ifa_netmask).ipv4_addr().sin_addr.s_addr);
    }
  }

  return Status::OK();
}

Status GetFQDN(string* hostname) {
  TRACE_EVENT0("net", "GetFQDN");
  // Start with the non-qualified hostname
  RETURN_NOT_OK(GetHostname(hostname));
  if (!FLAGS_host_for_tests.empty()) {
    return Status::OK();
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_family = AF_INET;
  hints.ai_flags = AI_CANONNAME;
  AddrInfo result;
  const string op_description =
      Substitute("look up canonical hostname for localhost '$0'", *hostname);
  LOG_SLOW_EXECUTION(WARNING, 200, op_description) {
    TRACE_EVENT0("net", "getaddrinfo");
    RETURN_NOT_OK(GetAddrInfo(*hostname, hints, op_description, &result));
  }
  // On macOS ai_cannonname returns null when FQDN doesn't have domain name (ex .local)
  if (result->ai_canonname != nullptr) {
    *hostname = result->ai_canonname;
  }
  return Status::OK();
}

Status SockaddrFromHostPort(const HostPort& host_port, Sockaddr* addr) {
  vector<Sockaddr> addrs;
  RETURN_NOT_OK(host_port.ResolveAddresses(&addrs));
  if (addrs.empty()) {
    return Status::NetworkError("Unable to resolve address", host_port.ToString());
  }
  *addr = addrs[0];
  if (addrs.size() > 1) {
    VLOG(1) << "Hostname " << host_port.host() << " resolved to more than one address. "
            << "Using address: " << addr->ToString();
  }
  return Status::OK();
}

bool IsAddrOneOf(const Sockaddr& addr, const vector<Sockaddr>& ref_addresses) {
  if (!addr.is_ip()) {
    return false;
  }
  DCHECK_NE(0, addr.port());
  const bool have_match = std::any_of(
      ref_addresses.begin(),
      ref_addresses.end(),
      [&addr](const Sockaddr& s) {
        if (!s.is_ip()) {
          return false;
        }
        const bool is_same_or_wildcard_port = s.port() == addr.port() ||
            s.port() == 0;
        if (s.IsWildcard()) {
          return is_same_or_wildcard_port;
        }
        const auto& lhs = s.ipv4_addr().sin_addr;
        const auto& rhs = addr.ipv4_addr().sin_addr;
        return is_same_or_wildcard_port &&
            memcmp(&lhs.s_addr, &rhs.s_addr, sizeof(decltype(lhs))) == 0;
  });
  VLOG(2) << Substitute("found IP address match for $0 among $1",
                        addr.ToString(),
                        JoinMapped(ref_addresses, [](const Sockaddr& addr) {
                          return addr.ToString();
                        }, ","));
  return have_match;
}

Status HostPortsFromAddrs(const vector<Sockaddr>& addrs, vector<HostPort>* hps) {
  DCHECK(!addrs.empty());
  for (const auto& addr : addrs) {
    if (!addr.is_ip()) continue;
    HostPort hp;
    RETURN_NOT_OK_PREPEND(HostPortFromSockaddrReplaceWildcard(addr, &hp),
                          "could not get RPC hostport");
    hps->emplace_back(std::move(hp));
  }
  return Status::OK();
}

Status GetRandomPort(const string& address, uint16_t* port) {
  Sockaddr sockaddr;
  RETURN_NOT_OK(sockaddr.ParseString(address, 0));
  Socket listener;
  RETURN_NOT_OK(listener.Init(sockaddr.family(), 0));
  RETURN_NOT_OK(listener.Bind(sockaddr));
  Sockaddr listen_address;
  RETURN_NOT_OK(listener.GetSocketAddress(&listen_address));
  *port = listen_address.port();
  return Status::OK();
}

void TryRunLsof(const Sockaddr& addr, vector<string>* log) {
#if defined(__APPLE__)
  string cmd = Substitute(
      "lsof -n -i 'TCP:$0' -sTCP:LISTEN ; "
      "for pid in $$(lsof -F p -n -i 'TCP:$0' -sTCP:LISTEN | cut -f 2 -dp) ; do"
      "  pstree $$pid || ps h -p $$pid;"
      "done",
      addr.port());
#else
  // Little inline bash script prints the full ancestry of any pid listening
  // on the same port as 'addr'. We could use 'pstree -s', but that option
  // doesn't exist on el6.
  string cmd = Substitute(
      "export PATH=$$PATH:/usr/sbin ; "
      "lsof -n -i 'TCP:$0' -sTCP:LISTEN ; "
      "for pid in $$(lsof -F p -n -i 'TCP:$0' -sTCP:LISTEN | grep p | cut -f 2 -dp) ; do"
      "  while [ $$pid -gt 1 ] ; do"
      "    ps h -fp $$pid ;"
      "    stat=($$(</proc/$$pid/stat)) ;"
      "    pid=$${stat[3]} ;"
      "  done ; "
      "done",
      addr.port());
#endif // defined(__APPLE__)
  LOG_STRING(WARNING, log)
      << "Trying to use lsof to find any processes listening on "
      << addr.ToString();
  LOG_STRING(INFO, log) << "$ " << cmd;
  vector<string> argv = { "bash", "-c", cmd };
  string results;
  Status s = Subprocess::Call(argv, "", &results);
  if (PREDICT_FALSE(!s.ok())) {
    LOG_STRING(WARNING, log) << s.ToString();
  }
  LOG_STRING(WARNING, log) << results;
}

string GetBindIpForDaemon(int index, BindMode bind_mode) {
  // The server index should range from (0, max_servers] since
  // the range for last octet for a valid unicast IP address ranges is (0, 255).
  CHECK(0 < index && index <= kServersMaxNum) << Substitute(
      "server index $0 is not in range ($1, $2]", index, 0, kServersMaxNum);

  static constexpr uint32_t kMaxPid = 1 << kPidBits;
  switch (bind_mode) {
    case BindMode::UNIQUE_LOOPBACK: {
      uint32_t pid = getpid();
      if (pid >= kMaxPid) {
        LOG(INFO) << Substitute(
            "PID $0 is more than $1 bits wide, substituted with $2",
            pid, kPidBits, pid % kMaxPid);
        pid %= kMaxPid;
      }
      uint32_t ip = (pid << kServerIdxBits) | static_cast<uint32_t>(index);
      uint8_t octets[] = {
          static_cast<uint8_t>((ip >> 16) & 0xff),
          static_cast<uint8_t>((ip >>  8) & 0xff),
          static_cast<uint8_t>((ip >>  0) & 0xff),
      };
      // Range for the last octet of a valid unicast IP address is (0, 255).
      CHECK(0 < octets[2] && octets[2] < UINT8_MAX) << Substitute(
          "last IP octet $0 is not in range ($1, $2)", octets[2], 0, UINT8_MAX);
      return Substitute("127.$0.$1.$2", octets[0], octets[1], octets[2]);
    }
    case BindMode::WILDCARD:
      return kWildcardIpAddr;
    case BindMode::LOOPBACK:
      return kLoopbackIpAddr;
    default:
      LOG(FATAL) << "unknown bind mode";
  }
}

} // namespace kudu
