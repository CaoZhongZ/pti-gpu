//==============================================================
// Copyright (C) Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <math.h>
#include <string.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "pti/pti_view.h"
#include "utils.h"
#include "utils/test_helpers.h"
#include "ze_utils.h"

#define ALIGN 64
#define A_VALUE 0.128f
#define B_VALUE 0.256f
#define MAX_EPS 1.0e-4f

namespace {
constexpr size_t kPtiDeviceId = 0;  // run on first device

size_t requested_buffer_calls = 0;
size_t rejected_buffer_calls = 0;  // Buffer requests that are called and rejected by the API
size_t completed_buffer_calls = 0;
size_t completed_buffer_used_bytes = 0;
bool memory_view_record_created = false;
bool kernel_view_record_created = false;
uint64_t memory_view_record_count = 0;
uint64_t kernel_view_record_count = 0;
bool buffer_size_atleast_largest_record = false;
bool ze_initialization_succeeded = false;
bool ze_tracing_enabled_env = true;

float Check(const std::vector<float>& a, float value) {
  PTI_ASSERT(value > MAX_EPS);

  float eps = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    eps += fabs((a[i] - value) / value);
  }

  return eps / a.size();
}

float RunAndCheck(ze_kernel_handle_t kernel, ze_device_handle_t device, ze_context_handle_t context,
                  const std::vector<float>& a, const std::vector<float>& b, std::vector<float>& c,
                  unsigned size, float expected_result) {
  PTI_ASSERT(kernel != nullptr);
  PTI_ASSERT(device != nullptr);
  PTI_ASSERT(context != nullptr);

  PTI_ASSERT(size > 0);
  PTI_ASSERT(a.size() == size * size);
  PTI_ASSERT(b.size() == size * size);
  PTI_ASSERT(c.size() == size * size);

  ze_result_t status = ZE_RESULT_SUCCESS;

  uint32_t group_size[3] = {0};
  status = zeKernelSuggestGroupSize(kernel, size, size, 1, &(group_size[0]), &(group_size[1]),
                                    &(group_size[2]));
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  if ((size % group_size[0]) != 0 || (size % group_size[1]) != 0) {
    std::cout << "Non-uniform workgroups are not supported" << std::endl;
    return 0.0f;
  }

  void* dev_a = nullptr;
  ze_device_mem_alloc_desc_t alloc_desc = {ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC, nullptr, 0, 0};
  status =
      zeMemAllocDevice(context, &alloc_desc, size * size * sizeof(float), ALIGN, device, &dev_a);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  void* dev_b = nullptr;
  status =
      zeMemAllocDevice(context, &alloc_desc, size * size * sizeof(float), ALIGN, device, &dev_b);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  void* dev_c = nullptr;
  status =
      zeMemAllocDevice(context, &alloc_desc, size * size * sizeof(float), ALIGN, device, &dev_c);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeKernelSetGroupSize(kernel, group_size[0], group_size[1], group_size[2]);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeKernelSetArgumentValue(kernel, 0, sizeof(dev_a), &dev_a);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  status = zeKernelSetArgumentValue(kernel, 1, sizeof(dev_a), &dev_b);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  status = zeKernelSetArgumentValue(kernel, 2, sizeof(dev_a), &dev_c);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  status = zeKernelSetArgumentValue(kernel, 3, sizeof(size), &size);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  ze_command_list_desc_t cmd_list_desc = {ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC, nullptr, 0, 0};
  ze_command_list_handle_t cmd_list = nullptr;
  status = zeCommandListCreate(context, device, &cmd_list_desc, &cmd_list);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeCommandListAppendMemoryCopy(cmd_list, dev_a, a.data(), size * size * sizeof(float),
                                         nullptr, 0, nullptr);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  status = zeCommandListAppendMemoryCopy(cmd_list, dev_b, b.data(), size * size * sizeof(float),
                                         nullptr, 0, nullptr);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeCommandListAppendBarrier(cmd_list, nullptr, 0, nullptr);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  ze_event_pool_desc_t event_pool_desc = {
      ZE_STRUCTURE_TYPE_EVENT_POOL_DESC, nullptr,
      ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP | ZE_EVENT_POOL_FLAG_HOST_VISIBLE, 1};
  ze_event_pool_handle_t event_pool = nullptr;
  status = zeEventPoolCreate(context, &event_pool_desc, 0, nullptr, &event_pool);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  ze_event_desc_t event_desc = {ZE_STRUCTURE_TYPE_EVENT_DESC, nullptr, 0, ZE_EVENT_SCOPE_FLAG_HOST,
                                ZE_EVENT_SCOPE_FLAG_HOST};
  ze_event_handle_t event = nullptr;
  zeEventCreate(event_pool, &event_desc, &event);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  ze_group_count_t dim = {size / group_size[0], size / group_size[1], 1};
  status = zeCommandListAppendLaunchKernel(cmd_list, kernel, &dim, event, 0, nullptr);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeCommandListAppendBarrier(cmd_list, nullptr, 0, nullptr);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeCommandListAppendMemoryCopy(cmd_list, c.data(), dev_c, size * size * sizeof(float),
                                         nullptr, 0, nullptr);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeCommandListClose(cmd_list);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  ze_command_queue_desc_t cmd_queue_desc = {
      ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC, nullptr, 0, 0, 0, ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS,
      ZE_COMMAND_QUEUE_PRIORITY_NORMAL};
  ze_command_queue_handle_t cmd_queue = nullptr;
  status = zeCommandQueueCreate(context, device, &cmd_queue_desc, &cmd_queue);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS && cmd_queue != nullptr);

  status = zeCommandQueueExecuteCommandLists(cmd_queue, 1, &cmd_list, nullptr);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeCommandQueueSynchronize(cmd_queue, UINT32_MAX);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeCommandQueueDestroy(cmd_queue);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeCommandListDestroy(cmd_list);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeMemFree(context, dev_a);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  status = zeMemFree(context, dev_b);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  status = zeMemFree(context, dev_c);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  ze_device_properties_t props{};
  props.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES_1_2;
  status = zeDeviceGetProperties(device, &props);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  ze_kernel_timestamp_result_t timestamp{};
  status = zeEventQueryKernelTimestamp(event, &timestamp);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  status = zeEventDestroy(event);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  status = zeEventPoolDestroy(event_pool);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  double time = static_cast<double>(timestamp.global.kernelEnd - timestamp.global.kernelStart) /
                props.timerResolution;
  std::cout << "Matrix multiplication time: " << time << " sec" << std::endl;

  return Check(c, expected_result);
}

