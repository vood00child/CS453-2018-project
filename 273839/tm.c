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
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

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

#ifndef _GVCONFIGURATION
#define _GVCONFIGURATION 4
#endif

#if _GVCONFIGURATION == 4
#define _GVFLAVOR "GV4"
#define GVGenerateWV GVGenerateWV_GV4
#endif

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
static __inline__ vwLock GVRead(Thread *Self as(unused))
{
    return _GCLOCK;
}

/* =============================================================================
 * GVGenerateWV_GV4
 *
 * The GV4 form of GVGenerateWV() does not have a CAS retry loop. If the CAS
 * fails then we have 2 writers that are racing, trying to bump the global
 * clock. One increment succeeded and one failed. Because the 2 writers hold
 * locks at the time we bump, we know that their write-sets don't intersect. If
 * the write-set of one thread intersects the read-set of the other then we know
 * that one will subsequently fail validation (either because the lock associated
 * with the read-set entry is held by the other thread, or because the other
 * thread already made the update and dropped the lock, installing the new
 * version #). In this particular case it's safe if two threads call
 * GVGenerateWV() concurrently and they both generate the same (duplicate) WV.
 * That is, if we have writers that concurrently try to increment the
 * clock-version and then we allow them both to use the same wv. The failing
 * thread can "borrow" the wv of the successful thread.
 * =============================================================================
 */
static __inline__ vwLock GVGenerateWV_GV4(Thread *Self, vwLock maxv)
{
    vwLock gv = _GCLOCK;
    vwLock wv = gv + 2;
    vwLock k = CAS(&_GCLOCK, gv, wv);
    if (k != gv)
    {
        wv = k;
    }
    assert((wv & LOCKBIT) == 0);
    assert(wv != 0); /* overflow */
    assert(wv > Self->wv);
    assert(wv > Self->rv);
    assert(wv > maxv);
    Self->wv = wv;
    return wv;
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
        assert(p->Addr != NULL);
        assert(p->LockFor != NULL);
        if (p->Held == 0)
        {
            continue;
        }
        assert(OwnerOf(*(p->LockFor)) == Self);
        assert(*(p->LockFor) == ((uintptr_t)(p) | LOCKBIT));
        assert((p->rdv & LOCKBIT) == 0);
        p->Held = 0;
        *(p->LockFor) = p->rdv;
    }

    Self->HoldsLocks = 0;
}

/* =============================================================================
 * DropLocks
 * =============================================================================
 */
static __inline__ void DropLocks(Thread *Self, vwLock wv)
{
    Log *wr = &Self->wrSet;

    AVPair *p;
    AVPair *const End = wr->put;
    for (p = wr->List; p != End; p = p->Next)
    {
        assert(p->Addr != NULL);
        assert(p->LockFor != NULL);
        if (p->Held == 0)
        {
            continue;
        }
        p->Held = 0;
        assert(wv > p->rdv);
        assert(OwnerOf(*(p->LockFor)) == Self);
        assert(*(p->LockFor) == ((uintptr_t)(p) | LOCKBIT));
        *(p->LockFor) = wv;
    }

    Self->HoldsLocks = 0;
}

/* =============================================================================
 * ReadSetCoherent
 *
 * Is the read-set mutually consistent? Can be called at any time--before the
 * caller acquires locks or after.
 * =============================================================================
 */
