#include "Curier.h"
#include "internal/CurierProtocol.h"
#include "internal/CurierRetry.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace {

void testResultContract() {
	const CurierResult ok = CurierResult::success();
	const CurierResult failed = CurierResult::failure(CurierStatus::InvalidArgument, "bad input");

	assert(ok);
	assert(ok.status == CurierStatus::Ok);
	assert(!failed);
	assert(failed.status == CurierStatus::InvalidArgument);
}

void testEndpointOrigins() {
	std::string origin;

	assert(curier_internal::endpointOrigin("https://Push.Example.com/send/abc", 2048, origin));
	assert(origin == "https://push.example.com");

	assert(curier_internal::endpointOrigin("HTTPS://push.example.com:443/send", 2048, origin));
	assert(origin == "https://push.example.com");

	assert(curier_internal::endpointOrigin("https://push.example.com:8443/send", 2048, origin));
	assert(origin == "https://push.example.com:8443");

	assert(curier_internal::endpointOrigin("https://[2001:db8::1]/send", 2048, origin));
	assert(origin == "https://[2001:db8::1]");

	assert(curier_internal::endpointOrigin("https://[2001:db8::1]:8443/send", 2048, origin));
	assert(origin == "https://[2001:db8::1]:8443");

	assert(!curier_internal::endpointOrigin("http://push.example.com/send", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://user@push.example.com/send", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://push.example.com/send#fragment", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://push.example.com/send", 8, origin));
	assert(!curier_internal::endpointOrigin("https://push.example.com:443:8443/send", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://2001:db8::1/send", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://[2001:db8::1/send", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://[2001:db8::1]extra/send", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://push.example.com\\/send", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://push.example.com:/send", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://:443/send", 2048, origin));
	assert(!curier_internal::endpointOrigin("https://push..example.com/send", 2048, origin));
}

void testRetryValidation() {
	CurierRetryConfig retry;
	assert(curier_internal::validateRetryConfig(retry));

	retry.jitterPercent = 101;
	assert(!curier_internal::validateRetryConfig(retry));

	retry.jitterPercent = 0;
	retry.baseDelayMs = 2000;
	retry.maxDelayMs = 1000;
	assert(!curier_internal::validateRetryConfig(retry));

	retry.mode = CurierRetryMode::Disabled;
	assert(curier_internal::validateRetryConfig(retry));
}

void testDefaultRetryPolicy() {
	CurierRetryConfig retry;
	retry.maxRetries = 3;
	retry.baseDelayMs = 1000;
	retry.maxDelayMs = 5000;
	retry.jitterPercent = 0;

	CurierRetryContext context;
	context.attempts = 1;
	context.status = CurierStatus::TransportError;

	CurierRetryDecision decision = curier_internal::defaultRetryDecision(retry, context, 0);
	assert(decision.retry);
	assert(decision.delayMs == 1000);

	context.attempts = 2;
	context.status = CurierStatus::HttpError;
	context.statusCode = 503;
	decision = curier_internal::defaultRetryDecision(retry, context, 0);
	assert(decision.retry);
	assert(decision.delayMs == 2000);

	context.statusCode = 400;
	decision = curier_internal::defaultRetryDecision(retry, context, 0);
	assert(!decision.retry);

	context.statusCode = 429;
	context.retryAfterMs = 4500;
	retry.jitterPercent = 20;
	decision = curier_internal::defaultRetryDecision(retry, context, 0);
	assert(decision.retry);
	assert(decision.delayMs == 4500);

	context.attempts = 4;
	decision = curier_internal::defaultRetryDecision(retry, context, 0);
	assert(!decision.retry);
}

void testRetryAfterParsing() {
	uint32_t delayMs = 0;

	assert(curier_internal::parseRetryAfter("12", 0, 20000, delayMs));
	assert(delayMs == 12000);

	assert(curier_internal::parseRetryAfter("999999", 0, 20000, delayMs));
	assert(delayMs == 20000);

	constexpr uint64_t target = 784111777;
	assert(curier_internal::parseRetryAfter(
	    "Sun, 06 Nov 1994 08:49:37 GMT",
	    target - 10,
	    20000,
	    delayMs
	));
	assert(delayMs == 10000);

	assert(!curier_internal::parseRetryAfter("not-a-date", target, 20000, delayMs));
}

} // namespace

int main() {
	testResultContract();
	testEndpointOrigins();
	testRetryValidation();
	testDefaultRetryPolicy();
	testRetryAfterParsing();
	return 0;
}
