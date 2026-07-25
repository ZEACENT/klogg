/*
 * Copyright (C) 2024 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KLOGG_SIMDUTF_WRAPPER_H
#define KLOGG_SIMDUTF_WRAPPER_H

// simdutf has sign-conversion warnings under -Werror / -Wsign-conversion.
// This wrapper centralises the suppression so call sites do not need to
// replicate the pragma block.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif

#include <simdutf.h>

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif // KLOGG_SIMDUTF_WRAPPER_H
