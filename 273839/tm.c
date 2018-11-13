/**
 * @file   tm.c
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

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

// External headers
#include <stdlib.h>
#include <setjmp.h>
#include <stdatomic.h>

// Internal headers
#include "tm_c.h"
#include <tm.h>

#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
#include <xmmintrin.h>
#else
#include <sched.h>
#endif

// -------------------------------------------------------------------------- //

/** Define a proposition as likely true.
 * @param prop Proposition
**/
#undef likely
#ifdef __GNUC__
#define likely(prop) \
    __builtin_expect((prop) ? 1 : 0, 1)
#else
#define likely(prop) \
    (prop)
#endif

/** Define a proposition as likely false.
 * @param prop Proposition
**/
#undef unlikely
#ifdef __GNUC__
#define unlikely(prop) \
    __builtin_expect((prop) ? 1 : 0, 0)
#else
#define unlikely(prop) \
    (prop)
#endif

/** Define one or several attributes.
 * @param type... Attribute names
**/
#undef as
#ifdef __GNUC__
#define as(type...) \
    __attribute__((type))
#else
#define as(type...)
#warning This compiler has no support for GCC attributes
#endif

// -------------------------------------------------------------------------- //

/*
 * We use GClock[32] as the global counter.  It must be the sole occupant
 * of its cache line to avoid false sharing.  Even so, accesses to
 * GCLock will cause significant cache coherence & communication costs
 * as it is multi-read multi-write.
 */
static volatile vwLock GClock[TL2_CACHE_LINE_SIZE];
#define _GCLOCK GClock[32]

/* =============================================================================
 * GVInit: Initialize the global version clock
 * =============================================================================
 */
static __inline__ void GVInit()
{
    _GCLOCK = 0;
}

/* =============================================================================
 * GVRead: Read the global version clock
 * =============================================================================
 */
static __inline__ vwLock GVRead(Thread *Self)
{
    return _GCLOCK;
}

/* =============================================================================
 * OwnerOf: Return the owner of a given lock
 * =============================================================================
 */
static __inline__ Thread *OwnerOf(vwLock v)
{
    return ((v & LOCKBIT) ? (((AVPair *)(v ^ LOCKBIT))->Owner) : NULL);
}

/* =============================================================================
 * MarsagliaXORV
 *
 * Simplistlic low-quality Marsaglia SHIFT-XOR RNG.
 * Bijective except for the trailing mask operation.
 * =============================================================================
 */
static __inline__ unsigned long long MarsagliaXORV(unsigned long long x)
{
    if (x == 0)
    {
        x = 1;
    }
    x ^= x << 6;
    x ^= x >> 21;
    x ^= x << 7;
    return x;
}

/* =============================================================================
 * MarsagliaXOR
 *
 * Simplistlic low-quality Marsaglia SHIFT-XOR RNG.
 * Bijective except for the trailing mask operation.
 * =============================================================================
 */
static __inline__ unsigned long long MarsagliaXOR(unsigned long long *seed)
{
    unsigned long long x = MarsagliaXORV(*seed);
    *seed = x;
    return x;
}

/* =============================================================================
 * AtomicAdd
 * =============================================================================
 */
static __inline__ intptr_t AtomicAdd(volatile intptr_t *addr, intptr_t dx)
{
    intptr_t v;
    for (v = *addr; CAS(addr, v, v + dx) != v; v = *addr)
    {
    }
    return (v + dx);
}

/* =============================================================================
 * RestoreLocks: Unlock every entry of the write-set of the thread (set the lock
 * to the read-version at time of 1st read observed)
 * =============================================================================
 */
static __inline__ void RestoreLocks(Thread *Self)
{
    Log *wr = &Self->wrSet;
    AVPair *p;
    AVPair *const End = wr->put;
    for (p = wr->List; p != End; p = p->Next)
    {
        ASSERT(p->Addr != NULL);
        ASSERT(p->LockFor != NULL);
        if (p->Held == 0)
        {
            continue;
        }
        ASSERT(OwnerOf(*(p->LockFor)) == Self);
        ASSERT(*(p->LockFor) == ((uintptr_t)(p) | LOCKBIT));
        ASSERT((p->rdv & LOCKBIT) == 0);
        p->Held = 0;
        *(p->LockFor) = p->rdv;
    }
    Self->HoldsLocks = 0;
}

