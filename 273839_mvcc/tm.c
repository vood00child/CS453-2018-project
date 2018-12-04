/**
 * @file   tm.c
 * @author [...]
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
// #include <setjmp.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
#include <xmmintrin.h>
#else
#include <sched.h>
#endif

// Internal headers
#include <tm.h>
#include "tm_h.h"

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

/** Pause for a very short amount of time.
**/
static inline void pause()
{
#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
    _mm_pause();
#else
    sched_yield();
#endif
}

/** Wait and acquire the given lock.
 * @param lock Lock to acquire
 * @return Whether the operation is a success
**/
static bool lock_acquire(struct lock_t *lock)
{
    bool expected = false;
    while (unlikely(!atomic_compare_exchange_weak_explicit(&(lock->locked), &expected, true, memory_order_acquire, memory_order_relaxed)))
    {
        expected = false;
        while (unlikely(atomic_load_explicit(&(lock->locked), memory_order_relaxed)))
            pause();
    }
    return true;
}

/** Release the given lock.
 * @param lock Lock to release
**/
static void lock_release(struct lock_t *lock)
{
    atomic_store_explicit(&(lock->locked), false, memory_order_release);
}

size_t get_start_index(shared_t shared, char const *mem_ptr)
{
    size_t alignment = tm_align(shared);
    char *start = (char *)tm_start(shared);
    size_t start_index = (mem_ptr - start) / alignment;
    return start_index;
}

// -------------------------------------------------------------------------- //

/* =============================================================================
 * MakeList AVPair
 *
 * Allocate the primary list as a large chunk so we can guarantee ascending &
 * adjacent addresses through the list. This improves D$ and DTLB behavior.
 * =============================================================================
 */

static __inline__ AVPair *MakeListAVPair(long sz, Thread *Self)
{
    AVPair *ap = (AVPair *)malloc((sizeof(*ap) * sz) + TL2_CACHE_LINE_SIZE);
    ASSERT(ap);
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
        Tail = e;
    }
    Tail->Next = NULL;

    return List;
}

/* =============================================================================
 * MakeList Object
 *
 * Allocate the primary list as a large chunk so we can guarantee ascending &
 * adjacent addresses through the list. This improves D$ and DTLB behavior.
 * =============================================================================
 */

static __inline__ Object *MakeListObject(long sz)
{
    Object *oj = (Object *)malloc((sizeof(*oj) * sz) + TL2_CACHE_LINE_SIZE);
    ASSERT(oj);
    memset(oj, 0, sizeof(*oj) * sz);
    Object *List = oj;
    Object *Tail = NULL;
    long i;
    for (i = 0; i < sz; i++)
    {
        Object *e = oj++;
        e->Next = oj;
        e->Prev = Tail;
        e->Ordinal = i;
        e->isLocked = false;
        e->version = 0;
        Tail = e;
    }
    Tail->Next = NULL;

    return List;
}

/* =============================================================================
 * FreeList AVPair
 * =============================================================================
 */
void FreeListAVPair(Log *, long) __attribute__((noinline));
/*__INLINE__*/ void FreeListAVPair(Log *k, long sz as(unused))
{
    // /* Free appended overflow entries first */
    // AVPair *e = k->end;
    // if (e != NULL)
    // {
    //     while (e->Ordinal >= sz)
    //     {
    //         AVPair *tmp = e;
    //         e = e->Prev;
    //         free(tmp);
    //     }
    // }

    /* Free continguous beginning */
    free(k->List);
}

/* =============================================================================
 * FreeList Object
 * =============================================================================
 */
void FreeListObject(ListObject *, long) __attribute__((noinline));
/*__INLINE__*/ void FreeListObject(ListObject *k, long sz as(unused))
{
    // /* Free appended overflow entries first */
    // Object *e = k->end;
    // if (e != NULL)
    // {
    //     while (e->Ordinal >= sz)
    //     {
    //         Object *tmp = e;
    //         e = e->Prev;
    //         free(tmp);
    //     }
    // }

    /* Free continguous beginning */
    free(k->List);
}

/* =============================================================================
 * AppendWeakReference
 * =============================================================================
 */
