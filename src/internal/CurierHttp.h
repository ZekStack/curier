#pragma once

#include "../Curier.h"

#include <string>
#include <vector>

namespace curier_internal {

struct CurierHttpResponse {
	CurierSendResult result;
	std::string retryAfter;
};

CurierHttpResponse sendWebPushRequest(
    const CurierConfig &config,
    const CurierSubscription &subscription,
    const std::string &jwt,
    const std::vector<uint8_t> &body
);

} // namespace curier_internal