void Compute(ze_device_handle_t device, ze_driver_handle_t driver, const std::vector<float>& a,
             const std::vector<float>& b, std::vector<float>& c, unsigned size,
             unsigned repeat_count, float expected_result) {
  PTI_ASSERT(device != nullptr && driver != nullptr);
  PTI_ASSERT(size > 0 && repeat_count > 0);

  std::string module_name = "gemm.spv";
  std::cout << utils::GetExecutablePath() + module_name << std::endl;
  std::vector<uint8_t> binary = utils::LoadBinaryFile(utils::GetExecutablePath() + module_name);
  if (binary.size() == 0) {
    std::cout << "Unable to find module " << module_name << std::endl;
    return;
  }

  ze_result_t status = ZE_RESULT_SUCCESS;
  ze_context_handle_t context = utils::ze::GetContext(driver);
  PTI_ASSERT(context != nullptr);

  ze_module_desc_t module_desc = {ZE_STRUCTURE_TYPE_MODULE_DESC,
                                  nullptr,
                                  ZE_MODULE_FORMAT_IL_SPIRV,
                                  static_cast<uint32_t>(binary.size()),
                                  binary.data(),
                                  nullptr,
                                  nullptr};
  ze_module_handle_t module = nullptr;
  status = zeModuleCreate(context, device, &module_desc, &module, nullptr);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS && module != nullptr);

  ze_kernel_desc_t kernel_desc = {ZE_STRUCTURE_TYPE_KERNEL_DESC, nullptr, 0, "GEMM"};
  ze_kernel_handle_t kernel = nullptr;
  status = zeKernelCreate(module, &kernel_desc, &kernel);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS && kernel != nullptr);

  for (unsigned i = 0; i < repeat_count; ++i) {
    if (i == 0) {  // Enable data collection for the first iteration
      utils::SetEnv("PTI_ENABLE_COLLECTION", "1");
    }

    float eps = RunAndCheck(kernel, device, context, a, b, c, size, expected_result);
    std::cout << "Results are " << ((eps < MAX_EPS) ? "" : "IN") << "CORRECT with accuracy: " << eps
              << std::endl;

    if (i == 0) {  // Disable data collection for the rest iterations
      utils::SetEnv("PTI_ENABLE_COLLECTION", "");
    }
  }

  status = zeKernelDestroy(kernel);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeModuleDestroy(module);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);

  status = zeContextDestroy(context);
  PTI_ASSERT(status == ZE_RESULT_SUCCESS);
}

}  // namespace

