/*
 * Copyright (c) 2014-2015, Hewlett-Packard Development Company, LP.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * HP designates this particular file as subject to the "Classpath" exception
 * as provided by HP in the LICENSE.txt file that accompanied this code.
 */
#include "foedus/storage/hash/hash_page_impl.hpp"

#include <glog/logging.h>

#include <cstring>
#include <thread>
#include <vector>

#include "foedus/assert_nd.hpp"
#include "foedus/engine.hpp"
#include "foedus/memory/engine_memory.hpp"
#include "foedus/memory/numa_core_memory.hpp"
#include "foedus/memory/numa_node_memory.hpp"
#include "foedus/memory/page_pool.hpp"
#include "foedus/storage/hash/hash_metadata.hpp"
#include "foedus/storage/hash/hash_storage.hpp"
#include "foedus/thread/thread.hpp"

namespace foedus {
namespace storage {
namespace hash {
void HashIntermediatePage::initialize_volatile_page(
  StorageId storage_id,
  VolatilePagePointer page_id,
  const HashIntermediatePage* parent,
  const HashBinRange& bin_range) {
  std::memset(this, 0, kPageSize);
  header_.init_volatile(page_id, storage_id, kHashIntermediatePageType);
  bin_range_ = bin_range;
  if (parent) {
    ASSERT_ND(!parent->is_leaf());
    ASSERT_ND(parent->get_level() > 0);
    ASSERT_ND(parent->bin_range_.contains(bin_range_));
    header_.set_in_layer_level(parent->get_level() - 1U);
  } else {
    // in this case (root page), header_.set_in_layer_level must be called separately
  }
}

void HashDataPage::initialize_volatile_page(
  StorageId storage_id,
  VolatilePagePointer page_id,
  const Page* parent,
  HashBin bin) {
  std::memset(this, 0, kPageSize);
  header_.init_volatile(page_id, storage_id, kHashDataPageType);
  bin_ = bin;
  ASSERT_ND(parent);
  if (parent->get_header().get_page_type() == kHashIntermediatePageType) {
    const HashIntermediatePage* parent_casted
      = reinterpret_cast<const HashIntermediatePage*>(parent);
    ASSERT_ND(parent_casted->is_leaf());
    ASSERT_ND(parent_casted->get_bin_range().contains(bin));
  } else {
    const HashDataPage* parent_casted = reinterpret_cast<const HashDataPage*>(parent);
    ASSERT_ND(parent_casted->get_bin() == bin);
  }
}

DataPageSlotIndex HashDataPage::search_key(
  HashValue hash,
  const BloomFilterFingerprint& fingerprint,
  const char* key,
  uint16_t key_length,
  uint16_t record_count) const {
  // invariant checks
  ASSERT_ND(hash == hashinate(key, key_length));
  ASSERT_ND(DataPageBloomFilter::extract_fingerprint(hash) == fingerprint);
  ASSERT_ND(record_count <= get_record_count());  // it must be increasing.

  // check bloom filter first.
  if (!bloom_filter_.contains(fingerprint)) {
    return kSlotNotFound;
  }

  // then most likely this page contains it. let's check one by one.
  for (uint16_t i = 0; i < record_count; ++i) {
    const Slot& s = slot(i);
    if (LIKELY(s.hash_ != hash) || s.key_length_ != key_length || s.tid_.is_moved()) {
      continue;
    }
    if (s.tid_.is_moved()) {
      // not so rare. this happens.
      DVLOG(1) << "Hash matched, but the record was moved";
      continue;
    }

    const char* data = record_from_offset(s.offset_);
    if (s.key_length_ == key_length && std::memcmp(data, key, key_length) == 0) {
      return i;
    }
    // hash matched, but key didn't match? wow, that's rare
    DLOG(INFO) << "Hash matched, but key didn't match. interesting. hash="
      << assorted::Hex(hash, 16) << ", key=" << assorted::HexString(std::string(key, key_length))
      << ", key_slot="  << assorted::HexString(std::string(data, s.key_length_));
  }

  // should be 1~2%
  DVLOG(0) << "Nope, bloom filter contained it, but key not found in this page. false positive";
  return kSlotNotFound;
}


void hash_intermediate_volatile_page_init(const VolatilePageInitArguments& args) {
  ASSERT_ND(args.parent_);  // because this is always called for non-root pages.
  ASSERT_ND(args.page_);
  ASSERT_ND(args.index_in_parent_ < kHashIntermediatePageFanout);
  StorageId storage_id = args.parent_->get_header().storage_id_;
  HashIntermediatePage* page = reinterpret_cast<HashIntermediatePage*>(args.page_);

  ASSERT_ND(args.parent_->get_header().get_page_type() == kHashIntermediatePageType);
  const HashIntermediatePage* parent = reinterpret_cast<const HashIntermediatePage*>(args.parent_);

  ASSERT_ND(parent->get_bin_range().length() % kHashIntermediatePageFanout == 0);
  HashBinRange bin_range = parent->get_bin_range().subrange(args.index_in_parent_);
  page->initialize_volatile_page(storage_id, args.page_id, parent, bin_range);
}

void hash_data_volatile_page_init(const VolatilePageInitArguments& args) {
  ASSERT_ND(args.parent_);
  ASSERT_ND(args.page_);
  StorageId storage_id = args.parent_->get_header().storage_id_;
  HashDataPage* page = reinterpret_cast<HashDataPage*>(args.page_);
  PageType parent_type = args.parent_->get_header().get_page_type();
  uint64_t bin;
  if (parent_type == kHashIntermediatePageType) {
    const HashIntermediatePage* parent
      = reinterpret_cast<const HashIntermediatePage*>(args.parent_);

    ASSERT_ND(args.index_in_parent_ < kHashIntermediatePageFanout);
    ASSERT_ND(parent->is_leaf());
    ASSERT_ND(parent->get_bin_range().length() == kHashIntermediatePageFanout);
    bin = parent->get_bin_range().begin_ + args.index_in_parent_;
  } else {
    ASSERT_ND(parent_type == kHashDataPageType);
    ASSERT_ND(args.index_in_parent_ == 0);
    const HashDataPage* parent = reinterpret_cast<const HashDataPage*>(args.parent_);
    bin = parent->get_bin();
  }
  page->initialize_volatile_page(storage_id, args.page_id, args.parent_, bin);
}

// Parallel page release for shutdown/drop. simpler than masstree package

void release_parallel(Engine* engine, VolatilePagePointer pointer) {
  const memory::GlobalVolatilePageResolver& page_resolver
    = engine->get_memory_manager()->get_global_volatile_page_resolver();
  HashIntermediatePage* p
    = reinterpret_cast<HashIntermediatePage*>(page_resolver.resolve_offset(pointer));
  ASSERT_ND(!p->is_leaf());
  memory::PageReleaseBatch release_batch(engine);
  p->release_pages_recursive(page_resolver, &release_batch);
  release_batch.release_all();
}

void HashIntermediatePage::release_pages_recursive_parallel(Engine* engine) {
  if (is_leaf()) {
    // root page is a leaf page.. don't bother parallelize.
    const memory::GlobalVolatilePageResolver& page_resolver
      = engine->get_memory_manager()->get_global_volatile_page_resolver();
    memory::PageReleaseBatch release_batch(engine);
    release_pages_recursive(page_resolver, &release_batch);
    release_batch.release_all();
  } else {
    // so far, we spawn a thread for every single pointer.
    // it might be an oversubscription, but not a big issue.
    std::vector<std::thread> threads;
    for (uint8_t i = 0; i < kHashIntermediatePageFanout; ++i) {
      VolatilePagePointer pointer = pointers_[i].volatile_pointer_;
      if (pointer.components.offset != 0) {
        threads.emplace_back(release_parallel, engine, pointer);
      }
    }

    for (auto& t : threads) {
      t.join();
    }

    VolatilePagePointer volatile_id;
    volatile_id.word = header().page_id_;
    memory::PagePool* pool = engine->get_memory_manager()->get_node_memory(
      volatile_id.components.numa_node)->get_volatile_pool();
    pool->release_one(volatile_id.components.offset);
  }
}

void HashIntermediatePage::release_pages_recursive(
  const memory::GlobalVolatilePageResolver& page_resolver,
  memory::PageReleaseBatch* batch) {
  for (uint8_t i = 0; i < kHashIntermediatePageFanout; ++i) {
    VolatilePagePointer pointer = pointers_[i].volatile_pointer_;
    if (pointer.components.offset != 0) {
      Page* page = page_resolver.resolve_offset(pointer);
      if (is_leaf()) {
        HashDataPage* child = reinterpret_cast<HashDataPage*>(page);
        ASSERT_ND(child->header().get_in_layer_level() == 0);
        child->release_pages_recursive(page_resolver, batch);
      } else {
        HashIntermediatePage* child = reinterpret_cast<HashIntermediatePage*>(page);
        ASSERT_ND(!child->is_leaf());
        child->release_pages_recursive(page_resolver, batch);
      }
      pointer.components.offset = 0;
    }
  }

  VolatilePagePointer volatile_id;
  volatile_id.word = header().page_id_;
  batch->release(volatile_id);
}

void HashDataPage::release_pages_recursive(
  const memory::GlobalVolatilePageResolver& page_resolver,
  memory::PageReleaseBatch* batch) {
  VolatilePagePointer pointer = next_page_.volatile_pointer_;
  if (next_page_.volatile_pointer_.components.offset != 0) {
    HashDataPage* next = reinterpret_cast<HashDataPage*>(
      page_resolver.resolve_offset(next_page_.volatile_pointer_));
    ASSERT_ND(next->header().get_in_layer_level() == 0);
    ASSERT_ND(next->get_bin() == get_bin());
    next->release_pages_recursive(page_resolver, batch);
    next_page_.volatile_pointer_.components.offset = 0;
  }

  VolatilePagePointer volatile_id;
  volatile_id.word = header().page_id_;
  batch->release(volatile_id);
}

}  // namespace hash
}  // namespace storage
}  // namespace foedus
