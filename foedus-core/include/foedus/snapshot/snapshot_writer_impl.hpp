/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_SNAPSHOT_SNAPSHOT_WRITER_IMPL_HPP_
#define FOEDUS_SNAPSHOT_SNAPSHOT_WRITER_IMPL_HPP_
#include <stdint.h>

#include <iosfwd>
#include <string>

#include "foedus/assert_nd.hpp"
#include "foedus/compiler.hpp"
#include "foedus/fwd.hpp"
#include "foedus/initializable.hpp"
#include "foedus/fs/fwd.hpp"
#include "foedus/fs/path.hpp"
#include "foedus/memory/aligned_memory.hpp"
#include "foedus/snapshot/fwd.hpp"
#include "foedus/snapshot/snapshot_id.hpp"
#include "foedus/storage/page.hpp"
#include "foedus/thread/thread_id.hpp"

namespace foedus {
namespace snapshot {
/**
 * @brief Writes out one snapshot file for all data pages in one reducer.
 * @ingroup SNAPSHOT
 * @details
 * In a nutshell, snapshot writer is a in-memory page pool that dumps out some or all of the
 * pages to a snapshot file. It consists of 3 phases \e for \e each \e storage.
 *
 * @par Compose Phase
 * This first phase is invoked by the composers, loading prior snapshot pages and modifying them.
 * Here, snapshot writers behave just a usual in-memory page pool.
 * This part depends on composer, so the snapshot writer calls composer's method.
 *
 * @par Fix Phase
 * Next phase is invoked at the end of composer for the storage, finalizing page ID in the snapshot
 * file for each modified page and replacing volatile page pointers with snapshot pointers.
 * This part also depends on composer (or page format of the storage), so the snapshot writer calls
 * composer's method.
 *
 * @par Dump Phase
 * The last phase simply dumps out the pages to snapshot file. This is a sequential write
 * because no two storages have overlapping pages.
 * This is independent from storage type, thus done in snapshot writer.
 *
 * @par Conquer already-divided
 * Snapshot writer might not have enough pages to hold all pages of the storage modified in this
 * snapshot. This can happen for a large storage with lots of changes.
 * No worry, we have already sorted log entries by keys for this reason.
 * When the page pool becomes fully occupied, we go on to the fix/dump phase, only keeping the
 * \b right-most pages in all levels. After dumping everything else, we repeat the compose phase
 * just like moving on to another storage.
 *
 * @note
 * This is a private implementation-details of \ref SNAPSHOT, thus file name ends with _impl.
 * Do not include this header from a client program. There is no case client program needs to
 * access this internal class.
 */
class SnapshotWriter final : public DefaultInitializable {
 public:
  SnapshotWriter(Engine* engine, LogReducer* parent);
  ErrorStack  initialize_once() override;
  ErrorStack  uninitialize_once() override;
  bool        close();

  SnapshotWriter() = delete;
  SnapshotWriter(const SnapshotWriter &other) = delete;
  SnapshotWriter& operator=(const SnapshotWriter &other) = delete;


  bool                    is_full() ALWAYS_INLINE { return next_page_ >= pool_size_; }
  memory::PagePoolOffset  allocate_new_page() ALWAYS_INLINE {
    ASSERT_ND(!is_full());
    return next_page_++;
  }
  storage::Page*          resolve(memory::PagePoolOffset offset) ALWAYS_INLINE {
    ASSERT_ND(offset > 0);
    ASSERT_ND(offset < pool_size_);
    return page_base_ + offset;
  }
  memory::PagePoolOffset  resolve(storage::Page* page) ALWAYS_INLINE {
    memory::PagePoolOffset offset = page - page_base_;
    ASSERT_ND(offset > 0);
    ASSERT_ND(offset < pool_size_);
    return offset;
  }

  /**
   * @brief Maps given in-memory pages to page IDs in the snapshot file.
   * @param[in] count number of pages to fix
   * @return the base local page ID, or the page ID of the first page when it is written to a file.
   * All the following pages contiguous page IDs, so * next page ID is returned_value + 1.
   * @details
   * This is called by composers to obtain page IDs in the file when it finishes composing
   * the pages. Receiving the base page ID, composers will finalize their data pages to replace
   * page IDs in data pages. When it's done, they will call dump_pages().
   */
  storage::SnapshotLocalPageId fix_pages(uint32_t count) {
    storage::SnapshotLocalPageId ret = fixed_upto_;
    fixed_upto_ += count;
    return ret;
  }