static __inline__ long ReadSetCoherent(Thread *Self)
{
    intptr_t dx = 0;
    vwLock rv = Self->rv;
    Log *const rd = &Self->rdSet;
    AVPair *const EndOfList = rd->put;
    AVPair *e;

    assert((rv & LOCKBIT) == 0);

    for (e = rd->List; e != EndOfList; e = e->Next)
    {
        assert(e->LockFor != NULL);
        vwLock v = *(e->LockFor);
        if (v & LOCKBIT)
        {
            dx |= (uintptr_t)(((AVPair *)((uintptr_t)(v) & ~LOCKBIT))->Owner) ^ (uintptr_t)(Self);
        }
        else
        {
            dx |= (v > rv);
        }
    }

    return (dx == 0);
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

    assert((rv & LOCKBIT) == 0);

    for (e = rd->List; e != EndOfList; e = e->Next)
    {
        assert(e->LockFor != NULL);
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
 * WriteBackForward
 *
 * Transfer the data in the log its ultimate location.
 * =============================================================================
 */
static __inline__ void WriteBackForward(Log *k)
{
    AVPair *e;
    AVPair *End = k->put;
    for (e = k->List; e != End; e = e->Next)
    {
        *(e->Addr) = e->Val;
    }
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
 * FindFirst
 *
 * Search for first log entry that contains lock.
 * =============================================================================
 */
static __inline__ AVPair *FindFirst(Log *k, volatile vwLock *Lock)
{
    AVPair *e;
    AVPair *const End = k->put;
    for (e = k->List; e != End; e = e->Next)
    {
        if (e->LockFor == Lock)
        {
            return e;
        }
    }
    return NULL;
}

/* =============================================================================
 * backoff
 * =============================================================================
 */
static __inline__ void backoff(Thread *Self, long attempt)
{
    unsigned long long stall = MarsagliaXOR(&Self->rng) & 0xF;
    stall += attempt >> 2;
    stall *= 10;

    /* CCM: timer function may misbehave */
    volatile unsigned long long i = 0;
    while (i++ < stall)
    {
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
    assert(Addr != NULL);
    k->tail = e;
    k->put = e->Next;
    e->Addr = Addr;
    e->Val = Val;
    e->LockFor = Lock;
    e->Held = 0;
    e->rdv = LOCKBIT; /* use either 0 or LOCKBIT */
}

/* =============================================================================
 * TryFastUpdate
 * =============================================================================
 */
static __inline__ long TryFastUpdate(Thread *Self)
{
    Log *const wr = &Self->wrSet;
    Log *const rd = &Self->rdSet;
    long ctr;
    vwLock wv;

    assert(Self->Mode == TTXN);

    /*
     * Consider: if the write-set is long or Self->Retries is high we
     * could run a pre-pass and sort the write-locks by LockFor address.
     * We could either use a separate LockRecord list (sorted) or
     * link the write-set entries via SortedNext
     */

    /*
     * Lock-acquisition phase ...
     *
     * CONSIDER: While iterating over the locks that cover the write-set
     * track the maximum observed version# in maxv.
     * In GV4:   wv = GVComputeWV(); ASSERT wv > maxv
     * In GV5|6: wv = GVComputeWV(); if (maxv >= wv) wv = maxv + 2
     * This is strictly an optimization.
     * maxv isn't required for algorithmic correctness
     */
    Self->HoldsLocks = 1;
    ctr = 1000; /* Spin budget - TUNABLE */
    vwLock maxv = 0;
    AVPair *p;

    AVPair *const End = wr->put;
    for (p = wr->List; p != End; p = p->Next)
    {
        volatile vwLock *const LockFor = p->LockFor;
        vwLock cv;
        assert(p->Addr != NULL);
        assert(p->LockFor != NULL);
        assert(p->Held == 0);
        assert(p->Owner == Self);
        /* Consider prefetching only when Self->Retries == 0 */
        // prefetchw(LockFor);
        cv = *(LockFor);
        if ((cv & LOCKBIT) && ((AVPair *)(cv ^ LOCKBIT))->Owner == Self)
        {
            /* CCM: revalidate read because could be a hash collision */
            if (FindFirst(rd, LockFor) != NULL)
            {
                if (((AVPair *)(cv ^ LOCKBIT))->rdv > Self->rv)
                {
                    Self->abv = cv;
                    return 0;
                }
            }
            /* Already locked by an earlier iteration. */
            continue;
        }

        /* SIGTM does not maintain a read set */
        if (FindFirst(rd, LockFor) != NULL)
        {
            /*
                 * READ-WRITE stripe
                 */
            if ((cv & LOCKBIT) == 0 &&
                cv <= Self->rv &&
                ((uintptr_t)(CAS(LockFor, cv, ((uintptr_t)(p) | (uintptr_t)(LOCKBIT))))) == (uintptr_t)(cv))
            {
                if (cv > maxv)
                {
                    maxv = cv;
                }
                p->rdv = cv;
                p->Held = 1;
                continue;
            }
            /*
                 * The stripe is either locked or the previously observed read-
                 * version changed.  We must abort. Spinning makes little sense.
                 * In theory we could spin if the read-version is the same but
                 * the lock is held in the faint hope that the owner might
                 * abort and revert the lock
                 */
            Self->abv = cv;
            return 0;
        }
        else
        {
            /*
                 * WRITE-ONLY stripe
                 * Note that we already have a fresh copy of *LockFor in cv.
                 * If we find a write-set element locked then we can either
                 * spin or try to find something useful to do, such as :
                 * A. Validate the read-set by calling ReadSetCoherent()
                 *    We can abort earlier if the transaction is doomed.
                 * B. optimistically proceed to the next element in the write-set.
                 *    Skip the current locked element and advance to the
                 *    next write-set element, later retrying the skipped elements
                 */

            long c = ctr;

            for (;;)
            {
                cv = *(LockFor);
                /* CCM: for SIGTM, this IF and its true path need to be "atomic" */
                if ((cv & LOCKBIT) == 0 &&
                    (uintptr_t)(CAS(LockFor, cv, ((uintptr_t)(p) | (uintptr_t)(LOCKBIT)))) == (uintptr_t)(cv))
                {
                    if (cv > maxv)
                    {
                        maxv = cv;
                    }
                    p->rdv = cv; /* save so we can restore or increment */
                    p->Held = 1;
                    break;
                }
                if (--c < 0)
                {
                    /* Will fall through to TxAbort */
                    return 0;
                }
                /*
                     * Consider: while spinning we might validate
                     * the read-set by calling ReadSetCoherent()
                     */
            }
        } /* write-only stripe */
    }     /* foreach (entry in write-set) */

    wv = GVGenerateWV(Self, maxv);

    /*
     * We now hold all the locks for RW and W objects.
     * Next we validate that the values we've fetched from pure READ objects
     * remain coherent.
     *
     * If GVGenerateWV() is implemented as a simplistic atomic fetch-and-add
     * then we can optimize by skipping read-set validation in the common-case.
     * Namely,
     *   if (Self->rv != (wv-2) && !ReadSetCoherent(Self)) { ... abort ... }
     * That is, we could elide read-set validation for pure READ objects if
     * there were no intervening write txns between the fetch of _GCLOCK into
     * Self->rv in TxStart() and the increment of _GCLOCK in GVGenerateWV()
     */

    /*
     * CCM: for SIGTM, the read filter would have triggered an abort already
     * if the read-set was not consistent.
     */
    if (!ReadSetCoherent(Self))
    {
        /*
         * The read-set is inconsistent.
         * The transaction is spoiled as the read-set is stale.
         * The candidate results produced by the txn and held in
         * the write-set are a function of the read-set, and thus invalid
         */
        return 0;
    }

    /*
     * We are now committed - this txn is successful.
     */

    WriteBackForward(wr); /* write-back the deferred stores */

    DropLocks(Self, wv); /* Release locks and increment the version */

    /*
     * Ensure that all the prior STs have drained before starting the next
     * txn.  We want to avoid the scenario where STs from "this" txn
     * languish in the write-buffer and inadvertently satisfy LDs in
     * a subsequent txn via look-aside into the write-buffer
     */

    return 1; /* success */
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

    t->rng = (intptr_t)t + 1;
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

    Self->wrSet.put = Self->wrSet.List;
    Self->wrSet.tail = NULL;

    Self->wrSet.BloomFilter = 0;
    Self->rdSet.put = Self->rdSet.List;
    Self->rdSet.tail = NULL;

    Self->LocalUndo.put = Self->LocalUndo.List;
    Self->LocalUndo.tail = NULL;
    Self->HoldsLocks = 0;
}

/* =============================================================================
 * txCommitReset
 * =============================================================================
 */
static __inline__ void txCommitReset(Thread *Self)
{
    txReset(Self);
    Self->Retries = 0;
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

int TxLoad(Thread *Self, intptr_t *Addr, intptr_t *target, size_t alignment)
{
    assert(Self->Mode == TTXN);

    /* Tx previously wrote to the location: return value from write-set */
    intptr_t msk = FILTERBITS(Addr);
    if ((Self->wrSet.BloomFilter & msk) == msk)
    {
        printf("Value to be read already in wrSet\n");
        Log *wr = &(Self->wrSet);
        AVPair *e;
        for (e = wr->tail; e != NULL; e = e->Prev)
        {
            assert(e->Addr != NULL);
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
        if (!Self->isRO)
        {
            if (!TrackLoad(Self, LockFor))
            {
                printf("TrackLoad failed\n");
                TxAbort(Self);
            }
        }
        memcpy(target, Addr, alignment);
        return 0;
    }
    printf("Value to be read already locked or rv too big\n");
    TxAbort(Self);
    return -1;
}

/* =============================================================================
 * TxStore
 * =============================================================================
 */
int TxStore(Thread *Self, intptr_t *Addr, intptr_t *target, size_t alignment)
{
    volatile vwLock *LockFor;
    vwLock rdv;

    LockFor = PSLOCK(target);
    rdv = *(LockFor);

    intptr_t *valu = calloc(alignment, sizeof(intptr_t));
    memcpy(valu, Addr, alignment);

    Log *wr = &Self->wrSet;
    if (memcmp(target, Addr, alignment) == 0)
    {
        AVPair *e;
        for (e = wr->tail; e != NULL; e = e->Prev)
        {
            assert(e->Addr != NULL);
            if (e->Addr == target)
            {
                assert(LockFor == e->LockFor);
                e->Val = *valu; /* update associated value in write-set */
                return 1;
            }
        }
        /* Not writing new value; convert to load */
        if ((rdv & LOCKBIT) == 0 && rdv <= Self->rv && *(LockFor) == rdv)
        {
            if (!TrackLoad(Self, LockFor))
            {
                TxAbort(Self);
            }
            return 1;
        }
    }

    wr->BloomFilter |= FILTERBITS(target);
    RecordStore(wr, target, *valu, LockFor);
    free(valu);
    return 1;
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
    wrSetOvf += wr->ovf;

    AtomicAdd((volatile intptr_t *)((void *)(&WriteOverflowTally)), wrSetOvf);

    AtomicAdd((volatile intptr_t *)((void *)(&LocalOverflowTally)), t->LocalUndo.ovf);

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
    // printf("\n\n---------- tm_create ----------\n");
    // printf("Create new region, size: %zu, align: %zu\n", size, align);
    struct region *region = (struct region *)malloc(sizeof(struct region));
    if (unlikely(!region))
    {
        // printf("unlikely(!region) returned true...\n");
        return invalid_shared;
    }

    size_t align_alloc = align < sizeof(void *) ? sizeof(void *) : align;
    // printf("align_alloc: %zu\n", align_alloc);
    if (unlikely(posix_memalign(&(region->start), align_alloc, size) != 0))
    {
        // printf("unlikely(posix_memalign(&(region->start), align_alloc, size) != 0) returned true\n");
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
    // printf("\n\n---------- tm_destroy ----------\n");
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
tx_t tm_begin(shared_t shared as(unused), bool is_ro)
{
    // printf("\n\n---------- tm_begin ----------\n");
    // printf("Create new thread\n");
    Thread *t = TxNewThread(); /* Create a new thread */

    // printf("Initialize thread\n");
    TxInitThread(t); /* Initialize it */

    assert(t->Mode == TIDLE || t->Mode == TABORTED);
    txReset(t);

    // printf("Sample global version clock\n");
    t->rv = GVRead(t);
    t->isRO = is_ro;
    // printf("Global version clock: %" PRIxPTR "\n", (uintptr_t)t->rv);
    assert((t->rv & LOCKBIT) == 0);

    t->Mode = TTXN;

    assert(t->LocalUndo.put == t->LocalUndo.List);
    assert(t->wrSet.put == t->wrSet.List);

    return ((tx_t)t);
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t shared as(unused), tx_t tx)
{
    // printf("\n\n---------- tm_end ----------\n");
    Thread *t = (Thread *)tx;

    assert(t->Mode == TTXN);

    if (t->wrSet.put == t->wrSet.List)
    {
        /* Given TL2 the read-set is already known to be coherent. */
        txCommitReset(t);
        // tmalloc_clear(t->allocPtr);
        // tmalloc_releaseAllForward(t->freePtr, &txSterilize);
        TxFreeThread(t);
        return true;
    }

    if (TryFastUpdate(t))
    {
        // printf("TryFastUpdate\n");
        txCommitReset(t);
        // tmalloc_clear(t->allocPtr);
        // tmalloc_releaseAllForward(t->freePtr, &txSterilize);
        TxFreeThread(t);
        return true;
    }

    TxAbort(t);
    return false;
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
    // printf("\n\n---------- tm_read ----------\n");
    Thread *t = (Thread *)tx;

    size_t alignment = tm_align(shared);
    if (size % alignment != 0)
    {
        return false;
    }

    char *tmp_slot = calloc(size, sizeof(char));
    // memset(tmp_slot, 0, size * sizeof(char *));

    if (unlikely(!tmp_slot))
    {
        free(tmp_slot);
        return false;
    }

    size_t number_of_items = size / alignment; // number of items we want to read
    const void *current_src_slot = source;
    size_t tmp_slot_index = 0;
    int err_load = 0;
    for (size_t i = 0; i < number_of_items; i++)
    {
        err_load = TxLoad(t, (intptr_t *)(current_src_slot), (intptr_t *)(&tmp_slot[tmp_slot_index]), alignment);

        if (err_load == -1)
        {
            free(tmp_slot);
            printf("error in TxLoad\n");
            return false;
        }

        // You may want to replace char* by uintptr_t
        current_src_slot = alignment + (char *)current_src_slot;
        tmp_slot_index += alignment;
    }
    memcpy(target, tmp_slot, size);
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
    // printf("\n\n---------- tm_write ----------\n");

    Thread *t = (Thread *)tx;
    assert(t->Mode == TTXN);

    if (t->isRO)
    {
        printf("Is read-only in tm_write ?\n");
        TxAbort(t);
        return true;
    }

    size_t alignment = tm_align(shared);
    if (size % alignment != 0)
    {
        return false;
    }

    size_t number_of_items = size / alignment; // number of items we want to read
    const void *current_src_slot = source;

    int err_write = 0;
    for (size_t i = 0; i < number_of_items; i++)
    {
        err_write = TxStore(t, (intptr_t *)(current_src_slot), (intptr_t *)(target), alignment);
        if (err_write == -1)
        {
            // printf("error in TxStore\n");
            return false;
        }

        // You may want to replace char* by uintptr_t
        current_src_slot = alignment + (char *)current_src_slot;
        target = alignment + (char *)target;
    }

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
