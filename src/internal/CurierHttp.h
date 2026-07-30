#pragma once

#include "../Curier.h"

#include <functional>
#include <string>
#include <vector>

namespace curier_internal {

struct CurierHttpResponse {
	CurierSendResult result;
	std::string retryAfter;
};

using CurierHttpTransport = std::function<CurierHttpResponse(
    const CurierConfig &config,
    const CurierSubscription &subscription,
    const std::string &jwt,
    const std::vector<uint8_t> &body
)>;

CurierHttpResponse sendWebPushRequest(
    const CurierConfig &config,
    const CurierSubscription &subscription,
    const std::string &jwt,
    const std::vector<uint8_t> &body
);

#ifdef CURIER_ENABLE_TEST_HOOKS
void setHttpTransportForTesting(CurierHttpTransport transport);
void clearHttpTransportForTesting();
#endif

} // namespace curier_internal
