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

typedef int BitMap;

enum tl2_config
{
  TL2_INIT_WRSET_NUM_ENTRY = 2048,
  TL2_INIT_RDSET_NUM_ENTRY = 8192,
  INIT_NUM_VERSION = 102400,
};

typedef struct lock_t
{
  atomic_bool locked; /* Whether the lock is taken */
} lock_t;

typedef struct
{
  lock_t lock;
  bool isLocked;
  uintptr_t version;
  struct _Thread *Owner;
} shared_memory_state;

typedef struct
{
  bool isLocked;
  uintptr_t version;
  struct _Thread *Owner;
  intptr_t Index;
} saved_memory_state;

typedef struct _States
{
  struct _States *Next;
  uintptr_t from;
  uintptr_t to;
  shared_memory_state *memory_state; /* State of each items in the shared memory region */
} States;

struct region
{
  // ListObject *weakRef;               /* History version of the items in the shared memory region */
  atomic_uint VClock;     /* Global clock */
  struct lock_t timeLock; /* Global lock */
  void *start;            /* Start of the shared memory region */
  size_t size;            /* Size of the shared memory region (in bytes) */
  size_t align;           /* Claimed alignment of the shared memory region (in bytes) */
  size_t align_alloc;     /* Actual alignment of the memory allocations (in bytes) */
  States *states;
} region;

/* Read-set and write-set log entry */
typedef struct _AVPair
{
  struct _AVPair *Next;
  struct _AVPair *Prev;
  uintptr_t *Addr;
  intptr_t Val;
  int Held;
  shared_memory_state *State; // starts at 0
  long Ordinal;               /* local index of the entry */
} AVPair;

/* Read-set and write-set log */
typedef struct _Log
{
  AVPair *List;
  AVPair *put;  /* Insert position - cursor */
  AVPair *tail; /* Pointer to last valid entry */
  AVPair *end;  /* Pointer to last entry */
  long ovf;     /* Overflow - request to grow */
  BitMap BloomFilter;
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

/* History version of the items in the shared memory region */
typedef struct _ListObject
{
  Object *List;
  Object *put;  /* Insert position - cursor */
  Object *tail; /* Pointer to last valid entry */
  Object *end;  /* Pointer to last entry */
  long ovf;     /* Overflow - request to grow */
} ListObject;

struct _Thread
{
  tx_t UniqID;
  volatile long HoldsLocks;
  volatile uintptr_t startTime;
  bool isRO;
  Log rdSet;
  Log wrSet;
};

typedef struct _Thread Thread;

/*
 * We use a degenerate Bloom filter with only one hash function generating
 * a single bit. A traditional Bloom filter use multiple hash functions and
 * multiple bits.
 */

#define FILTERHASH(a) (((uintptr_t)(a) >> 2) ^ ((uintptr_t)(a) >> 5))
#define FILTERBITS(a) (1 << (FILTERHASH(a) & 0x1F))