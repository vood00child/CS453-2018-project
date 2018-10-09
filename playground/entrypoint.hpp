/**
 * @file   entrypoint.hpp
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
 * Interface for the "entry point" source file.
**/

#pragma once

// -------------------------------------------------------------------------- //

/** Your lock class.
**/
class Lock final {
public:
    Lock();
    ~Lock();
public:
    void acquire();
    void release();
public:
    /** Forwards call to 'acquire', for this class to satisfy 'BasicLockable' concept.
    **/
    void lock() {
        acquire();
    }
    /** Forwards call to 'release', for this class to satisfy 'BasicLockable' concept.
    **/
    void unlock() {
        release();
    }
};

// -------------------------------------------------------------------------- //

void entry_point(size_t, size_t, Lock&);
