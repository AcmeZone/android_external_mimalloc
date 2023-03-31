/* ----------------------------------------------------------------------------
Copyright (c) 2019-2022, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
"Arenas" are fixed area's of OS memory from which we can allocate
large blocks (>= MI_ARENA_BLOCK_SIZE, 32MiB).
In contrast to the rest of mimalloc, the arenas are shared between
threads and need to be accessed using atomic operations.

Arenas are used to for huge OS page (1GiB) reservations or for reserving
OS memory upfront which can be improve performance or is sometimes needed
on embedded devices. We can also employ this with WASI or `sbrk` systems 
to reserve large arenas upfront and be able to reuse the memory more effectively.

The arena allocation needs to be thread safe and we use an atomic bitmap to allocate.
-----------------------------------------------------------------------------*/
#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/atomic.h"

#include <string.h>  // memset
#include <errno.h> // ENOMEM

#include "bitmap.h"  // atomic bitmap

/* -----------------------------------------------------------
  Arena allocation
----------------------------------------------------------- */

#define MI_ARENA_BLOCK_SIZE   (4*MI_SEGMENT_ALIGN)     // 32MiB
#define MI_ARENA_MIN_OBJ_SIZE (MI_ARENA_BLOCK_SIZE/2)  // 16MiB
#define MI_MAX_ARENAS         (64)                     // not more than 126 (since we use 7 bits in the memid and an arena index + 1)

// A memory arena descriptor
typedef struct mi_arena_s {
  mi_arena_id_t id;                       // arena id; 0 for non-specific
  bool     exclusive;                     // only allow allocations if specifically for this arena
  _Atomic(uint8_t*) start;                // the start of the memory area
  size_t   block_count;                   // size of the area in arena blocks (of `MI_ARENA_BLOCK_SIZE`)
  size_t   field_count;                   // number of bitmap fields (where `field_count * MI_BITMAP_FIELD_BITS >= block_count`)
  int      numa_node;                     // associated NUMA node
  bool     is_zero_init;                  // is the arena zero initialized?
  bool     allow_decommit;                // is decommit allowed? if true, is_large should be false and blocks_committed != NULL
  bool     is_large;                      // large- or huge OS pages (always committed)
  _Atomic(size_t) search_idx;             // optimization to start the search for free blocks
  _Atomic(mi_msecs_t) purge_expire;    // expiration time when blocks should be decommitted from `blocks_decommit`.  
  mi_bitmap_field_t* blocks_dirty;        // are the blocks potentially non-zero?
  mi_bitmap_field_t* blocks_committed;    // are the blocks committed? (can be NULL for memory that cannot be decommitted)
  mi_bitmap_field_t* blocks_purge;     // blocks that can be (reset) decommitted. (can be NULL for memory that cannot be (reset) decommitted)  
  mi_bitmap_field_t  blocks_inuse[1];     // in-place bitmap of in-use blocks (of size `field_count`)
} mi_arena_t;


// The available arenas
static mi_decl_cache_align _Atomic(mi_arena_t*) mi_arenas[MI_MAX_ARENAS];
static mi_decl_cache_align _Atomic(size_t)      mi_arena_count; // = 0


/* -----------------------------------------------------------
  Arena id's
  0 is used for non-arena's (like OS memory)
  id = arena_index + 1
----------------------------------------------------------- */

static size_t mi_arena_id_index(mi_arena_id_t id) {
  return (size_t)(id <= 0 ? MI_MAX_ARENAS : id - 1);
}

static mi_arena_id_t mi_arena_id_create(size_t arena_index) {
  mi_assert_internal(arena_index < MI_MAX_ARENAS);
  mi_assert_internal(MI_MAX_ARENAS <= 126);
  int id = (int)arena_index + 1;
  mi_assert_internal(id >= 1 && id <= 127);
  return id;
}

mi_arena_id_t _mi_arena_id_none(void) {
  return 0;
}

static bool mi_arena_id_is_suitable(mi_arena_id_t arena_id, bool arena_is_exclusive, mi_arena_id_t req_arena_id) {
  return ((!arena_is_exclusive && req_arena_id == _mi_arena_id_none()) ||
          (arena_id == req_arena_id));
}


