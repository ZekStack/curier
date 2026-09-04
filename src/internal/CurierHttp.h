#pragma once

#include "CurierMemory.h"

#include <functional>
#include <span>
#include <string_view>

namespace curier_internal {

struct CurierHttpResponse {
	explicit CurierHttpResponse(Strata::Placement placement = Strata::Placement::Default) noexcept
	    : retryAfter(Strata::Allocator<char>{placement}) {
	}

	CurierSendResult result;
	CurierString retryAfter;
};

using CurierHttpTransport = std::function<CurierHttpResponse(
    const CurierRuntimeConfig &config,
    CurierSubscriptionView subscription,
    std::string_view jwt,
    std::span<const uint8_t> body
)>;

CurierHttpResponse sendWebPushRequest(
    const CurierRuntimeConfig &config,
    CurierSubscriptionView subscription,
    std::string_view jwt,
    std::span<const uint8_t> body
);

#ifdef CURIER_ENABLE_TEST_HOOKS
void setHttpTransportForTesting(CurierHttpTransport transport);
void clearHttpTransportForTesting();
#endif

} // namespace curier_internal
