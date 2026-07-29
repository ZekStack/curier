#pragma once

#include "../Curier.h"

#include <cstdint>

namespace curier_internal {

CurierResult validateRetryConfig(const CurierRetryConfig &config);
CurierRetryDecision defaultRetryDecision(
    const CurierRetryConfig &config, const CurierRetryContext &context, uint32_t randomValue
);
bool parseRetryAfter(
    const char *value, uint64_t nowEpochSeconds, uint32_t maxDelayMs, uint32_t &delayMs
);

} // namespace curier_internal
