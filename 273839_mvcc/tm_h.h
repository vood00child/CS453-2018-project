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
  TL2_INIT_WRSET_NUM_ENTRY = 512,
  TL2_INIT_RDSET_NUM_ENTRY = 4096,
  INIT_NUM_VERSION = 102400,
};

typedef struct lock_t
{
  atomic_bool locked; // Whether the lock is taken
} lock_t;

/* Read-set and write-set log entry */
typedef struct _AVPair
{
  struct _AVPair *Next;
  struct _AVPair *Prev;
  volatile intptr_t *Addr;
  intptr_t Val;
  int Held;
  long Index;
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

typedef struct _Object
{
  struct _Object *Next;
  struct _Object *Prev;
  intptr_t Val;
  bool isLocked;
  uintptr_t version;
  long Ordinal;
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
};

typedef struct _Thread Thread;

typedef struct
{
  bool isLocked;
  uintptr_t version;
  struct _Thread *Owner;
} shared_memory_state;

typedef struct
{
  bool isLocked;
  uintptr_t version;
  struct _Thread *Owner;
  long Index;
} saved_memory_state;

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