class MainZeFixtureTest : public ::testing::Test {
 protected:
  MainZeFixtureTest() {
    // Setup work for each test
  }

  ~MainZeFixtureTest() override {
    // Cleanup work for each test
  }

  void SetUp() override {  // Called right after constructor before each test
    if (!ze_tracing_enabled_env) {
      GTEST_SKIP();
    }
    buffer_cb_registered = true;
    requested_buffer_calls = 0;
    rejected_buffer_calls = 0;
    completed_buffer_calls = 0;
    completed_buffer_used_bytes = 0;
    memory_view_record_created = false;
    kernel_view_record_created = false;
    memory_view_record_count = 0;
    kernel_view_record_count = 0;
  }

  void TearDown() override {
    // Called right before destructor after each test
  }

  // Class members commonly used by all tests in the test suite for MainFixture
  unsigned size = 1024;

  unsigned repeat_count = 1;

  bool buffer_cb_registered = false;

  static void BufferCompleted(unsigned char* buf, size_t buf_size, size_t used_bytes) {
    if (!buf || !used_bytes || !buf_size) {
      std::cerr << "Received empty buffer" << '\n';
      ::operator delete(buf);
      return;
    }

    completed_buffer_calls += 1;
    completed_buffer_used_bytes = used_bytes;
    pti_view_record_base* ptr = nullptr;
    while (true) {
      auto buf_status = ptiViewGetNextRecord(buf, used_bytes, &ptr);
      if (buf_status == pti_result::PTI_STATUS_END_OF_BUFFER) {
        break;
      }
      if (buf_status != pti_result::PTI_SUCCESS) {
        std::cerr << "Found Error Parsing Records from PTI" << '\n';
        break;
      }
      switch (ptr->_view_kind) {
        case pti_view_kind::PTI_VIEW_INVALID: {
          std::cout << "Found Invalid Record" << '\n';
          break;
        }
        case pti_view_kind::PTI_VIEW_DEVICE_GPU_MEM_COPY: {
          memory_view_record_created = true;
          memory_view_record_count += 1;
          break;
        }
        case pti_view_kind::PTI_VIEW_DEVICE_GPU_MEM_FILL: {
          memory_view_record_created = true;
          memory_view_record_count += 1;
          break;
        }
        case pti_view_kind::PTI_VIEW_DEVICE_GPU_KERNEL: {
          kernel_view_record_created = true;
          kernel_view_record_count += 1;
          break;
        }
        default: {
          std::cerr << "This shouldn't happen" << '\n';
          break;
        }
      }
    }
    ::operator delete(buf);
  }
  static void NullBufferRequested(unsigned char** buf, size_t* buf_size) {
    *buf_size = sizeof(pti_view_record_memory_copy) - sizeof(pti_view_record_memory_copy);
    void* ptr = ::operator new(*buf_size);
    requested_buffer_calls += 1;
    rejected_buffer_calls += 1;
    *buf = static_cast<unsigned char*>(ptr);
    buffer_size_atleast_largest_record = (*buf_size) >= sizeof(pti_view_record_memory_copy);
  }

