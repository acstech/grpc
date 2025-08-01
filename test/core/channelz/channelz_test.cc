//
//
// Copyright 2017 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include "src/core/channelz/channelz.h"

#include <grpc/credentials.h>
#include <grpc/event_engine/event_engine.h>
#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpc/support/alloc.h>
#include <grpc/support/json.h>
#include <grpc/support/time.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "src/core/channelz/channelz_registry.h"
#include "src/core/channelz/v2tov1/legacy_api.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/event_engine/default_event_engine.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/server/server.h"
#include "src/core/util/json/json.h"
#include "src/core/util/json/json_reader.h"
#include "src/core/util/json/json_writer.h"
#include "src/core/util/notification.h"
#include "src/core/util/upb_utils.h"
#include "src/core/util/useful.h"
#include "src/core/util/wait_for_single_owner.h"
#include "test/core/event_engine/event_engine_test_utils.h"
#include "test/core/test_util/test_config.h"
#include "test/cpp/util/channel_trace_proto_helper.h"

using grpc_event_engine::experimental::GetDefaultEventEngine;

namespace grpc_core {
namespace channelz {
namespace testing {

// testing peer to access channel internals
class CallCountingHelperPeer {
 public:
  explicit CallCountingHelperPeer(CallCountingHelper* node) : node_(node) {}

  gpr_timespec last_call_started_time() const {
    return gpr_cycle_counter_to_time(
        node_->last_call_started_cycle_.load(std::memory_order_relaxed));
  }

 private:
  CallCountingHelper* node_;
};

namespace {

std::vector<intptr_t> GetUuidListFromArray(const Json::Array& arr) {
  std::vector<intptr_t> uuids;
  for (const Json& value : arr) {
    EXPECT_EQ(value.type(), Json::Type::kObject);
    if (value.type() != Json::Type::kObject) continue;
    const Json::Object& object = value.object();
    auto it = object.find("ref");
    EXPECT_NE(it, object.end());
    if (it == object.end()) continue;
    EXPECT_EQ(it->second.type(), Json::Type::kObject);
    if (it->second.type() != Json::Type::kObject) continue;
    const Json::Object& ref_object = it->second.object();
    it = ref_object.find("channelId");
    EXPECT_NE(it, ref_object.end());
    if (it != ref_object.end()) {
      uuids.push_back(atoi(it->second.string().c_str()));
    }
  }
  return uuids;
}

void ValidateJsonArraySize(const Json& array, size_t expected) {
  if (expected == 0) {
    if (array.type() == Json::Type::kArray) {
      ASSERT_EQ(array.array().size(), 0);
    } else {
      ASSERT_EQ(array.type(), Json::Type::kNull);
    }
  } else {
    ASSERT_EQ(array.type(), Json::Type::kArray);
    EXPECT_EQ(array.array().size(), expected);
  }
}

void ValidateJsonEnd(const Json& json, bool end) {
  auto it = json.object().find("end");
  if (end) {
    ASSERT_NE(it, json.object().end());
    ASSERT_EQ(it->second.type(), Json::Type::kBoolean);
    EXPECT_TRUE(it->second.boolean());
  } else {
    if (it != json.object().end()) {
      ASSERT_EQ(it->second.type(), Json::Type::kBoolean);
      EXPECT_FALSE(it->second.boolean());
    }
  }
}

void ValidateGetTopChannels(size_t expected_channels) {
  char* json_str = grpc_channelz_get_top_channels(0);
  grpc::testing::ValidateGetTopChannelsResponseProtoJsonTranslation(json_str);
  auto parsed_json = JsonParse(json_str);
  gpr_free(json_str);
  ASSERT_TRUE(parsed_json.ok()) << parsed_json.status();
  ASSERT_EQ(parsed_json->type(), Json::Type::kObject);
  // This check will naturally have to change when we support pagination.
  // tracked: https://github.com/grpc/grpc/issues/16019.
  Json channel_json;
  auto it = parsed_json->object().find("channel");
  if (it != parsed_json->object().end()) channel_json = it->second;
  ValidateJsonArraySize(channel_json, expected_channels);
  ValidateJsonEnd(*parsed_json, true);
  // Also check that the core API formats this correctly.
  char* core_api_json_str = grpc_channelz_get_top_channels(0);
  grpc::testing::ValidateGetTopChannelsResponseProtoJsonTranslation(
      core_api_json_str);
  gpr_free(core_api_json_str);
}

void ValidateGetServers(size_t expected_servers) {
  char* json_str = grpc_channelz_get_servers(0);
  grpc::testing::ValidateGetServersResponseProtoJsonTranslation(json_str);
  auto parsed_json = JsonParse(json_str);
  gpr_free(json_str);
  ASSERT_TRUE(parsed_json.ok()) << parsed_json.status();
  ASSERT_EQ(parsed_json->type(), Json::Type::kObject);
  // This check will naturally have to change when we support pagination.
  // tracked: https://github.com/grpc/grpc/issues/16019.
  Json server_json;
  auto it = parsed_json->object().find("server");
  if (it != parsed_json->object().end()) server_json = it->second;
  ValidateJsonArraySize(server_json, expected_servers);
  ValidateJsonEnd(*parsed_json, true);
  // Also check that the core API formats this correctly.
  char* core_api_json_str = grpc_channelz_get_servers(0);
  grpc::testing::ValidateGetServersResponseProtoJsonTranslation(
      core_api_json_str);
  gpr_free(core_api_json_str);
}

class ChannelFixture {
 public:
  explicit ChannelFixture(int max_tracer_event_memory = 0) {
    grpc_arg client_a[] = {
        grpc_channel_arg_integer_create(
            const_cast<char*>(GRPC_ARG_MAX_CHANNEL_TRACE_EVENT_MEMORY_PER_NODE),
            max_tracer_event_memory),
        grpc_channel_arg_integer_create(
            const_cast<char*>(GRPC_ARG_ENABLE_CHANNELZ), true)};
    grpc_channel_args client_args = {GPR_ARRAY_SIZE(client_a), client_a};
    grpc_channel_credentials* creds = grpc_insecure_credentials_create();
    channel_ = grpc_channel_create("fake_target", creds, &client_args);
    grpc_channel_credentials_release(creds);
  }

