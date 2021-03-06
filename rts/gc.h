#ifndef INCLUDED_RTS_GC_H
#define INCLUDED_RTS_GC_H

#include <cassert>
#include <cstdint>
#include <mutex>
#include <queue>
#include <unordered_set>
#include "rts/thread_local.h"
#include <boost/lockfree/queue.hpp>

namespace rts {

using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uintptr_t;
using std::mutex;
using std::unordered_set;
using std::unique_lock;

namespace spaces {
  enum {
    external = 0,   ///< non-heap pointers or 0-extended 32-bit integers and the like
    local_min = 1,  ///< the first usable local heap
    local_max = 7,  ///< the last usable local heap
    global_min = 8, ///< the first usable global heap
    global_max = 15 ///< the last usable global heap
  };
}

namespace triggers {
  enum {
    nmt         = 1, ///< this pointer is not-marked-through for the garbage collector. queue it
    relocation  = 2, ///< the page is locked and is in the middle of a relocation, help it along.
    contraction = 4  ///< this pointer has delusions of uniqueness. disabuse it
  };
}

namespace types {
  enum {
    constructor = 0, ///< a data constructor
    closure     = 1, ///< a closure
    indirection = 2, ///< an indirection to a closure, blackhole or final answer
    blackhole   = 3  ///< a blackhole stuffed into an indirection that we are currently executing.
  };
}

class hec;

struct gc_ptr;

// global garbage collector configuration

extern mutex gc_mutex;

// contents under the gc_mutex
extern uint32_t regions_begin;    // lo <= x < hi
extern uint32_t regions_end;
extern uint64_t * mapped_regions; // 1 bit per region, packed
extern unordered_set<hec*> hecs;
// end contents under the gc_mutex

extern boost::lockfree::queue<gc_ptr> global_mark_queue[8];

static inline bool protected_region(uint32_t r) {
  assert(regions_begin <= r && r < regions_end);
  return mapped_regions[r>>6]&(1<<((r-regions_begin)&0x3f));
}


/// A heap pointer.
///
struct gc_ptr {
  union {
    // layout chosen so that a 0-extended 32 bit integer is a legal 'gc_ptr' as are legal native c pointers
    // TODO: consider setting space 15 to also be a native pointer that way 0 and 1 extended pointers would
    // be legal.
    uint64_t unique   : 1,  ///< does this reference locally believe it is unique?
             type     : 2,  ///< what type of this pointer is this?
             offset   : 9,  ///< offset within a 4k page
             segment  : 9,  ///< which 4k page within a 2mb region
             region   : 19, ///< which 2mb region in the system? 1tb addressable.
             nmt      : 1,  ///< not-marked-through toggle for LVB read-barrier
             space    : 4,  ///< which generation/space are we in?
             tag      : 19; ///< constructor #
    uint64_t addr;
  };

  /// mask for the offset, segment and region
  static const uint64_t mask = 0x7ffffffff8;

  template <typename T> T & operator * () {
    return *reinterpret_cast<T *>(addr&mask);
  };

  template <typename T> T * operator -> () {
    return reinterpret_cast<T *>(addr&mask);
  };

  template <typename T> T & operator [] (std::ptrdiff_t i) {
    return *reinterpret_cast<T *>((addr&mask) + (i * sizeof(T)));
  }

  // TODO: partially template specialize these to make it so gc_ptr loads from those addresses automatically apply the

  /// loaded-value-barrier read-barrier, modified to do contraction of "locally unique" references when the context isn't unique.
  void lvb(uint64_t * address, bool unique_context = true);

  private:
    /// this implements the slow-path of the loaded-value-barrier
    void lvb_slow_path(uint64_t * address, int trigger);
};

inline bool operator==(const gc_ptr& lhs, const gc_ptr& rhs){ return lhs.addr == rhs.addr; }
inline bool operator!=(const gc_ptr& lhs, const gc_ptr& rhs){ return lhs.addr != rhs.addr; }

/// A "Haskell execution context".
class hec {
  public:
    static thread_local hec * current; ///< track the current haskell execution context in a thread_local variable.
    uint16_t expected_nmt; ///< 16 bits, one per space
    std::queue<gc_ptr> local_mark_queue[8]; ///< mark queues for local spaces

  private:
    hec(hec const &);               ///< private copy constructor for RAII
    hec & operator = (hec const &); ///< private assignment operator for RAII

  public:
    hec() {
      current = this;
      unique_lock<mutex> lock(gc_mutex);
      hecs.insert(this);
    }
    ~hec() {
      current = nullptr;
      unique_lock<mutex> lock(gc_mutex);
      hecs.erase(this);
    }

    /// extract an appropriate bit from out expected_nmt mask for what the expected value of the 'not-marked-through'
    /// flag is for a given space.
    inline bool get_expected_nmt(int i) { return expected_nmt & (1 << i); }
};

inline void gc_ptr::lvb(uint64_t * address, bool unique_context) {
  if (space != 0) {
    int trigger = 0;
    if (nmt != hec::current->get_expected_nmt(space)) trigger |= triggers::nmt;
    if (protected_region(region))                     trigger |= triggers::relocation;
    if (!unique_context && unique)                    trigger |= triggers::contraction;
    if (trigger != 0) lvb_slow_path(address, trigger);
  }
}

} // namespace rts

#endif
