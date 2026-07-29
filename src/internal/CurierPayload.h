#pragma once

#include "../Curier.h"

#include <string>

namespace curier_internal {

CurierResult
serializePayload(const CurierPayload &payload, size_t maxPayloadBytes, std::string &json);
CurierResult
serializePayload(const JsonDocument &payload, size_t maxPayloadBytes, std::string &json);

} // namespace curier_internal