  ~ChannelFixture() { grpc_channel_destroy(channel_); }

  grpc_channel* channel() { return channel_; }

 private:
  grpc_channel* channel_;
};

class ServerFixture {
 public:
  explicit ServerFixture(int max_tracer_event_memory = 0) {
    grpc_arg server_a[] = {
        grpc_channel_arg_integer_create(
            const_cast<char*>(GRPC_ARG_MAX_CHANNEL_TRACE_EVENT_MEMORY_PER_NODE),
            max_tracer_event_memory),
        grpc_channel_arg_integer_create(
            const_cast<char*>(GRPC_ARG_ENABLE_CHANNELZ), true),
    };
    grpc_channel_args server_args = {GPR_ARRAY_SIZE(server_a), server_a};
    server_ = grpc_server_create(&server_args, nullptr);
  }

  ~ServerFixture() { grpc_server_destroy(server_); }

  grpc_server* server() const { return server_; }

 private:
  grpc_server* server_;
};

struct ValidateChannelDataArgs {
  int64_t calls_started;
  int64_t calls_failed;
  int64_t calls_succeeded;
};

void ValidateChildInteger(const Json::Object& object, const std::string& key,
                          int64_t expected) {
  auto it = object.find(key);
  if (expected == 0) {
    ASSERT_EQ(it, object.end());
    return;
  }
  ASSERT_NE(it, object.end());
  ASSERT_EQ(it->second.type(), Json::Type::kString);
  int64_t gotten_number =
      static_cast<int64_t>(strtol(it->second.string().c_str(), nullptr, 0));
  EXPECT_EQ(gotten_number, expected);
}

void ValidateCounters(const std::string& json_str,
                      const ValidateChannelDataArgs& args) {
  auto json = JsonParse(json_str);
  ASSERT_TRUE(json.ok()) << json.status();
  ASSERT_EQ(json->type(), Json::Type::kObject);
  const Json::Object& object = json->object();
  auto it = object.find("data");
  ASSERT_NE(it, object.end());
  const Json& data = it->second;
  ASSERT_EQ(data.type(), Json::Type::kObject);
  ValidateChildInteger(data.object(), "callsStarted", args.calls_started);
  ValidateChildInteger(data.object(), "callsFailed", args.calls_failed);
  ValidateChildInteger(data.object(), "callsSucceeded", args.calls_succeeded);
}

void ValidateChannel(ChannelNode* channel,
                     const ValidateChannelDataArgs& args) {
  std::string json_str = channel->RenderJsonString();
  grpc::testing::ValidateChannelProtoJsonTranslation(
      v2tov1::StripAdditionalInfoFromJson(json_str).c_str());
  ValidateCounters(json_str, args);
  // also check that the core API formats this the correct way
  char* core_api_json_str = grpc_channelz_get_channel(channel->uuid());
  grpc::testing::ValidateGetChannelResponseProtoJsonTranslation(
      core_api_json_str);
  gpr_free(core_api_json_str);
}

void ValidateServer(ServerNode* server, const ValidateChannelDataArgs& args) {
  std::string json_str = server->RenderJsonString();
  grpc::testing::ValidateServerProtoJsonTranslation(
      v2tov1::StripAdditionalInfoFromJson(json_str).c_str());
  ValidateCounters(json_str, args);
  // also check that the core API formats this the correct way
  char* core_api_json_str = grpc_channelz_get_server(server->uuid());
  grpc::testing::ValidateGetServerResponseProtoJsonTranslation(
      core_api_json_str);
  gpr_free(core_api_json_str);
}

gpr_timespec GetLastCallStartedTime(CallCountingHelper* channel) {
  CallCountingHelperPeer peer(channel);
  return peer.last_call_started_time();
}

void ChannelzSleep(int64_t sleep_us) {
  gpr_sleep_until(gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                               gpr_time_from_micros(sleep_us, GPR_TIMESPAN)));
  ExecCtx::Get()->InvalidateNow();
}

}  // anonymous namespace

class ChannelzChannelTest : public ::testing::TestWithParam<size_t> {};

TEST_P(ChannelzChannelTest, BasicChannel) {
  ExecCtx exec_ctx;
  ChannelFixture channel(GetParam());
  ChannelNode* channelz_channel =
      grpc_channel_get_channelz_node(channel.channel());
  ValidateChannel(channelz_channel, {0, 0, 0});
}

TEST_P(ChannelzChannelTest, BasicChannelProto) {
  ExecCtx exec_ctx;
  ChannelFixture channel(GetParam());
  ChannelNode* channelz_channel =
      grpc_channel_get_channelz_node(channel.channel());
  upb_Arena* arena = upb_Arena_New();
  grpc_channelz_v2_Entity* entity = grpc_channelz_v2_Entity_new(arena);
  channelz_channel->SerializeEntity(entity, arena);
  EXPECT_EQ(grpc_channelz_v2_Entity_id(entity), channelz_channel->uuid());
  EXPECT_EQ(UpbStringToStdString(grpc_channelz_v2_Entity_kind(entity)),
            "channel");
  EXPECT_EQ(grpc_channelz_v2_Entity_orphaned(entity), false);
  size_t size;
  const grpc_channelz_v2_TraceEvent* const* trace =
      grpc_channelz_v2_Entity_trace(entity, &size);
  if (GetParam() <= 48) {
    EXPECT_EQ(size, 0);
  } else if (GetParam() > 1024 || size > 0) {
    ASSERT_EQ(size, 1);
    // TODO(ctiller): This should be a test of the trace, not the channel.
    // Also, emitting "Channel created" every time a channel is created is
    // probably not the most helpful thing to do.
    EXPECT_EQ(
        UpbStringToStdString(grpc_channelz_v2_TraceEvent_description(trace[0])),
        "Channel created");
  }
  upb_Arena_Free(arena);
}

class TestZTrace final : public ZTrace {
 public:
  void Run(Timestamp deadline, std::map<std::string, std::string> args,
           std::shared_ptr<grpc_event_engine::experimental::EventEngine> engine,
           absl::AnyInvocable<void(Json)> callback) override {
    engine->RunAfter(Duration::Milliseconds(100),
                     [callback = std::move(callback)]() mutable {
                       Json::Object object;
                       object["test"] = Json::FromString("yes");
                       callback(Json::FromObject(std::move(object)));
                     });
  }
};

class TestDataSource final : public DataSource {
 public:
  explicit TestDataSource(RefCountedPtr<BaseNode> node)
      : DataSource(std::move(node)) {
    SourceConstructed();
  }
  ~TestDataSource() { SourceDestructing(); }
  void AddData(DataSink sink) override {
    sink.AddData("testData", PropertyList().Set("test", "yes"));
  }
  std::unique_ptr<ZTrace> GetZTrace(absl::string_view name) override {
    if (name == "test_ztrace") return std::make_unique<TestZTrace>();
    return DataSource::GetZTrace(name);
  }
};

TEST_P(ChannelzChannelTest, BasicDataSource) {
  ExecCtx exec_ctx;
  ChannelFixture channel(GetParam());
  ChannelNode* channelz_channel =
      grpc_channel_get_channelz_node(channel.channel());
  {
    TestDataSource data_source(channelz_channel->Ref());
    Json json = channelz_channel->RenderJson();
    ASSERT_EQ(json.type(), Json::Type::kObject);
    const Json::Object& object = json.object();
    auto it_additional_info = object.find("additionalInfo");
    ASSERT_NE(it_additional_info, object.end());
    ASSERT_EQ(it_additional_info->second.type(), Json::Type::kObject);
    const Json::Object& additional_info = it_additional_info->second.object();
    auto it_test_data = additional_info.find("testData");
    ASSERT_NE(it_test_data, additional_info.end());
    ASSERT_EQ(it_test_data->second.type(), Json::Type::kObject);
    const Json::Object& test_data = it_test_data->second.object();
    auto it = test_data.find("test");
    ASSERT_NE(it, test_data.end());
    ASSERT_EQ(it->second.type(), Json::Type::kString);
    EXPECT_EQ(it->second.string(), "yes");
  }
  // Render again without the data source
  {
    Json json = channelz_channel->RenderJson();
    ASSERT_EQ(json.type(), Json::Type::kObject);
    const Json::Object& object = json.object();
    auto it = object.find("additionalInfo");
    if (it != object.end()) {
      ASSERT_EQ(it->second.type(), Json::Type::kObject);
      const Json::Object& additional_info = it->second.object();
      auto it_test_data = additional_info.find("testData");
      EXPECT_EQ(it_test_data, additional_info.end());
    }
  }
}

TEST_P(ChannelzChannelTest, ZTrace) {
  ExecCtx exec_ctx;
  ChannelFixture channel(GetParam());
  ChannelNode* channelz_channel =
      grpc_channel_get_channelz_node(channel.channel());
  TestDataSource data_source(channelz_channel->Ref());
  Notification done;
  std::string json_text;
  channelz_channel->RunZTrace(
      "test_ztrace", Timestamp::Now() + Duration::Milliseconds(300), {},
      grpc_event_engine::experimental::GetDefaultEventEngine(), [&](Json json) {
        json_text = JsonDump(json);
        done.Notify();
      });
  done.WaitForNotification();
  EXPECT_EQ(json_text, "{\"test\":\"yes\"}");
}

TEST(ChannelzChannelTest, ChannelzDisabled) {
  ExecCtx exec_ctx;
  // explicitly disable channelz
  grpc_arg arg[] = {
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_MAX_CHANNEL_TRACE_EVENT_MEMORY_PER_NODE),
          0),
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_ENABLE_CHANNELZ), false)};
  grpc_channel_args args = {GPR_ARRAY_SIZE(arg), arg};
  grpc_channel_credentials* creds = grpc_insecure_credentials_create();
  grpc_channel* channel = grpc_channel_create("fake_target", creds, &args);
  grpc_channel_credentials_release(creds);
  ChannelNode* channelz_channel = grpc_channel_get_channelz_node(channel);
  ASSERT_EQ(channelz_channel, nullptr);
  grpc_channel_destroy(channel);
}

