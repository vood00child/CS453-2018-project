/**
 * @file   entrypoint.h
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
 * Interface for the entry point.
**/

#pragma once

// External headers
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

/** Your lock type.
**/
struct lock_t {
    atomic_bool locked;
};

bool lock_init(struct lock_t*);
void lock_cleanup(struct lock_t*);
void lock_acquire(struct lock_t*);
void lock_release(struct lock_t*);

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

void entry_point(size_t, size_t, struct lock_t*);
