/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include <gtest/gtest.h>

#include <cstring>
#include <iostream>

#include "foedus/engine.hpp"
#include "foedus/engine_options.hpp"
#include "foedus/epoch.hpp"
#include "foedus/test_common.hpp"
#include "foedus/storage/storage_manager.hpp"
#include "foedus/storage/masstree/masstree_metadata.hpp"
#include "foedus/storage/masstree/masstree_storage.hpp"
#include "foedus/thread/thread.hpp"
#include "foedus/thread/thread_pool.hpp"
#include "foedus/xct/xct_manager.hpp"

namespace foedus {
namespace storage {
namespace masstree {
DEFINE_TEST_CASE_PACKAGE(MasstreeBasicTest, foedus.storage.masstree);
TEST(MasstreeBasicTest, Create) {
  EngineOptions options = get_tiny_options();
  Engine engine(options);
  COERCE_ERROR(engine.initialize());
  {
    UninitializeGuard guard(&engine);
    MasstreeStorage* out;
    Epoch commit_epoch;
    MasstreeMetadata meta("test");
    COERCE_ERROR(engine.get_storage_manager().create_masstree(&meta, &out, &commit_epoch));
    EXPECT_TRUE(out != nullptr);
    COERCE_ERROR(engine.uninitialize());
  }
  cleanup_test(options);
}

class QueryTask : public thread::ImpersonateTask {
 public:
  ErrorStack run(thread::Thread* context) {
    MasstreeStorage *masstree =
      dynamic_cast<MasstreeStorage*>(
        context->get_engine()->get_storage_manager().get_storage("test2"));
    char buf[16];
    xct::XctManager& xct_manager = context->get_engine()->get_xct_manager();
    CHECK_ERROR(xct_manager.begin_xct(context, xct::kSerializable));
    char key[100];
    std::memset(key, 0, 100);
    uint16_t payload_capacity = 16;
    ErrorCode result = masstree->get_record(context, key, 100, buf, &payload_capacity);
    EXPECT_EQ(kErrorCodeStrKeyNotFound, result);
    Epoch commit_epoch;
    CHECK_ERROR(xct_manager.precommit_xct(context, &commit_epoch));
    CHECK_ERROR(xct_manager.wait_for_commit(commit_epoch));
    return foedus::kRetOk;
  }
};

TEST(MasstreeBasicTest, CreateAndQuery) {
  EngineOptions options = get_tiny_options();
  Engine engine(options);
  COERCE_ERROR(engine.initialize());
  {
    UninitializeGuard guard(&engine);
    MasstreeStorage* out;
    Epoch commit_epoch;
    MasstreeMetadata meta("test2");
    COERCE_ERROR(engine.get_storage_manager().create_masstree(&meta, &out, &commit_epoch));
    EXPECT_TRUE(out != nullptr);
    QueryTask task;
    thread::ImpersonateSession session = engine.get_thread_pool().impersonate(&task);
    COERCE_ERROR(session.get_result());
    COERCE_ERROR(engine.uninitialize());
  }
  cleanup_test(options);
}

class InsertTask : public thread::ImpersonateTask {
 public:
  ErrorStack run(thread::Thread* context) {
    MasstreeStorage *masstree =
      dynamic_cast<MasstreeStorage*>(
        context->get_engine()->get_storage_manager().get_storage("ggg"));
    xct::XctManager& xct_manager = context->get_engine()->get_xct_manager();
    CHECK_ERROR(xct_manager.begin_xct(context, xct::kSerializable));
    KeySlice key = normalize_primitive(12345ULL);
    uint64_t data = 897565433333126ULL;
    CHECK_ERROR(masstree->insert_record_normalized(context, key, &data, sizeof(data)));
    Epoch commit_epoch;
    CHECK_ERROR(xct_manager.precommit_xct(context, &commit_epoch));
    CHECK_ERROR(xct_manager.wait_for_commit(commit_epoch));
    return foedus::kRetOk;
  }
};

TEST(MasstreeBasicTest, CreateAndInsert) {
  EngineOptions options = get_tiny_options();
  Engine engine(options);
  COERCE_ERROR(engine.initialize());
  {
    UninitializeGuard guard(&engine);
    MasstreeStorage* out;
    Epoch commit_epoch;
    MasstreeMetadata meta("ggg");
    COERCE_ERROR(engine.get_storage_manager().create_masstree(&meta, &out, &commit_epoch));
    EXPECT_TRUE(out != nullptr);
    InsertTask task;
    thread::ImpersonateSession session = engine.get_thread_pool().impersonate(&task);
    COERCE_ERROR(session.get_result());
    COERCE_ERROR(engine.uninitialize());
  }
  cleanup_test(options);
}

class InsertAndReadTask : public thread::ImpersonateTask {
 public:
  ErrorStack run(thread::Thread* context) {
    MasstreeStorage *masstree =
      dynamic_cast<MasstreeStorage*>(
        context->get_engine()->get_storage_manager().get_storage("ggg"));
    xct::XctManager& xct_manager = context->get_engine()->get_xct_manager();
    CHECK_ERROR(xct_manager.begin_xct(context, xct::kSerializable));
    KeySlice key =  normalize_primitive(12345ULL);
    uint64_t data = 897565433333126ULL;
    CHECK_ERROR(masstree->insert_record_normalized(context, key, &data, sizeof(data)));
    Epoch commit_epoch;
    CHECK_ERROR(xct_manager.precommit_xct(context, &commit_epoch));

    uint64_t data2;
    CHECK_ERROR(xct_manager.begin_xct(context, xct::kSerializable));
    uint16_t data_capacity = sizeof(data2);
    CHECK_ERROR(masstree->get_record_normalized(context, key, &data2, &data_capacity));
    EXPECT_EQ(data, data2);
    CHECK_ERROR(xct_manager.precommit_xct(context, &commit_epoch));

    CHECK_ERROR(xct_manager.wait_for_commit(commit_epoch));
    return foedus::kRetOk;
  }
};

TEST(MasstreeBasicTest, CreateAndInsertAndRead) {
  EngineOptions options = get_tiny_options();
  Engine engine(options);
  COERCE_ERROR(engine.initialize());
  {
    UninitializeGuard guard(&engine);
    MasstreeStorage* out;
    Epoch commit_epoch;
    MasstreeMetadata meta("ggg");
    COERCE_ERROR(engine.get_storage_manager().create_masstree(&meta, &out, &commit_epoch));
    EXPECT_TRUE(out != nullptr);
    InsertAndReadTask task;
    thread::ImpersonateSession session = engine.get_thread_pool().impersonate(&task);
    COERCE_ERROR(session.get_result());
    COERCE_ERROR(engine.uninitialize());
  }
  cleanup_test(options);
}

class OverwriteTask : public thread::ImpersonateTask {
 public:
  ErrorStack run(thread::Thread* context) {
    MasstreeStorage *masstree =
      dynamic_cast<MasstreeStorage*>(
        context->get_engine()->get_storage_manager().get_storage("ggg"));
    xct::XctManager& xct_manager = context->get_engine()->get_xct_manager();
    CHECK_ERROR(xct_manager.begin_xct(context, xct::kSerializable));
    KeySlice key = normalize_primitive(12345ULL);
    uint64_t data = 897565433333126ULL;
    CHECK_ERROR(masstree->insert_record_normalized(context, key, &data, sizeof(data)));
    Epoch commit_epoch;
    CHECK_ERROR(xct_manager.precommit_xct(context, &commit_epoch));
    CHECK_ERROR(xct_manager.begin_xct(context, xct::kSerializable));
    uint64_t data2 = 321654987ULL;
    CHECK_ERROR(masstree->overwrite_record_normalized(context, key, &data2, 0, sizeof(data2)));
    CHECK_ERROR(xct_manager.precommit_xct(context, &commit_epoch));

    uint64_t data3;
    CHECK_ERROR(xct_manager.begin_xct(context, xct::kSerializable));
    CHECK_ERROR(masstree->get_record_primitive_normalized<uint64_t>(context, key, &data3, 0));
    EXPECT_EQ(data2, data3);
    CHECK_ERROR(xct_manager.precommit_xct(context, &commit_epoch));
    CHECK_ERROR(xct_manager.wait_for_commit(commit_epoch));
    return foedus::kRetOk;
  }
};

TEST(MasstreeBasicTest, Overwrite) {
  EngineOptions options = get_tiny_options();
  Engine engine(options);
  COERCE_ERROR(engine.initialize());
  {
    UninitializeGuard guard(&engine);
    MasstreeStorage* out;
    Epoch commit_epoch;
    MasstreeMetadata meta("ggg");
    COERCE_ERROR(engine.get_storage_manager().create_masstree(&meta, &out, &commit_epoch));
    EXPECT_TRUE(out != nullptr);
    OverwriteTask task;
    thread::ImpersonateSession session = engine.get_thread_pool().impersonate(&task);
    COERCE_ERROR(session.get_result());
    COERCE_ERROR(engine.uninitialize());
  }
  cleanup_test(options);
}
TEST(MasstreeBasicTest, CreateAndDrop) {
  EngineOptions options = get_tiny_options();
  Engine engine(options);
  COERCE_ERROR(engine.initialize());
  {
    UninitializeGuard guard(&engine);
    MasstreeStorage* out;
    Epoch commit_epoch;
    MasstreeMetadata meta("dd");
    COERCE_ERROR(engine.get_storage_manager().create_masstree(&meta, &out, &commit_epoch));
    EXPECT_TRUE(out != nullptr);
    COERCE_ERROR(engine.get_storage_manager().drop_storage(out->get_id(), &commit_epoch));
    COERCE_ERROR(engine.uninitialize());
  }
  cleanup_test(options);
}

}  // namespace masstree
}  // namespace storage
}  // namespace foedus