TEST_P(ChannelzChannelTest, BasicChannelAPIFunctionality) {
  ExecCtx exec_ctx;
  ChannelFixture channel(GetParam());
  ChannelNode* channelz_channel =
      grpc_channel_get_channelz_node(channel.channel());
  channelz_channel->RecordCallStarted();
  channelz_channel->RecordCallFailed();
  channelz_channel->RecordCallSucceeded();
  ValidateChannel(channelz_channel, {1, 1, 1});
  channelz_channel->RecordCallStarted();
  channelz_channel->RecordCallFailed();
  channelz_channel->RecordCallSucceeded();
  channelz_channel->RecordCallStarted();
  channelz_channel->RecordCallFailed();
  channelz_channel->RecordCallSucceeded();
  ValidateChannel(channelz_channel, {3, 3, 3});
}

TEST_P(ChannelzChannelTest, LastCallStartedTime) {
  ExecCtx exec_ctx;
  CallCountingHelper counter;
  // start a call to set the last call started timestamp
  counter.RecordCallStarted();
  gpr_timespec time1 = GetLastCallStartedTime(&counter);
  // time gone by should not affect the timestamp
  ChannelzSleep(100);
  gpr_timespec time2 = GetLastCallStartedTime(&counter);
  EXPECT_EQ(gpr_time_cmp(time1, time2), 0);
  // calls succeeded or failed should not affect the timestamp
  ChannelzSleep(100);
  counter.RecordCallFailed();
  counter.RecordCallSucceeded();
  gpr_timespec time3 = GetLastCallStartedTime(&counter);
  EXPECT_EQ(gpr_time_cmp(time1, time3), 0);
  // another call started should affect the timestamp
  // sleep for extra long to avoid flakes (since we cache Now())
  ChannelzSleep(5000);
  counter.RecordCallStarted();
  gpr_timespec time4 = GetLastCallStartedTime(&counter);
  EXPECT_NE(gpr_time_cmp(time1, time4), 0);
}