/* -----------------------------------------------------------
  Arena allocations get a memory id where the lower 8 bits are
  the arena id, and the upper bits the block index.
----------------------------------------------------------- */

// Use `0` as a special id for direct OS allocated memory.
#define MI_MEMID_OS   0

static size_t mi_arena_memid_create(mi_arena_id_t id, bool exclusive, mi_bitmap_index_t bitmap_index) {
  mi_assert_internal(((bitmap_index << 8) >> 8) == bitmap_index); // no overflow?
  mi_assert_internal(id >= 0 && id <= 0x7F);
  return ((bitmap_index << 8) | ((uint8_t)id & 0x7F) | (exclusive ? 0x80 : 0));
}

static bool mi_arena_memid_indices(size_t arena_memid, size_t* arena_index, mi_bitmap_index_t* bitmap_index) {
  mi_assert_internal(arena_memid != MI_MEMID_OS);
  *bitmap_index = (arena_memid >> 8);
  mi_arena_id_t id = (int)(arena_memid & 0x7F);
  *arena_index = mi_arena_id_index(id);
  return ((arena_memid & 0x80) != 0);
}

bool _mi_arena_memid_is_suitable(size_t arena_memid, mi_arena_id_t request_arena_id) {
  mi_assert_internal(arena_memid != MI_MEMID_OS);
  mi_arena_id_t id = (int)(arena_memid & 0x7F);
  bool exclusive = ((arena_memid & 0x80) != 0);
  return mi_arena_id_is_suitable(id, exclusive, request_arena_id);
}

bool _mi_arena_is_os_allocated(size_t arena_memid) {
  return (arena_memid == MI_MEMID_OS);
}

static size_t mi_block_count_of_size(size_t size) {
  return _mi_divide_up(size, MI_ARENA_BLOCK_SIZE);
}

/* -----------------------------------------------------------
  Thread safe allocation in an arena
----------------------------------------------------------- */
static bool mi_arena_alloc(mi_arena_t* arena, size_t blocks, mi_bitmap_index_t* bitmap_idx)
{
  size_t idx = mi_atomic_load_acquire(&arena->search_idx);  // start from last search
  if (_mi_bitmap_try_find_from_claim_across(arena->blocks_inuse, arena->field_count, idx, blocks, bitmap_idx)) {
    mi_atomic_store_release(&arena->search_idx, idx);  // start search from here next time
    return true;
  };
  return false;
}


/* -----------------------------------------------------------
  Arena Allocation
----------------------------------------------------------- */

static void* mi_arena_alloc_from(mi_arena_t* arena, size_t arena_index, size_t needed_bcount,
                                 bool* commit, bool* large, bool* is_pinned, bool* is_zero,
                                 mi_arena_id_t req_arena_id, size_t* memid, mi_os_tld_t* tld)
{
  MI_UNUSED(arena_index);
  mi_assert_internal(mi_arena_id_index(arena->id) == arena_index);
  if (!mi_arena_id_is_suitable(arena->id, arena->exclusive, req_arena_id)) return NULL;

  mi_bitmap_index_t bitmap_index;
  if (!mi_arena_alloc(arena, needed_bcount, &bitmap_index)) return NULL;

  // claimed it! 
  void* p    = arena->start + (mi_bitmap_index_bit(bitmap_index)*MI_ARENA_BLOCK_SIZE);
  *memid     = mi_arena_memid_create(arena->id, arena->exclusive, bitmap_index);
  *large     = arena->is_large;
  *is_pinned = (arena->is_large || !arena->allow_decommit);

  // none of the claimed blocks should be scheduled for a decommit
  if (arena->blocks_purge != NULL) {
    // this is thread safe as a potential purge only decommits parts that are not yet claimed as used (in `in_use`).
    _mi_bitmap_unclaim_across(arena->blocks_purge, arena->field_count, needed_bcount, bitmap_index);
  }

  // set the dirty bits (todo: no need for an atomic op here?)
  *is_zero   = _mi_bitmap_claim_across(arena->blocks_dirty, arena->field_count, needed_bcount, bitmap_index, NULL);

  // set commit state
  if (arena->blocks_committed == NULL) {
    // always committed
    *commit = true;
  }
  else if (*commit) {
    // arena not committed as a whole, but commit requested: ensure commit now
    bool any_uncommitted;
    _mi_bitmap_claim_across(arena->blocks_committed, arena->field_count, needed_bcount, bitmap_index, &any_uncommitted);
    if (any_uncommitted) {
      bool commit_zero;
      _mi_os_commit(p, needed_bcount * MI_ARENA_BLOCK_SIZE, &commit_zero, tld->stats);
      if (commit_zero) *is_zero = true;
    }
  }
  else {
    // no need to commit, but check if already fully committed
    *commit = _mi_bitmap_is_claimed_across(arena->blocks_committed, arena->field_count, needed_bcount, bitmap_index);
  }
  return p;
}

