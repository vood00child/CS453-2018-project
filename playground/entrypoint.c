#if !defined(CONFIG_USE_CPP)
// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

// External headers
#include <stdatomic.h>
#include <stdio.h>

// Internal headers
#include "entrypoint.h"

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

/** Lock initialization.
 * @param lock Uninitialized lock structure
 * @return Whether initialization is a success
**/
bool lock_init(struct lock_t* lock __attribute__((unused))) {
    atomic_init(&(lock->locked), false);
    return true;
}

/** Lock clean up.
 * @param lock Initialized lock structure
**/
void lock_cleanup(struct lock_t* lock __attribute__((unused))) {
    // Code here
}

/** [thread-safe] Acquire the given lock, wait if it has already been acquired.
 * @param lock Initialized lock structure
**/
void lock_acquire(struct lock_t* lock __attribute__((unused))) {
   
    bool expected = false;
    while (!atomic_compare_exchange_weak(&(lock->locked), &expected, true)) {
        expected = false; // reset it to false 
    }
}

/** [thread-safe] Release the given lock.
 * @param lock Initialized lock structure
**/
void lock_release(struct lock_t* lock __attribute__((unused))) {
    atomic_store(&(lock->locked), false);
}

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

/** Thread entry point.
 * @param nb   Total number of threads
 * @param id   This thread ID (from 0 to nb-1 included)
 * @param lock Instance of your lock
**/
void entry_point(size_t nb, size_t id, struct lock_t* lock) {
    printf("Hello from C version in thread %lu/%lu\n", id, nb); // Feel free to remove me
    for (int i = 0; i < 10000; ++i) {
        lock_acquire(lock);
        shared_access();
        lock_release(lock);
    }
}

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
#endif
