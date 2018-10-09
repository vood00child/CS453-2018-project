/**
 * @file   tm.c
 * @author Sébastien Rouault <sebastien.rouault@epfl.ch>
 *
 * @section LICENSE
 *
 * Copyright © 2018 Sébastien Rouault.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version. Please see https://gnu.org/licenses/gpl.html
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * @section DESCRIPTION
 *
 * Lock-based transaction manager implementation used as the reference.
**/

// Compile-time configuration
// #define USE_MM_PAUSE
// #define USE_TICKET_LOCK

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE   200809L
#ifdef __STDC_NO_ATOMICS__
    #error Current C11 compiler does not support atomic operations
#endif

// External headers
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
    #include <xmmintrin.h>
#else
    #include <sched.h>
#endif

// Internal headers
#include <tm.h>

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

#if defined(USE_TICKET_LOCK)
struct lock_t {
    atomic_ulong pass; // Ticket that acquires the lock
    atomic_ulong take; // Ticket the next thread takes
};
#else
struct lock_t {
    atomic_bool locked; // Whether the lock is taken
};
#endif

struct region {
    struct lock_t lock; // Global lock
    void* start;        // Start of the shared memory region
    size_t size;        // Size of the shared memory region (in bytes)
    size_t align;       // Claimed alignment of the shared memory region (in bytes)
    size_t align_alloc; // Actual alignment of the memory allocations (in bytes)
};

// -------------------------------------------------------------------------- //

/** Pause for a very short amount of time.
**/
static void pause() {
#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
    _mm_pause();
#else
    sched_yield();
#endif
}

/** Initialize the given lock.
 * @param lock Lock to initialize
 * @return Whether the operation is a success
**/
static bool lock_init(struct lock_t* lock) {
#if defined(USE_TICKET_LOCK)
    atomic_init(&(lock->pass), 0ul);
    atomic_init(&(lock->take), 0ul);
    return true;
#else
    atomic_init(&(lock->locked), false);
    return true;
#endif
}

/** Clean the given lock up.
 * @param lock Lock to clean up
**/
static void lock_cleanup(struct lock_t* lock as(unused)) {
#if defined(USE_TICKET_LOCK)
    return;
#else
    return;
#endif
}

/** Wait and acquire the given lock.
 * @param lock Lock to acquire
 * @return Whether the operation is a success
**/
static bool lock_acquire(struct lock_t* lock) {
#if defined(USE_TICKET_LOCK)
    unsigned long ticket = atomic_fetch_add_explicit(&(lock->take), 1ul, memory_order_relaxed);
    while (atomic_load_explicit(&(lock->pass), memory_order_relaxed) != ticket)
        pause();
    atomic_thread_fence(memory_order_acquire);
    return true;
#else
    bool expected = false;
    while (unlikely(!atomic_compare_exchange_weak_explicit(&(lock->locked), &expected, true, memory_order_acquire, memory_order_relaxed))) {
        expected = false;
        while (unlikely(atomic_load_explicit(&(lock->locked), memory_order_relaxed)))
            pause();
    }
    return true;
#endif
}

/** Release the given lock.
 * @param lock Lock to acquire
 * @return Whether the operation is a success
**/
static void lock_release(struct lock_t* lock) {
#if defined(USE_TICKET_LOCK)
    atomic_fetch_add_explicit(&(lock->pass), 1, memory_order_release);
#else
    atomic_store_explicit(&(lock->locked), false, memory_order_release);
#endif
}

// -------------------------------------------------------------------------- //

shared_t tm_create(size_t size, size_t align) {
    struct region* region = (struct region*) malloc(sizeof(struct region));
    if (unlikely(!region)) {
        return invalid_shared;
    }
    size_t align_alloc = align < sizeof(void*) ? sizeof(void*) : align;
    if (unlikely(posix_memalign(&(region->start), align_alloc, size) != 0)) {
        free(region);
        return invalid_shared;
    }
    if (unlikely(!lock_init(&(region->lock)))) {
        free(region->start);
        free(region);
        return invalid_shared;
    }
    region->size        = size;
    region->align       = align;
    region->align_alloc = align_alloc;
    return region;
}

void tm_destroy(shared_t shared) {
    struct region* region = (struct region*) shared;
    lock_cleanup(&(region->lock));
    free(region->start);
    free(region);
}

void* tm_start(shared_t shared) {
    return ((struct region*) shared)->start;
}

size_t tm_size(shared_t shared) {
    return ((struct region*) shared)->size;
}

size_t tm_align(shared_t shared) {
    return ((struct region*) shared)->align;
}

tx_t tm_begin(shared_t shared) {
    if (unlikely(!lock_acquire(&(((struct region*) shared)->lock))))
        return invalid_tx;
    return invalid_tx + 1; // There can be only one transaction running => ID is useless
}

bool tm_end(shared_t shared, tx_t tx as(unused)) {
    lock_release(&(((struct region*) shared)->lock));
    return true;
}

bool tm_read(shared_t shared as(unused), tx_t tx as(unused), void const* source, size_t size, void* target) {
    memcpy(target, source, size);
    return true;
}

bool tm_write(shared_t shared as(unused), tx_t tx as(unused), void const* source, size_t size, void* target) {
    memcpy(target, source, size);
    return true;
}

alloc_t tm_alloc(shared_t shared, tx_t tx as(unused), size_t size, void** target) {
    if (unlikely(posix_memalign(target, ((struct region*) shared)->align_alloc, size) != 0)) // Allocation failed
        return nomem_alloc;
    return success_alloc;
}

bool tm_free(shared_t shared as(unused), tx_t tx as(unused), void* target) {
    free(target);
    return true;
}