// allocate in a speficic arena
static void* mi_arena_alloc_in(mi_arena_id_t arena_id, int numa_node, size_t size, size_t alignment, 
                               bool* commit, bool* large, bool* is_pinned, bool* is_zero,
                               mi_arena_id_t req_arena_id, size_t* memid, mi_os_tld_t* tld ) 
{
  MI_UNUSED_RELEASE(alignment);
  mi_assert_internal(alignment <= MI_SEGMENT_ALIGN);
  const size_t max_arena = mi_atomic_load_relaxed(&mi_arena_count);
  const size_t bcount = mi_block_count_of_size(size);  
  const size_t arena_index = mi_arena_id_index(arena_id);
  mi_assert_internal(arena_index < max_arena);
  mi_assert_internal(size <= bcount * MI_ARENA_BLOCK_SIZE);
  if (arena_index >= max_arena) return NULL;

  mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[arena_index]);
  if (arena == NULL) return NULL;
  if (arena->numa_node >= 0 && arena->numa_node != numa_node) return NULL;
  if (!(*large) && arena->is_large) return NULL;
  return mi_arena_alloc_from(arena, arena_index, bcount, commit, large, is_pinned, is_zero, req_arena_id, memid, tld);
}


// allocate from an arena with fallback to the OS
static mi_decl_noinline void* mi_arena_allocate(int numa_node, size_t size, size_t alignment, bool* commit, bool* large,
                                                bool* is_pinned, bool* is_zero,
                                                mi_arena_id_t req_arena_id, size_t* memid, mi_os_tld_t* tld )
{
  MI_UNUSED_RELEASE(alignment);
  mi_assert_internal(alignment <= MI_SEGMENT_ALIGN);
  const size_t max_arena = mi_atomic_load_relaxed(&mi_arena_count);
  const size_t bcount = mi_block_count_of_size(size);
  if mi_likely(max_arena == 0) return NULL;
  mi_assert_internal(size <= bcount * MI_ARENA_BLOCK_SIZE);

  size_t arena_index = mi_arena_id_index(req_arena_id);
  if (arena_index < MI_MAX_ARENAS) {
    // try a specific arena if requested
    mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[arena_index]);
    if ((arena != NULL) &&
        (arena->numa_node < 0 || arena->numa_node == numa_node) && // numa local?
        (*large || !arena->is_large)) // large OS pages allowed, or arena is not large OS pages
    {
      void* p = mi_arena_alloc_from(arena, arena_index, bcount, commit, large, is_pinned, is_zero, req_arena_id, memid, tld);
      mi_assert_internal((uintptr_t)p % alignment == 0);
      if (p != NULL) return p;
    }
  }
  else {
    // try numa affine allocation
    for (size_t i = 0; i < max_arena; i++) {
      mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[i]);
      if (arena == NULL) break; // end reached
      if ((arena->numa_node < 0 || arena->numa_node == numa_node) && // numa local?
          (*large || !arena->is_large)) // large OS pages allowed, or arena is not large OS pages
      {
        void* p = mi_arena_alloc_from(arena, i, bcount, commit, large, is_pinned, is_zero, req_arena_id, memid, tld);
        mi_assert_internal((uintptr_t)p % alignment == 0);
        if (p != NULL) return p;
      }
    }

    // try from another numa node instead..
    for (size_t i = 0; i < max_arena; i++) {
      mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[i]);
      if (arena == NULL) break; // end reached
      if ((arena->numa_node >= 0 && arena->numa_node != numa_node) && // not numa local!
          (*large || !arena->is_large)) // large OS pages allowed, or arena is not large OS pages
      {
        void* p = mi_arena_alloc_from(arena, i, bcount, commit, large, is_pinned, is_zero, req_arena_id, memid, tld);
        mi_assert_internal((uintptr_t)p % alignment == 0);
        if (p != NULL) return p;
      }
    }
  }
  return NULL;
}