class ChannelzRegistryBasedTest : public ::testing::TestWithParam<size_t> {
 protected:
  // ensure we always have a fresh registry for tests.
  void SetUp() override {
    WaitForSingleOwner(GetDefaultEventEngine());
    ResourceQuota::TestOnlyResetDefaultResourceQuota();
    ChannelzRegistry::TestOnlyReset();
  }

  void TearDown() override {
    WaitForSingleOwner(GetDefaultEventEngine());
    ResourceQuota::TestOnlyResetDefaultResourceQuota();
    ChannelzRegistry::TestOnlyReset();
  }
};

TEST_F(ChannelzRegistryBasedTest, BasicGetTopChannelsTest) {
  ExecCtx exec_ctx;
  ChannelFixture channel;
  ValidateGetTopChannels(1);
}

TEST_F(ChannelzRegistryBasedTest, NoChannelsTest) {
  ExecCtx exec_ctx;
  ValidateGetTopChannels(0);
}

TEST_F(ChannelzRegistryBasedTest, ManyChannelsTest) {
  ExecCtx exec_ctx;
  ChannelFixture channels[10];
  (void)channels;  // suppress unused variable error
  ValidateGetTopChannels(10);
}

TEST_F(ChannelzRegistryBasedTest, GetTopChannelsPagination) {
  ExecCtx exec_ctx;
  // This is over the pagination limit.
  ChannelFixture channels[150];
  (void)channels;  // suppress unused variable error
  char* json_str = grpc_channelz_get_top_channels(0);
  grpc::testing::ValidateGetTopChannelsResponseProtoJsonTranslation(json_str);
  auto parsed_json = JsonParse(json_str);
  gpr_free(json_str);
  ASSERT_TRUE(parsed_json.ok()) << parsed_json.status();
  ASSERT_EQ(parsed_json->type(), Json::Type::kObject);
  // 100 is the pagination limit.
  Json channel_json;
  auto it = parsed_json->object().find("channel");
  if (it != parsed_json->object().end()) channel_json = it->second;
  ValidateJsonArraySize(channel_json, 100);
  ValidateJsonEnd(*parsed_json, false);
  // Now we get the rest.
  char* json_str_2 = grpc_channelz_get_top_channels(101);
  grpc::testing::ValidateGetTopChannelsResponseProtoJsonTranslation(json_str_2);
  parsed_json = JsonParse(json_str_2);
  gpr_free(json_str_2);
  ASSERT_TRUE(parsed_json.ok()) << parsed_json.status();
  ASSERT_EQ(parsed_json->type(), Json::Type::kObject);
  channel_json = Json();
  it = parsed_json->object().find("channel");
  if (it != parsed_json->object().end()) channel_json = it->second;
  ValidateJsonArraySize(channel_json, 50);
  ValidateJsonEnd(*parsed_json, true);
}