static __inline__ int AppendWeakReference(shared_t shared, size_t ind_oj, char *source)
{
    struct region *region = (struct region *)shared;

    ListObject *lo = &region->weakRef[ind_oj];
    Object *oj = lo->put;
    if (oj == NULL)
    {
        ASSERT(1 == 0);
        // if (!ReadSetCoherentPessimistic(Self))
        // {
        //     return 0;
        // }
        // k->ovf++;
        // e = ExtendList(k->tail);
        // k->end = e;
    }

    lo->tail = oj;
    lo->put = oj->Next;
    memcpy(&oj->Val, source, tm_align(shared));

    return 1;
}

// -------------------------------------------------------------------------- //

/* =============================================================================
 * TxNewThread: Allocate a new thread-local transaction object
 * =============================================================================
 */

Thread *TxNewThread()
{
    Thread *t = (Thread *)malloc(sizeof(Thread));
    ASSERT(t);
    return t;
}

/* =============================================================================
 * TxInitThread: Initialize the transaction object
 * =============================================================================
 */
void TxInitThread(Thread *t)
{
    memset(t, 0, sizeof(*t)); /* Default value for most members */

    t->UniqID = (tx_t)t; /* The id corresponds to the address of the thread */

    t->wrSet.List = MakeListAVPair(TL2_INIT_WRSET_NUM_ENTRY, t);
    t->wrSet.put = t->wrSet.List;

    t->rdSet.List = MakeListAVPair(TL2_INIT_RDSET_NUM_ENTRY, t);
    t->rdSet.put = t->rdSet.List;
}

/* =============================================================================
 * TxReset
 * =============================================================================
 */
static __inline__ void TxReset(Thread *Self)
{
    Self->wrSet.put = Self->wrSet.List;
    Self->wrSet.tail = NULL;

    Self->rdSet.put = Self->rdSet.List;
    Self->rdSet.tail = NULL;

    Self->HoldsLocks = 0;
}

/* =============================================================================
 * TxAbort
 * =============================================================================
 */
void TxAbort(shared_t shared, Thread *Self)
{
    struct region *region = (struct region *)shared;
    if (Self->HoldsLocks)
    {
        printf("Restore locks\n");
        Log *wr = &Self->wrSet;

        AVPair *p;
        AVPair *const End = wr->put;
        for (p = wr->List; p != End; p = p->Next)
        {
            if (p->Held == 0)
            {
                continue;
            }
            p->Held = 0;
            ((region->memory_state)[p->Index]).isLocked = false;
        }
    }
}

/* =============================================================================
 * TrackLoad
 * =============================================================================
 */
static __inline__ int TrackLoad(Thread *Self, size_t index_oj)
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
        ASSERT(64 == 0);
        // if (!ReadSetCoherentPessimistic(Self))
        // {
        //     return 0;
        // }
        // k->ovf++;
        // e = ExtendList(k->tail);
        // k->end = e;
    }

    k->tail = e;
    k->put = e->Next;
    e->Index = index_oj;
    e->Held = 0;
    /* Note that Val and Addr fields are undefined for tracked loads */

    return 1;
}

/* =============================================================================
 * RecordStore
 * =============================================================================
 */
static __inline__ void RecordStore(Log *k, intptr_t *Addr, intptr_t *target, size_t index_oj, size_t alignment)
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
        ASSERT(64 == 12);
        // printf("wr list overflow\n");
        // k->ovf++;
        // e = ExtendList(k->tail);
        // k->end = e;
    }
    ASSERT(Addr != NULL);
    k->tail = e;
    k->put = e->Next;
    e->Addr = target;
    memcpy(&(e->Val), Addr, alignment);
    e->Index = index_oj;
    e->Held = 0;
}

/* =============================================================================
 * TxValidateRead
 * =============================================================================
 */

static __inline__ bool TxValidateRead(Thread *Self, shared_memory_state oj_state)
{
    return !oj_state.isLocked && (oj_state.version <= Self->startTime);
}

/* =============================================================================
 * TxLoad
 * =============================================================================
 */