void* _mi_arena_alloc_aligned(size_t size, size_t alignment, size_t align_offset, bool* commit, bool* large, bool* is_pinned, bool* is_zero,
                              mi_arena_id_t req_arena_id, size_t* memid, mi_os_tld_t* tld)
{
  mi_assert_internal(commit != NULL && is_pinned != NULL && is_zero != NULL && memid != NULL && tld != NULL);
  mi_assert_internal(size > 0);
  *memid   = MI_MEMID_OS;
  *is_zero = false;
  *is_pinned = false;

  bool default_large = false;
  if (large == NULL) large = &default_large;   // ensure `large != NULL`
  const int numa_node = _mi_os_numa_node(tld); // current numa node

  // try to allocate in an arena if the alignment is small enough and the object is not too small (as for heap meta data)
  if (size >= MI_ARENA_MIN_OBJ_SIZE && alignment <= MI_SEGMENT_ALIGN && align_offset == 0) {
    void* p = mi_arena_allocate(numa_node, size, alignment, commit, large, is_pinned, is_zero, req_arena_id, memid, tld);
    if (p != NULL) return p;

    // otherwise, try to first eagerly reserve a new arena 
    size_t eager_reserve = mi_option_get_size(mi_option_arena_reserve);
    eager_reserve = _mi_align_up(eager_reserve, MI_ARENA_BLOCK_SIZE);
    if (eager_reserve > 0 && eager_reserve >= size &&                    // eager reserve enabled and large enough?
        req_arena_id == _mi_arena_id_none() &&                           // not exclusive?
        mi_atomic_load_relaxed(&mi_arena_count) < 3*(MI_MAX_ARENAS/4) )  // not too many arenas already?        
    {      
      mi_arena_id_t arena_id = 0;
      if (mi_reserve_os_memory_ex(eager_reserve, false /* commit */, *large /* allow large*/, false /* exclusive */, &arena_id) == 0) {
         p = mi_arena_alloc_in(arena_id, numa_node, size, alignment, commit, large, is_pinned, is_zero, req_arena_id, memid, tld);        
         if (p != NULL) return p;
      }
    }
  }

  // finally, fall back to the OS
  if (mi_option_is_enabled(mi_option_limit_os_alloc) || req_arena_id != _mi_arena_id_none()) {
    errno = ENOMEM;
    return NULL;
  }
  *is_zero = true;
  *memid   = MI_MEMID_OS;
  void* p = _mi_os_alloc_aligned_offset(size, alignment, align_offset, *commit, large, tld->stats);
  if (p != NULL) { *is_pinned = *large; }
  return p;
}

void* _mi_arena_alloc(size_t size, bool* commit, bool* large, bool* is_pinned, bool* is_zero, mi_arena_id_t req_arena_id, size_t* memid, mi_os_tld_t* tld)
{
  return _mi_arena_alloc_aligned(size, MI_ARENA_BLOCK_SIZE, 0, commit, large, is_pinned, is_zero, req_arena_id, memid, tld);
}


void* mi_arena_area(mi_arena_id_t arena_id, size_t* size) {
  if (size != NULL) *size = 0;
  size_t arena_index = mi_arena_id_index(arena_id);
  if (arena_index >= MI_MAX_ARENAS) return NULL;
  mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[arena_index]);
  if (arena == NULL) return NULL;
  if (size != NULL) *size = arena->block_count * MI_ARENA_BLOCK_SIZE;
  return arena->start;
}

/* -----------------------------------------------------------
  Arena purge
----------------------------------------------------------- */

// either resets or decommits memory, returns true if the memory was decommitted.
static bool mi_os_purge(void* p, size_t size, mi_stats_t* stats) {
  if (mi_option_is_enabled(mi_option_reset_decommits) &&   // should decommit?
      !_mi_preloading())                                   // don't decommit during preloading (unsafe)
  {
    _mi_os_decommit(p, size, stats);
    return true;  // decommitted
  }
  else {
    _mi_os_reset(p, size, stats);
    return false;  // not decommitted
  }
}