TEST_F(ChannelzRegistryBasedTest, GetTopChannelsUuidCheck) {
  const intptr_t kNumChannels = 50;
  ExecCtx exec_ctx;
  ChannelFixture channels[kNumChannels];
  (void)channels;  // suppress unused variable error
  char* json_str = grpc_channelz_get_top_channels(0);
  auto parsed_json = JsonParse(json_str);
  gpr_free(json_str);
  ASSERT_TRUE(parsed_json.ok()) << parsed_json.status();
  ASSERT_EQ(parsed_json->type(), Json::Type::kObject);
  Json channel_json;
  auto it = parsed_json->object().find("channel");
  if (it != parsed_json->object().end()) channel_json = it->second;
  ValidateJsonArraySize(channel_json, kNumChannels);
  std::vector<intptr_t> uuids = GetUuidListFromArray(channel_json.array());
  for (int i = 0; i < kNumChannels; ++i) {
    EXPECT_EQ(i + 1, uuids[i]);
  }
}

TEST_F(ChannelzRegistryBasedTest, InternalChannelTest) {
  ExecCtx exec_ctx;
  ChannelFixture channels[10];
  (void)channels;  // suppress unused variable error
  // create an internal channel
  grpc_arg client_a[] = {
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_CHANNELZ_IS_INTERNAL_CHANNEL), 1),
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_ENABLE_CHANNELZ), true),
  };
  grpc_channel_args client_args = {GPR_ARRAY_SIZE(client_a), client_a};
  grpc_channel_credentials* creds = grpc_insecure_credentials_create();
  grpc_channel* internal_channel =
      grpc_channel_create("fake_target", creds, &client_args);
  grpc_channel_credentials_release(creds);
  // The internal channel should not be returned from the request
  ValidateGetTopChannels(10);
  grpc_channel_destroy(internal_channel);
}

