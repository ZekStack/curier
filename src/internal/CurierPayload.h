#pragma once

#include "CurierMemory.h"

namespace curier_internal {

CurierResult serializePayload(
    const CurierPayload &payload,
    size_t maxPayloadBytes,
    Strata::Placement placement,
    CurierString &json
);
CurierResult serializePayload(
    const JsonDocument &payload,
    size_t maxPayloadBytes,
    CurierString &json
);

} // namespace curier_internal