int TxLoad(shared_t shared, Thread *Self, intptr_t *Addr, intptr_t *target, size_t index_oj)
{
    struct region *region = (struct region *)shared;

    // if (!Self->isRO)
    // {
    /* Tx previously wrote to the location: return value from write-set */
    intptr_t msk = FILTERBITS(Addr);
    if ((Self->wrSet.BloomFilter & msk) == msk)
    {
        printf("Value to be read already in wrSet\n");
        Log *wr = &(Self->wrSet);
        AVPair *e;
        for (e = wr->tail; e != NULL; e = e->Prev)
        {
            ASSERT(e->Addr != NULL);
            if (e->Addr == Addr)
            {
                memcpy(target, &(e->Val), region->align);
                return 0;
            }
        }
    }

    shared_memory_state oj_state = (region->memory_state)[index_oj];
    /* Tx has not been written to the location */
    if (TxValidateRead(Self, oj_state))
    {
        if (!TrackLoad(Self, index_oj))
        {
            TxAbort(shared, Self);
        }
        memcpy(target, Addr, region->align);
        return 0;
    }

    TxAbort(shared, Self);
    return -1;
    // }
}

/* =============================================================================
 * TxStore
 * =============================================================================
 */
int TxStore(shared_t shared, Thread *Self, intptr_t *Addr, intptr_t *target, size_t index_oj)
{
    struct region *region = (struct region *)shared;
    Log *wr = &Self->wrSet;

    intptr_t msk = FILTERBITS(target);
    if ((Self->wrSet.BloomFilter & msk) == msk)
    {
        AVPair *e;
        for (e = wr->tail; e != NULL; e = e->Prev)
        {
            ASSERT(e->Addr != NULL);
            if (e->Addr == target)
            {
                memcpy(&(e->Val), Addr, region->align);
                return 0;
            }
        }
    }

    wr->BloomFilter |= FILTERBITS(target);
    RecordStore(wr, Addr, target, index_oj, region->align);
    return 0;
}

static __inline__ long TxCommit(shared_t shared, Thread *Self)
{
    struct region *region = (struct region *)shared;

    Log *const wr = &Self->wrSet;
    Log *const rd = &Self->rdSet;
    Self->HoldsLocks = 1;
    AVPair *const End_wr = wr->put;
    for (AVPair *p = wr->List; p != End_wr; p = p->Next)
    {
        ASSERT(p->Owner == Self);
        long ind_oj = p->Index;
        if (((region->memory_state)[ind_oj]).isLocked)
        {
            TxAbort(shared, Self);
            return 0;
        }
        ((region->memory_state)[ind_oj]).isLocked = true;
        p->Held = 1;
    }

    AVPair *const End_rd = rd->put;
    for (AVPair *p = rd->List; p != End_rd; p = p->Next)
    {
        long ind_oj = p->Index;
        if (!TxValidateRead(Self, (region->memory_state)[ind_oj]))
        {
            return 0;
        }
    }

    if (unlikely(!lock_acquire(&(region->timeLock))))
    {
        //TODO release locks
        ASSERT(58 == 120);
        return 0;
    }

    region->VClock += 1;

    for (AVPair *p = wr->List; p != End_wr; p = p->Next)
    {
        long ind_oj = p->Index;
        memcpy(&(((char *)(region->start))[ind_oj * region->align]), &p->Val, region->align);
        AppendWeakReference(shared, ind_oj, (char *)&p->Val);
        (region->memory_state[ind_oj]).version = region->VClock;
        (region->memory_state[ind_oj]).isLocked = false;
    }

    lock_release(&(region->timeLock));

    return 1;
}

/* =============================================================================
 * TxFreeThread
 * =============================================================================
 */
