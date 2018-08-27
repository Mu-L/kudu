/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

package org.apache.kudu.flume.sink;

import static java.nio.charset.StandardCharsets.UTF_8;
import static org.apache.kudu.util.ClientTestUtil.scanTableToStrings;
import static org.apache.kudu.util.SecurityUtil.KUDU_TICKETCACHE_PROPERTY;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

import com.google.common.collect.ImmutableList;
import org.apache.flume.Event;
import org.apache.flume.EventDeliveryException;
import org.apache.flume.event.EventBuilder;
import org.junit.Before;
import org.junit.Test;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.apache.kudu.ColumnSchema;
import org.apache.kudu.Schema;
import org.apache.kudu.Type;
import org.apache.kudu.client.BaseKuduTest;
import org.apache.kudu.client.CreateTableOptions;
import org.apache.kudu.client.KuduTable;
import org.apache.kudu.client.MiniKuduCluster.MiniKuduClusterBuilder;

public class SecureKuduSinkTest extends BaseKuduTest {
  private static final Logger LOG = LoggerFactory.getLogger(SecureKuduSinkTest.class);
  private static final int TICKET_LIFETIME_SECONDS = 10;
  private static final int RENEWABLE_LIFETIME_SECONDS = 30;

  @Before
  public void clearTicketCacheProperty() {
    // Let Flume authenticate.
    System.clearProperty(KUDU_TICKETCACHE_PROPERTY);
  }

  @Override
  protected MiniKuduClusterBuilder getMiniClusterBuilder() {
    return super.getMiniClusterBuilder()
      .kdcTicketLifetime(TICKET_LIFETIME_SECONDS + "s")
      .kdcRenewLifetime(RENEWABLE_LIFETIME_SECONDS + "s")
      .enableKerberos();
  }

  @Test
  public void testEventsWithShortTickets() throws Exception {
    LOG.info("Creating new table...");
    ArrayList<ColumnSchema> columns = new ArrayList<>(1);
    columns.add(new ColumnSchema.ColumnSchemaBuilder("payload", Type.BINARY).key(true).build());
    CreateTableOptions createOptions =
        new CreateTableOptions().setRangePartitionColumns(ImmutableList.of("payload"))
        .setNumReplicas(1);
    String tableName = "test_long_lived_events";
    KuduTable table = createTable(tableName, new Schema(columns), createOptions);
    LOG.info("Created new table.");

    KuduSink sink = KuduSinkTestUtil.createSecureSink(
        tableName, getMasterAddressesAsString(), getClusterRoot());
    sink.start();

    LOG.info("Testing events at the beginning.");
    int eventCount = 10;

    processEvents(sink, 0, eventCount / 2);

    LOG.info("Waiting for tickets to expire");
    TimeUnit.SECONDS.sleep(RENEWABLE_LIFETIME_SECONDS * 2);

    LOG.info("Testing events after ticket renewal.");
    processEvents(sink, eventCount / 2, eventCount);

    List<String> rows = scanTableToStrings(table);
    assertEquals(eventCount + " row(s) expected", eventCount, rows.size());

    for (int i = 0; i < eventCount; i++) {
      assertTrue("incorrect payload", rows.get(i).contains("payload body " + i));
    }

    LOG.info("Testing {} events finished successfully.", eventCount);
  }

  private void processEvents(KuduSink sink, int from, int to) throws EventDeliveryException {
    List<Event> events = new ArrayList<>();
    for (int i = from; i < to; i++) {
      Event e = EventBuilder.withBody(String.format("payload body %s", i).getBytes(UTF_8));
      events.add(e);
    }

    KuduSinkTestUtil.processEvents(sink, events);
    LOG.info("Events flushed.");
  }
}
