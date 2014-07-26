/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include "foedus/storage/masstree/masstree_storage_pimpl.hpp"

#include <glog/logging.h>

#include <string>

#include "foedus/engine.hpp"
#include "foedus/log/log_type.hpp"
#include "foedus/log/thread_log_buffer_impl.hpp"
#include "foedus/memory/engine_memory.hpp"
#include "foedus/memory/numa_core_memory.hpp"
#include "foedus/memory/page_pool.hpp"
#include "foedus/storage/record.hpp"
#include "foedus/storage/storage_manager.hpp"
#include "foedus/storage/storage_manager_pimpl.hpp"
#include "foedus/storage/masstree/masstree_id.hpp"
#include "foedus/storage/masstree/masstree_log_types.hpp"
#include "foedus/storage/masstree/masstree_metadata.hpp"
#include "foedus/storage/masstree/masstree_page_impl.hpp"
#include "foedus/storage/masstree/masstree_storage.hpp"
#include "foedus/thread/thread.hpp"
#include "foedus/xct/xct.hpp"
#include "foedus/xct/xct_optimistic_read_impl.hpp"

namespace foedus {
namespace storage {
namespace masstree {

// Defines MasstreeStorage methods so that we can inline implementation calls
bool        MasstreeStorage::is_initialized()   const  { return pimpl_->is_initialized(); }
bool        MasstreeStorage::exists()           const  { return pimpl_->exist_; }
StorageId   MasstreeStorage::get_id()           const  { return pimpl_->metadata_.id_; }
const std::string& MasstreeStorage::get_name()  const  { return pimpl_->metadata_.name_; }
const Metadata* MasstreeStorage::get_metadata() const  { return &pimpl_->metadata_; }
const MasstreeMetadata* MasstreeStorage::get_masstree_metadata() const  {
  return &pimpl_->metadata_;
}

MasstreeStoragePimpl::MasstreeStoragePimpl(
  Engine* engine,
  MasstreeStorage* holder,
  const MasstreeMetadata &metadata,
  bool create)
  :
    engine_(engine),
    holder_(holder),
    metadata_(metadata),
    exist_(!create) {
  ASSERT_ND(create || metadata.id_ > 0);
  ASSERT_ND(metadata.name_.size() > 0);
  first_root_pointer_.snapshot_pointer_ = 0;
  first_root_pointer_.volatile_pointer_.word = 0;
}

ErrorStack MasstreeStoragePimpl::initialize_once() {
  LOG(INFO) << "Initializing an masstree-storage " << *holder_ << " exists=" << exist_;
  first_root_pointer_.snapshot_pointer_ = 0;
  first_root_pointer_.volatile_pointer_.word = 0;

  if (exist_) {
    // TODO(Hideaki): initialize head_root_page_id_
  }
  return kRetOk;
}

ErrorStack MasstreeStoragePimpl::uninitialize_once() {
  LOG(INFO) << "Uninitializing a masstree-storage " << *holder_;

  if (first_root_pointer_.volatile_pointer_.components.offset) {
    // release volatile pages
    const memory::GlobalVolatilePageResolver& page_resolver
      = engine_->get_memory_manager().get_global_volatile_page_resolver();
    MasstreePage* first_root = reinterpret_cast<MasstreePage*>(
      page_resolver.resolve_offset(first_root_pointer_.volatile_pointer_));
    memory::PageReleaseBatch release_batch(engine_);
    first_root->release_pages_recursive_common(page_resolver, &release_batch);
    release_batch.release_all();
    first_root_pointer_.volatile_pointer_.word = 0;
  }
  return kRetOk;
}

ErrorCode MasstreeStoragePimpl::get_first_root(
  thread::Thread* context,
  MasstreePage** root,
  PageVersion* version) {
  while (true) {
    ASSERT_ND(first_root_pointer_.volatile_pointer_.components.offset);
    VolatilePagePointer pointer = first_root_pointer_.volatile_pointer_;
    MasstreePage* page = reinterpret_cast<MasstreePage*>(
      context->get_global_volatile_page_resolver().resolve_offset(pointer));
    *version = page->get_stable_version();
    *root = page;

    // root page has a foster child... time for tree growth!
    if (UNLIKELY(version->has_foster_child())) {
      CHECK_ERROR_CODE(grow_root(context, &first_root_pointer_, page));
      continue;
    }

    // a root pointer might be swapped, so add it to pointer set
    CHECK_ERROR_CODE(
      context->get_current_xct().add_to_pointer_set(
        &first_root_pointer_.volatile_pointer_,
        pointer));
    return kErrorCodeOk;
  }
}

ErrorCode MasstreeStoragePimpl::grow_root(
  thread::Thread* context,
  DualPagePointer* root_pointer,
  MasstreePage* root) {
  if (root->get_layer() == 0) {
    LOG(INFO) << "growing B-tree in first layer! " << *holder_;
  } else {
    DVLOG(0) << "growing B-tree in non-first layer " << *holder_;
  }
  root->lock();
  UnlockScope scope(root);
  PageVersion locked_version = root->get_version();
  if (!locked_version.has_foster_child()) {
    LOG(INFO) << "interesting. someone else has already grown B-tree in first layer";
    return kErrorCodeStrMasstreeRetry;
  }
  ASSERT_ND(root->is_locked());
  ASSERT_ND(root->has_foster_child());
  const memory::LocalPageResolver &resolver = context->get_local_volatile_page_resolver();
  memory::PagePoolOffset offset = context->get_thread_memory()->grab_free_volatile_page();
  if (offset == 0) {
    return kErrorCodeMemoryNoFreePages;
  }

  MasstreeIntermediatePage* new_root =
    reinterpret_cast<MasstreeIntermediatePage*>(resolver.resolve_offset(offset));
  VolatilePagePointer new_pointer;
  new_pointer = combine_volatile_page_pointer(
    context->get_numa_node(),
    kVolatilePointerFlagSwappable,  // pointer to root page might be swapped!
    root_pointer->volatile_pointer_.components.mod_count + 1,
    offset);

  new_root->initialize_volatile_page(
    metadata_.id_,
    new_pointer,
    root->get_layer(),
    true,  // yes, root
    kInfimumSlice,    // infimum slice
    kSupremumSlice,   // high-fence is supremum
    true,             // high-fence is supremum
    kInfimumSlice,    // not foster key first
    nullptr,  // no foster child
    true);    // lock it
  UnlockScope new_scope(new_root);

  new_root->get_version().set_key_count(0);
  MasstreeIntermediatePage::MiniPage& mini_page = new_root->get_minipage(0);
  MasstreePage* foster_child = root->get_foster_child();
  mini_page.mini_version_.lock_version();
  mini_page.mini_version_.set_key_count(1);
  mini_page.pointers_[0].snapshot_pointer_ = 0;
  mini_page.pointers_[0].volatile_pointer_ = root_pointer->volatile_pointer_;
  mini_page.pointers_[0].volatile_pointer_.components.flags = 0;
  ASSERT_ND(reinterpret_cast<Page*>(root) ==
    context->get_global_volatile_page_resolver().resolve_offset(
      mini_page.pointers_[0].volatile_pointer_));
  mini_page.pointers_[1].snapshot_pointer_ = 0;
  mini_page.pointers_[1].volatile_pointer_.word = foster_child->header().page_id_;
  mini_page.pointers_[1].volatile_pointer_.components.flags = 0;
  ASSERT_ND(reinterpret_cast<Page*>(foster_child) ==
    context->get_global_volatile_page_resolver().resolve_offset(
      mini_page.pointers_[1].volatile_pointer_));
  mini_page.separators_[0] = root->get_foster_fence();
  mini_page.mini_version_.unlock_version();
  ASSERT_ND(!new_root->is_border());

  root->clear_foster();

  // Let's install a pointer to the new root page
  root_pointer->volatile_pointer_ = new_pointer;
  root_pointer->snapshot_pointer_ = 0;
  ASSERT_ND(reinterpret_cast<Page*>(new_root) ==
    context->get_global_volatile_page_resolver().resolve_offset(
      root_pointer->volatile_pointer_));

  // As we have changed the pointer, we should update our pointer set too to avoid aborting myself
  context->get_current_xct().overwrite_to_pointer_set(
    &root_pointer->volatile_pointer_,
    new_pointer);
  return kErrorCodeOk;
}

ErrorStack MasstreeStoragePimpl::create(thread::Thread* context) {
  if (exist_) {
    LOG(ERROR) << "This masstree-storage already exists: " << *holder_;
    return ERROR_STACK(kErrorCodeStrAlreadyExists);
  }

  LOG(INFO) << "Newly created an masstree-storage " << *holder_;
  memory::NumaCoreMemory* memory = context->get_thread_memory();
  const memory::LocalPageResolver &local_resolver = context->get_local_volatile_page_resolver();

  // just allocate an empty root page for the first layer
  memory::PagePoolOffset root_offset = memory->grab_free_volatile_page();
  ASSERT_ND(root_offset);
  MasstreeBorderPage* root_page = reinterpret_cast<MasstreeBorderPage*>(
    local_resolver.resolve_offset(root_offset));
  first_root_pointer_.snapshot_pointer_ = 0;
  first_root_pointer_.volatile_pointer_ = combine_volatile_page_pointer(
    context->get_numa_node(),
    kVolatilePointerFlagSwappable,  // pointer to root page might be swapped!
    0,
    root_offset);
  root_page->initialize_volatile_page(
    metadata_.id_,
    first_root_pointer_.volatile_pointer_,
    0,  // first layer
    true,  // yes, root
    kInfimumSlice,    // infimum slice
    kSupremumSlice,   // high-fence is supremum
    true,             // high-fence is supremum
    kInfimumSlice,    // not foster key first
    nullptr,  // no foster child
    false);  // not locked
  ASSERT_ND(root_page->get_version().is_high_fence_supremum());

  exist_ = true;
  engine_->get_storage_manager().get_pimpl()->register_storage(holder_);
  return kRetOk;
}

inline ErrorCode MasstreeStoragePimpl::find_border(
  thread::Thread* context,
  MasstreePage* layer_root,
  uint8_t   current_layer,
  bool      for_writes,
  KeySlice  slice,
  MasstreeBorderPage** border,
  PageVersion* border_version) {
  ASSERT_ND(layer_root->get_layer() == current_layer);
  ASSERT_ND(layer_root->within_fences(slice));
  layer_root->prefetch_general();
  bool is_border = layer_root->is_border();
  while (true) {  // for retry
    PageVersion stable(layer_root->get_stable_version());
    ErrorCode subroutine_result;
    if (is_border) {
      subroutine_result = find_border_leaf(
        reinterpret_cast<MasstreeBorderPage*>(layer_root),
        stable,
        current_layer,
        slice,
        border,
        border_version);
    } else {
      subroutine_result = find_border_descend(
        context,
        reinterpret_cast<MasstreeIntermediatePage*>(layer_root),
        stable,
        current_layer,
        for_writes,
        slice,
        border,
        border_version);
    }
    if (subroutine_result == kErrorCodeStrMasstreeRetry) {
      DVLOG(0) << "Masstree retry find_border";
      continue;
    } else {
      return subroutine_result;
    }
  }
}

ErrorCode MasstreeStoragePimpl::find_border_descend(
  thread::Thread* context,
  MasstreeIntermediatePage* cur,
  PageVersion cur_stable,
  uint8_t   current_layer,
  bool      for_writes,
  KeySlice  slice,
  MasstreeBorderPage** out,
  PageVersion* out_version) {
  ASSERT_ND(cur->get_layer() == current_layer);
  while (true) {  // retry loop
    ASSERT_ND(cur->within_fences(slice));
    if (cur_stable.has_foster_child() && cur->within_foster_child(slice)) {
      // then we have to follow foster chain
      MasstreeIntermediatePage* next = reinterpret_cast<MasstreeIntermediatePage*>(
        cur->get_foster_child());
      PageVersion next_stable(next->get_stable_version());

      // check cur's version again for hand-over-hand verification
      assorted::memory_fence_acquire();
      uint64_t diff = (cur->get_version().data_ ^ cur_stable.data_);
      if (diff <= kPageVersionLockedBit) {
        // this means nothing important has changed. we can now follow foster child
        cur = next;
        cur_stable = next_stable;
        continue;
      } else {
        DVLOG(0) << "find_border_descend encountered a changed version in foster child. retry";
        PageVersion cur_new_stable(cur->get_stable_version());
        if (cur_new_stable.get_split_counter() != cur_stable.get_split_counter()) {
          // we have to retry from root in this case
          return kErrorCodeStrMasstreeRetry;
        }
        // otherwise retry locally
        cur_stable = cur_new_stable;
        continue;
      }
    }
    ASSERT_ND(!cur_stable.has_foster_child() || !cur->within_foster_child(slice));

    uint8_t cur_stable_key_count = cur_stable.get_key_count();
    uint8_t minipage_index = cur->find_minipage(cur_stable_key_count, slice);
    MasstreeIntermediatePage::MiniPage& minipage = cur->get_minipage(minipage_index);

    minipage.prefetch();
    PageVersion mini_stable(minipage.get_stable_version());
    uint8_t mini_stable_key_count = mini_stable.get_key_count();
    uint8_t pointer_index = minipage.find_pointer(mini_stable_key_count, slice);
    DualPagePointer& pointer = minipage.pointers_[pointer_index];
    ASSERT_ND(!pointer.is_both_null());

    MasstreePage* next;
    CHECK_ERROR_CODE(follow_page(context, for_writes, false, &pointer, &next));

    next->prefetch_general();
    bool next_is_border = next->is_border();
    if (next->has_foster_child()) {
      // oh, the page has foster child, so we should adopt it.
      CHECK_ERROR_CODE(cur->adopt_from_child(
        context,
        slice,
        cur_stable,
        minipage_index,
        mini_stable,
        pointer_index,
        next));
      cur_stable = cur->get_stable_version();
      continue;  // we could keep going with a few cautions, but retrying is simpler.
    }

    PageVersion next_stable(next->get_stable_version());

    // check cur's version again for hand-over-hand verification
    assorted::memory_fence_acquire();
    uint64_t diff = (cur->get_version().data_ ^ cur_stable.data_);
    uint64_t diff_mini = (minipage.mini_version_.data_ ^ mini_stable.data_);
    if (diff <= kPageVersionLockedBit && diff_mini <= kPageVersionLockedBit) {
      // this means nothing important has changed.
      if (next_is_border) {
        return find_border_leaf(
          reinterpret_cast<MasstreeBorderPage*>(next),
          next_stable,
          current_layer,
          slice,
          out,
          out_version);
      } else {
        return find_border_descend(
          context,
          reinterpret_cast<MasstreeIntermediatePage*>(next),
          next_stable,
          current_layer,
          for_writes,
          slice,
          out,
          out_version);
      }
    } else {
      DVLOG(0) << "find_border encountered a changed version. retry";
      PageVersion cur_new_stable(cur->get_stable_version());
      if (cur_new_stable.get_split_counter() != cur_stable.get_split_counter()) {
        // we have to retry from root in this case
        return kErrorCodeStrMasstreeRetry;
      }
      // otherwise retry locally
      cur_stable = cur_new_stable;
      continue;
    }
  }
}

inline ErrorCode MasstreeStoragePimpl::find_border_leaf(
  MasstreeBorderPage* cur,
  PageVersion cur_stable,
  uint8_t   current_layer,
  KeySlice  slice,
  MasstreeBorderPage** out,
  PageVersion* out_version) {
  while (true) {  // retry loop
    ASSERT_ND(cur->get_layer() == current_layer);
    ASSERT_ND(cur->within_fences(slice));
    if (!cur_stable.has_foster_child() || !cur->within_foster_child(slice)) {
      *out = cur;
      *out_version = cur_stable;
      return kErrorCodeOk;
    }
    // follow foster child
    MasstreeBorderPage* next = reinterpret_cast<MasstreeBorderPage*>(cur->get_foster_child());
    PageVersion next_stable(next->get_stable_version());

    // check cur's version again for hand-over-hand verification
    assorted::memory_fence_acquire();
    uint64_t diff = (cur->get_version().data_ ^ cur_stable.data_);
    if (diff <= kPageVersionLockedBit) {
      // this means nothing important has changed. we can now follow foster child
      cur = next;
      cur_stable = next_stable;
      continue;
    } else {
      DVLOG(0) << "find_border_leaf encountered a changed version. retry";
      PageVersion cur_new_stable(cur->get_stable_version());
      if (cur_new_stable.get_split_counter() != cur_stable.get_split_counter()) {
        // we have to retry from root in this case
        return kErrorCodeStrMasstreeRetry;
      }
      // otherwise retry locally
      cur_stable = cur_new_stable;
      continue;
    }
  }
}
ErrorCode MasstreeStoragePimpl::locate_record(
  thread::Thread* context,
  const void* key,
  uint16_t key_length,
  bool for_writes,
  MasstreeBorderPage** out_page,
  uint8_t* record_index) {
  ASSERT_ND(key_length <= kMaxKeyLength);
  MasstreePage* layer_root;
  PageVersion root_version;
  CHECK_ERROR_CODE(get_first_root(context, &layer_root, &root_version));
  for (uint16_t current_layer = 0;; ++current_layer) {
    uint8_t remaining_length = key_length - current_layer * 8;
    KeySlice slice = slice_layer(key, key_length, current_layer);
    const void* suffix = reinterpret_cast<const char*>(key) + (current_layer + 1) * 8;
    MasstreeBorderPage* border;
    PageVersion border_version;
    CHECK_ERROR_CODE(find_border(
      context,
      layer_root,
      current_layer,
      for_writes,
      slice,
      &border,
      &border_version));
    uint8_t stable_key_count = border_version.get_key_count();
    uint8_t index = border->find_key(stable_key_count, slice, suffix, remaining_length);

    if (index == MasstreeBorderPage::kMaxKeys) {
      // this means not found
      // TODO(Hideaki) range lock
      return kErrorCodeStrKeyNotFound;
    }
    if (border->does_point_to_layer(index)) {
      CHECK_ERROR_CODE(follow_layer(context, for_writes, border, index, &layer_root));
      continue;
    } else {
      *out_page = border;
      *record_index = index;
      return kErrorCodeOk;
    }
  }
}

ErrorCode MasstreeStoragePimpl::locate_record_normalized(
  thread::Thread* context,
  KeySlice key,
  bool for_writes,
  MasstreeBorderPage** out_page,
  uint8_t* record_index) {
  MasstreeBorderPage* border;
  PageVersion border_version;

  MasstreePage* layer_root;
  PageVersion root_version;
  CHECK_ERROR_CODE(get_first_root(context, &layer_root, &root_version));
  CHECK_ERROR_CODE(find_border(context, layer_root, 0, for_writes, key, &border, &border_version));
  uint8_t index = border->find_key_normalized(0, border_version.get_key_count(), key);
  if (index == MasstreeBorderPage::kMaxKeys) {
    // this means not found
    // TODO(Hideaki) range lock
    return kErrorCodeStrKeyNotFound;
  }
  // because this is just one slice, we never go to second layer
  ASSERT_ND(!border->does_point_to_layer(index));
  *out_page = border;
  *record_index = index;
  return kErrorCodeOk;
}

ErrorCode MasstreeStoragePimpl::create_next_layer(
  thread::Thread* context,
  MasstreeBorderPage* parent,
  uint8_t parent_index) {
  memory::NumaCoreMemory* memory = context->get_thread_memory();
  memory::PagePoolOffset offset = memory->grab_free_volatile_page();
  if (offset == 0) {
    return kErrorCodeMemoryNoFreePages;
  }

  const memory::LocalPageResolver &resolver = context->get_local_volatile_page_resolver();
  MasstreeBorderPage* root = reinterpret_cast<MasstreeBorderPage*>(resolver.resolve_offset(offset));
  DualPagePointer pointer;
  pointer.snapshot_pointer_ = 0;
  pointer.volatile_pointer_ = combine_volatile_page_pointer(context->get_numa_node(), 0, 0, offset);

  xct::XctId* parent_lock = parent->get_owner_id(parent_index);

  // as an independent system transaction, here we do an optimistic version check.
  parent_lock->keylock_unconditional();
  if (parent->does_point_to_layer(parent_index)) {
    // someone else has also made this to a next layer!
    // our effort was a waste, but anyway the goal was achieved.
    LOG(INFO) << "interesting. a concurrent thread has already made a next layer";
    memory->release_free_volatile_page(offset);
    parent_lock->release_keylock();
  } else {
    // initialize the root page by copying the recor
    root->initialize_volatile_page(
      metadata_.id_,
      pointer.volatile_pointer_,
      parent->get_layer() + 1,
      true,  // yes, root
      kInfimumSlice,    // infimum slice
      kSupremumSlice,   // high-fence is supremum
      true,             // high-fence is supremum
      kInfimumSlice,    // not foster key first
      nullptr,  // no foster child
      true);  // initially locked
    UnlockScope scope(root);
    root->copy_initial_record(parent, parent_index);

    // point to the new page
    parent->set_next_layer(parent_index, pointer);

    xct::XctId unlocked_id = *parent_lock;
    unlocked_id.release_keylock();
    // set one next. we don't have to make the new xct id really in serialization order because
    // this is a system transaction that doesn't change anything logically.
    // this is just to make sure other threads get aware of this change at commit time.
    uint16_t ordinal = unlocked_id.get_ordinal();
    if (ordinal != 0xFFFFU) {
      ++ordinal;
    } else {
      unlocked_id.set_epoch(unlocked_id.get_epoch().one_more());
      ordinal = 0;
    }
    unlocked_id.set_ordinal(ordinal);
    if (unlocked_id.is_deleted()) {
      // if the original record was deleted, we inherited it in the new record too.
      // again, we didn't do anything logically.
      ASSERT_ND(root->get_owner_id(0)->is_deleted());
      // as a pointer, now it should be an active pointer.
      unlocked_id.set_notdeleted();
    }
    *parent_lock = unlocked_id;  // now unlock and set the new version
  }
  return kErrorCodeOk;
}

ErrorCode MasstreeStoragePimpl::follow_page(
  thread::Thread* context,
  bool for_writes,
  bool root_in_layer,
  storage::DualPagePointer* pointer,
  MasstreePage** page) {
  return context->follow_page_pointer(
    &kDummyPageInitializer,  // masstree doesn't create a new page except splits.
    false,  // so, there is no null page possible
    for_writes,  // always get volatile pages for writes
    true,
    root_in_layer,  // a root pointer might be swapped, so add it to pointer set even if volatile
    pointer,
    reinterpret_cast<Page**>(page));
}

inline ErrorCode MasstreeStoragePimpl::follow_layer(
  thread::Thread* context,
  bool for_writes,
  MasstreeBorderPage* parent,
  uint8_t record_index,
  MasstreePage** page) {
  ASSERT_ND(record_index < MasstreeBorderPage::kMaxKeys);
  ASSERT_ND(parent->does_point_to_layer(record_index));
  DualPagePointer* pointer = parent->get_next_layer(record_index);
  ASSERT_ND(!pointer->is_both_null());
  MasstreePage* next_root;
  CHECK_ERROR_CODE(follow_page(context, for_writes, true, pointer, &next_root));

  // root page has a foster child... time for tree growth!
  if (next_root->has_foster_child()) {
    CHECK_ERROR_CODE(grow_root(context, pointer, next_root));
    CHECK_ERROR_CODE(follow_page(context, for_writes, true, pointer, &next_root));
  }

  ASSERT_ND(next_root);
  *page = next_root;
  return kErrorCodeOk;
}

ErrorCode MasstreeStoragePimpl::reserve_record(
  thread::Thread* context,
  const void* key,
  uint16_t key_length,
  uint16_t payload_count,
  MasstreeBorderPage** out_page,
  uint8_t* record_index) {
  ASSERT_ND(key_length <= kMaxKeyLength);

  MasstreePage* layer_root;
  PageVersion root_version;
  CHECK_ERROR_CODE(get_first_root(context, &layer_root, &root_version));
  for (uint16_t layer = 0;; ++layer) {
    uint8_t remaining = key_length - layer * sizeof(KeySlice);
    KeySlice slice = slice_layer(key, key_length, layer);
    const void* suffix = reinterpret_cast<const char*>(key) + (layer + 1) * sizeof(KeySlice);
    MasstreeBorderPage* border;
    PageVersion version;
    CHECK_ERROR_CODE(find_border(context, layer_root, layer, true, slice, &border, &version));
    while (true) {  // retry loop for following foster child
      // if, after locking, we found out that the page was split and we should follow foster child,
      // do it.
      if (version.has_foster_child() && border->within_foster_child(slice)) {
        CHECK_ERROR_CODE(find_border_leaf(
          border,
          version,
          layer,
          slice,
          &border,
          &version));
        continue;
      }
      ASSERT_ND(!border->within_foster_child(slice));

      uint8_t count = version.get_key_count();
      MasstreeBorderPage::FindKeyForReserveResult match = border->find_key_for_reserve(
        0,
        count,
        slice,
        suffix,
        remaining);

      if (match.match_type_ == MasstreeBorderPage::kExactMatchLayerPointer) {
        ASSERT_ND(match.index_ < MasstreeBorderPage::kMaxKeys);
        CHECK_ERROR_CODE(follow_layer(context, true, border, match.index_, &layer_root));
        break;  // next layer
      } else if (match.match_type_ == MasstreeBorderPage::kExactMatchLocalRecord) {
        // TODO(Hideaki) even if in this case, if the record space is too small, we can't insert.
        // in that case, we should do delete then insert.
        *out_page = border;
        *record_index = match.index_;
        return kErrorCodeOk;
      }

      // no matching or conflicting keys. so we will create a brand new record.
      // this is a system transaction to just create a deleted record.
      border->lock();
      UnlockScope scope(border);
      // now finally we took a lock, finalizing the version. up to now, everything could happen.
      // check all of them and retry if fails.
      if (border->get_version().get_split_counter() != version.get_split_counter() ||
        !border->within_fences(slice) ||
        border->within_foster_child(slice)) {
        return kErrorCodeStrMasstreeRetry;
      }
      // even resume the searches if new record was installed (only new record area)
      if (count != version.get_key_count()) {
        ASSERT_ND(count < version.get_key_count());
        // someone else has inserted a new record. Is it conflicting?
        // search again, but only for newly inserted record(s)
        uint8_t new_count = version.get_key_count();
        match = border->find_key_for_reserve(count, new_count, slice, suffix, remaining);
        count = new_count;
      }

      if (match.match_type_ == MasstreeBorderPage::kExactMatchLayerPointer) {
        CHECK_ERROR_CODE(follow_layer(context, true, border, match.index_, &layer_root));
        break;  // next layer
      } else if (match.match_type_ == MasstreeBorderPage::kExactMatchLocalRecord) {
        *out_page = border;
        *record_index = match.index_;
        return kErrorCodeOk;
      } else if (match.match_type_ == MasstreeBorderPage::kNotFound) {
        // okay, surely new record
        return reserve_record_new_record(
          context,
          border,
          slice,
          remaining,
          suffix,
          payload_count,
          out_page,
          record_index);
      } else {
        ASSERT_ND(match.match_type_ == MasstreeBorderPage::kConflictingLocalRecord);
        ASSERT_ND(match.index_ < MasstreeBorderPage::kMaxKeys);
        // this means now we have to create a next layer.
        // this is also one system transaction.
        CHECK_ERROR_CODE(create_next_layer(context, border, match.index_));
        CHECK_ERROR_CODE(follow_layer(context, true, border, match.index_, &layer_root));
        break;  // next layer
      }
    }
  }
}

ErrorCode MasstreeStoragePimpl::reserve_record_normalized(
  thread::Thread* context,
  KeySlice key,
  uint16_t payload_count,
  MasstreeBorderPage** out_page,
  uint8_t* record_index) {
  const uint8_t kRemaining = sizeof(KeySlice);
  MasstreeBorderPage* border;
  PageVersion version;

  MasstreePage* layer_root;
  PageVersion root_version;
  CHECK_ERROR_CODE(get_first_root(context, &layer_root, &root_version));

  CHECK_ERROR_CODE(find_border(context, layer_root, 0, true, key, &border, &version));
  while (true) {  // retry loop for following foster child
    border->lock();
    UnlockScope scope(border);
    if (border->get_version().get_split_counter() != version.get_split_counter()) {
      return kErrorCodeStrMasstreeRetry;
    }
    ASSERT_ND(border->within_fences(key));

    // if, after locking, we found out that the page was split and we should follow foster child,
    // do it.
    if (border->get_version().has_foster_child() && border->within_foster_child(key)) {
      CHECK_ERROR_CODE(find_border_leaf(border, border->get_version(), 0, key, &border, &version));
      continue;
    }
    ASSERT_ND(!border->within_foster_child(key));

    // because we never go on to second layer in this case, it's either a full match or not-found
    uint8_t count = border->get_version().get_key_count();
    uint8_t index = border->find_key_normalized(0, count, key);

    if (index != MasstreeBorderPage::kMaxKeys) {
      // TODO(Hideaki) even if in this case, if the record space is too small, we can't insert.
      // in that case, we should do delete then insert.
      *out_page = border;
      *record_index = index;
      return kErrorCodeOk;
    }

    return reserve_record_new_record(
      context,
      border,
      key,
      kRemaining,
      nullptr,
      payload_count,
      out_page,
      record_index);
  }
}

ErrorCode MasstreeStoragePimpl::reserve_record_new_record(
  thread::Thread* context,
  MasstreeBorderPage* border,
  KeySlice key,
  uint8_t remaining,
  const void* suffix,
  uint16_t payload_count,
  MasstreeBorderPage** out_page,
  uint8_t* record_index) {
  ASSERT_ND(border->is_locked());
  uint8_t count = border->get_version().get_key_count();
  if (border->can_accomodate(count, remaining, payload_count)) {
    reserve_record_new_record_apply(context, border, count, key, remaining, suffix, payload_count);
    *out_page = border;
    *record_index = count;
  } else {
    // have to split to make room
    CHECK_ERROR_CODE(border->split_foster(context, key));
    ASSERT_ND(border->get_foster_child()->get_version().is_locked());
    UnlockScope foster_scope(border->get_foster_child());
    MasstreeBorderPage* target;
    if (border->within_foster_child(key)) {
      // new key should be in the foster child.
      target = reinterpret_cast<MasstreeBorderPage*>(border->get_foster_child());
    } else {
      target = border;  // new key should be still in this page
    }
    ASSERT_ND(target->within_fences(key) && !target->within_foster_child(key));
    count = target->get_version().get_key_count();
    ASSERT_ND(target->find_key(border->get_version().get_key_count(), key, suffix, remaining)
      == MasstreeBorderPage::kMaxKeys);
    if (!target->can_accomodate(count, remaining, payload_count)) {
      // this might happen if payload_count is huge. so far just error out.
      LOG(WARNING) << "Wait, not enough space even after splits? should be pretty rare...";
      return kErrorCodeStrTooLongPayload;
    }
    reserve_record_new_record_apply(context, target, count, key, remaining, suffix, payload_count);
    *out_page = target;
    *record_index = count;
  }
  return kErrorCodeOk;
}

void MasstreeStoragePimpl::reserve_record_new_record_apply(
  thread::Thread* context,
  MasstreeBorderPage* target,
  uint8_t target_index,
  KeySlice slice,
  uint8_t remaining_key_length,
  const void* suffix,
  uint16_t payload_count) {
  ASSERT_ND(target->get_version().is_locked());
  ASSERT_ND(target->can_accomodate(target_index, remaining_key_length, payload_count));
  target->get_version().set_inserting_and_increment_key_count();
  xct::XctId initial_id;
  initial_id.set_clean(
    Epoch::kEpochInitialCurrent,  // TODO(Hideaki) this should be something else
    0,
    context->get_thread_id());
  initial_id.set_deleted();
  target->reserve_record_space(
    target_index,
    initial_id,
    slice,
    suffix,
    remaining_key_length,
    payload_count);
}

ErrorCode MasstreeStoragePimpl::retrieve_general(
  thread::Thread* context,
  MasstreeBorderPage* border,
  uint8_t index,
  void* payload,
  uint16_t* payload_capacity) {
  CHECK_ERROR_CODE(xct::optimistic_read_protocol(
    &context->get_current_xct(),
    holder_,
    border->get_owner_id(index),
    border->header().snapshot_,
    [border, index, payload, payload_capacity](xct::XctId observed){
      if (border->does_point_to_layer(index)) {
        return kErrorCodeStrMasstreeRetry;
      } else if (observed.is_deleted()) {
        // in this case, we don't need a range lock. the physical record is surely there.
        return kErrorCodeStrKeyNotFound;
      }
      uint16_t payload_length = border->get_payload_length(index);
      if (payload_length > *payload_capacity) {
        // buffer too small
        DVLOG(0) << "buffer too small??" << payload_length << ":" << *payload_capacity;
        *payload_capacity = payload_length;
        return kErrorCodeStrTooSmallPayloadBuffer;
      }
      *payload_capacity = payload_length;
      uint16_t suffix_length = border->get_suffix_length(index);
      std::memcpy(payload, border->get_record(index) + suffix_length, payload_length);
      return kErrorCodeOk;
    }));
  return kErrorCodeOk;
}

ErrorCode MasstreeStoragePimpl::retrieve_part_general(
  thread::Thread* context,
  MasstreeBorderPage* border,
  uint8_t index,
  void* payload,
  uint16_t payload_offset,
  uint16_t payload_count) {
  CHECK_ERROR_CODE(xct::optimistic_read_protocol(
    &context->get_current_xct(),
    holder_,
    border->get_owner_id(index),
    border->header().snapshot_,
    [border, index, payload, payload_offset, payload_count](xct::XctId observed){
      if (border->does_point_to_layer(index)) {
        return kErrorCodeStrMasstreeRetry;
      } else if (observed.is_deleted()) {
        // in this case, we don't need a range lock. the physical record is surely there.
        return kErrorCodeStrKeyNotFound;
      } else if (border->get_payload_length(index) < payload_offset + payload_count) {
        LOG(WARNING) << "short record";  // probably this is a rare error. so warn.
        return kErrorCodeStrTooShortPayload;
      }
      uint16_t suffix_len = border->get_suffix_length(index);
      std::memcpy(payload, border->get_record(index) + suffix_len + payload_offset, payload_count);
      return kErrorCodeOk;
    }));
  return kErrorCodeOk;
}

ErrorCode MasstreeStoragePimpl::insert_general(
  thread::Thread* context,
  MasstreeBorderPage* border,
  uint8_t index,
  const void* be_key,
  uint16_t key_length,
  const void* payload,
  uint16_t payload_count) {
  xct::XctId* owner_id = border->get_owner_id(index);
  ASSERT_ND(owner_id->is_deleted());
  ASSERT_ND(border->get_payload_length(index) == payload_count);

  uint16_t log_length = MasstreeInsertLogType::calculate_log_length(key_length, payload_count);
  MasstreeInsertLogType* log_entry = reinterpret_cast<MasstreeInsertLogType*>(
    context->get_thread_log_buffer().reserve_new_log(log_length));
  log_entry->populate(
    metadata_.id_,
    be_key,
    key_length,
    payload,
    payload_count,
    border->get_layer());

  return context->get_current_xct().add_to_write_set(
    holder_,
    owner_id,
    border->get_record(index),
    log_entry);
}

ErrorCode MasstreeStoragePimpl::delete_general(
  thread::Thread* context,
  MasstreeBorderPage* border,
  uint8_t index,
  const void* be_key,
  uint16_t key_length) {
  CHECK_ERROR_CODE(xct::optimistic_read_protocol(
    &context->get_current_xct(),
    holder_,
    border->get_owner_id(index),
    false,
    [border, index](xct::XctId observed){
      if (border->does_point_to_layer(index)) {
        return kErrorCodeStrMasstreeRetry;
      } else if (observed.is_deleted()) {
        // in this case, we don't need a range lock. the physical record is surely there.
        return kErrorCodeStrKeyNotFound;
      } else {
        return kErrorCodeOk;
      }
    }));
  uint16_t log_length = MasstreeDeleteLogType::calculate_log_length(key_length);
  MasstreeDeleteLogType* log_entry = reinterpret_cast<MasstreeDeleteLogType*>(
    context->get_thread_log_buffer().reserve_new_log(log_length));
  log_entry->populate(metadata_.id_, be_key, key_length, border->get_layer());

  return context->get_current_xct().add_to_write_set(
    holder_,
    border->get_owner_id(index),
    border->get_record(index),
    log_entry);
}

ErrorCode MasstreeStoragePimpl::overwrite_general(
  thread::Thread* context,
  MasstreeBorderPage* border,
  uint8_t index,
  const void* be_key,
  uint16_t key_length,
  const void* payload,
  uint16_t payload_offset,
  uint16_t payload_count) {
  CHECK_ERROR_CODE(xct::optimistic_read_protocol(
    &context->get_current_xct(),
    holder_,
    border->get_owner_id(index),
    false,
    [border, index, payload_offset, payload_count](xct::XctId observed){
      if (border->does_point_to_layer(index)) {
        return kErrorCodeStrMasstreeRetry;
      } else if (observed.is_deleted()) {
        // in this case, we don't need a range lock. the physical record is surely there.
        return kErrorCodeStrKeyNotFound;
      } else if (border->get_payload_length(index) < payload_offset + payload_count) {
        LOG(WARNING) << "short record ";  // probably this is a rare error. so warn.
        return kErrorCodeStrTooShortPayload;
      } else {
        return kErrorCodeOk;
      }
    }));
  uint16_t log_length = MasstreeOverwriteLogType::calculate_log_length(key_length, payload_count);
  MasstreeOverwriteLogType* log_entry = reinterpret_cast<MasstreeOverwriteLogType*>(
    context->get_thread_log_buffer().reserve_new_log(log_length));
  log_entry->populate(
    metadata_.id_,
    be_key,
    key_length,
    payload,
    payload_offset,
    payload_count,
    border->get_layer());

  return context->get_current_xct().add_to_write_set(
    holder_,
    border->get_owner_id(index),
    border->get_record(index),
    log_entry);
}

template <typename PAYLOAD>
ErrorCode MasstreeStoragePimpl::increment_general(
  thread::Thread* context,
  MasstreeBorderPage* border,
  uint8_t index,
  const void* be_key,
  uint16_t key_length,
  PAYLOAD* value,
  uint16_t payload_offset) {
  // NOTE if we directly pass value and increment there, we might do it multiple times!
  // optimistic_read_protocol() retries if there are version mismatch.
  // so it must be idempotent. be careful!
  PAYLOAD tmp;
  PAYLOAD* tmp_address = &tmp;
  CHECK_ERROR_CODE(xct::optimistic_read_protocol(
    &context->get_current_xct(),
    holder_,
    border->get_owner_id(index),
    false,
    [border, index, tmp_address, payload_offset](xct::XctId observed){
      if (border->does_point_to_layer(index)) {
        return kErrorCodeStrMasstreeRetry;
      } else if (observed.is_deleted()) {
        // in this case, we don't need a range lock. the physical record is surely there.
        return kErrorCodeStrKeyNotFound;
      } else if (border->get_payload_length(index) < payload_offset + sizeof(PAYLOAD)) {
        LOG(WARNING) << "short record ";  // probably this is a rare error. so warn.
        return kErrorCodeStrTooShortPayload;
      }

      uint16_t suffix_length = border->get_suffix_length(index);
      char* ptr = border->get_record(index) + suffix_length + payload_offset;
      *tmp_address = *reinterpret_cast<const PAYLOAD*>(ptr);
      return kErrorCodeOk;
    }));
  *value += tmp;

  uint16_t log_length = MasstreeOverwriteLogType::calculate_log_length(key_length, sizeof(PAYLOAD));
  MasstreeOverwriteLogType* log_entry = reinterpret_cast<MasstreeOverwriteLogType*>(
    context->get_thread_log_buffer().reserve_new_log(log_length));
  log_entry->populate(
    metadata_.id_,
    be_key,
    key_length,
    value,
    payload_offset,
    sizeof(PAYLOAD),
    border->get_layer());

  return context->get_current_xct().add_to_write_set(
    holder_,
    border->get_owner_id(index),
    border->get_record(index),
    log_entry);
}


// Explicit instantiations for each payload type
// @cond DOXYGEN_IGNORE
#define EXPIN_5(x) template ErrorCode MasstreeStoragePimpl::increment_general< x > \
  (thread::Thread* context, MasstreeBorderPage* border, uint8_t index, const void* be_key, \
  uint16_t key_length, x* value, uint16_t payload_offset)
INSTANTIATE_ALL_NUMERIC_TYPES(EXPIN_5);
// @endcond

}  // namespace masstree
}  // namespace storage
}  // namespace foedus