// reset or decommit in an arena and update the committed/decommit bitmaps
static void mi_arena_purge(mi_arena_t* arena, size_t bitmap_idx, size_t blocks, mi_stats_t* stats) {
  mi_assert_internal(arena->blocks_committed != NULL);
  mi_assert_internal(arena->blocks_purge != NULL);
  mi_assert_internal(arena->allow_decommit);
  const size_t size = blocks * MI_ARENA_BLOCK_SIZE;
  void* const p = arena->start + (mi_bitmap_index_bit(bitmap_idx) * MI_ARENA_BLOCK_SIZE);
  const bool decommitted = mi_os_purge(p, size, stats);
  // update committed bitmap
  if (decommitted) {
    _mi_bitmap_unclaim_across(arena->blocks_committed, arena->field_count, blocks, bitmap_idx);
    _mi_bitmap_unclaim_across(arena->blocks_purge, arena->field_count, blocks, bitmap_idx);
  }
}

// Schedule a purge. This is usually delayed to avoid repeated decommit/commit calls.
static void mi_arena_schedule_purge(mi_arena_t* arena, size_t bitmap_idx, size_t blocks, mi_stats_t* stats) {
  mi_assert_internal(arena->blocks_purge != NULL);
  const long delay = mi_option_get(mi_option_arena_purge_delay);
  if (_mi_preloading() || delay == 0) {
    // decommit directly
    mi_arena_purge(arena, bitmap_idx, blocks, stats);    
  }
  else {
    // schedule decommit
    mi_msecs_t expire = mi_atomic_loadi64_relaxed(&arena->purge_expire);
    if (expire != 0) {
      mi_atomic_add_acq_rel(&arena->purge_expire, delay/10);  // add smallish extra delay
    }
    else {
      mi_atomic_storei64_release(&arena->purge_expire, _mi_clock_now() + delay);
    }
    _mi_bitmap_claim_across(arena->blocks_purge, arena->field_count, blocks, bitmap_idx, NULL);
  }
}

// return true if the full range was purged
static bool mi_arena_purge_range(mi_arena_t* arena, size_t idx, size_t startidx, size_t bitlen, size_t purge, mi_stats_t* stats) {
  const size_t endidx = startidx + bitlen;
  size_t bitidx = startidx;
  bool all_purged = false;
  while (bitidx < endidx) {
    size_t count = 0;
    while (bitidx + count < endidx && (purge & ((size_t)1 << (bitidx + count))) == 1) {
      count++;
    }
    if (count > 0) {
      // found range to be purged
      const mi_bitmap_index_t bitmap_idx = mi_bitmap_index_create(idx, bitidx);
      mi_arena_purge(arena, bitmap_idx, count, stats);
      if (count == bitlen) {
        all_purged = true;
      }
    }
    bitidx += (count+1);
  }
  return all_purged;
}

// returns true if anything was decommitted
static bool mi_arena_try_purge(mi_arena_t* arena, mi_msecs_t now, bool force, mi_stats_t* stats) 
{
  if (!arena->allow_decommit || arena->blocks_purge == NULL) return false;
  mi_msecs_t expire = mi_atomic_loadi64_relaxed(&arena->purge_expire);
  if (expire == 0) return false;
  if (!force && expire > now) return false;

  // reset expire (if not already set concurrently)
  mi_atomic_cas_strong_acq_rel(&arena->purge_expire, &expire, 0);
  
  // potential purges scheduled, walk through the bitmap
  bool any_purged = false;
  bool full_purge = true;  
  for (size_t i = 0; i < arena->field_count; i++) {
    size_t purge = mi_atomic_load_relaxed(&arena->blocks_purge[i]);
    if (purge != 0) {
      size_t bitidx = 0;
      while (bitidx < MI_BITMAP_FIELD_BITS) {
        size_t bitlen = 1;
        if ((purge & ((size_t)1 << bitidx)) != 0) {
          while ((bitidx + bitlen < MI_BITMAP_FIELD_BITS) &&
            ((purge & ((size_t)1 << (bitidx + bitlen))) != 0)) { bitlen++; }
          // found range of purgeable blocks 
          // try to claim the longest range of corresponding in_use bits
          const mi_bitmap_index_t bitmap_index = mi_bitmap_index_create(i, bitidx);
          while( bitlen > 0 ) {
            if (_mi_bitmap_try_claim(arena->blocks_inuse, arena->field_count, bitlen, bitmap_index)) {
              break;
            }
            bitlen--;
          }
          // claimed count bits at in_use
          if (bitlen > 0) {
            // read purge again now that we have the in_use bits
            purge = mi_atomic_load_acquire(&arena->blocks_purge[i]);
            if (!mi_arena_purge_range(arena, i, bitidx, bitlen, purge, stats)) {
              full_purge = false;
            }
            any_purged = true;
          }
          else {
            bitlen = 1;  // make progress
          }
        }
        bitidx += bitlen;
      }
    }
  }
  return any_purged;
}

