/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include "foedus/tpcc/tpcc_driver.hpp"

#include <gflags/gflags.h>

#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "foedus/engine.hpp"
#include "foedus/engine_options.hpp"
#include "foedus/error_stack.hpp"
#include "foedus/debugging/debugging_supports.hpp"
#include "foedus/fs/filesystem.hpp"
#include "foedus/memory/engine_memory.hpp"
#include "foedus/thread/thread.hpp"
#include "foedus/thread/thread_pool.hpp"
#include "foedus/tpcc/tpcc.hpp"
#include "foedus/tpcc/tpcc_client.hpp"
#include "foedus/tpcc/tpcc_load.hpp"

namespace foedus {
namespace tpcc {
DEFINE_bool(profile, false, "Whether to profile the execution with gperftools.");
DEFINE_int32(loggers_per_node, 1, "Number of log writers per numa node.");

const uint64_t kDurationMicro = 5000000;  // TODO(Hideaki) make it a flag

uint64_t TpccDriver::run() {
  const EngineOptions& options = engine_->get_options();
  std::cout << engine_->get_memory_manager().dump_free_memory_stat() << std::endl;

  TpccLoadTask loader;
  thread::ImpersonateSession loader_session = engine_->get_thread_pool().impersonate(&loader);
  if (!loader_session.is_valid()) {
    COERCE_ERROR(loader_session.invalid_cause_);
  }
  std::cout << "loader_result=" << loader_session.get_result() << std::endl;
  if (loader_session.get_result().is_error()) {
    COERCE_ERROR(loader_session.get_result());
  }

  std::cout << engine_->get_memory_manager().dump_free_memory_stat() << std::endl;

  storages_ = loader.get_storages();
  std::vector< thread::ImpersonateSession > sessions;
  auto& thread_pool = engine_->get_thread_pool();
  for (uint16_t node = 0; node < options.thread_.group_count_; ++node) {
    memory::ScopedNumaPreferred scope(node);
    for (uint16_t ordinal = 0; ordinal < options.thread_.thread_count_per_group_; ++ordinal) {
      clients_.push_back(new TpccClientTask((node << 8U) + ordinal, storages_, &start_rendezvous_));
      sessions.emplace_back(thread_pool.impersonate_on_numa_node(clients_.back(), node));
      if (!sessions.back().is_valid()) {
        COERCE_ERROR(sessions.back().invalid_cause_);
      }
    }
  }
  std::cout << "okay, launched all worker threads" << std::endl;

  // make sure all threads are done with random number generation
  std::this_thread::sleep_for(std::chrono::seconds(3));
  if (FLAGS_profile) {
    COERCE_ERROR(engine_->get_debug().start_profile("tpcc.prof"));
  }
  start_rendezvous_.signal();  // GO!
  std::cout << "Started!" << std::endl;
  std::this_thread::sleep_for(std::chrono::microseconds(kDurationMicro));
  std::cout << "Experiment ended." << std::endl;

  uint64_t total = 0;
  assorted::memory_fence_acquire();
  for (auto* client : clients_) {
    total += client->get_processed();
  }
  if (FLAGS_profile) {
    engine_->get_debug().stop_profile();
  }
  std::cout << "Shutting down..." << std::endl;

  assorted::memory_fence_release();
  for (auto* client : clients_) {
    client->request_stop();
  }
  assorted::memory_fence_release();

  for (uint16_t i = 0; i < sessions.size(); ++i) {
    std::cout << "result[" << i << "]=" << sessions[i].get_result() << std::endl;
    delete clients_[i];
  }
  return total;
}

int driver_main(int argc, char **argv) {
  gflags::SetUsageMessage("TPC-C implementation for FOEDUS");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  fs::Path folder("/dev/shm/foedus_tpcc");
  if (fs::exists(folder)) {
    fs::remove_all(folder);
  }
  if (!fs::create_directories(folder)) {
    std::cerr << "Couldn't create " << folder << ". err=" << assorted::os_error() << std::endl;
    return 1;
  }

  EngineOptions options;

  fs::Path savepoint_path(folder);
  savepoint_path /= "savepoint.xml";
  options.savepoint_.savepoint_path_ = savepoint_path.string();
  ASSERT_ND(!fs::exists(savepoint_path));

  std::cout << "NUMA node count=" << static_cast<int>(options.thread_.group_count_) << std::endl;
  options.snapshot_.folder_path_pattern_ = "/dev/shm/foedus_tpcc/snapshot/node_$NODE$";
  options.log_.folder_path_pattern_ = "/dev/shm/foedus_tpcc/log/node_$NODE$/logger_$LOGGER$";
  options.log_.loggers_per_node_ = FLAGS_loggers_per_node;
  options.log_.flush_at_shutdown_ = false;
  options.debugging_.debug_log_min_threshold_
    = debugging::DebuggingOptions::kDebugLogInfo;
    // = debugging::DebuggingOptions::kDebugLogWarning;
  options.debugging_.verbose_modules_ = "";
  options.debugging_.verbose_log_level_ = -1;
  options.log_.log_buffer_kb_ = 1 << 18;  // 256MB * 16 cores = 4 GB. nothing.
  options.log_.log_file_size_mb_ = 1 << 10;
  options.memory_.page_pool_size_mb_per_node_ = 1 << 12;  // 8GB per node = 16GB

  uint64_t total_processed;
  {
    Engine engine(options);
    COERCE_ERROR(engine.initialize());
    {
      UninitializeGuard guard(&engine);
      TpccDriver driver(&engine);
      total_processed = driver.run();
      COERCE_ERROR(engine.uninitialize());
    }
  }

  // wait just for a bit to avoid mixing stdout
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::cout << "total=" << total_processed << ", MTPS="
    << (static_cast<double>(total_processed) / kDurationMicro) << std::endl;
  if (FLAGS_profile) {
    std::cout << "Check out the profile result: pprof --pdf tpcc.prof tpcc > prof.pdf; "
      "okular prof.pdf" << std::endl;
  }
  return 0;
}

}  // namespace tpcc
}  // namespace foedus