  static void InadequateBufferRequested(unsigned char** buf, size_t* buf_size) {
    *buf_size = sizeof(pti_view_record_kernel) - 1;
    void* ptr = ::operator new(*buf_size);
    requested_buffer_calls += 1;
    rejected_buffer_calls += 1;
    ptr = std::align(8, sizeof(unsigned char), ptr, *buf_size);
    *buf = static_cast<unsigned char*>(ptr);
    if (!*buf) {
      std::abort();
    }
    buffer_size_atleast_largest_record = (*buf_size) >= sizeof(pti_view_record_memory_copy);
  }
  static void BufferRequested(unsigned char** buf, size_t* buf_size) {
    *buf_size = sizeof(pti_view_record_kernel);
    void* ptr = ::operator new(*buf_size);
    requested_buffer_calls += 1;
    ptr = std::align(8, sizeof(unsigned char), ptr, *buf_size);
    *buf = static_cast<unsigned char*>(ptr);
    if (!*buf) {
      std::abort();
    }
    buffer_size_atleast_largest_record = (*buf_size) >= sizeof(pti_view_record_memory_copy);
  }

  int RunGemm() {
    ze_result_t status = ZE_RESULT_SUCCESS;
    status = zeInit(ZE_INIT_FLAG_GPU_ONLY);
    ze_initialization_succeeded = (status == ZE_RESULT_SUCCESS);

    ze_device_handle_t device = utils::ze::GetGpuDevice(kPtiDeviceId);
    ze_driver_handle_t driver = utils::ze::GetGpuDriver(kPtiDeviceId);
    if (device == nullptr || driver == nullptr) {
      std::cout << "Unable to find GPU device" << std::endl;
      return 0;
    }

    ptiViewEnable(PTI_VIEW_DEVICE_GPU_KERNEL);
    ptiViewEnable(PTI_VIEW_DEVICE_GPU_MEM_COPY);
    ptiViewEnable(PTI_VIEW_DEVICE_GPU_MEM_FILL);

    std::cout << "Level Zero Matrix Multiplication (matrix size: " << size << " x " << size
              << ", repeats " << repeat_count << " times)" << std::endl;
    std::cout << "Target device: " << utils::ze::GetDeviceName(device) << std::endl;

    std::vector<float> a(size * size, A_VALUE);
    std::vector<float> b(size * size, B_VALUE);
    std::vector<float> c(size * size, 0.0f);

    auto start = std::chrono::steady_clock::now();
    float expected_result = A_VALUE * B_VALUE * size;
    Compute(device, driver, a, b, c, size, repeat_count, expected_result);
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<float> time = end - start;

    ptiViewDisable(PTI_VIEW_DEVICE_GPU_KERNEL);
    ptiViewDisable(PTI_VIEW_DEVICE_GPU_MEM_COPY);
    ptiViewDisable(PTI_VIEW_DEVICE_GPU_MEM_FILL);
    std::cout << "Total execution time: " << time.count() << " sec" << std::endl;
    auto flush_results = ptiFlushAllViews();
    return flush_results;
  }
};

TEST_F(MainZeFixtureTest, ZeInitializationSucceeded) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunGemm();
  EXPECT_EQ(ze_initialization_succeeded, true);
}

TEST_F(MainZeFixtureTest, NegTestBufferSizeAtleastLargestRecord) {
  // Checks if ptiViewSetCallbacks rejects callback and using default
  // or existing callbacks.
  EXPECT_EQ(ptiViewSetCallbacks(InadequateBufferRequested, BufferCompleted),
            pti_result::PTI_ERROR_BAD_ARGUMENT);
  RunGemm();
  ASSERT_EQ(rejected_buffer_calls, 1 * repeat_count);
}

TEST_F(MainZeFixtureTest, BufferSizeAtleastLargestRecord) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunGemm();
  ASSERT_EQ(buffer_size_atleast_largest_record, true);
}

