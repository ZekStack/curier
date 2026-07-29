#pragma once

#include "../Curier.h"

#include <string>

namespace curier_internal {

CurierResult
endpointOrigin(const std::string &endpoint, size_t maxEndpointBytes, std::string &origin);

} // namespace curier_internal