void TxFreeThread(Thread *t)
{
    FreeListAVPair(&(t->rdSet), TL2_INIT_RDSET_NUM_ENTRY);
    FreeListAVPair(&(t->wrSet), TL2_INIT_WRSET_NUM_ENTRY);

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
    printf("\n\n---------- tm_create ----------\n\n");
    // TODO: tm_create(size_t, size_t)
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

    size_t nb_objects = size / align;

    memset(region->start, 0, size);
    region->size = size;
    region->align = align;
    region->align_alloc = align_alloc;
    region->VClock = 0;

    shared_memory_state *memory_state = (shared_memory_state *)calloc(nb_objects, sizeof(shared_memory_state));
    if (unlikely(!memory_state))
    {
        free(region);
        return invalid_shared;
    }
    for (size_t i = 0; i < nb_objects; i++)
    {
        memory_state[i].isLocked = false;
        memory_state[i].version = 0;
    }
    region->memory_state = memory_state;

    region->weakRef = (ListObject *)calloc(nb_objects, sizeof(ListObject));
    for (size_t i = 0; i < nb_objects; i++)
    {
        (region->weakRef[i]).List = MakeListObject(TL2_INIT_CURPOINT_NUM_ENTRY);
        (region->weakRef[i]).put = (region->weakRef[i]).List;
        (region->weakRef[i]).tail = NULL;

        AppendWeakReference((shared_t)region, i, &(((char *)(region->start))[i * region->align]));
    }

    return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t shared)
{
    printf("\n\n---------- tm_destroy ----------\n\n");
    // printf("Destroy region\n");
    struct region *region = (struct region *)shared;
    size_t nb_objects = tm_size(shared) / tm_align(shared);
    free(region->start);
    for (size_t i = 0; i < nb_objects; i++)
    {
        FreeListObject(&region->weakRef[i], TL2_INIT_CURPOINT_NUM_ENTRY);
    }
    free(region->weakRef);
    free(region->memory_state);
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
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t shared, bool is_ro)
{
    // printf("\n\n---------- tm_begin ----------\n");
    struct region *region = (struct region *)shared;
    // printf("Create new thread\n");
    Thread *t = TxNewThread(); /* Create a new thread */

    // printf("Initialize thread\n");
    TxInitThread(t); /* Initialize it */
    TxReset(t);

    // printf("Sample global version clock\n");
    if (unlikely(!lock_acquire(&(region->timeLock))))
    {
        //TODO release locks
        ASSERT(2 == 1);
        return 0;
    }
    t->startTime = (uintptr_t)region->VClock;
    // t->startTime = atomic_load(&(((struct region *)shared)->VClock));
    lock_release(&(region->timeLock));

    t->isRO = is_ro;
    // printf("Global version clock: %" PRIxPTR "\n", (uintptr_t)t->rv);

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
    // printf("--- tm_end ---\n");
    // fflush(stdout);
    Thread *t = (Thread *)tx;

    // We have written nothing
    if (t->wrSet.put == t->wrSet.List)
    {
        TxFreeThread(t);
        return true;
    }

    if (TxCommit(shared, t))
    {
        TxFreeThread(t);
        return true;
    }

    // printf("fail to commit\n");
    TxAbort(shared, t);
    TxFreeThread(t);
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
    Thread *t = (Thread *)tx;

    size_t alignment = tm_align(shared);
    ASSERT(size % alignment == 0);

    char *tmp_slot = calloc(size, sizeof(char));

    if (unlikely(!tmp_slot))
    {
        free(tmp_slot);
        TxFreeThread(t);
        return false;
    }

    size_t nb_items = size / alignment; // number of items we want to read
    const char *current_src_slot = source;
    size_t tmp_slot_index = 0;
    int err_load = 0;
    size_t start_ind_source = get_start_index(shared, source);

    for (size_t ind = start_ind_source; ind < start_ind_source + nb_items; ind++)
    {
        err_load = TxLoad(shared, t, (intptr_t *)(current_src_slot), (intptr_t *)(&tmp_slot[tmp_slot_index]), ind);

        if (err_load == -1)
        {
            free(tmp_slot);
            TxFreeThread(t);
            // printf("error in TxLoad\n");
            return false;
        }

        current_src_slot += alignment;
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
    Thread *t = (Thread *)tx;

    ASSERT(!t->isRO);

    size_t alignment = tm_align(shared);
    ASSERT(size % alignment == 0);

    size_t nb_items = size / alignment; // number of items we want to read
    const char *current_src_slot = source;
    char *current_target_slot = target;

    int err_write = 0;
    size_t start_ind_source = get_start_index(shared, source);
    for (size_t ind = start_ind_source; ind < start_ind_source + nb_items; ind++)
    {
        err_write = TxStore(shared, t, (intptr_t *)(current_src_slot), (intptr_t *)(current_target_slot), ind);
        if (err_write == -1)
        {
            // printf("error in TxStore\n");
            TxFreeThread(t);
            return false;
        }

        // You may want to replace char* by uintptr_t
        current_src_slot += alignment;
        current_target_slot += alignment;
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