/* =============================================================================
 * ReadSetCoherentPessimistic: return 0 as soon as we discover inconsistency.
 * =============================================================================
 */
static __inline__ long ReadSetCoherentPessimistic(Thread *Self)
{
    vwLock rv = Self->rv;
    Log *const rd = &Self->rdSet;
    AVPair *const EndOfList = rd->put;
    AVPair *e;

    ASSERT((rv & LOCKBIT) == 0);

    for (e = rd->List; e != EndOfList; e = e->Next)
    {
        ASSERT(e->LockFor != NULL);
        vwLock v = *(e->LockFor);
        if (v & LOCKBIT)
        {
            if ((uintptr_t)(((AVPair *)((uintptr_t)(v) ^ LOCKBIT))->Owner) != (uintptr_t)(Self))
            {
                return 0;
            }
        }
        else
        {
            if (v > rv)
            {
                return 0;
            }
        }
    }

    return 1;
}

/* =============================================================================
 * MakeList
 *
 * Allocate the primary list as a large chunk so we can guarantee ascending &
 * adjacent addresses through the list. This improves D$ and DTLB behavior.
 * =============================================================================
 */
static __inline__ AVPair *MakeList(long sz, Thread *Self)
{
    AVPair *ap = (AVPair *)malloc((sizeof(*ap) * sz) + TL2_CACHE_LINE_SIZE);
    assert(ap);
    memset(ap, 0, sizeof(*ap) * sz);
    AVPair *List = ap;
    AVPair *Tail = NULL;
    long i;
    for (i = 0; i < sz; i++)
    {
        AVPair *e = ap++;
        e->Next = ap;
        e->Prev = Tail;
        e->Owner = Self;
        e->Ordinal = i;
        Tail = e;
    }
    Tail->Next = NULL;

    return List;
}

/* =============================================================================
 * ExtendList
 *
 * Postpend at the tail. We want the front of the list, which sees the most
 * traffic, to remains contiguous.
 * =============================================================================
 */
static __inline__ AVPair *ExtendList(AVPair *tail)
{
    AVPair *e = (AVPair *)malloc(sizeof(*e));
    assert(e);
    memset(e, 0, sizeof(*e));
    tail->Next = e;
    e->Prev = tail;
    e->Next = NULL;
    e->Owner = tail->Owner;
    e->Ordinal = tail->Ordinal + 1;
    /*e->Held    = 0; -- done by memset*/
    return e;
}

/* =============================================================================
 * FreeList
 * =============================================================================
 */
void FreeList(Log *, long) __attribute__((noinline));
/*__INLINE__*/ void FreeList(Log *k, long sz)
{
    /* Free appended overflow entries first */
    AVPair *e = k->end;
    if (e != NULL)
    {
        while (e->Ordinal >= sz)
        {
            AVPair *tmp = e;
            e = e->Prev;
            free(tmp);
        }
    }

    /* Free continguous beginning */
    free(k->List);
}

/* =============================================================================
 * TrackLoad
 * =============================================================================
 */
static __inline__ int TrackLoad(Thread *Self, volatile vwLock *LockFor)
{
    Log *k = &(Self->rdSet);

    /*
     * Consider collapsing back-to-back track loads ...
     * if the previous LockFor and rdv match the incoming arguments then
     * simply return
     */

    /*
     * Read log overflow suggests a rogue or incoherent transaction.
     * Consider calling SpeculativeReadSetCoherent() and, if needed, TxAbort().
     * This lets us distinguish between a doomed txn that's gone rogue
     * and a large transaction that legitimately overflows the buffer.
     * In the latter case we might extend the buffer or chain an overflow
     * buffer onto "k".
     * Options: print, abort, panic, extend, ignore & discard
     * Beware of inlining effects - TrackLoad() is performance-critical.
     * Decreasing the sample period tunable in TxValid() will reduce the
     * rate of overflows caused by zombie transactions.
     */

    AVPair *e = k->put;
    if (e == NULL)
    {
        if (!ReadSetCoherentPessimistic(Self))
        {
            return 0;
        }
        k->ovf++;
        e = ExtendList(k->tail);
        k->end = e;
    }

    k->tail = e;
    k->put = e->Next;
    e->LockFor = LockFor;
    /* Note that Val and Addr fields are undefined for tracked loads */

    return 1;
}