TEST_F(MainZeFixtureTest, BufferCallBacksRegistered) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunGemm();
  EXPECT_EQ(buffer_cb_registered, true);
}

TEST_F(MainZeFixtureTest, SecondCallbackCalled) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunGemm();
  EXPECT_GT(completed_buffer_used_bytes, static_cast<size_t>(0));
}

TEST_F(MainZeFixtureTest, MemoryViewRecordCreated) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunGemm();
  EXPECT_EQ(memory_view_record_created, true);
}

TEST_F(MainZeFixtureTest, KernelViewRecordCreated) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunGemm();
  EXPECT_EQ(kernel_view_record_created, true);
}

TEST_F(MainZeFixtureTest, NumberOfExpectedMemoryRecords) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunGemm();
  EXPECT_EQ(memory_view_record_count, 3 * repeat_count);
}

TEST_F(MainZeFixtureTest, NumberOfExpectedKernelRecords) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunGemm();
  EXPECT_EQ(kernel_view_record_count, 1 * repeat_count);
}

TEST_F(MainZeFixtureTest, RequestedAndCompletedBuffers) {
  EXPECT_EQ(ptiViewSetCallbacks(BufferRequested, BufferCompleted), pti_result::PTI_SUCCESS);
  RunGemm();
  EXPECT_EQ(requested_buffer_calls, completed_buffer_calls);
}

TEST_F(MainZeFixtureTest, NegTestNullBufferSize) {
  ASSERT_EQ(ptiViewSetCallbacks(NullBufferRequested, BufferCompleted),
            pti_result::PTI_ERROR_BAD_ARGUMENT);
  RunGemm();
  ASSERT_EQ(rejected_buffer_calls, 1 * repeat_count);
}

using pti::test::utils::CreateFullBuffer;
using pti::test::utils::RecordInserts;

class GetNextRecordTestSuite : public ::testing::Test {
 protected:
  static constexpr std::size_t kNumMemRecs = 15;
  static constexpr std::size_t kNumExtRecs = 100;
  static constexpr std::size_t kNumKernelRecs = 3;
  static constexpr std::size_t kNumOhRecs = 1;
  static constexpr std::size_t kTotalRecs =
      2 * kNumOhRecs + 2 * kNumMemRecs + kNumKernelRecs + kNumExtRecs;
  GetNextRecordTestSuite()
      : test_buf_(CreateFullBuffer<RecordInserts<pti_view_record_overhead, kNumOhRecs>,
                                   RecordInserts<pti_view_record_memory_copy, kNumMemRecs>,
                                   RecordInserts<pti_view_record_memory_fill, kNumMemRecs>,
                                   RecordInserts<pti_view_record_external_correlation, kNumExtRecs>,
                                   RecordInserts<pti_view_record_kernel, kNumKernelRecs>,
                                   RecordInserts<pti_view_record_overhead, kNumOhRecs> >()) {}
  std::vector<unsigned char> test_buf_;
};

TEST_F(GetNextRecordTestSuite, NullBufferTest) {
  pti_view_record_base* current_record = nullptr;
  auto result = ptiViewGetNextRecord(nullptr, 0, &current_record);
  ASSERT_EQ(result, pti_result::PTI_STATUS_END_OF_BUFFER);
}

TEST_F(GetNextRecordTestSuite, NullBufferBadSizeTest) {
  pti_view_record_base* current_record = nullptr;
  auto result = ptiViewGetNextRecord(nullptr, static_cast<std::size_t>(-1), &current_record);
  ASSERT_EQ(result, pti_result::PTI_STATUS_END_OF_BUFFER);
}

TEST_F(GetNextRecordTestSuite, NullRecordBufferTest) {
  auto result = ptiViewGetNextRecord(test_buf_.data(), test_buf_.size(), nullptr);
  ASSERT_EQ(result, pti_result::PTI_ERROR_BAD_ARGUMENT);
}