static void mi_arenas_try_purge( bool force, bool visit_all, mi_stats_t* stats ) {
  const long delay = mi_option_get(mi_option_arena_purge_delay);
  if (_mi_preloading() || delay == 0) return;  // nothing will be scheduled
  const size_t max_arena = mi_atomic_load_relaxed(&mi_arena_count);
  if (max_arena == 0) return;

  // allow only one thread to purge at a time
  static mi_atomic_guard_t purge_guard;
  mi_atomic_guard(&purge_guard) 
  {
    mi_msecs_t now = _mi_clock_now();
    size_t max_purge_count = (visit_all ? max_arena : 1);
    for (size_t i = 0; i < max_arena; i++) {
      mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t, &mi_arenas[i]);
      if (mi_arena_try_purge(arena, now, force, stats)) {
        if (max_purge_count <= 1) break;
        max_purge_count--;
      }
    }
  }  
}


/* -----------------------------------------------------------
  Arena free
----------------------------------------------------------- */

void _mi_arena_free(void* p, size_t size, size_t alignment, size_t align_offset, size_t memid, bool all_committed, mi_stats_t* stats) {
  mi_assert_internal(size > 0 && stats != NULL);
  if (p==NULL) return;
  if (size==0) return;
  if (memid == MI_MEMID_OS) {
    // was a direct OS allocation, pass through
    _mi_os_free_aligned(p, size, alignment, align_offset, all_committed, stats);
  }
  else {
    // allocated in an arena
    mi_assert_internal(align_offset == 0);
    size_t arena_idx;
    size_t bitmap_idx;
    mi_arena_memid_indices(memid, &arena_idx, &bitmap_idx);
    mi_assert_internal(arena_idx < MI_MAX_ARENAS);
    mi_arena_t* arena = mi_atomic_load_ptr_relaxed(mi_arena_t,&mi_arenas[arena_idx]);
    mi_assert_internal(arena != NULL);
    const size_t blocks = mi_block_count_of_size(size);
    
    // checks
    if (arena == NULL) {
      _mi_error_message(EINVAL, "trying to free from non-existent arena: %p, size %zu, memid: 0x%zx\n", p, size, memid);
      return;
    }
    mi_assert_internal(arena->field_count > mi_bitmap_index_field(bitmap_idx));
    if (arena->field_count <= mi_bitmap_index_field(bitmap_idx)) {
      _mi_error_message(EINVAL, "trying to free from non-existent arena block: %p, size %zu, memid: 0x%zx\n", p, size, memid);
      return;
    }
    
    // potentially decommit
    if (!arena->allow_decommit || arena->blocks_committed == NULL) {
      mi_assert_internal(all_committed); // note: may be not true as we may "pretend" to be not committed (in segment.c)
    }
    else {
      mi_assert_internal(arena->blocks_committed != NULL);
      mi_assert_internal(arena->blocks_purge != NULL);
      mi_arena_schedule_purge(arena, bitmap_idx, blocks, stats);      
    }
    
    // and make it available to others again
    bool all_inuse = _mi_bitmap_unclaim_across(arena->blocks_inuse, arena->field_count, blocks, bitmap_idx);
    if (!all_inuse) {
      _mi_error_message(EAGAIN, "trying to free an already freed block: %p, size %zu\n", p, size);
      return;
    };
  }
}


/* -----------------------------------------------------------
  Add an arena.
----------------------------------------------------------- */

