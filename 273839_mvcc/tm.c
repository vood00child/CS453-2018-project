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

/** Get the index in the shared memory region of the item pointed by mem_ptr
 * @param shared Shared memory region associated with the transaction
 * @param mem_ptr Memory location
**/
size_t get_index(shared_t shared, char const *mem_ptr)
{
    size_t alignment = tm_align(shared);
    char *start = (char *)tm_start(shared);
    size_t start_index = ((uintptr_t)mem_ptr - (uintptr_t)start) / alignment;
    return start_index;
}

/** Allocate an AVPair list as a large chunk
 * @param sz Size of the list we initialize
**/
static __inline__ AVPair *MakeListAVPair(long sz)
{
    AVPair *ap = (AVPair *)calloc(sz, sizeof(AVPair));
    ASSERT(ap);
    AVPair *List = ap;
    AVPair *Tail = NULL;
    for (long i = 0; i < sz; i++)
    {
        AVPair *e = ap++;
        e->Next = ap;
        e->Prev = Tail;
        e->Ordinal = i;
        Tail = e;
    }
    Tail->Next = NULL;

    return List;
}

/** Allocate an Object list as a large chunk
 * @param sz Size of the list we initialize
**/
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

static __inline__ AVPair *ExtendListAVPair(AVPair *tail)
{
    printf("Extend\n");
    AVPair *e = (AVPair *)malloc(sizeof(*e));
    ASSERT(e);
    memset(e, 0, sizeof(*e));
    tail->Next = e;
    e->Prev = tail;
    e->Next = NULL;
    e->Ordinal = tail->Ordinal + 1;
    /*e->Held    = 0; -- done by memset*/
    return e;
}

static __inline__ Object *ExtendListObject(Object *tail)
{
    Object *e = (Object *)malloc(sizeof(*e));
    ASSERT(e);
    memset(e, 0, sizeof(*e));
    tail->Next = e;
    e->Prev = tail;
    e->Next = NULL;
    e->Ordinal = tail->Ordinal + 1;
    /*e->Held    = 0; -- done by memset*/
    return e;
}

/** Free an AVPair list
 * @param sz Size of the list we initialize at the beginning
**/
void FreeListAVPair(Log *k, long sz)
{
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

    free(k->List);
}

/** Free an Object list
 * @param sz Size of the list we initialize at the beginning
**/
void FreeListObject(ListObject *k, long sz)
{
    Object *e = k->end;
    if (e != NULL)
    {
        while (e->Ordinal >= sz)
        {
            Object *tmp = e;
            e = e->Prev;
            free(tmp);
        }
    }

    free(k->List);
}

/** Append a weak reference into the history of a shared item
 * @param shared Shared memory region associated with the transaction
 * @param ind_oj The index of the object we want to record
 * @param source Source address pointing to the value we want to record
 * @param version The version associated with the object
**/
static __inline__ int AppendWeakReference(shared_t shared, size_t ind_oj, char *source, uintptr_t version)
{
    // struct region *region = (struct region *)shared;

    // ListObject *lo = &region->weakRef[ind_oj];
    // Object *oj = lo->put;
    // if (oj == NULL)
    // {
    //     ASSERT(1 == 0);
    //     // lo->ovf++;
    //     // oj = ExtendList(lo->tail);
    //     // lo->end = oj;
    // }

    // lo->tail = oj;
    // lo->put = oj->Next;
    // memcpy(&oj->Val, source, tm_align(shared));
    // oj->version = version;
    // return 1;
}

/** Allocate a new thread-local transaction object
**/
Thread *TxNewThread()
{
    Thread *t = (Thread *)malloc(sizeof(Thread));
    ASSERT(t);
    return t;
}

/** Initialize the transaction object
 * @param t The transaction object we initialize
**/
void TxInitThread(Thread *t)
{
    memset(t, 0, sizeof(*t));

    t->UniqID = (tx_t)t;

    t->wrSet.List = MakeListAVPair(TL2_INIT_WRSET_NUM_ENTRY);
    t->wrSet.put = t->wrSet.List;
    t->wrSet.tail = NULL;

    t->rdSet.List = MakeListAVPair(TL2_INIT_RDSET_NUM_ENTRY);
    t->rdSet.put = t->rdSet.List;
    t->rdSet.tail = NULL;

    t->HoldsLocks = 0;
}

/** Release the locks held by the transaction
 * @param shared Shared memory region associated with the transaction
 * @param self The transaction object
**/
void TxAbort(shared_t shared, Thread *self)
{
    struct region *region = (struct region *)shared;
    if (self->HoldsLocks)
    {
        Log *wr = &self->wrSet;

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
            ((region->memory_state)[p->Index]).Owner = NULL;
            lock_release(&(((region->memory_state)[p->Index]).lock));
        }
    }
    self->HoldsLocks = 0;
}

/** Record the load in the read set
 * @param self The transaction object
 * @param index_oj The index of the object we want to record
**/
static __inline__ int RecordLoad(Thread *self, size_t index_oj)
{
    Log *k = &(self->rdSet);

    AVPair *e = k->put;
    if (e == NULL)
    {
        // ASSERT(0 == 1); // (means rd list overflows)
        e = ExtendListAVPair(k->tail);
        k->end = e;
    }

    k->tail = e;
    k->put = e->Next;
    e->Index = index_oj;
    e->Held = 0;

    return 1;
}

