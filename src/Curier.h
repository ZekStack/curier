#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Strata.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct CurierImpl;

enum class CurierStatus : uint8_t {
	Ok,
	NotInitialized,
	AlreadyInitialized,
	Stopping,
	InvalidConfig,
	InvalidArgument,
	InvalidVapidSubject,
	InvalidVapidKey,
	VapidKeyMismatch,
	InvalidSubscription,
	InvalidPayload,
	PayloadTooLarge,
	QueueFull,
	AllocationFailed,
	QueueCreateFailed,
	TaskCreateFailed,
	ClockUnavailable,
	CryptoError,
	JwtError,
	TransportError,
	HttpError,
	Cancelled,
	Busy,
	Timeout,
	InternalError,
};

enum class CurierRetryMode : uint8_t {
	Disabled,
	Fixed,
	Exponential,
};

struct CurierResult {
	bool result = false;
	CurierStatus status = CurierStatus::InternalError;
	const char *message = "internal error";

	explicit operator bool() const {
		return result;
	}

	static CurierResult success(const char *message = "ok") {
		return {true, CurierStatus::Ok, message};
	}

	static CurierResult failure(CurierStatus status, const char *message) {
		return {false, status, message};
	}
};

struct CurierSendResult : CurierResult {
	esp_err_t transportError = ESP_OK;
	int statusCode = 0;
	uint8_t attempts = 0;

	bool ok() const {
		return result;
	}
};

struct CurierVapid {
	std::string subject;
	std::string publicKeyBase64;
	std::string privateKeyBase64;
};

struct CurierSubscription {
	std::string endpoint;
	std::string p256dh;
	std::string auth;
};

struct CurierAction {
	std::string action;
	std::string title;
	std::optional<std::string> icon;
	std::optional<std::string> navigate;
};

struct CurierPayload {
	std::string title;
	std::string body;
	std::optional<std::string> tag;
	std::optional<std::string> icon;
	std::optional<std::string> badge;
	std::optional<std::string> image;
	JsonDocument data;
	bool hasData = false;
	std::vector<CurierAction> actions;
	std::optional<bool> renotify;
	std::optional<bool> requireInteraction;
	std::optional<bool> silent;
	std::optional<uint64_t> timestamp;
};

struct CurierRetryConfig {
	CurierRetryMode mode = CurierRetryMode::Exponential;
	uint8_t maxRetries = 5;
	uint32_t baseDelayMs = 1500;
	uint32_t maxDelayMs = 15000;
	uint8_t jitterPercent = 20;
	bool respectRetryAfter = true;
	bool retryClockUnavailable = true;
	bool retryTransportErrors = true;
	bool retryHttp408 = true;
	bool retryHttp429 = true;
	bool retryHttp5xx = true;
};

struct CurierRetryContext {
	uint8_t attempts = 0;
	CurierStatus status = CurierStatus::InternalError;
	esp_err_t transportError = ESP_OK;
	int statusCode = 0;
	uint32_t retryAfterMs = 0;
};

struct CurierRetryDecision {
	bool retry = false;
	uint32_t delayMs = 0;
};

using CurierSendCallback = std::function<void(CurierSendResult)>;
using CurierTimeProvider = std::function<bool(uint64_t &epochSeconds)>;
using CurierRetryPolicy = std::function<CurierRetryDecision(const CurierRetryContext &context)>;

struct CurierConfig {
	Strata::MemoryPolicy memory{
	    .allocation = Strata::Placement::Default,
	    .taskStack = Strata::Placement::PreferExternal,
	};

	CurierVapid vapidConfig;

	size_t queueSize = 16;
	size_t maxPayloadBytes = 3993;
	size_t maxEndpointBytes = 2048;

	uint32_t stackSize = 4096;
	UBaseType_t priority = 1;
	BaseType_t coreId = tskNO_AFFINITY;
	std::string taskName = "curier-task";

	uint32_t requestTimeoutMs = 10000;
	uint32_t ttlSeconds = 2419200;

	CurierRetryConfig retry;
	CurierRetryPolicy retryPolicy;

	bool useTlsCertBundle = true;
	bool useGlobalCaStore = false;
	bool skipTlsCommonNameCheck = false;
	std::string caCertificatePem;
};

struct CurierDiagnostics {
	size_t queueSize = 0;
	size_t queueDepth = 0;
	size_t inFlight = 0;
	size_t queueHighWaterMark = 0;
	uint32_t enqueued = 0;
	uint32_t completed = 0;
	uint32_t succeeded = 0;
	uint32_t failed = 0;
	uint32_t retried = 0;
	uint32_t cancelled = 0;
	size_t stackHighWaterMarkBytes = 0;
	Strata::Placement allocationPlacement = Strata::Placement::Default;
	Strata::Placement requestedStackPlacement = Strata::Placement::Default;
	Strata::Region stackRegion = Strata::Region::Unknown;
	Strata::Placement queueStoragePlacement = Strata::Placement::Default;
	Strata::Region queueStorageRegion = Strata::Region::Unknown;
	Strata::Region shutdownSignalControlRegion = Strata::Region::Unknown;
};

class Curier {
  public:
	Curier();
	~Curier();

	Curier(const Curier &) = delete;
	Curier &operator=(const Curier &) = delete;

	CurierResult init(const CurierConfig &config = CurierConfig());
	CurierResult end(uint32_t timeoutMs = 5000);
	bool initialized() const;

	CurierResult send(
	    const CurierSubscription &subscription,
	    const CurierPayload &payload,
	    CurierSendCallback callback
	);
	CurierResult send(
	    const CurierSubscription &subscription,
	    const JsonDocument &payload,
	    CurierSendCallback callback
	);

	CurierResult setTimeProvider(CurierTimeProvider provider);
	CurierResult clearTimeProvider();

	CurierDiagnostics diagnostics() const;
	const char *statusToString(CurierStatus status) const;

  private:
	Strata::UniquePtr<CurierImpl> _impl;
};
