/**
 * @file   entrypoint.cpp
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
 * "Entry point" source file, implementing the playground function 'entry_point' and the lock.
**/

// External headers
#include <atomic>
#include <iostream>
#include <mutex>

// Internal headers
#include "entrypoint.hpp"
#include "runner.hpp"

// -------------------------------------------------------------------------- //

/** Lock default constructor.
**/
Lock::Lock() {
    // Code here
}

/** Lock destructor.
**/
Lock::~Lock() {
    // Code here
}

/** [thread-safe] Acquire the given lock, wait if it has already been acquired.
**/
void Lock::acquire() {
    // Code here
}

/** [thread-safe] Release the given lock.
 * @param lock Initialized lock structure
**/
void Lock::release() {
    // Code here
}

// -------------------------------------------------------------------------- //

/** Thread entry point.
 * @param nb Total number of threads
 * @param id This thread ID (from 0 to nb-1 included)
**/
void entry_point(size_t nb, size_t id, Lock& lock) {
    ::printf("Hello from thread %lu/%lu\n", id, nb); // Feel free to remove me
    for (int i = 0; i < 10000; ++i) {
        ::std::lock_guard<Lock> guard{lock};
        ::shared_access();
    }
}
