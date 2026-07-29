#include "CurierProtocol.h"

#include <cctype>
#include <cstdlib>

namespace {

bool startsWithHttps(const std::string &value) {
	static constexpr const char *kScheme = "https://";
	if (value.size() < 8) {
		return false;
	}
	for (size_t index = 0; index < 8; ++index) {
		if (std::tolower(static_cast<unsigned char>(value[index])) != kScheme[index]) {
			return false;
		}
	}
	return true;
}

std::string lowerAscii(std::string value) {
	for (char &character : value) {
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	return value;
}

bool parsePort(const std::string &value, uint16_t &port) {
	if (value.empty()) {
		return false;
	}
	uint32_t parsed = 0;
	for (char character : value) {
		if (!std::isdigit(static_cast<unsigned char>(character))) {
			return false;
		}
		parsed = parsed * 10U + static_cast<uint32_t>(character - '0');
		if (parsed > 65535U) {
			return false;
		}
	}
	if (parsed == 0) {
		return false;
	}
	port = static_cast<uint16_t>(parsed);
	return true;
}

} // namespace

namespace curier_internal {

CurierResult
endpointOrigin(const std::string &endpoint, size_t maxEndpointBytes, std::string &origin) {
	origin.clear();
	if (endpoint.empty() || endpoint.size() > maxEndpointBytes || !startsWithHttps(endpoint) ||
	    endpoint.find('#') != std::string::npos) {
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription endpoint must be a bounded HTTPS URL without a fragment"
		);
	}

	const size_t authorityStart = 8;
	const size_t authorityEnd = endpoint.find_first_of("/?#", authorityStart);
	const size_t end = authorityEnd == std::string::npos ? endpoint.size() : authorityEnd;
	if (end <= authorityStart) {
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription endpoint host is required"
		);
	}
	const std::string authority = endpoint.substr(authorityStart, end - authorityStart);
	if (authority.find('@') != std::string::npos) {
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription endpoint userinfo is not allowed"
		);
	}

	std::string host;
	uint16_t port = 443;
	bool explicitPort = false;
	if (authority.front() == '[') {
		const size_t closing = authority.find(']');
		if (closing == std::string::npos || closing <= 1) {
			return CurierResult::failure(
			    CurierStatus::InvalidSubscription,
			    "subscription endpoint IPv6 host is invalid"
			);
		}
		host = authority.substr(0, closing + 1);
		if (closing + 1 < authority.size()) {
			if (authority[closing + 1] != ':' || !parsePort(authority.substr(closing + 2), port)) {
				return CurierResult::failure(
				    CurierStatus::InvalidSubscription,
				    "subscription endpoint port is invalid"
				);
			}
			explicitPort = true;
		}
	} else {
		const size_t colon = authority.rfind(':');
		if (colon != std::string::npos) {
			host = authority.substr(0, colon);
			if (!parsePort(authority.substr(colon + 1), port)) {
				return CurierResult::failure(
				    CurierStatus::InvalidSubscription,
				    "subscription endpoint port is invalid"
				);
			}
			explicitPort = true;
		} else {
			host = authority;
		}
	}
	if (host.empty()) {
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription endpoint host is required"
		);
	}
	for (char character : host) {
		const unsigned char value = static_cast<unsigned char>(character);
		if (value <= 32 || value >= 127) {
			return CurierResult::failure(
			    CurierStatus::InvalidSubscription,
			    "subscription endpoint host is invalid"
			);
		}
	}

	origin = "https://" + lowerAscii(host);
	if (explicitPort && port != 443) {
		origin += ":" + std::to_string(port);
	}
	return CurierResult::success();
}

} // namespace curier_internal
