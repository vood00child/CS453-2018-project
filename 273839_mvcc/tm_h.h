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
  TL2_INIT_WRSET_NUM_ENTRY = 256,
  TL2_INIT_RDSET_NUM_ENTRY = 256,
  TL2_INIT_NUM_VERSION = 10240,
};

typedef struct lock_t
{
  atomic_bool locked; /* Whether the lock is taken */
} lock_t;

/* Read-set and write-set log entry */
typedef struct _AVPair
{
  struct _AVPair *Next;
  struct _AVPair *Prev;
  volatile uintptr_t *Addr;
  intptr_t Val;
  int Held;
  long Index;   // starts at 0
  long Ordinal; /* local index of the entry */
} AVPair;

/* Read-set and write-set log */
typedef struct _Log
{
  AVPair *List;
  AVPair *put;  /* Insert position - cursor */
  AVPair *tail; /* Pointer to last valid entry */
  AVPair *end;  /* Pointer to last entry */
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
  lock_t lock;
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
  long Index;
} saved_memory_state;

struct region
{
  shared_memory_state *memory_state; /* State of each items in the shared memory region */
  ListObject *weakRef;               /* History version of the items in the shared memory region */
  atomic_uint VClock;                /* Global clock */
  struct lock_t timeLock;            /* Global lock */
  void *start;                       /* Start of the shared memory region */
  size_t size;                       /* Size of the shared memory region (in bytes) */
  size_t align;                      /* Claimed alignment of the shared memory region (in bytes) */
  size_t align_alloc;                /* Actual alignment of the memory allocations (in bytes) */
};

/*
 * We use a degenerate Bloom filter with only one hash function generating
 * a single bit. A traditional Bloom filter use multiple hash functions and
 * multiple bits.
 */

#define FILTERHASH(a) (((uintptr_t)(a) >> 2) ^ ((uintptr_t)(a) >> 5))
#define FILTERBITS(a) (1 << (FILTERHASH(a) & 0x1F))