/** Record the store in the write set
 * @param self The transaction object
 * @param source Source start address (in a private region)
 * @param target Target start address (in the shared region)
 * @param index_oj The index of the object we want to record
 * @param alignment The number of bytes we want to copy
**/
static __inline__ void RecordStore(Thread *self, uintptr_t *source, uintptr_t *target, size_t index_oj, size_t alignment)
{
    Log *k = &(self->wrSet);

    AVPair *e = k->put;
    if (e == NULL)
    {
        // ASSERT(0 == 1); // (means wr list overflows)
        e = ExtendListAVPair(k->tail);
        k->end = e;
    }
    k->tail = e;
    k->put = e->Next;
    e->Addr = target;
    memcpy(&(e->Val), source, alignment);
    e->Index = index_oj;
    e->Held = 0;
}

/** Check if an item in shared memory can be read
 * @param self The transaction object
 * @param oj_state The state of the object
**/
static __inline__ bool TxValidateRead(Thread *self, shared_memory_state oj_state)
{
    return (!oj_state.isLocked || oj_state.Owner == self) && (oj_state.version <= self->startTime);
}

/** Load a single item in a private region
 * @param shared Shared memory region associated with the transaction
 * @param self The transaction object
 * @param source Source start address (in the shared region)
 * @param target Target start address (in a private region)
 * @param index_oj The index of the object we want to record
 * @param start_ind_source The index of the first item we want the read in tm_read
 * @param saved_state The state of the items just before reading them
**/
int TxLoad(shared_t shared, Thread *self, uintptr_t *source, uintptr_t *target, size_t index_oj, size_t start_ind_source, saved_memory_state *saved_state)
{
    struct region *region = (struct region *)shared;

    /* Tx previously wrote to the location: return value from write-set */
    intptr_t msk = FILTERBITS(source);
    if ((self->wrSet.BloomFilter & msk) == msk)
    {
        Log *wr = &(self->wrSet);
        AVPair *e;
        for (e = wr->tail; e != NULL; e = e->Prev)
        {
            ASSERT(e->Addr != NULL);
            if (e->Addr == source)
            {
                memcpy(target, &(e->Val), region->align);
                saved_state[index_oj - start_ind_source].Index = -1;
                return 0;
            }
        }
    }

    shared_memory_state oj_state = (region->memory_state)[index_oj];
    saved_state[index_oj - start_ind_source].version = oj_state.version;
    saved_state[index_oj - start_ind_source].isLocked = oj_state.isLocked;
    saved_state[index_oj - start_ind_source].Owner = oj_state.Owner;
    saved_state[index_oj - start_ind_source].Index = index_oj;

    /* Tx has not been written to the location */
    if (TxValidateRead(self, oj_state))
    {
        if (!RecordLoad(self, index_oj))
        {
            TxAbort(shared, self);
        }
        memcpy(target, source, region->align);
        return 0;
    }
    TxAbort(shared, self);
    return -1;
}

/** Store a single item in the write set
 * @param shared Shared memory region associated with the transaction
 * @param self The transaction object
 * @param source Source start address (in a private region)
 * @param target Target start address (in the shared region)
 * @param index_oj The index of the object we want to record
**/
int TxStore(shared_t shared, Thread *self, uintptr_t *source, uintptr_t *target, size_t index_oj)
{
    struct region *region = (struct region *)shared;
    Log *wr = &self->wrSet;

    intptr_t msk = FILTERBITS(target);
    if ((self->wrSet.BloomFilter & msk) == msk)
    {
        AVPair *e;
        for (e = wr->tail; e != NULL; e = e->Prev)
        {
            ASSERT(e->Addr != NULL);
            if (e->Addr == target)
            {
                memcpy(&(e->Val), source, region->align);
                return 0;
            }
        }
    }

    wr->BloomFilter |= FILTERBITS(target);
    RecordStore(self, source, target, index_oj, region->align);
    return 0;
}

/** Commit the changes made locally by a given transaction
 * @param shared Shared memory region associated with the transaction
 * @param self The transaction object
**/
static __inline__ long TxCommit(shared_t shared, Thread *self)
{
    struct region *region = (struct region *)shared;

    Log *const wr = &self->wrSet;
    Log *const rd = &self->rdSet;
    self->HoldsLocks = 1;
    AVPair *const End_wr = wr->put;

    for (AVPair *p = wr->List; p != End_wr; p = p->Next)
    {
        long ind_oj = p->Index;
        if (!lock_acquire(&((region->memory_state)[ind_oj]).lock))
        {
            return 0;
        }
        ((region->memory_state)[ind_oj]).isLocked = true;
        ((region->memory_state)[ind_oj]).Owner = self;
        p->Held = 1;
    }

    AVPair *const End_rd = rd->put;
    for (AVPair *p = rd->List; p != End_rd; p = p->Next)
    {
        long ind_oj = p->Index;
        if (!TxValidateRead(self, (region->memory_state)[ind_oj]))
        {

            return 0;
        }
    }

    if (unlikely(!lock_acquire(&(region->timeLock))))
    {
        ASSERT(0 == 1); // should not happen
        return 0;
    }

    region->VClock += 1;

    for (AVPair *p = wr->List; p != End_wr; p = p->Next)
    {
        long ind_oj = p->Index;
        memcpy(&(((char *)(region->start))[ind_oj * region->align]), &p->Val, region->align);
        // AppendWeakReference(shared, ind_oj, (char *)&p->Val, region->VClock);
        (region->memory_state[ind_oj]).version = region->VClock;
        (region->memory_state[ind_oj]).isLocked = false;
        ((region->memory_state)[ind_oj]).Owner = NULL;
        lock_release(&(((region->memory_state)[ind_oj]).lock));
    }

    lock_release(&(region->timeLock));

    return 1;
}

