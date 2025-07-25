// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/gcs/gcs_client/global_state_accessor.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ray/common/asio/instrumented_io_context.h"

namespace ray {
namespace gcs {

GlobalStateAccessor::GlobalStateAccessor(const GcsClientOptions &gcs_client_options) {
  gcs_client_ = std::make_unique<GcsClient>(gcs_client_options);
  io_service_ = std::make_unique<instrumented_io_context>();

  std::promise<bool> promise;
  thread_io_service_ = std::make_unique<std::thread>([this, &promise] {
    SetThreadName("global.accessor");
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work(
        io_service_->get_executor());
    promise.set_value(true);
    io_service_->run();
  });
  promise.get_future().get();
}

GlobalStateAccessor::~GlobalStateAccessor() { Disconnect(); }

bool GlobalStateAccessor::Connect() {
  absl::WriterMutexLock lock(&mutex_);
  if (!is_connected_) {
    is_connected_ = true;
    return gcs_client_->Connect(*io_service_).ok();
  }
  RAY_LOG(DEBUG) << "Duplicated connection for GlobalStateAccessor.";
  return true;
}

void GlobalStateAccessor::Disconnect() {
  absl::WriterMutexLock lock(&mutex_);
  RAY_LOG(DEBUG) << "Global state accessor disconnect";
  if (is_connected_) {
    io_service_->stop();
    thread_io_service_->join();
    gcs_client_->Disconnect();
    is_connected_ = false;
  }
}

std::vector<std::string> GlobalStateAccessor::GetAllJobInfo(
    bool skip_submission_job_info_field, bool skip_is_running_tasks_field) {
  // This method assumes GCS is HA and does not return any error. On GCS down, it
  // retries indefinitely.
  std::vector<std::string> job_table_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Jobs().AsyncGetAll(
        /*job_or_submission_id=*/std::nullopt,
        skip_submission_job_info_field,
        skip_is_running_tasks_field,
        TransformForMultiItemCallback<rpc::JobTableData>(job_table_data, promise),
        /*timeout_ms=*/-1);
  }
  promise.get_future().get();
  return job_table_data;
}

JobID GlobalStateAccessor::GetNextJobID() {
  std::promise<JobID> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Jobs().AsyncGetNextJobID(
        [&promise](const JobID &job_id) { promise.set_value(job_id); });
  }
  return promise.get_future().get();
}

std::vector<std::string> GlobalStateAccessor::GetAllNodeInfo() {
  // This method assumes GCS is HA and does not return any error. On GCS down, it
  // retries indefinitely.
  std::vector<std::string> node_table_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Nodes().AsyncGetAll(
        TransformForMultiItemCallback<rpc::GcsNodeInfo>(node_table_data, promise),
        /*timeout_ms=*/-1);
  }
  promise.get_future().get();
  return node_table_data;
}

std::vector<std::string> GlobalStateAccessor::GetAllTaskEvents() {
  std::vector<std::string> task_events;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Tasks().AsyncGetTaskEvents(
        TransformForMultiItemCallback<rpc::TaskEvents>(task_events, promise));
  }
  promise.get_future().get();
  return task_events;
}

std::vector<std::string> GlobalStateAccessor::GetAllAvailableResources() {
  std::vector<std::string> available_resources;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->NodeResources().AsyncGetAllAvailableResources(
        TransformForMultiItemCallback<rpc::AvailableResources>(available_resources,
                                                               promise));
  }
  promise.get_future().get();
  return available_resources;
}

std::vector<std::string> GlobalStateAccessor::GetAllTotalResources() {
  std::vector<std::string> total_resources;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->NodeResources().AsyncGetAllTotalResources(
        TransformForMultiItemCallback<rpc::TotalResources>(total_resources, promise));
  }
  promise.get_future().get();
  return total_resources;
}

std::unordered_map<NodeID, int64_t> GlobalStateAccessor::GetDrainingNodes() {
  std::promise<std::unordered_map<NodeID, int64_t>> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->NodeResources().AsyncGetDrainingNodes(
        [&promise](const std::unordered_map<NodeID, int64_t> &draining_nodes) {
          promise.set_value(draining_nodes);
        });
  }
  return promise.get_future().get();
}

