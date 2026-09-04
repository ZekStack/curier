#pragma once

#include "CurierMemory.h"

#include <string_view>

namespace curier_internal {

CurierResult
endpointOrigin(std::string_view endpoint, size_t maxEndpointBytes, CurierString &origin);

} // namespace curier_internal