  /**
   * @brief Writes out in-memory pages to the snapshot file.
   * @param[in] memory_pages in-memory pages to fix
   * @param[in] count length of memory_pages
   * @pre fixed_upto_ - count == dumped_upto_
   * @post fixed_upto_ == dumped_upto_
   * @details
   * All pages will be written contiguously. So, this method first stitches the in-memory pages
   * to IO buffer then call write(). We do so even if the in-memory pages are (luckily) contiguous.
   */
  ErrorCode dump_pages(const memory::PagePoolOffset* memory_pages, uint32_t count);

  /**
   * @brief Called when one storage is fully or partially written.
   * @param[in] excluded_pages a small number of pages that are retained while this resetting.
   * @param[in] excluded_count count of excluded_pages
   * @return new page offset for excluded_pages[0]. excluded_pages[i] would be returned_value + i.
   * @pre excluded_pages are sorted by offset in ascending order. This is trivially guaranteed
   * if you pass pages from root to leaf order.
   * @post next_page_ == 1 + excluded_count
   * @details
   * Returns all in-memory pages to the pool \b except the excluded pages.
   * The excluded pages are given only when the storage is partially written to avoid OOM.
   * These pages are \b moved to the beginning of the page pool, so their page offsets
   * will \b change. The returned value (which is so far always 1) tells the new page offset for
   * the excluded pages.
   *
   * We do this compaction to guarantee that there is no hole in page allocation in this object.
   * The excluded pages are very few, so this won't cause an issue.
   * This is the only interface in snapshot writer to return pages to pool.
   * Compared to releasing each page, this is much more efficient.
   */
  memory::PagePoolOffset reset_pool(
    const memory::PagePoolOffset* excluded_pages,
    uint32_t excluded_count);

  /** for recycling dump_io_buffer. */
  void      set_dump_io_buffer(memory::AlignedMemory* dump_io_buffer) {
    dump_io_buffer_ = dump_io_buffer;
  }

  std::string             to_string() const {
    return "SnapshotWriter-" + std::to_string(numa_node_);
  }
  friend std::ostream&    operator<<(std::ostream& o, const SnapshotWriter& v);


 private:
  Engine* const                   engine_;
  LogReducer* const               parent_;
  /** Also parent's ID. One NUMA node = one reducer = one snapshot writer. */
  const thread::ThreadGroupId     numa_node_;
  /** Same as parent_->get_parent()->get_snapshot()->id_. Stored for convenience. */
  const SnapshotId                snapshot_id_;

  /** The snapshot file to write to. */
  fs::DirectIoFile*               snapshot_file_;

  /** This is the only page pool for all composers using this snapshot writer. */
  memory::AlignedMemory           pool_memory_;
  /** Same as pool_memory_.get_block(). */
  storage::Page*                  page_base_;
  /** How many pages allocated from the pool. Cleared after completion of each storage. */
  memory::PagePoolOffset          next_page_;
  memory::PagePoolOffset          pool_size_;

  /**
   * Used to sequentially write out data pages to a file.
   * The writer does NOT own this buffer. It's actually a second-hand buffer given by
   * reducer (was reducer's dump IO buffer).
   */
  memory::AlignedMemory*          dump_io_buffer_;

  /**
   * This writer has fixed pages up to this page.
   * In other word, the next page will be fixed_upto_ + 1.
   */
  storage::SnapshotLocalPageId    fixed_upto_;
  /**
   * This writer has written out pages up to this page.
   * This number should become same as fixed_upto_ after each dump.
   */
  storage::SnapshotLocalPageId    dumped_upto_;

  void      clear_snapshot_file();
  fs::Path  get_snapshot_file_path() const;
};
}  // namespace snapshot
}  // namespace foedus
#endif  // FOEDUS_SNAPSHOT_SNAPSHOT_WRITER_IMPL_HPP_