std::unique_ptr<std::string> GlobalStateAccessor::GetAllResourceUsage() {
  std::unique_ptr<std::string> resource_batch_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->NodeResources().AsyncGetAllResourceUsage(
        TransformForItemCallback<rpc::ResourceUsageBatchData>(resource_batch_data,
                                                              promise));
  }
  promise.get_future().get();
  return resource_batch_data;
}

std::vector<std::string> GlobalStateAccessor::GetAllActorInfo(
    const std::optional<ActorID> &actor_id,
    const std::optional<JobID> &job_id,
    const std::optional<std::string> &actor_state_name) {
  std::vector<std::string> actor_table_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Actors().AsyncGetAllByFilter(
        actor_id,
        job_id,
        actor_state_name,
        TransformForMultiItemCallback<rpc::ActorTableData>(actor_table_data, promise));
  }
  promise.get_future().get();
  return actor_table_data;
}

std::unique_ptr<std::string> GlobalStateAccessor::GetActorInfo(const ActorID &actor_id) {
  std::unique_ptr<std::string> actor_table_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Actors().AsyncGet(
        actor_id,
        TransformForOptionalItemCallback<rpc::ActorTableData>(actor_table_data, promise));
  }
  promise.get_future().get();
  return actor_table_data;
}

std::unique_ptr<std::string> GlobalStateAccessor::GetWorkerInfo(
    const WorkerID &worker_id) {
  std::unique_ptr<std::string> worker_table_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Workers().AsyncGet(
        worker_id,
        TransformForOptionalItemCallback<rpc::WorkerTableData>(worker_table_data,
                                                               promise));
  }
  promise.get_future().get();
  return worker_table_data;
}

std::vector<std::string> GlobalStateAccessor::GetAllWorkerInfo() {
  std::vector<std::string> worker_table_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Workers().AsyncGetAll(
        TransformForMultiItemCallback<rpc::WorkerTableData>(worker_table_data, promise));
  }
  promise.get_future().get();
  return worker_table_data;
}

bool GlobalStateAccessor::AddWorkerInfo(const std::string &serialized_string) {
  auto data_ptr = std::make_shared<rpc::WorkerTableData>();
  data_ptr->ParseFromString(serialized_string);
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Workers().AsyncAdd(data_ptr, [&promise](const Status &status) {
      RAY_CHECK_OK(status);
      promise.set_value(true);
    });
  }
  promise.get_future().get();
  return true;
}

uint32_t GlobalStateAccessor::GetWorkerDebuggerPort(const WorkerID &worker_id) {
  absl::ReaderMutexLock debugger_lock(&debugger_port_mutex_);
  std::promise<uint32_t> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Workers().AsyncGet(
        worker_id,
        [&promise](const Status &status,
                   const std::optional<rpc::WorkerTableData> &result) {
          RAY_CHECK_OK(status);
          if (result.has_value()) {
            promise.set_value(result->debugger_port());
            return;
          }
          promise.set_value(0);
        });
  }
  // Setup a timeout
  auto future = promise.get_future();
  if (future.wait_for(std::chrono::seconds(
          RayConfig::instance().gcs_server_request_timeout_seconds())) !=
      std::future_status::ready) {
    RAY_LOG(FATAL) << "Failed to get the debugger port within the timeout setting.";
    return 0;
  }
  return future.get();
}

bool GlobalStateAccessor::UpdateWorkerDebuggerPort(const WorkerID &worker_id,
                                                   const uint32_t debugger_port) {
  // debugger mutex is used to avoid concurrent updates to the same worker
  absl::WriterMutexLock debugger_lock(&debugger_port_mutex_);
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Workers().AsyncUpdateDebuggerPort(
        worker_id, debugger_port, [&promise](const Status &status) {
          RAY_CHECK_OK(status);
          promise.set_value(status.ok());
        });
  }
  // Setup a timeout for the update request
  auto future = promise.get_future();
  if (future.wait_for(std::chrono::seconds(
          RayConfig::instance().gcs_server_request_timeout_seconds())) !=
      std::future_status::ready) {
    RAY_LOG(FATAL) << "Failed to update the debugger port within the timeout setting.";
    return false;
  }
  return future.get();
}