TEST_F(GetNextRecordTestSuite, NullRecordBadSizeBufferTest) {
  auto result = ptiViewGetNextRecord(nullptr, static_cast<std::size_t>(-1), nullptr);
  ASSERT_EQ(result, pti_result::PTI_ERROR_BAD_ARGUMENT);
}

TEST_F(GetNextRecordTestSuite, CheckBufferEndTest) {
  pti_view_record_base* current_record = nullptr;
  std::size_t total_records = 0;
  while (true) {
    auto result = ptiViewGetNextRecord(test_buf_.data(), test_buf_.size(), &current_record);
    if (result == pti_result::PTI_STATUS_END_OF_BUFFER) {
      EXPECT_NE(current_record, nullptr);
      break;
    }
    total_records++;
  }
  EXPECT_EQ(total_records, kTotalRecs);
  auto result = ptiViewGetNextRecord(test_buf_.data(), test_buf_.size(), &current_record);

  EXPECT_NE(current_record, nullptr);
  ASSERT_EQ(result, pti_result::PTI_STATUS_END_OF_BUFFER);
}

TEST_F(GetNextRecordTestSuite, RegularParseRecordsTest) {
  pti_view_record_base* current_record = nullptr;
  std::size_t total_records = 0;
  std::size_t number_of_memory_copies = 0;
  std::size_t number_of_kernel = 0;
  std::size_t number_of_overhead = 0;
  while (true) {
    auto result = ptiViewGetNextRecord(test_buf_.data(), test_buf_.size(), &current_record);
    if (result == pti_result::PTI_STATUS_END_OF_BUFFER) {
      break;
    }
    if (result == pti_result::PTI_ERROR_INTERNAL) {
      FAIL();
    }
    if (result == pti_result::PTI_ERROR_BAD_ARGUMENT) {
      FAIL();
    }
    total_records++;
    if (current_record->_view_kind == PTI_VIEW_DEVICE_GPU_KERNEL) {
      number_of_kernel++;
    }
    if (current_record->_view_kind == PTI_VIEW_DEVICE_GPU_MEM_COPY) {
      number_of_memory_copies++;
    }
    if (current_record->_view_kind == PTI_VIEW_COLLECTION_OVERHEAD) {
      number_of_overhead++;
    }
  }
  EXPECT_EQ(number_of_memory_copies, kNumMemRecs);
  EXPECT_EQ(number_of_overhead, 2 * kNumOhRecs);
  EXPECT_EQ(number_of_kernel, kNumKernelRecs);
  ASSERT_EQ(total_records, kTotalRecs);
}

TEST(PtiVersionTestSuite, TestVersionMacros) {
  // Check against first public PTI version 0.1.0
  EXPECT_GE(PTI_VERSION_MAJOR, 0);
  EXPECT_GE(PTI_VERSION_MINOR, PTI_VERSION_MAJOR == 0 ? 1 : 0);
  EXPECT_GE(PTI_VERSION_PATCH, 0);
}

TEST(PtiVersionTestSuite, TestVersionFunction) {
  // Unit tests should be run against same version of header and lib
  auto pti_ver = ptiVersion();
  EXPECT_EQ(pti_ver._major, static_cast<uint32_t>(PTI_VERSION_MAJOR));
  EXPECT_EQ(pti_ver._minor, static_cast<uint32_t>(PTI_VERSION_MINOR));
  EXPECT_EQ(pti_ver._patch, static_cast<uint32_t>(PTI_VERSION_PATCH));
}

TEST(PtiVersionTestSuite, TestVersionString) {
  using ::testing::ContainsRegex;
  // Unit tests should be run against same version of header and lib
#if !defined(WIN32)
  constexpr const char* const test_version_regex = "^[0-9]+\\.[0-9]+\\.[0-9]+";
#else
  constexpr const char* const test_version_regex = "^\\d+\\.\\d+\\.\\d+";
#endif
  EXPECT_THAT(ptiVersionString(), ContainsRegex(test_version_regex));
  EXPECT_STREQ(PTI_VERSION_STRING, ptiVersionString());
}