TEST(ChannelzServerTest, BasicServerAPIFunctionality) {
  ExecCtx exec_ctx;
  ServerFixture server(10);
  ServerNode* channelz_server = Server::FromC(server.server())->channelz_node();
  channelz_server->RecordCallStarted();
  channelz_server->RecordCallFailed();
  channelz_server->RecordCallSucceeded();
  ValidateServer(channelz_server, {1, 1, 1});
  channelz_server->RecordCallStarted();
  channelz_server->RecordCallFailed();
  channelz_server->RecordCallSucceeded();
  channelz_server->RecordCallStarted();
  channelz_server->RecordCallFailed();
  channelz_server->RecordCallSucceeded();
  ValidateServer(channelz_server, {3, 3, 3});
}

TEST_F(ChannelzRegistryBasedTest, BasicGetServersTest) {
  ExecCtx exec_ctx;
  ServerFixture server;
  ValidateGetServers(1);
}

TEST_F(ChannelzRegistryBasedTest, NoServersTest) {
  ExecCtx exec_ctx;
  ValidateGetServers(0);
}

TEST_F(ChannelzRegistryBasedTest, ManyServersTest) {
  ExecCtx exec_ctx;
  ServerFixture servers[10];
  (void)servers;  // suppress unused variable error
  ValidateGetServers(10);
}

INSTANTIATE_TEST_SUITE_P(ChannelzChannelTestSweep, ChannelzChannelTest,
                         ::testing::Values(0, 8, 64, 1024, 1024 * 1024));

}  // namespace testing
}  // namespace channelz
}  // namespace grpc_core

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  ::testing::InitGoogleTest(&argc, argv);
  int ret = RUN_ALL_TESTS();
  grpc_shutdown();
  return ret;
}