bool GlobalStateAccessor::UpdateWorkerNumPausedThreads(
    const WorkerID &worker_id, const int num_paused_threads_delta) {
  // Verify that the current thread is not the same as the thread_io_service_ to prevent
  // deadlock
  RAY_CHECK(thread_io_service_->get_id() != std::this_thread::get_id())
      << "This method should not be called from the same thread as the "
         "thread_io_service_";

  // debugger mutex is used to avoid concurrent updates to the same worker
  absl::WriterMutexLock debugger_lock(&debugger_threads_mutex_);
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->Workers().AsyncUpdateWorkerNumPausedThreads(
        worker_id, num_paused_threads_delta, [&promise](const Status &status) {
          RAY_CHECK_OK(status);
          promise.set_value(status.ok());
        });
  }
  // Setup a timeout for the update request
  auto future = promise.get_future();
  if (future.wait_for(std::chrono::seconds(
          RayConfig::instance().gcs_server_request_timeout_seconds())) !=
      std::future_status::ready) {
    RAY_LOG(FATAL)
        << "Failed to update the num of paused threads within the timeout setting.";
    return false;
  }
  return future.get();
}

std::vector<std::string> GlobalStateAccessor::GetAllPlacementGroupInfo() {
  std::vector<std::string> placement_group_table_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->PlacementGroups().AsyncGetAll(
        TransformForMultiItemCallback<rpc::PlacementGroupTableData>(
            placement_group_table_data, promise));
  }
  promise.get_future().get();
  return placement_group_table_data;
}

std::unique_ptr<std::string> GlobalStateAccessor::GetPlacementGroupInfo(
    const PlacementGroupID &placement_group_id) {
  std::unique_ptr<std::string> placement_group_table_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->PlacementGroups().AsyncGet(
        placement_group_id,
        TransformForOptionalItemCallback<rpc::PlacementGroupTableData>(
            placement_group_table_data, promise));
  }
  promise.get_future().get();
  return placement_group_table_data;
}

std::unique_ptr<std::string> GlobalStateAccessor::GetPlacementGroupByName(
    const std::string &placement_group_name, const std::string &ray_namespace) {
  std::unique_ptr<std::string> placement_group_table_data;
  std::promise<bool> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->PlacementGroups().AsyncGetByName(
        placement_group_name,
        ray_namespace,
        TransformForOptionalItemCallback<rpc::PlacementGroupTableData>(
            placement_group_table_data, promise));
  }
  promise.get_future().get();
  return placement_group_table_data;
}

std::unique_ptr<std::string> GlobalStateAccessor::GetInternalKV(const std::string &ns,
                                                                const std::string &key) {
  absl::ReaderMutexLock lock(&mutex_);
  std::string value;

  Status status = gcs_client_->InternalKV().Get(ns, key, GetGcsTimeoutMs(), value);
  return status.ok() ? std::make_unique<std::string>(value) : nullptr;
}

std::string GlobalStateAccessor::GetSystemConfig() {
  std::promise<std::string> promise;
  {
    absl::ReaderMutexLock lock(&mutex_);
    gcs_client_->InternalKV().AsyncGetInternalConfig(
        [&promise](const Status &status,
                   const std::optional<std::string> &stored_raylet_config) {
          RAY_CHECK_OK(status);
          promise.set_value(*stored_raylet_config);
        });
  }
  auto future = promise.get_future();
  if (future.wait_for(std::chrono::seconds(
          RayConfig::instance().gcs_server_request_timeout_seconds())) !=
      std::future_status::ready) {
    RAY_LOG(FATAL) << "Failed to get system config within the timeout setting.";
  }
  return future.get();
}