/** Free the transaction object
 * @param t the transaction object we want to free
**/
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

    size_t nb_objects = size / align;

    memset(region->start, 0, size);
    region->size = size;
    region->align = align;
    region->align_alloc = align_alloc;
    region->VClock = 0;
    region->timeLock.locked = false;

    shared_memory_state *memory_state = (shared_memory_state *)calloc(nb_objects, sizeof(shared_memory_state));
    if (unlikely(!memory_state))
    {
        free(region);
        return invalid_shared;
    }
    for (size_t i = 0; i < nb_objects; i++)
    {
        memory_state[i].lock.locked = false;
        memory_state[i].isLocked = false;
        memory_state[i].version = 0;
        memory_state[i].Owner = NULL;
    }
    region->memory_state = memory_state;

    // region->weakRef = (ListObject *)calloc(nb_objects, sizeof(ListObject));
    // for (size_t i = 0; i < nb_objects; i++)
    // {
    //     (region->weakRef[i]).List = MakeListObject(INIT_NUM_VERSION);
    //     (region->weakRef[i]).put = (region->weakRef[i]).List;
    //     (region->weakRef[i]).tail = NULL;

    //     AppendWeakReference((shared_t)region, i, &(((char *)(region->start))[i * region->align]), 0);
    // }

    return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t shared)
{
    struct region *region = (struct region *)shared;
    free(region->start);
    // size_t nb_objects = tm_size(shared) / tm_align(shared);
    // for (size_t i = 0; i < nb_objects; i++)
    // {
    //     FreeListObject(&region->weakRef[i], INIT_NUM_VERSION);
    // }
    // free(region->weakRef);
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
    struct region *region = (struct region *)shared;
    Thread *t = TxNewThread();

    TxInitThread(t);

    if (unlikely(!lock_acquire(&(region->timeLock))))
    {
        ASSERT(0 == 1); // should not happen
        return invalid_tx;
    }
    t->startTime = (uintptr_t)region->VClock;
    lock_release(&(region->timeLock));

    t->isRO = is_ro;

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

    /* We have written nothing */
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
    // printf("commit fails\n");

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
    struct region *region = (struct region *)shared;

    size_t alignment = tm_align(shared);
    ASSERT(size % alignment == 0);

    char *tmp_slot = (char *)calloc(size, sizeof(char));

    if (unlikely(!tmp_slot))
    {
        free(tmp_slot);
        TxFreeThread(t);
        return false;
    }

    size_t nb_items = size / alignment;
    const char *current_src_slot = source;
    size_t tmp_slot_index = 0;
    int err_load = 0;
    size_t start_ind_source = get_index(shared, source);

    saved_memory_state *saved_state = (saved_memory_state *)calloc(nb_items, sizeof(saved_memory_state));

    for (size_t ind = start_ind_source; ind < start_ind_source + nb_items; ind++)
    {
        err_load = TxLoad(shared, t, (uintptr_t *)(current_src_slot), (uintptr_t *)(&tmp_slot[tmp_slot_index]), ind, start_ind_source, saved_state);
        if (err_load == -1)
        {
            free(tmp_slot);
            free(saved_state);
            TxFreeThread(t);
            return false;
        }

        current_src_slot += alignment;
        tmp_slot_index += alignment;
    }
    for (size_t i = 0; i < nb_items; i++)
    {
        saved_memory_state memory_state = saved_state[i];

        if (memory_state.Index != -1 && (memory_state.version != region->memory_state[memory_state.Index].version ||
                                         memory_state.isLocked != region->memory_state[memory_state.Index].isLocked ||
                                         memory_state.Owner != region->memory_state[memory_state.Index].Owner))
        {
            free(tmp_slot);
            free(saved_state);
            TxFreeThread(t);
            return false;
        }
    }

    memcpy(target, tmp_slot, size);
    free(tmp_slot);
    free(saved_state);
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

    size_t nb_items = size / alignment;
    const char *current_src_slot = source;
    char *current_target_slot = target;

    size_t start_ind_source = get_index(shared, target);

    for (size_t ind = start_ind_source; ind < start_ind_source + nb_items; ind++)
    {
        TxStore(shared, t, (uintptr_t *)(current_src_slot), (uintptr_t *)(current_target_slot), ind);

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
