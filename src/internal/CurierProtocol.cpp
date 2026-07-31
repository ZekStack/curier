#include "CurierProtocol.h"

#include <array>
#include <cctype>
#include <cstdlib>

#if defined(ARDUINO_ARCH_ESP32)
extern "C" {
#include "lwip/inet.h"
#include "lwip/sockets.h"
}
#elif __has_include(<arpa/inet.h>)
#include <arpa/inet.h>
#endif

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

bool validDnsOrIpv4Host(const std::string &host) {
	if (host.empty() || host.front() == '.' || host.back() == '.') {
		return false;
	}
	size_t labelStart = 0;
	for (size_t index = 0; index <= host.size(); ++index) {
		if (index != host.size() && host[index] != '.') {
			const unsigned char value = static_cast<unsigned char>(host[index]);
			if (!std::isalnum(value) && host[index] != '-') {
				return false;
			}
			continue;
		}
		if (index == labelStart || index - labelStart > 63 || host[labelStart] == '-' ||
		    host[index - 1] == '-') {
			return false;
		}
		labelStart = index + 1;
	}
	return host.size() <= 253;
}

bool validBracketedIpv6(const std::string &host) {
	if (host.size() < 4 || host.front() != '[' || host.back() != ']') {
		return false;
	}
	const std::string address = host.substr(1, host.size() - 2);
	std::array<uint8_t, 16> parsed{};
	return inet_pton(AF_INET6, address.c_str(), parsed.data()) == 1;
}

} // namespace

namespace curier_internal {

CurierResult
endpointOrigin(const std::string &endpoint, size_t maxEndpointBytes, std::string &origin) {
	origin.clear();
	if (endpoint.empty() || endpoint.size() > maxEndpointBytes || !startsWithHttps(endpoint) ||
	    endpoint.find('#') != std::string::npos || endpoint.find('\\') != std::string::npos) {
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
		if (closing == std::string::npos || closing <= 1 ||
		    authority.find(']', closing + 1) != std::string::npos) {
			return CurierResult::failure(
			    CurierStatus::InvalidSubscription,
			    "subscription endpoint IPv6 host is invalid"
			);
		}
		host = authority.substr(0, closing + 1);
		if (!validBracketedIpv6(host)) {
			return CurierResult::failure(
			    CurierStatus::InvalidSubscription,
			    "subscription endpoint IPv6 host is invalid"
			);
		}
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
		if (authority.find('[') != std::string::npos || authority.find(']') != std::string::npos) {
			return CurierResult::failure(
			    CurierStatus::InvalidSubscription,
			    "subscription endpoint host is invalid"
			);
		}
		const size_t firstColon = authority.find(':');
		const size_t lastColon = authority.rfind(':');
		if (firstColon != std::string::npos && firstColon != lastColon) {
			return CurierResult::failure(
			    CurierStatus::InvalidSubscription,
			    "subscription endpoint IPv6 hosts must be bracketed"
			);
		}
		if (firstColon != std::string::npos) {
			host = authority.substr(0, firstColon);
			if (!parsePort(authority.substr(firstColon + 1), port)) {
				return CurierResult::failure(
				    CurierStatus::InvalidSubscription,
				    "subscription endpoint port is invalid"
				);
			}
			explicitPort = true;
		} else {
			host = authority;
		}
		if (!validDnsOrIpv4Host(host)) {
			return CurierResult::failure(
			    CurierStatus::InvalidSubscription,
			    "subscription endpoint host is invalid"
			);
		}
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
