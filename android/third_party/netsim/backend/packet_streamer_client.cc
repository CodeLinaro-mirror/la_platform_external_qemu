// Copyright 2022 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "backend/packet_streamer_client.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include "aemu/base/process/Command.h"
#include "android/base/system/System.h"
#include "android/emulation/control/interceptor/MetricsInterceptor.h"
#include "host-common/FeatureControl.h"
#include "grpcpp/channel.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "util/log.h"
#include "util/os_utils.h"
#include "util/string_utils.h"

using android::control::interceptor::MetricsInterceptorFactory;

namespace netsim::packet {
namespace {

constexpr int kMaxConnectRetries = 3;
std::chrono::milliseconds g_connection_deadline{1000};
// Exponential backoff of {1, 2, 4}s across kMaxConnectRetries preserves exact
// cold/warm start behavior while keeping the shared worst-case failure wait to
// 4 * 1s + (1 + 2 + 4)s = 11s (below a single 15s HangDetector interval).
std::chrono::milliseconds g_initial_backoff{1000};
constexpr std::chrono::seconds kFollowerWaitTimeout{15};

std::mutex channel_mutex;
std::condition_variable channel_cv;
std::string custom_packet_stream_endpoint = "";
std::shared_ptr<grpc::Channel> packet_stream_channel;
bool connect_in_progress = false;

std::shared_ptr<grpc::Channel> CreateGrpcChannel(std::string endpoint) {
  if (endpoint.empty()) {
    auto port = netsim::osutils::GetServerAddress();
    if (!port.has_value()) return nullptr;
    endpoint = "localhost:" + port.value();
  }

  if (endpoint.empty()) return nullptr;
  BtsLogInfo("Creating a Grpc channel to %s", endpoint.c_str());

  std::vector<
      std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>>
      interceptors;
  interceptors.emplace_back(std::make_unique<MetricsInterceptorFactory>());
  grpc::ChannelArguments args;
  return grpc::experimental::CreateCustomChannelWithInterceptors(
      endpoint, grpc::InsecureChannelCredentials(), args,
      std::move(interceptors));
}

bool GrpcChannelReady(const std::shared_ptr<grpc::Channel> &channel) {
  if (!channel) return false;
  std::chrono::milliseconds deadline_ms;
  {
    std::lock_guard<std::mutex> lock(channel_mutex);
    deadline_ms = g_connection_deadline;
  }
  auto deadline = std::chrono::system_clock::now() + deadline_ms;
  return channel->WaitForConnected(deadline);
}

std::unique_ptr<android::base::ObservableProcess> RunNetsimd(
    NetsimdOptions options) {
  std::string executable = "netsimd";
  if (android::featurecontrol::isEnabled(android::featurecontrol::NetsimX)) {
    auto netsimdx =
        android::base::System::get()->findBundledExecutable("netsimdx");
    if (!netsimdx.empty()) {
      executable = "netsimdx";
    }
  }
  auto exe = android::base::System::get()->findBundledExecutable(executable);
  std::vector<std::string> program_with_args{exe};
  if (options.no_cli_ui) program_with_args.push_back("--no-cli-ui");
  if (options.no_web_ui) program_with_args.push_back("--no-web-ui");
  if (!options.host_dns.empty()) {
    program_with_args.push_back("--host-dns=" + options.host_dns);
  }
  if (!options.http_proxy.empty()) {
    program_with_args.push_back("--http-proxy=" + options.http_proxy);
  }
  for (auto flag : stringutils::Split(options.netsim_args, " "))
    program_with_args.push_back(std::string(flag));

  BtsLogInfo("Netsimd launch command:");
  for (auto arg : program_with_args) BtsLogInfo("%s", arg.c_str());
  auto cmd = android::base::Command::create(program_with_args);

  auto netsimd = cmd.asDeamon().execute();
  if (netsimd) {
    BtsLogInfo("Running netsimd as pid: %d.", netsimd->pid());
  }

  return netsimd;
}

}  // namespace

void SetPacketStreamEndpoint(const std::string &endpoint) {
  std::lock_guard<std::mutex> lock(channel_mutex);
  custom_packet_stream_endpoint = (endpoint == "default") ? "" : endpoint;
  packet_stream_channel.reset();
}

void SetConnectionBudgetForTesting(std::chrono::milliseconds deadline,
                                   std::chrono::milliseconds initial_backoff) {
  std::lock_guard<std::mutex> lock(channel_mutex);
  g_connection_deadline = deadline;
  g_initial_backoff = initial_backoff;
}

std::shared_ptr<grpc::Channel> GetChannel(NetsimdOptions options) {
  std::unique_lock<std::mutex> lock(channel_mutex);
  if (packet_stream_channel &&
      packet_stream_channel->GetState(false) == GRPC_CHANNEL_READY) {
    return packet_stream_channel;
  }
  if (connect_in_progress) {
    if (!channel_cv.wait_for(lock, kFollowerWaitTimeout,
                             [] { return !connect_in_progress; })) {
      BtsLogError(
          "Timed out waiting for in-progress netsim channel connection.");
      return nullptr;
    }
    return packet_stream_channel;
  }
  connect_in_progress = true;
  std::shared_ptr<grpc::Channel> existing_channel =
      std::move(packet_stream_channel);
  const std::string endpoint_override = custom_packet_stream_endpoint;
  auto backoff = g_initial_backoff;
  lock.unlock();

  std::shared_ptr<grpc::Channel> channel;
  {
    // Publishes |channel| and wakes the followers when this block exits, even
    // if the attempt unwinds early. The block must end before `return channel`
    // so the guard never observes the moved-from value an implicit move on
    // return leaves behind when the compiler does not apply NRVO.
    struct ConnectScopeGuard {
      std::shared_ptr<grpc::Channel>& result;
      ~ConnectScopeGuard() {
        {
          std::lock_guard<std::mutex> g(channel_mutex);
          packet_stream_channel = result;
          connect_in_progress = false;
        }
        channel_cv.notify_all();
      }
    } connect_guard{channel};

    std::unique_ptr<android::base::ObservableProcess> netsimProc;
    for (int i = 0; i <= kMaxConnectRetries; ++i) {
      channel = existing_channel ? std::move(existing_channel)
                                 : CreateGrpcChannel(endpoint_override);
      if (GrpcChannelReady(channel)) break;
      channel.reset();
      // If this was the final attempt, break before we sleep or restart netsim.
      if (i == kMaxConnectRetries) break;

      if ((!netsimProc || !netsimProc->isAlive()) &&
          endpoint_override.empty()) {
        BtsLogInfo("Starting netsim since %s",
                   netsimProc ? "the process died" : "it is not yet launched");
        netsimProc = RunNetsimd(options);
      }
      BtsLogInfo("Retry connecting to netsim in %lld ms.",
                 static_cast<long long>(backoff.count()));
      std::this_thread::sleep_for(backoff);
      backoff *= 2;
    }
  }

  if (!channel) BtsLogError("Unable to get a packet stream channel.");
  return channel;
}

std::shared_ptr<grpc::Channel> CreateChannel(NetsimdOptions options) {
  return GetChannel(options);
}

std::shared_ptr<grpc::Channel> CreateChannel(
    std::string _rootcanal_controller_properties_file) {
  return GetChannel({});
}

}  // namespace netsim::packet