static bool mi_arena_add(mi_arena_t* arena, mi_arena_id_t* arena_id) {
  mi_assert_internal(arena != NULL);
  mi_assert_internal((uintptr_t)mi_atomic_load_ptr_relaxed(uint8_t,&arena->start) % MI_SEGMENT_ALIGN == 0);
  mi_assert_internal(arena->block_count > 0);
  if (arena_id != NULL) *arena_id = -1;

  size_t i = mi_atomic_increment_acq_rel(&mi_arena_count);
  if (i >= MI_MAX_ARENAS) {
    mi_atomic_decrement_acq_rel(&mi_arena_count);
    return false;
  }
  mi_atomic_store_ptr_release(mi_arena_t,&mi_arenas[i], arena);
  arena->id = mi_arena_id_create(i);
  if (arena_id != NULL) *arena_id = arena->id;
  return true;
}

bool mi_manage_os_memory_ex(void* start, size_t size, bool is_committed, bool is_large, bool is_zero, int numa_node, bool exclusive, mi_arena_id_t* arena_id) mi_attr_noexcept
{
  if (arena_id != NULL) *arena_id = _mi_arena_id_none();
  if (size < MI_ARENA_BLOCK_SIZE) return false;

  if (is_large) {
    mi_assert_internal(is_committed);
    is_committed = true;
  }

  const bool allow_decommit = !is_large && !is_committed; // only allow decommit for initially uncommitted memory

  const size_t bcount = size / MI_ARENA_BLOCK_SIZE;
  const size_t fields = _mi_divide_up(bcount, MI_BITMAP_FIELD_BITS);
  const size_t bitmaps = (allow_decommit ? 4 : 2);
  const size_t asize  = sizeof(mi_arena_t) + (bitmaps*fields*sizeof(mi_bitmap_field_t));
  mi_arena_t* arena   = (mi_arena_t*)_mi_os_alloc(asize, &_mi_stats_main); // TODO: can we avoid allocating from the OS?
  if (arena == NULL) return false;

  // already zero'd due to os_alloc
  // _mi_memzero(arena, asize);
  arena->id = _mi_arena_id_none();
  arena->exclusive = exclusive;
  arena->block_count = bcount;
  arena->field_count = fields;
  arena->start = (uint8_t*)start;
  arena->numa_node    = numa_node; // TODO: or get the current numa node if -1? (now it allows anyone to allocate on -1)
  arena->is_large     = is_large;
  arena->is_zero_init = is_zero;
  arena->allow_decommit = allow_decommit; 
  arena->purge_expire = 0;
  arena->search_idx   = 0;
  arena->blocks_dirty = &arena->blocks_inuse[fields]; // just after inuse bitmap
  arena->blocks_committed = (!arena->allow_decommit ? NULL : &arena->blocks_inuse[2*fields]); // just after dirty bitmap
  arena->blocks_purge  = (!arena->allow_decommit ? NULL : &arena->blocks_inuse[3*fields]); // just after committed bitmap  
  // initialize committed bitmap?
  if (arena->blocks_committed != NULL && is_committed) {
    memset((void*)arena->blocks_committed, 0xFF, fields*sizeof(mi_bitmap_field_t)); // cast to void* to avoid atomic warning
  }
  // and claim leftover blocks if needed (so we never allocate there)
  ptrdiff_t post = (fields * MI_BITMAP_FIELD_BITS) - bcount;
  mi_assert_internal(post >= 0);
  if (post > 0) {
    // don't use leftover bits at the end
    mi_bitmap_index_t postidx = mi_bitmap_index_create(fields - 1, MI_BITMAP_FIELD_BITS - post);
    _mi_bitmap_claim(arena->blocks_inuse, fields, post, postidx, NULL);
  }

  return mi_arena_add(arena, arena_id);

}

// Reserve a range of regular OS memory
int mi_reserve_os_memory_ex(size_t size, bool commit, bool allow_large, bool exclusive, mi_arena_id_t* arena_id) mi_attr_noexcept
{
  if (arena_id != NULL) *arena_id = _mi_arena_id_none();
  size = _mi_align_up(size, MI_ARENA_BLOCK_SIZE); // at least one block
  bool large = allow_large;
  void* start = _mi_os_alloc_aligned(size, MI_SEGMENT_ALIGN, commit, &large, &_mi_stats_main);
  if (start==NULL) return ENOMEM;
  if (!mi_manage_os_memory_ex(start, size, (large || commit), large, true, -1, exclusive, arena_id)) {
    _mi_os_free_ex(start, size, commit, &_mi_stats_main);
    _mi_verbose_message("failed to reserve %zu k memory\n", _mi_divide_up(size,1024));
    return ENOMEM;
  }
  _mi_verbose_message("reserved %zu KiB memory%s\n", _mi_divide_up(size,1024), large ? " (in large os pages)" : "");
  return 0;
}

