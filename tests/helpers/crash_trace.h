/*
 * Copyright (C) 2026 ZEACENT and other contributors
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

#pragma once

namespace klogg::testing {

// Installs a first-chance crash trace reporter for test binaries.
//
// Windows: registers a vectored exception handler that prints the faulting
// thread's stack to stderr for fatal exception codes (access violation,
// heap corruption, stack overflow, illegal instruction) before Catch2's own
// SEH handling reports the failure. Catch2's handler swallows the exception
// for reporting, so WER LocalDumps never sees an unhandled crash and no dump
// is produced; the printed trace is the only stack we get on CI (the
// Windows-x86 heap-corruption flake on PR #76 was undiagnosable without it).
//
// Other platforms: no-op.
void installFirstChanceCrashTrace();

} // namespace klogg::testing
