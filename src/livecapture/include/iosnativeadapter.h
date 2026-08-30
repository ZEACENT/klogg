/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#pragma once

#include <string>

#include "iosnativeapi.h"

namespace klogg::livecapture::ios {

// Loads only the private dylib closure rooted at stackRoot. The returned API is
// empty on failure; no host package manager, daemon executable, or Apple private
// framework fallback is attempted.
IosNativeApi loadIosNativeApiFromBundle( const std::string& stackRoot,
                                         std::string* error = nullptr );

} // namespace klogg::livecapture::ios