/* =============================================================================
 * WriteBackReverse
 *
 * Transfer the data in the log its ultimate location.
 * =============================================================================
 */
static __inline__ void WriteBackReverse(Log *k)
{
    AVPair *e;
    for (e = k->tail; e != NULL; e = e->Prev)
    {
        *(e->Addr) = e->Val;
    }
}

/* =============================================================================
 * backoff
 * =============================================================================
 */
static __inline__ void backoff(Thread *Self, long attempt)
{
    unsigned long long stall = 1;
    MarsagliaXOR(&Self->rng) & 0xF;
    stall += attempt >> 2;
    stall *= 10;

    /* CCM: timer function may misbehave */
    volatile typeof(stall) i = 0;
    while (i++ < stall)
    {
        PAUSE();
    }
}

/* =============================================================================
 * RecordStore
 * =============================================================================
 */

static __inline__ void RecordStore(Log *k, volatile intptr_t *Addr, intptr_t Val, volatile vwLock *Lock)
{
    /*
     * As an optimization we could squash multiple stores to the same location.
     * Maintain FIFO order to avoid WAW hazards.
     * TODO-FIXME - CONSIDER
     * Keep Self->LockSet as a sorted linked list of unique LockFor addresses.
     * We'd scan the LockSet for Lock.  If not found we'd insert a new
     * LockRecord at the appropriate location in the list.
     * Call InsertIfAbsent (Self, LockFor)
     */
    AVPair *e = k->put;
    if (e == NULL)
    {
        k->ovf++;
        e = ExtendList(k->tail);
        k->end = e;
    }
    ASSERT(Addr != NULL);
    k->tail = e;
    k->put = e->Next;
    e->Addr = Addr;
    e->Val = Val;
    e->LockFor = Lock;
    e->Held = 0;
    e->rdv = LOCKBIT; /* use either 0 or LOCKBIT */
}

/* =============================================================================
 * TxNewThread: Allocate a new thread-local transaction object
 * =============================================================================
 */

Thread *TxNewThread()
{
    Thread *t = (Thread *)malloc(sizeof(Thread));
    assert(t);
    return t;
}

/* =============================================================================
 * TxInitThread: Initialize the transaction object
 * =============================================================================
 */
void TxInitThread(Thread *t)
{
    /* CCM: so we can access TL2's thread metadata in signal handlers */
    // pthread_setspecific(global_key_self, (void *)t);

    memset(t, 0, sizeof(*t)); /* Default value for most members */

    // t->rng = id + 1;
    // t->xorrng[0] = t->rng;
    t->UniqID = (tx_t)t; /* The id corresponds to the address of the thread */

    t->wrSet.List = MakeList(TL2_INIT_WRSET_NUM_ENTRY, t);
    t->wrSet.put = t->wrSet.List;

    t->rdSet.List = MakeList(TL2_INIT_RDSET_NUM_ENTRY, t);
    t->rdSet.put = t->rdSet.List;

    t->LocalUndo.List = MakeList(TL2_INIT_LOCAL_NUM_ENTRY, t);
    t->LocalUndo.put = t->LocalUndo.List;

    // t->allocPtr = tmalloc_alloc(1);
    // assert(t->allocPtr);
    // t->freePtr = tmalloc_alloc(1);
    // assert(t->freePtr);
}

/* =============================================================================
 * txReset
 * =============================================================================
 */
static __inline__ void txReset(Thread *Self)
{
    Self->Mode = TIDLE;

    Self->rdSet.put = Self->rdSet.List;
    Self->rdSet.tail = NULL;

    Self->LocalUndo.put = Self->LocalUndo.List;
    Self->LocalUndo.tail = NULL;
    Self->HoldsLocks = 0;
}

/* =============================================================================
 * txSterilize
 *
 * Use txSterilize() any time an object passes out of the transactional domain
 * and will be accessed solely with normal non-transactional load and store
 * operations.
 * =============================================================================
 */
static void txSterilize(void *Base, size_t Length)
{
    intptr_t *Addr = (intptr_t *)Base;
    intptr_t *End = Addr + Length;
    ASSERT(Addr <= End);
    while (Addr < End)
    {
        volatile vwLock *Lock = PSLOCK(Addr);
        intptr_t val = *Lock;
        /* CCM: invalidate future readers */
        CAS(Lock, val, (_GCLOCK & ~LOCKBIT));
        Addr++;
    }
    memset(Base, (unsigned char)TL2_USE_AFTER_FREE_MARKER, Length);
}

