//==============================================================
// Copyright © 2020 Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#ifndef PTI_SAMPLES_CL_HOT_FUNCTIONS_CL_API_COLLECTOR_H_
#define PTI_SAMPLES_CL_HOT_FUNCTIONS_CL_API_COLLECTOR_H_

#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>

#include "cl_tracer.h"
#include "cl_utils.h"

struct Function {
  uint64_t total_time;
  uint64_t min_time;
  uint64_t max_time;
  uint64_t call_count;

  bool operator>(const Function& r) const {
    if (total_time != r.total_time) {
      return total_time > r.total_time;
    }
    return call_count > r.call_count;
  }

  bool operator!=(const Function& r) const {
    if (total_time == r.total_time) {
      return call_count != r.call_count;
    }
    return true;
  }
};

using FunctionInfoMap = std::map<std::string, Function>;
using FunctionTimePoint = std::chrono::time_point<std::chrono::steady_clock>;

class ClApiCollector {
 public: // User Interface
  static ClApiCollector* Create(
      cl_device_id device,
      FunctionTimePoint base_time = std::chrono::steady_clock::now()) {
    PTI_ASSERT(device != nullptr);

    ClApiCollector* collector = new ClApiCollector(base_time);
    PTI_ASSERT(collector != nullptr);

    ClTracer* tracer = new ClTracer(device, Callback, collector);
    if (tracer == nullptr || !tracer->IsValid()) {
      std::cerr << "[WARNING] Unable to create OpenCL tracer " <<
        "for target device" << std::endl;
      if (tracer != nullptr) {
        delete tracer;
        delete collector;
      }
      return nullptr;
    }

    collector->EnableTracing(tracer);
    return collector;
  }

  ~ClApiCollector() {
    if (tracer_ != nullptr) {
      delete tracer_;
    }
  }

  void DisableTracing() {
    PTI_ASSERT(tracer_ != nullptr);
    bool disabled = tracer_->Disable();
    PTI_ASSERT(disabled);
  }

  const FunctionInfoMap& GetFunctionInfoMap() const {
    return function_info_map_;
  }

  ClApiCollector(const ClApiCollector& copy) = delete;
  ClApiCollector& operator=(const ClApiCollector& copy) = delete;

  static void PrintFunctionsTable(const FunctionInfoMap& function_info_map) {
    std::set< std::pair<std::string, Function>,
              utils::Comparator > sorted_list(
        function_info_map.begin(), function_info_map.end());

    uint64_t total_duration = 0;
    size_t max_name_length = kFunctionLength;
    for (auto& value : sorted_list) {
      total_duration += value.second.total_time;
      if (value.first.size() > max_name_length) {
        max_name_length = value.first.size();
      }
    }

    if (total_duration == 0) {
      return;
    }

    std::cerr << std::setw(max_name_length) << "Function" << "," <<
      std::setw(kCallsLength) << "Calls" << "," <<
      std::setw(kTimeLength) << "Time (ns)" << "," <<
      std::setw(kPercentLength) << "Time (%)" << "," <<
      std::setw(kTimeLength) << "Average (ns)" << "," <<
      std::setw(kTimeLength) << "Min (ns)" << "," <<
      std::setw(kTimeLength) << "Max (ns)" << std::endl;

    for (auto& value : sorted_list) {
      const std::string& function = value.first;
      uint64_t call_count = value.second.call_count;
      uint64_t duration = value.second.total_time;
      uint64_t avg_duration = duration / call_count;
      uint64_t min_duration = value.second.min_time;
      uint64_t max_duration = value.second.max_time;
      float percent_duration = 100.0f * duration / total_duration;
      std::cerr << std::setw(max_name_length) << function << "," <<
        std::setw(kCallsLength) << call_count << "," <<
        std::setw(kTimeLength) << duration << "," <<
        std::setw(kPercentLength) << std::setprecision(2) <<
          std::fixed << percent_duration << "," <<
        std::setw(kTimeLength) << avg_duration << "," <<
        std::setw(kTimeLength) << min_duration << "," <<
        std::setw(kTimeLength) << max_duration << std::endl;
    }
  }

 private: // Implementation Details
  ClApiCollector(FunctionTimePoint base_time) : base_time_(base_time) {}

  void EnableTracing(ClTracer* tracer) {
    PTI_ASSERT(tracer != nullptr);
    tracer_ = tracer;

    for (int id = 0; id < CL_FUNCTION_COUNT; ++id) {
      bool set = tracer_->SetTracingFunction(static_cast<cl_function_id>(id));
      PTI_ASSERT(set);
    }

    bool enabled = tracer_->Enable();
    PTI_ASSERT(enabled);
  }

  uint64_t GetTimestamp() const {
    std::chrono::duration<uint64_t, std::nano> timestamp =
      std::chrono::steady_clock::now() - base_time_;
    return timestamp.count();
  }

  void AddFunctionTime(const std::string& name, uint64_t time) {
    const std::lock_guard<std::mutex> lock(lock_);
    if (function_info_map_.count(name) == 0) {
      function_info_map_[name] = {time, time, time, 1};
    } else {
      Function& function = function_info_map_[name];
      function.total_time += time;
      if (time < function.min_time) {
        function.min_time = time;
      }
      if (time > function.max_time) {
        function.max_time = time;
      }
      ++function.call_count;
    }
  }

 private: // Callbacks
  static void OnFunctionEnter(cl_callback_data* data, void* user_data) {
    ClApiCollector* collector = reinterpret_cast<ClApiCollector*>(user_data);
    PTI_ASSERT(collector != nullptr);

    PTI_ASSERT(data != nullptr);
    uint64_t& start_time = *reinterpret_cast<uint64_t*>(data->correlationData);
    start_time = collector->GetTimestamp();
  }

  static void OnFunctionExit(cl_callback_data* data, void* user_data) {
    ClApiCollector* collector = reinterpret_cast<ClApiCollector*>(user_data);
    PTI_ASSERT(collector != nullptr);
    uint64_t end_time = collector->GetTimestamp();

    PTI_ASSERT(data != nullptr);
    uint64_t& start_time = *reinterpret_cast<uint64_t*>(data->correlationData);
    collector->AddFunctionTime(data->functionName, end_time - start_time);
  }

  static void Callback(
      cl_function_id function,
      cl_callback_data* callback_data,
      void* user_data) {
    if (callback_data->site == CL_CALLBACK_SITE_ENTER) {
      OnFunctionEnter(callback_data, user_data);
    } else {
      OnFunctionExit(callback_data, user_data);
    }
  }

 private: // Data
  ClTracer* tracer_ = nullptr;

  FunctionTimePoint base_time_;

  std::mutex lock_;
  FunctionInfoMap function_info_map_;

  static const uint32_t kFunctionLength = 10;
  static const uint32_t kCallsLength = 12;
  static const uint32_t kTimeLength = 20;
  static const uint32_t kPercentLength = 10;
};

#endif // PTI_SAMPLES_CL_HOT_FUNCTIONS_CL_API_COLLECTOR_H_