ray::Status GlobalStateAccessor::GetNode(const std::string &node_id_hex_str,
                                         std::string *node_info) {
  const auto end_time_point =
      current_time_ms() + RayConfig::instance().raylet_start_wait_time_s() * 1000;
  const auto node_id_binary = NodeID::FromHex(node_id_hex_str).Binary();

  std::vector<rpc::GcsNodeInfo> node_infos;
  while (true) {
    rpc::GetAllNodeInfoRequest_Filters filters;
    filters.set_state(rpc::GcsNodeInfo_GcsNodeState::GcsNodeInfo_GcsNodeState_ALIVE);
    filters.set_node_id(node_id_binary);
    {
      absl::ReaderMutexLock lock(&mutex_);
      auto timeout_ms =
          std::max(end_time_point - current_time_ms(), static_cast<int64_t>(0));
      RAY_ASSIGN_OR_RETURN(
          node_infos,
          gcs_client_->Nodes().GetAllNoCacheWithFilters(timeout_ms, std::move(filters)));
    }
    if (!node_infos.empty()) {
      *node_info = node_infos[0].SerializeAsString();
      return Status::OK();
    }

    if (current_time_ms() >= end_time_point) {
      return Status::NotFound(
          "GCS cannot find the node with node ID " + node_id_hex_str +
          ". The node registration may not be complete yet before the timeout." +
          " Try increase the RAY_raylet_start_wait_time_s config.");
    }
    RAY_LOG(WARNING) << "Retrying to get node with node ID " << node_id_hex_str;
    // Some of the information may not be in GCS yet, so wait a little bit.
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

ray::Status GlobalStateAccessor::GetNodeToConnectForDriver(
    const std::string &node_ip_address, std::string *node_to_connect) {
  const auto end_time_point =
      current_time_ms() + RayConfig::instance().raylet_start_wait_time_s() * 1000;

  std::vector<rpc::GcsNodeInfo> node_infos;
  rpc::GetAllNodeInfoRequest_Filters filters;
  filters.set_state(rpc::GcsNodeInfo_GcsNodeState::GcsNodeInfo_GcsNodeState_ALIVE);
  filters.set_node_ip_address(node_ip_address);
  while (true) {
    {
      absl::ReaderMutexLock lock(&mutex_);
      auto timeout_ms =
          std::max(end_time_point - current_time_ms(), static_cast<int64_t>(0));
      RAY_ASSIGN_OR_RETURN(
          node_infos, gcs_client_->Nodes().GetAllNoCacheWithFilters(timeout_ms, filters));
    }
    if (!node_infos.empty()) {
      *node_to_connect = node_infos[0].SerializeAsString();
      return Status::OK();
    }

    std::string gcs_address;
    {
      absl::WriterMutexLock lock(&mutex_);
      auto [address, _] = gcs_client_->GetGcsServerAddress();
      gcs_address = std::move(address);
    }
    filters.set_node_ip_address(gcs_address);
    {
      absl::ReaderMutexLock lock(&mutex_);
      auto timeout_ms = end_time_point - current_time_ms();
      RAY_ASSIGN_OR_RETURN(
          node_infos, gcs_client_->Nodes().GetAllNoCacheWithFilters(timeout_ms, filters));
    }
    if (node_infos.empty() && node_ip_address == gcs_address) {
      filters.set_node_ip_address("127.0.0.1");
      {
        absl::ReaderMutexLock lock(&mutex_);
        auto timeout_ms =
            std::max(end_time_point - current_time_ms(), static_cast<int64_t>(0));
        RAY_ASSIGN_OR_RETURN(
            node_infos,
            gcs_client_->Nodes().GetAllNoCacheWithFilters(timeout_ms, filters));
      }
    }
    if (!node_infos.empty()) {
      RAY_LOG(INFO) << "This node has an IP address of " << node_ip_address
                    << ", but we cannot find a local Raylet with the same address. "
                    << "This can happen when you connect to the Ray cluster "
                    << "with a different IP address or when connecting to a container.";
      *node_to_connect = node_infos[0].SerializeAsString();
      return Status::OK();
    }

    if (current_time_ms() >= end_time_point) {
      std::ostringstream oss;
      oss << "This node has an IP address of " << node_ip_address << ", and Ray "
          << "expects this IP address to be either the GCS address or one of"
          << " the Raylet addresses. Connected to GCS at " << gcs_address
          << ", and found no Raylet with this IP address. "
          << "You might need to provide --node-ip-address to specify the IP "
          << "address that the head should use when sending to this node.";
      return Status::NotFound(oss.str());
    }
    RAY_LOG(WARNING) << "Some processes that the driver needs to connect to have "
                        "not registered with GCS, so retrying. Have you run "
                        "'ray start' on this node?";
    // Some of the information may not be in GCS yet, so wait a little bit.
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace gcs
}  // namespace ray
