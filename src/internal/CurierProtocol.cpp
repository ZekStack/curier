#include "CurierProtocol.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string_view>

#if defined(ARDUINO_ARCH_ESP32)
extern "C" {
#include "lwip/inet.h"
#include "lwip/sockets.h"
}
#elif __has_include(<arpa/inet.h>)
#include <arpa/inet.h>
#endif

namespace {

bool startsWithHttps(std::string_view value) {
	static constexpr std::string_view kScheme = "https://";
	if (value.size() < kScheme.size()) {
		return false;
	}
	for (size_t index = 0; index < kScheme.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(value[index])) != kScheme[index]) {
			return false;
		}
	}
	return true;
}

bool parsePort(std::string_view value, uint16_t &port) {
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

bool validDnsOrIpv4Host(std::string_view host) {
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

bool validBracketedIpv6(std::string_view host) {
	if (host.size() < 4 || host.front() != '[' || host.back() != ']') {
		return false;
	}
	const std::string_view address = host.substr(1, host.size() - 2);
	std::array<char, INET6_ADDRSTRLEN> text{};
	if (address.size() + 1 > text.size()) {
		return false;
	}
	std::memcpy(text.data(), address.data(), address.size());
	text[address.size()] = '\0';
	std::array<uint8_t, 16> parsed{};
	return inet_pton(AF_INET6, text.data(), parsed.data()) == 1;
}

void appendLowerAscii(curier_internal::CurierString &output, std::string_view value) {
	for (char character : value) {
		output.push_back(
		    static_cast<char>(std::tolower(static_cast<unsigned char>(character)))
		);
	}
}

} // namespace

namespace curier_internal {

CurierResult
endpointOrigin(std::string_view endpoint, size_t maxEndpointBytes, CurierString &origin) {
	origin.clear();
	if (endpoint.empty() || endpoint.size() > maxEndpointBytes || !startsWithHttps(endpoint) ||
	    endpoint.find('#') != std::string_view::npos || endpoint.find('\\') != std::string_view::npos) {
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription endpoint must be a bounded HTTPS URL without a fragment"
		);
	}

	const size_t authorityStart = 8;
	const size_t authorityEnd = endpoint.find_first_of("/?#", authorityStart);
	const size_t end = authorityEnd == std::string_view::npos ? endpoint.size() : authorityEnd;
	if (end <= authorityStart) {
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription endpoint host is required"
		);
	}
	const std::string_view authority = endpoint.substr(authorityStart, end - authorityStart);
	if (authority.find('@') != std::string_view::npos) {
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription endpoint userinfo is not allowed"
		);
	}

	std::string_view host;
	uint16_t port = 443;
	bool explicitPort = false;
	if (authority.front() == '[') {
		const size_t closing = authority.find(']');
		if (closing == std::string_view::npos || closing <= 1 ||
		    authority.find(']', closing + 1) != std::string_view::npos) {
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
		if (authority.find('[') != std::string_view::npos ||
		    authority.find(']') != std::string_view::npos) {
			return CurierResult::failure(
			    CurierStatus::InvalidSubscription,
			    "subscription endpoint host is invalid"
			);
		}
		const size_t firstColon = authority.find(':');
		const size_t lastColon = authority.rfind(':');
		if (firstColon != std::string_view::npos && firstColon != lastColon) {
			return CurierResult::failure(
			    CurierStatus::InvalidSubscription,
			    "subscription endpoint IPv6 hosts must be bracketed"
			);
		}
		if (firstColon != std::string_view::npos) {
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

	origin.assign("https://");
	appendLowerAscii(origin, host);
	if (explicitPort && port != 443) {
		origin.push_back(':');
		std::array<char, 6> portText{};
		auto converted = std::to_chars(portText.data(), portText.data() + portText.size(), port);
		if (converted.ec != std::errc{}) {
			origin.clear();
			return CurierResult::failure(
			    CurierStatus::InternalError,
			    "subscription endpoint port formatting failed"
			);
		}
		origin.append(portText.data(), static_cast<size_t>(converted.ptr - portText.data()));
	}
	return CurierResult::success();
}

} // namespace curier_internal