bool mi_manage_os_memory(void* start, size_t size, bool is_committed, bool is_large, bool is_zero, int numa_node) mi_attr_noexcept {
  return mi_manage_os_memory_ex(start, size, is_committed, is_large, is_zero, numa_node, false, NULL);
}

int mi_reserve_os_memory(size_t size, bool commit, bool allow_large) mi_attr_noexcept {
  return mi_reserve_os_memory_ex(size, commit, allow_large, false, NULL);
}


/* -----------------------------------------------------------
  Reserve a huge page arena.
----------------------------------------------------------- */
// reserve at a specific numa node
int mi_reserve_huge_os_pages_at_ex(size_t pages, int numa_node, size_t timeout_msecs, bool exclusive, mi_arena_id_t* arena_id) mi_attr_noexcept {
  if (arena_id != NULL) *arena_id = -1;
  if (pages==0) return 0;
  if (numa_node < -1) numa_node = -1;
  if (numa_node >= 0) numa_node = numa_node % _mi_os_numa_node_count();
  size_t hsize = 0;
  size_t pages_reserved = 0;
  void* p = _mi_os_alloc_huge_os_pages(pages, numa_node, timeout_msecs, &pages_reserved, &hsize);
  if (p==NULL || pages_reserved==0) {
    _mi_warning_message("failed to reserve %zu GiB huge pages\n", pages);
    return ENOMEM;
  }
  _mi_verbose_message("numa node %i: reserved %zu GiB huge pages (of the %zu GiB requested)\n", numa_node, pages_reserved, pages);

  if (!mi_manage_os_memory_ex(p, hsize, true, true, true, numa_node, exclusive, arena_id)) {
    _mi_os_free_huge_pages(p, hsize, &_mi_stats_main);
    return ENOMEM;
  }
  return 0;
}

int mi_reserve_huge_os_pages_at(size_t pages, int numa_node, size_t timeout_msecs) mi_attr_noexcept {
  return mi_reserve_huge_os_pages_at_ex(pages, numa_node, timeout_msecs, false, NULL);
}

// reserve huge pages evenly among the given number of numa nodes (or use the available ones as detected)
int mi_reserve_huge_os_pages_interleave(size_t pages, size_t numa_nodes, size_t timeout_msecs) mi_attr_noexcept {
  if (pages == 0) return 0;

  // pages per numa node
  size_t numa_count = (numa_nodes > 0 ? numa_nodes : _mi_os_numa_node_count());
  if (numa_count <= 0) numa_count = 1;
  const size_t pages_per = pages / numa_count;
  const size_t pages_mod = pages % numa_count;
  const size_t timeout_per = (timeout_msecs==0 ? 0 : (timeout_msecs / numa_count) + 50);

  // reserve evenly among numa nodes
  for (size_t numa_node = 0; numa_node < numa_count && pages > 0; numa_node++) {
    size_t node_pages = pages_per;  // can be 0
    if (numa_node < pages_mod) node_pages++;
    int err = mi_reserve_huge_os_pages_at(node_pages, (int)numa_node, timeout_per);
    if (err) return err;
    if (pages < node_pages) {
      pages = 0;
    }
    else {
      pages -= node_pages;
    }
  }

  return 0;
}

int mi_reserve_huge_os_pages(size_t pages, double max_secs, size_t* pages_reserved) mi_attr_noexcept {
  MI_UNUSED(max_secs);
  _mi_warning_message("mi_reserve_huge_os_pages is deprecated: use mi_reserve_huge_os_pages_interleave/at instead\n");
  if (pages_reserved != NULL) *pages_reserved = 0;
  int err = mi_reserve_huge_os_pages_interleave(pages, 0, (size_t)(max_secs * 1000.0));
  if (err==0 && pages_reserved!=NULL) *pages_reserved = pages;
  return err;
}
