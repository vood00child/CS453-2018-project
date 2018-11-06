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

/* Read set and write-set log entry */
typedef struct _AVPair
{
  struct _AVPair *Next;
  struct _AVPair *Prev;
  volatile intptr_t *Addr;
  intptr_t Valu;
  volatile vwLock *LockFor; /* points to the vwLock covering Addr */
  vwLock rdv;               /* read-version @ time of 1st read - observed */
  struct _Thread *Owner;
  long Ordinal;
} AVPair;

typedef struct _Log
{
  AVPair *List;
  AVPair *put;  /* Insert position - cursor */
  AVPair *tail; /* CCM: Pointer to last valid entry */
  AVPair *end;  /* CCM: Pointer to last entry */
  long ovf;     /* Overflow - request to grow */
} Log;

struct _Thread
{
  long UniqID;
  volatile long Mode;
  volatile long HoldsLocks; /* passed start of update */
  volatile long Retries;
  volatile vwLock rv;
  vwLock wv;
  vwLock abv;
  long Starts;
  long Aborts; /* Tally of # of aborts */
  unsigned long long rng;
  unsigned long long xorrng[1];
  void *memCache;
  tmalloc_t *allocPtr; /* CCM: speculatively allocated */
  tmalloc_t *freePtr;  /* CCM: speculatively free'd */
  Log rdSet;
  Log wrSet;
  Log LocalUndo;
  sigjmp_buf *envPtr;
};

typedef struct _Thread Thread;

typedef enum
{
  TIDLE = 0,     /* Non-transactional */
  TTXN = 1,      /* Transactional mode */
  TABORTING = 3, /* aborting - abort pending */
  TABORTED = 5,  /* defunct - moribund txn */
  TCOMMITTING = 7,
} Modes;

enum tl2_config
{
  TL2_INIT_WRSET_NUM_ENTRY = 1024,
  TL2_INIT_RDSET_NUM_ENTRY = 8192,
  TL2_INIT_LOCAL_NUM_ENTRY = 1024,
};

typedef enum
{
  LOCKBIT = 1,
  NADA
} ManifestContants;