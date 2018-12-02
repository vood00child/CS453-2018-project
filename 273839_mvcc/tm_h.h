/**
 * @file   tm.h
 * @author Mathieu Ducroux
 *
 * @section LICENSE
 *
 * [...]
 *
 * @section DESCRIPTION
 *
 * Implementation of your own transaction manager.
 * You can completely rewrite this file (and create more files) as you wish.
 * Only the interface (i.e. exported symbols and semantic) must be preserved.
**/

#include <stdint.h>
#include <assert.h>
#include <tm.h>

#define ASSERT(x) assert(x)

#ifndef TL2_CACHE_LINE_SIZE
#define TL2_CACHE_LINE_SIZE (64)
#endif

/* CCM: misaligned address (0xFF bytes) to generate bus error / segfault */
#define TL2_USE_AFTER_FREE_MARKER (-1)

typedef int BitMap;

enum tl2_config
{
  TL2_INIT_WRSET_NUM_ENTRY = 1024,
  TL2_INIT_RDSET_NUM_ENTRY = 8192,
  TL2_INIT_CURPOINT_NUM_ENTRY = 102400,
};

struct lock_t
{
  atomic_bool locked; // Whether the lock is taken
};

/* Read-set and write-set log entry */
typedef struct _AVPair
{
  struct _AVPair *Next;
  struct _AVPair *Prev;
  volatile intptr_t *Addr;
  intptr_t Val;
  volatile uintptr_t *LockFor; /* points to the uintptr_t covering Addr */
  uintptr_t rdv;               /* read-version at time of 1st read observed */
  struct _Thread *Owner;
  int Held;
  long Ordinal; /* local index of the entry */
} AVPair;

/* Read-set and write-set log */
typedef struct _Log
{
  AVPair *List;
  AVPair *put;        /* Insert position - cursor */
  AVPair *tail;       /* CCM: Pointer to last valid entry */
  AVPair *end;        /* CCM: Pointer to last entry */
  long ovf;           /* Overflow - request to grow */
  BitMap BloomFilter; /* Address exclusion fast-path test */
} Log;

typdef struct _Object
{
  struct _Object *Next;
  struct _Object *Prev;
  intptr_t Val;
  bool isLocked;
  uintptr_t version;
} Object;

typedef struct _ListObject
{
  Object *List;
  Object *put;  /* Insert position - cursor */
  Object *tail; /* CCM: Pointer to last valid entry */
  Object *end;  /* CCM: Pointer to last entry */
  long ovf;     /* Overflow - request to grow */
} ListObject;

struct _Thread
{
  tx_t UniqID;
  volatile long HoldsLocks;     /* passed start of update */
  volatile uintptr_t startTime; /* read version number */
  bool isRO;
  Log rdSet;
  Log wrSet;
  size_t alignment;
};

typedef struct _Thread Thread;

typedef struct
{
  bool isLocked;
  uintptr_t version;
} shared_memory_state;

struct region
{
  shared_memory_state *memory_state;
  ListObject *weakRef;
  atomic_uint VClock;
  struct lock_t timeLock; // Global lock
  void *start;            // Start of the shared memory region
  size_t size;            // Size of the shared memory region (in bytes)
  size_t align;           // Claimed alignment of the shared memory region (in bytes)
  size_t align_alloc;     // Actual alignment of the memory allocations (in bytes)
};

/*
 * We use a degenerate Bloom filter with only one hash function generating
 * a single bit.  A traditional Bloom filter use multiple hash functions and
 * multiple bits.  Relatedly, the size our filter is small, so it can saturate
 * and become useless with a rather small write-set.
 * A better solution might be small per-thread hash tables keyed by address that
 * point into the write-set.
 * Beware that 0x1F == (MIN(sizeof(int),sizeof(intptr_t))*8)-
 */

#define FILTERHASH(a) (((uintptr_t)(a) >> 2) ^ ((uintptr_t)(a) >> 5))
#define FILTERBITS(a) (1 << (FILTERHASH(a) & 0x1F))