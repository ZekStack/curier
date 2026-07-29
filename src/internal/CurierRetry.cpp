#include "CurierRetry.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

uint32_t clampDelay(uint64_t value, uint32_t maximum) {
	return value > maximum ? maximum : static_cast<uint32_t>(value);
}

bool isLeapYear(int year) {
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) {
	static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month < 1 || month > 12) {
		return 0;
	}
	if (month == 2 && isLeapYear(year)) {
		return 29;
	}
	return kDays[month - 1];
}

int monthNumber(const char *month) {
	static constexpr const char *kMonths[] = {
	    "Jan",
	    "Feb",
	    "Mar",
	    "Apr",
	    "May",
	    "Jun",
	    "Jul",
	    "Aug",
	    "Sep",
	    "Oct",
	    "Nov",
	    "Dec",
	};
	for (int index = 0; index < 12; ++index) {
		if (std::strncmp(month, kMonths[index], 3) == 0) {
			return index + 1;
		}
	}
	return 0;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
	year -= month <= 2;
	const int era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
	const int adjustedMonth = static_cast<int>(month) + (month > 2 ? -3 : 9);
	const unsigned dayOfYear = (153U * static_cast<unsigned>(adjustedMonth) + 2U) / 5U + day - 1;
	const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
	return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

bool parseHttpDate(const char *value, uint64_t &epochSeconds) {
	char weekday[4] = {};
	char monthName[4] = {};
	char timezone[4] = {};
	int day = 0;
	int year = 0;
	int hour = 0;
	int minute = 0;
	int second = 0;
	char trailing = '\0';
	const int matched = std::sscanf(
	    value,
	    "%3s, %d %3s %d %d:%d:%d %3s%c",
	    weekday,
	    &day,
	    monthName,
	    &year,
	    &hour,
	    &minute,
	    &second,
	    timezone,
	    &trailing
	);
	if (matched != 8 || std::strcmp(timezone, "GMT") != 0) {
		return false;
	}
	const int month = monthNumber(monthName);
	if (year < 1970 || month == 0 || day < 1 || day > daysInMonth(year, month) || hour < 0 ||
	    hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
		return false;
	}
	const int64_t days =
	    daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
	if (days < 0) {
		return false;
	}
	const uint64_t seconds = static_cast<uint64_t>(days) * 86400ULL +
	                         static_cast<uint64_t>(hour) * 3600ULL +
	                         static_cast<uint64_t>(minute) * 60ULL + static_cast<uint64_t>(second);
	epochSeconds = seconds;
	return true;
}

uint32_t applyJitter(uint32_t delayMs, uint8_t percent, uint32_t randomValue) {
	if (delayMs == 0 || percent == 0) {
		return delayMs;
	}
	const uint64_t spread64 =
	    (static_cast<uint64_t>(delayMs) * static_cast<uint64_t>(percent)) / 100ULL;
	const uint32_t spread = spread64 > std::numeric_limits<uint32_t>::max()
	                            ? std::numeric_limits<uint32_t>::max()
	                            : static_cast<uint32_t>(spread64);
	if (spread == 0) {
		return delayMs;
	}
	const uint64_t range = static_cast<uint64_t>(spread) * 2ULL + 1ULL;
	const int64_t offset = static_cast<int64_t>(randomValue % range) - static_cast<int64_t>(spread);
	const int64_t jittered = static_cast<int64_t>(delayMs) + offset;
	return jittered <= 0 ? 0 : static_cast<uint32_t>(jittered);
}

} // namespace

namespace curier_internal {

CurierResult validateRetryConfig(const CurierRetryConfig &config) {
	if (config.mode != CurierRetryMode::Disabled && config.mode != CurierRetryMode::Fixed &&
	    config.mode != CurierRetryMode::Exponential) {
		return CurierResult::failure(CurierStatus::InvalidConfig, "retry mode is invalid");
	}
	if (config.jitterPercent > 100) {
		return CurierResult::failure(
		    CurierStatus::InvalidConfig,
		    "retry jitter must be at most 100 percent"
		);
	}
	if (config.mode != CurierRetryMode::Disabled &&
	    (config.baseDelayMs == 0 || config.maxDelayMs == 0 || config.baseDelayMs > config.maxDelayMs
	    )) {
		return CurierResult::failure(CurierStatus::InvalidConfig, "retry delay bounds are invalid");
	}
	return CurierResult::success();
}

CurierRetryDecision defaultRetryDecision(
    const CurierRetryConfig &config, const CurierRetryContext &context, uint32_t randomValue
) {
	CurierRetryDecision decision;
	if (config.mode == CurierRetryMode::Disabled || context.attempts == 0 ||
	    context.attempts > config.maxRetries) {
		return decision;
	}

	bool retryable = false;
	if (context.status == CurierStatus::ClockUnavailable) {
		retryable = config.retryClockUnavailable;
	} else if (context.status == CurierStatus::TransportError) {
		retryable = config.retryTransportErrors;
	} else if (context.status == CurierStatus::HttpError) {
		retryable = (context.statusCode == 408 && config.retryHttp408) ||
		            (context.statusCode == 429 && config.retryHttp429) ||
		            (context.statusCode >= 500 && context.statusCode <= 599 && config.retryHttp5xx);
	}
	if (!retryable) {
		return decision;
	}

	uint32_t delayMs = config.baseDelayMs;
	if (config.mode == CurierRetryMode::Exponential) {
		const uint8_t shift = context.attempts > 1 ? context.attempts - 1 : 0;
		if (shift >= 32 || config.baseDelayMs > (std::numeric_limits<uint32_t>::max() >> shift)) {
			delayMs = config.maxDelayMs;
		} else {
			delayMs =
			    clampDelay(static_cast<uint64_t>(config.baseDelayMs) << shift, config.maxDelayMs);
		}
	}
	const bool usedRetryAfter = config.respectRetryAfter && context.retryAfterMs > 0;
	if (usedRetryAfter) {
		delayMs =
		    context.retryAfterMs > config.maxDelayMs ? config.maxDelayMs : context.retryAfterMs;
	}

	decision.retry = true;
	decision.delayMs =
	    usedRetryAfter ? delayMs : applyJitter(delayMs, config.jitterPercent, randomValue);
	return decision;
}

bool parseRetryAfter(
    const char *value, uint64_t nowEpochSeconds, uint32_t maxDelayMs, uint32_t &delayMs
) {
	delayMs = 0;
	if (value == nullptr || *value == '\0') {
		return false;
	}

	bool numeric = true;
	uint64_t seconds = 0;
	for (const char *cursor = value; *cursor != '\0'; ++cursor) {
		if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
			numeric = false;
			break;
		}
		const uint8_t digit = static_cast<uint8_t>(*cursor - '0');
		if (seconds > (std::numeric_limits<uint64_t>::max() - digit) / 10ULL) {
			seconds = std::numeric_limits<uint64_t>::max();
			break;
		}
		seconds = seconds * 10ULL + digit;
	}

	if (!numeric) {
		uint64_t target = 0;
		if (!parseHttpDate(value, target)) {
			return false;
		}
		seconds = target > nowEpochSeconds ? target - nowEpochSeconds : 0;
	}

	const uint64_t milliseconds = seconds > std::numeric_limits<uint64_t>::max() / 1000ULL
	                                  ? std::numeric_limits<uint64_t>::max()
	                                  : seconds * 1000ULL;
	delayMs = clampDelay(milliseconds, maxDelayMs);
	return true;
}

} // namespace curier_internal
