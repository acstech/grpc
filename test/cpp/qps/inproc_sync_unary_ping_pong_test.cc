//
//
// Copyright 2015 gRPC authors.
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

#include <optional>
#include <set>

#include "absl/log/log.h"
#include "src/core/util/crash.h"
#include "test/core/test_util/test_config.h"
#include "test/cpp/qps/benchmark_config.h"
#include "test/cpp/qps/driver.h"
#include "test/cpp/qps/report.h"
#include "test/cpp/qps/server.h"
#include "test/cpp/util/test_config.h"
#include "test/cpp/util/test_credentials_provider.h"

namespace grpc {
namespace testing {

static const int WARMUP = 1;
static const int BENCHMARK = 3;

static void RunSynchronousUnaryPingPong() {
  LOG(INFO) << "Running Synchronous Unary Ping Pong";

  ClientConfig client_config;
  client_config.set_client_type(SYNC_CLIENT);
  client_config.set_outstanding_rpcs_per_channel(1);
  client_config.set_client_channels(1);
  client_config.set_rpc_type(UNARY);
  client_config.mutable_load_params()->mutable_closed_loop();

  ServerConfig server_config;
  server_config.set_server_type(SYNC_SERVER);

  RunScenarioOptions options(client_config, server_config);
  options.set_num_clients(1)
      .set_num_servers(1)
      .set_warmup_seconds(WARMUP)
      .set_benchmark_seconds(BENCHMARK)
      .set_spawn_local_worker_count(-2)
      .set_run_inproc(true);

  const auto result = RunScenario(options);

  GetReporter()->ReportQPS(*result);
  GetReporter()->ReportLatency(*result);
}

}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc::testing::InitTest(&argc, &argv, true);

  grpc::testing::RunSynchronousUnaryPingPong();

  return 0;
}