/* =============================================================================
 * TxAbort
 *
 * Our mechanism admits mutual abort with no progress - livelock.
 * Consider the following scenario where T1 and T2 execute concurrently:
 * Thread T1:  WriteLock A; Read B LockWord; detect locked, abort, retry
 * Thread T2:  WriteLock B; Read A LockWord; detect locked, abort, retry
 *
 * Possible solutions:
 *
 * - Try backoff (random and/or exponential), with some mixture
 *   of yield or spinning.
 *
 * - Use a form of deadlock detection and arbitration.
 *
 * In practice it's likely that a semi-random semi-exponential back-off
 * would be best.
 * =============================================================================
 */
void TxAbort(Thread *Self)
{
    Self->Mode = TABORTED;
    if (Self->HoldsLocks)
    {
        RestoreLocks(Self);
    }

    /* Clean up after an abort. Restore any modified locals */
    if (Self->LocalUndo.put != Self->LocalUndo.List)
    {
        WriteBackReverse(&Self->LocalUndo);
    }

    Self->Retries++;
    Self->Aborts++;
    if (Self->Retries > 3) /* TUNABLE */
    {
        backoff(Self, Self->Retries);
    }
}

/* =============================================================================
 * TxLoad
 * =============================================================================
 */

int TxLoad(Thread *Self, volatile intptr_t *Addr, volatile intptr_t *target, size_t alignment)
{
    ASSERT(Self->Mode == TTXN);

    /* Tx previously wrote to the location: return value from write-set */
    intptr_t msk = FILTERBITS(Addr);
    if ((Self->wrSet.BloomFilter & msk) == msk)
    {
        Log *wr = &(Self->wrSet);
        AVPair *e;
        for (e = wr->tail; e != NULL; e = e->Prev)
        {
            ASSERT(e->Addr != NULL);
            if (e->Addr == Addr)
            {
                memcpy(target, &(e->Val), alignment);
                return 0;
            }
        }
    }

    /* Tx has not been written to the location: add to read-set and read from memory */
    volatile vwLock *LockFor = PSLOCK(Addr);
    vwLock rdv = *(LockFor) & ~LOCKBIT;
    /* We need to check that the location is not locked by another committing transaction
    and that its version is less than or equal to our transaction’s read version */
    if (rdv <= Self->rv && *(LockFor) == rdv)
    {
        if (!TrackLoad(Self, LockFor))
        {
            TxAbort(Self);
        }
        memcpy(target, Addr, alignment);
        return 0;
    }
    TxAbort(Self);
    return -1;
}

/* =============================================================================
 * TxFreeThread
 * =============================================================================
 */
void TxFreeThread(Thread *t)
{
    AtomicAdd((volatile intptr_t *)((void *)(&ReadOverflowTally)), t->rdSet.ovf);

    long wrSetOvf = 0;
    Log *wr;
    wr = &t->wrSet;
    {
        wrSetOvf += wr->ovf;
    }
    AtomicAdd((volatile intptr_t *)((void *)(&WriteOverflowTally)), wrSetOvf);

    AtomicAdd((volatile intptr_t *)((void *)(&LocalOverflowTally)), t->LocalUndo.ovf);

    AtomicAdd((volatile intptr_t *)((void *)(&StartTally)), t->Starts);
    AtomicAdd((volatile intptr_t *)((void *)(&AbortTally)), t->Aborts);

    // tmalloc_free(t->allocPtr);
    // tmalloc_free(t->freePtr);

    FreeList(&(t->rdSet), TL2_INIT_RDSET_NUM_ENTRY);
    FreeList(&(t->wrSet), TL2_INIT_WRSET_NUM_ENTRY);
    FreeList(&(t->LocalUndo), TL2_INIT_LOCAL_NUM_ENTRY);

    free(t);
}

// -------------------------------------------------------------------------- //

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
**/
shared_t tm_create(size_t size, size_t align)
{
    struct region *region = (struct region *)malloc(sizeof(struct region));
    if (unlikely(!region))
    {
        return invalid_shared;
    }
    size_t align_alloc = align < sizeof(void *) ? sizeof(void *) : align;
    if (unlikely(posix_memalign(&(region->start), align_alloc, size) != 0))
    {
        free(region);
        return invalid_shared;
    }
    memset(region->start, 0, size);
    region->size = size;
    region->align = align;
    region->align_alloc = align_alloc;
    return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t shared)
{
    struct region *region = (struct region *)shared;
    free(region->start);
    free(region);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void *tm_start(shared_t shared)
{
    return ((struct region *)shared)->start;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t shared)
{
    return ((struct region *)shared)->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t shared)
{
    return ((struct region *)shared)->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t shared, bool is_ro as(unused))
{
    Thread *t = TxNewThread(); /* Create a new thread */
    TxInitThread(t);           /* Initialize it */

    ASSERT(t->Mode == TIDLE || t->Mode == TABORTED);
    txReset(t);

    t->rv = GVRead(t);
    ASSERT((t->rv & LOCKBIT) == 0);

    t->Mode = TTXN;

    ASSERT(t->LocalUndo.put == t->LocalUndo.List);
    ASSERT(t->wrSet.put == t->wrSet.List);

    return ((tx_t)t);
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t shared, tx_t tx)
{
    Thread *t = (Thread *)tx;

    ASSERT(t->Mode == TTXN);

    if (t->wrSet.put == t->wrSet.List)
    {
        /* Given TL2 the read-set is already known to be coherent. */
        txCommitReset(t);
        // tmalloc_clear(t->allocPtr);
        tmalloc_releaseAllForward(t->freePtr, &txSterilize);
        return 1;
    }

    if (TryFastUpdate(t))
    {
        txCommitReset(t);
        // tmalloc_clear(t->allocPtr);
        tmalloc_releaseAllForward(t->freePtr, &txSterilize);
        return 1;
    }

    TxAbort(t);
    return 0;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
**/
bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    Thread *t = (Thread *)t;

    size_t alignment = tm_align(shared);
    if (size % alignment != 0)
    {
        return false;
    }

    void *tmp_slot = (void *)calloc(sizeof(char *), size);
    if (unlikely(!tmp_slot))
    {
        return false;
    }

    size_t number_of_items = size / alignment; // number of items we want to read
    void *current_src_slot = source;
    size_t tmp_slot_index = 0;
    int err_load = 0;
    for (size_t i = 0; i < number_of_items; i++)
    {
        err_load = TxLoad(t, (intptr_t *)(current_src_slot), &tmp_slot[tmp_slot_index], alignment);

        if (err_load == -1)
        {
            free(tmp_slot);
            return false;
        }

        // You may want to replace char* by uintptr_t
        current_src_slot = alignment + (char *)current_src_slot;
        tmp_slot_index += alignment;
    }
    memcopy(target, tmp_slot, size);
    free(tmp_slot);
    return true;
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
**/
bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    volatile vwLock *LockFor;
    vwLock rdv;

    ASSERT(tx->Mode == TTXN);

    LockFor = PSLOCK(addr);
    rdv = *(LockFor);

    Log *wr = &tx->wrSet;

    if (memcmp(target, source, size) == 0)
    {
        AVPair *e;
        for (e = wr->tail; e != NULL; e = e->Prev)
        {
            ASSERT(e->Addr != NULL);
            if (e->Addr == target)
            {
                ASSERT(LockFor == e->LockFor);
                memcopy(e->Val, source, size); /* update associated value in write-set */
                return true;
            }
        }
        /* Not writing new value; convert to load */
        if ((rdv & LOCKBIT) == 0 && rdv <= tx->rv && *(LockFor) == rdv)
        {
            if (!TrackLoad(tx, LockFor))
            {
                TxAbort(tx);
            }
            return true;
        }
    }

    wr->BloomFilter |= FILTERBITS(addr);
    RecordStore(wr, addr, valu, LockFor);
    return true;
}

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
**/
alloc_t tm_alloc(shared_t shared as(unused), tx_t tx as(unused), size_t size as(unused), void **target as(unused))
{
    // TODO: tm_alloc(shared_t, tx_t, size_t, void**)
    return abort_alloc;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
**/
bool tm_free(shared_t shared as(unused), tx_t tx as(unused), void *target as(unused))
{
    // TODO: tm_free(shared_t, tx_t, void*)
    return false;
}
