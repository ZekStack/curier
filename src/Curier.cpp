#include "Curier.h"

#include "internal/CurierCrypto.h"
#include "internal/CurierHttp.h"
#include "internal/CurierPayload.h"
#include "internal/CurierProtocol.h"
#include "internal/CurierRetry.h"

#include <strata/freertos/BinarySemaphore.h>
#include <strata/freertos/Mutex.h>
#include <strata/freertos/Task.h>

#include <array>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

extern "C" {
#include "esp_system.h"
#include "freertos/task.h"
}

namespace {

constexpr uint32_t kNotificationWork = 1U << 0U;
constexpr uint32_t kNotificationStop = 1U << 1U;
constexpr uint32_t kVapidLifetimeSeconds = 12U * 60U * 60U;
constexpr uint32_t kVapidRefreshMarginSeconds = 5U * 60U;
constexpr size_t kJwtCacheSize = 4;
constexpr uint32_t kMaxTtlSeconds = 2147483648U;
constexpr uint8_t kMaxConfiguredRetries = 20;
constexpr size_t kMaxQueueSize = 256;
constexpr size_t kMinStackSizeBytes = 1024;

enum class CurierLifecycle : uint8_t {
	Uninitialized,
	Running,
	Stopping,
};

struct CurierJob {
	CurierSubscription subscription;
	std::string origin;
	std::string payload;
	CurierSendCallback callback;
};

using CurierJobPtr = Strata::UniquePtr<CurierJob>;
using CurierJobQueue = Strata::Vector<CurierJobPtr>;

struct CurierJwtCacheEntry {
	std::string origin;
	std::string token;
	uint64_t expiresAt = 0;
	uint32_t clockGeneration = 0;
	uint32_t lastUsed = 0;
};

class CurierLock {
  public:
	explicit CurierLock(Strata::FreeRTOS::RecursiveMutex &mutex)
	    : _mutex(mutex), _locked(mutex.lock()) {
	}

	~CurierLock() {
		if (_locked) {
			_mutex.unlock();
		}
	}

	CurierLock(const CurierLock &) = delete;
	CurierLock &operator=(const CurierLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	Strata::FreeRTOS::RecursiveMutex &_mutex;
	bool _locked = false;
};

CurierSendResult
sendResult(bool succeeded, CurierStatus status, const char *message, uint8_t attempts = 0) {
	CurierSendResult result;
	result.result = succeeded;
	result.status = status;
	result.message = message;
	result.attempts = attempts;
	return result;
}

TickType_t ticksForTimeout(uint32_t timeoutMs) {
	if (timeoutMs == 0) {
		return 0;
	}
	const TickType_t ticks = pdMS_TO_TICKS(timeoutMs);
	return ticks == 0 ? 1 : ticks;
}

bool validStackSize(size_t stackBytes) {
	return stackBytes >= kMinStackSizeBytes && (stackBytes % sizeof(StackType_t)) == 0;
}

void secureClear(std::string &value) {
	volatile char *cursor = value.empty() ? nullptr : value.data();
	for (size_t index = 0; index < value.size(); ++index) {
		cursor[index] = 0;
	}
	value.clear();
}

struct SecureStringGuard {
	explicit SecureStringGuard(std::string &value) : value(value) {
	}

	~SecureStringGuard() {
		secureClear(value);
	}

	std::string &value;
};

[[noreturn]] void suspendForever() {
	vTaskSuspend(nullptr);
	for (;;) {
		vTaskDelay(portMAX_DELAY);
	}
}

} // namespace

struct CurierImpl {
	CurierImpl() noexcept
	    : mutex(Strata::FreeRTOS::RecursiveMutex::create()),
	      lifecycleMutex(Strata::FreeRTOS::RecursiveMutex::create()) {
	}

	Strata::FreeRTOS::RecursiveMutex mutex;
	Strata::FreeRTOS::RecursiveMutex lifecycleMutex;
	CurierConfig config;
	CurierLifecycle lifecycle = CurierLifecycle::Uninitialized;

	std::optional<CurierJobQueue> queue;
	size_t queueHead = 0;
	size_t queueTail = 0;
	size_t queueCount = 0;
	size_t inFlight = 0;

	Strata::FreeRTOS::Task task;
	Strata::FreeRTOS::BinarySemaphore workerReadySignal;

	std::unique_ptr<curier_internal::CurierCrypto> crypto;
	CurierTimeProvider timeProvider;
	uint32_t clockGeneration = 1;
	std::array<CurierJwtCacheEntry, kJwtCacheSize> jwtCache{};

	CurierDiagnostics diag;

	bool stopping() {
		CurierLock lock(mutex);
		return !lock || lifecycle == CurierLifecycle::Stopping;
	}

	bool readClock(uint64_t &epochSeconds, uint32_t &generation) {
		CurierTimeProvider provider;
		{
			CurierLock lock(mutex);
			if (!lock) {
				return false;
			}
			provider = timeProvider;
			generation = clockGeneration;
		}
		if (provider) {
			epochSeconds = 0;
			return provider(epochSeconds) && epochSeconds > 0;
		}
		const std::time_t now = std::time(nullptr);
		if (now <= 0) {
			epochSeconds = 0;
			return false;
		}
		epochSeconds = static_cast<uint64_t>(now);
		return true;
	}

	CurierJobPtr popJob() {
		CurierLock lock(mutex);
		if (!lock || !queue || queueCount == 0) {
			return {};
		}
		CurierJobPtr job = std::move((*queue)[queueHead]);
		queueHead = (queueHead + 1) % config.queueSize;
		queueCount--;
		diag.queueDepth = queueCount;
		return job;
	}

	void recordCompletion(const CurierSendResult &result) {
		CurierLock lock(mutex);
		if (!lock) {
			return;
		}
		if (inFlight > 0) {
			inFlight--;
		}
		diag.inFlight = inFlight;
		diag.completed++;
		if (result.result) {
			diag.succeeded++;
		} else {
			diag.failed++;
			if (result.status == CurierStatus::Cancelled) {
				diag.cancelled++;
			}
		}
	}

	CurierResult
	jwtForOrigin(const std::string &origin, uint64_t now, uint32_t generation, std::string &jwt) {
		{
			CurierLock lock(mutex);
			if (!lock) {
				return CurierResult::failure(
				    CurierStatus::InternalError,
				    "Curier mutex lock failed"
				);
			}
			for (CurierJwtCacheEntry &entry : jwtCache) {
				if (entry.clockGeneration == generation && entry.origin == origin &&
				    !entry.token.empty() && entry.expiresAt > now + kVapidRefreshMarginSeconds) {
					entry.lastUsed = xTaskGetTickCount();
					jwt = entry.token;
					return CurierResult::success();
				}
			}
		}

		uint64_t expiresAt = 0;
		CurierResult created = crypto->createVapidJwt(
		    config.vapidConfig,
		    origin,
		    now,
		    kVapidLifetimeSeconds,
		    jwt,
		    expiresAt
		);
		if (!created) {
			return created;
		}

		CurierLock lock(mutex);
		if (!lock) {
			secureClear(jwt);
			return CurierResult::failure(CurierStatus::InternalError, "Curier mutex lock failed");
		}
		CurierJwtCacheEntry *target = nullptr;
		for (CurierJwtCacheEntry &entry : jwtCache) {
			if (entry.clockGeneration == generation && entry.origin == origin) {
				target = &entry;
				break;
			}
			if (target == nullptr && entry.token.empty()) {
				target = &entry;
			}
		}
		if (target == nullptr) {
			target = &jwtCache[0];
			for (CurierJwtCacheEntry &entry : jwtCache) {
				if (entry.lastUsed < target->lastUsed) {
					target = &entry;
				}
			}
		}
		secureClear(target->token);
		target->origin = origin;
		target->token = jwt;
		target->expiresAt = expiresAt;
		target->clockGeneration = generation;
		target->lastUsed = xTaskGetTickCount();
		return CurierResult::success();
	}

	bool waitForRetry(uint32_t delayMs) {
		if (delayMs == 0) {
			return !stopping();
		}
		TickType_t remaining = ticksForTimeout(delayMs);
		while (remaining > 0) {
			const TickType_t started = xTaskGetTickCount();
			uint32_t notification = 0;
			const BaseType_t notified =
			    xTaskNotifyWait(0, std::numeric_limits<uint32_t>::max(), &notification, remaining);
			if (notified != pdTRUE) {
				return !stopping();
			}
			if ((notification & kNotificationStop) != 0U || stopping()) {
				return false;
			}
			const TickType_t elapsed = xTaskGetTickCount() - started;
			if (elapsed >= remaining) {
				return !stopping();
			}
			remaining -= elapsed;
		}
		return !stopping();
	}

	CurierSendResult process(CurierJob &job) {
		std::vector<uint8_t> encryptedBody;
		std::string jwt;
		SecureStringGuard jwtGuard(jwt);
		uint16_t attemptCount = 0;
		while (attemptCount <= static_cast<uint16_t>(config.retry.maxRetries)) {
			attemptCount++;
			const uint8_t reportedAttempts = attemptCount > std::numeric_limits<uint8_t>::max()
			                                     ? std::numeric_limits<uint8_t>::max()
			                                     : static_cast<uint8_t>(attemptCount);
			if (stopping()) {
				return sendResult(
				    false,
				    CurierStatus::Cancelled,
				    "send cancelled during shutdown",
				    reportedAttempts
				);
			}

			uint64_t now = 0;
			uint32_t generation = 0;
			CurierSendResult current;
			std::string retryAfter;
			if (!readClock(now, generation)) {
				current = sendResult(
				    false,
				    CurierStatus::ClockUnavailable,
				    "system time is unavailable",
				    reportedAttempts
				);
			} else {
				CurierResult jwtResult = jwtForOrigin(job.origin, now, generation, jwt);
				if (!jwtResult) {
					current =
					    sendResult(false, jwtResult.status, jwtResult.message, reportedAttempts);
				} else {
					if (encryptedBody.empty()) {
						CurierResult encrypted =
						    crypto->encrypt(job.payload, job.subscription, encryptedBody);
						if (!encrypted) {
							return sendResult(
							    false,
							    encrypted.status,
							    encrypted.message,
							    reportedAttempts
							);
						}
					}
					curier_internal::CurierHttpResponse http = curier_internal::sendWebPushRequest(
					    config,
					    job.subscription,
					    jwt,
					    encryptedBody
					);
					current = http.result;
					current.attempts = reportedAttempts;
					retryAfter = std::move(http.retryAfter);
					if (current.result) {
						return current;
					}
				}
			}

			if (attemptCount > config.retry.maxRetries || stopping()) {
				return current;
			}

			uint32_t retryAfterMs = 0;
			if (!retryAfter.empty() && config.retry.respectRetryAfter) {
				(void)curier_internal::parseRetryAfter(
				    retryAfter.c_str(),
				    now,
				    config.retry.maxDelayMs,
				    retryAfterMs
				);
			}
			CurierRetryContext context;
			context.attempts = reportedAttempts;
			context.status = current.status;
			context.transportError = current.transportError;
			context.statusCode = current.statusCode;
			context.retryAfterMs = retryAfterMs;

			CurierRetryDecision decision;
			if (config.retryPolicy) {
				decision = config.retryPolicy(context);
				if (decision.delayMs > config.retry.maxDelayMs) {
					decision.delayMs = config.retry.maxDelayMs;
				}
			} else {
				decision =
				    curier_internal::defaultRetryDecision(config.retry, context, esp_random());
			}
			if (!decision.retry) {
				return current;
			}
			{
				CurierLock lock(mutex);
				if (lock) {
					diag.retried++;
				}
			}
			if (!waitForRetry(decision.delayMs)) {
				return sendResult(
				    false,
				    CurierStatus::Cancelled,
				    "send cancelled during retry delay",
				    reportedAttempts
				);
			}
		}
		return sendResult(false, CurierStatus::InternalError, "retry loop exhausted");
	}

	void invokeAndRelease(CurierJobPtr job, CurierSendResult result) {
		if (!job) {
			return;
		}
		if (job->callback) {
			job->callback(result);
		}
		secureClear(job->payload);
		secureClear(job->subscription.auth);
		job.reset();
		recordCompletion(result);
	}

	void cancelQueued() {
		while (CurierJobPtr job = popJob()) {
			invokeAndRelease(
			    std::move(job),
			    sendResult(false, CurierStatus::Cancelled, "send cancelled during shutdown")
			);
		}
	}

	void run() {
		while (true) {
			if (stopping()) {
				cancelQueued();
				break;
			}
			CurierJobPtr job = popJob();
			if (job) {
				CurierSendResult result = process(*job);
				if (stopping() && result.status != CurierStatus::Cancelled) {
					result = sendResult(
					    false,
					    CurierStatus::Cancelled,
					    "send cancelled during shutdown",
					    result.attempts
					);
				}
				invokeAndRelease(std::move(job), result);
				continue;
			}

			uint32_t notification = 0;
			(void
			)xTaskNotifyWait(0, std::numeric_limits<uint32_t>::max(), &notification, portMAX_DELAY);
			if ((notification & kNotificationStop) != 0U) {
				cancelQueued();
				break;
			}
		}

		{
			CurierLock lock(mutex);
			if (lock) {
				diag.stackHighWaterMarkBytes = task.stackHighWaterMarkBytes();
			}
		}
		(void)workerReadySignal.give();

		// The owner resets the Strata task after this publication point.
		// No CurierImpl state may be accessed after publishing workerReadySignal.
		suspendForever();
	}

	static void taskEntry(void *argument) {
		auto *impl = static_cast<CurierImpl *>(argument);
		if (impl == nullptr) {
			suspendForever();
		}
		impl->run();
	}

	void clearJwtCacheLocked() {
		for (CurierJwtCacheEntry &entry : jwtCache) {
			secureClear(entry.token);
		}
		jwtCache = {};
	}

	void clearConfigSecretsLocked() {
		secureClear(config.vapidConfig.privateKeyBase64);
		config = CurierConfig{};
	}
};

namespace {

CurierResult validateConfig(const CurierConfig &config) {
	if (!Strata::validMemoryPolicy(config.memory)) {
		return CurierResult::failure(CurierStatus::InvalidConfig, "memory policy is invalid");
	}
	if (config.queueSize == 0 || config.queueSize > kMaxQueueSize) {
		return CurierResult::failure(
		    CurierStatus::InvalidConfig,
		    "queue size must be between 1 and 256"
		);
	}
	if (config.maxPayloadBytes == 0 || config.maxPayloadBytes > 3993) {
		return CurierResult::failure(
		    CurierStatus::InvalidConfig,
		    "payload limit must be between 1 and 3993 bytes"
		);
	}
	if (config.maxEndpointBytes < 9 || config.maxEndpointBytes > static_cast<size_t>(INT_MAX)) {
		return CurierResult::failure(CurierStatus::InvalidConfig, "endpoint size limit is invalid");
	}
	if (!validStackSize(config.stackSize)) {
		return CurierResult::failure(CurierStatus::InvalidConfig, "task stack size is invalid");
	}
	if (config.priority >= configMAX_PRIORITIES) {
		return CurierResult::failure(
		    CurierStatus::InvalidConfig,
		    "task priority exceeds the FreeRTOS limit"
		);
	}
	if (config.coreId != tskNO_AFFINITY &&
	    (config.coreId < 0 || config.coreId >= portNUM_PROCESSORS)) {
		return CurierResult::failure(CurierStatus::InvalidConfig, "task core ID is invalid");
	}
	if (config.taskName.empty() ||
	    config.taskName.size() >= static_cast<size_t>(configMAX_TASK_NAME_LEN)) {
		return CurierResult::failure(CurierStatus::InvalidConfig, "task name is empty or too long");
	}
	if (config.requestTimeoutMs == 0 || config.requestTimeoutMs > static_cast<uint32_t>(INT_MAX)) {
		return CurierResult::failure(
		    CurierStatus::InvalidConfig,
		    "HTTP request timeout is invalid"
		);
	}
	if (config.ttlSeconds > kMaxTtlSeconds) {
		return CurierResult::failure(
		    CurierStatus::InvalidConfig,
		    "Web Push TTL exceeds the RFC limit"
		);
	}
	if (config.retry.maxRetries > kMaxConfiguredRetries) {
		return CurierResult::failure(
		    CurierStatus::InvalidConfig,
		    "retry count exceeds the supported limit"
		);
	}
	if (config.useGlobalCaStore && !config.caCertificatePem.empty()) {
		return CurierResult::failure(
		    CurierStatus::InvalidConfig,
		    "global CA store and custom CA certificate cannot be combined"
		);
	}
	if (!config.useTlsCertBundle && !config.useGlobalCaStore && config.caCertificatePem.empty()) {
		return CurierResult::failure(CurierStatus::InvalidConfig, "a TLS trust source is required");
	}
	return curier_internal::validateRetryConfig(config.retry);
}

CurierResult enqueueLocked(
    CurierImpl &impl,
    const CurierSubscription &subscription,
    std::string origin,
    std::string payload,
    CurierSendCallback callback,
    TaskHandle_t &taskToNotify
) {
	if (impl.inFlight >= impl.config.queueSize || impl.queueCount >= impl.config.queueSize ||
	    !impl.queue) {
		return CurierResult::failure(CurierStatus::QueueFull, "Curier queue is full");
	}
	CurierJobPtr job = Strata::makeUnique<CurierJob>(impl.config.memory.allocation);
	if (!job) {
		return CurierResult::failure(
		    CurierStatus::AllocationFailed,
		    "Curier job allocation failed"
		);
	}
	job->subscription = subscription;
	job->origin = std::move(origin);
	job->payload = std::move(payload);
	job->callback = std::move(callback);

	(*impl.queue)[impl.queueTail] = std::move(job);
	impl.queueTail = (impl.queueTail + 1) % impl.config.queueSize;
	impl.queueCount++;
	impl.inFlight++;
	impl.diag.queueDepth = impl.queueCount;
	impl.diag.inFlight = impl.inFlight;
	if (impl.inFlight > impl.diag.queueHighWaterMark) {
		impl.diag.queueHighWaterMark = impl.inFlight;
	}
	impl.diag.enqueued++;
	taskToNotify = impl.task.handle();
	return CurierResult::success("send queued");
}

} // namespace

Curier::Curier() : _impl(Strata::makeUnique<CurierImpl>(Strata::Placement::Internal)) {
}

Curier::~Curier() {
	if (!_impl) {
		return;
	}
	TaskHandle_t worker = nullptr;
	{
		CurierLock lock(_impl->mutex);
		if (lock) {
			worker = _impl->task.handle();
		}
	}
	if (worker != nullptr && xTaskGetCurrentTaskHandle() == worker) {
		// Destruction from a callback would invalidate the running member function.
		// Fail deterministically instead of leaking CurierImpl or continuing with UAF.
		std::abort();
	}
	while (true) {
		CurierResult result = end(60000);
		if (result.status != CurierStatus::Timeout) {
			break;
		}
	}
}

CurierResult Curier::init(const CurierConfig &config) {
	if (!_impl) {
		return CurierResult::failure(
		    CurierStatus::AllocationFailed,
		    "Curier implementation allocation failed"
		);
	}
	if (!_impl->mutex || !_impl->lifecycleMutex) {
		return CurierResult::failure(
		    CurierStatus::AllocationFailed,
		    "Curier synchronization allocation failed"
		);
	}
	CurierResult configResult = validateConfig(config);
	if (!configResult) {
		return configResult;
	}
	CurierLock lifecycleLock(_impl->lifecycleMutex);
	if (!lifecycleLock) {
		return CurierResult::failure(
		    CurierStatus::InternalError,
		    "Curier lifecycle mutex lock failed"
		);
	}
	CurierLock lock(_impl->mutex);
	if (!lock) {
		return CurierResult::failure(CurierStatus::InternalError, "Curier mutex lock failed");
	}
	if (_impl->lifecycle == CurierLifecycle::Running) {
		return CurierResult::failure(
		    CurierStatus::AlreadyInitialized,
		    "Curier is already initialized"
		);
	}
	if (_impl->lifecycle == CurierLifecycle::Stopping) {
		return CurierResult::failure(CurierStatus::Busy, "Curier is still stopping");
	}

	_impl->config = config;
	_impl->crypto.reset(new (std::nothrow) curier_internal::CurierCrypto());
	if (!_impl->crypto) {
		_impl->clearConfigSecretsLocked();
		return CurierResult::failure(
		    CurierStatus::AllocationFailed,
		    "Curier crypto allocation failed"
		);
	}
	CurierResult cryptoResult = _impl->crypto->validateVapid(config.vapidConfig);
	if (!cryptoResult) {
		_impl->crypto.reset();
		_impl->clearConfigSecretsLocked();
		return cryptoResult;
	}

	_impl->queue.reset();
	_impl->queue.emplace(Strata::Allocator<CurierJobPtr>{config.memory.allocation});
	_impl->queue->resize(config.queueSize);
	_impl->workerReadySignal = Strata::FreeRTOS::BinarySemaphore::create();
	if (!_impl->workerReadySignal) {
		_impl->queue.reset();
		_impl->crypto.reset();
		_impl->clearConfigSecretsLocked();
		return CurierResult::failure(
		    CurierStatus::AllocationFailed,
		    "Curier shutdown signal allocation failed"
		);
	}

	_impl->queueHead = 0;
	_impl->queueTail = 0;
	_impl->queueCount = 0;
	_impl->inFlight = 0;
	_impl->diag = CurierDiagnostics{};
	_impl->diag.queueSize = config.queueSize;
	_impl->diag.allocationPlacement = config.memory.allocation;
	_impl->diag.requestedStackPlacement = config.memory.taskStack;
	_impl->diag.queueStoragePlacement = config.memory.allocation;
	_impl->diag.queueStorageRegion =
	    _impl->queue->empty() ? Strata::Region::Unknown : Strata::regionOf(_impl->queue->data());
	_impl->diag.shutdownSignalControlRegion = _impl->workerReadySignal.controlRegion();
	_impl->clearJwtCacheLocked();
	_impl->lifecycle = CurierLifecycle::Running;

	Strata::FreeRTOS::TaskConfig taskConfig{
	    .name = _impl->config.taskName.c_str(),
	    .stackBytes = config.stackSize,
	    .stackPlacement = config.memory.taskStack,
	    .priority = config.priority,
	    .affinity = config.coreId,
	};
	_impl->task = Strata::FreeRTOS::Task::create(CurierImpl::taskEntry, _impl.get(), taskConfig);
	if (!_impl->task) {
		_impl->lifecycle = CurierLifecycle::Uninitialized;
		_impl->workerReadySignal.reset();
		_impl->queue.reset();
		_impl->crypto.reset();
		_impl->clearConfigSecretsLocked();
		return CurierResult::failure(
		    CurierStatus::TaskCreateFailed,
		    "Curier worker task creation failed"
		);
	}
	_impl->diag.stackRegion = _impl->task.stackRegion();
	return CurierResult::success("Curier initialized");
}

CurierResult Curier::end(uint32_t timeoutMs) {
	if (!_impl) {
		return CurierResult::success("Curier is not initialized");
	}
	CurierLock lifecycleLock(_impl->lifecycleMutex);
	if (!lifecycleLock) {
		return CurierResult::failure(
		    CurierStatus::InternalError,
		    "Curier lifecycle mutex lock failed"
		);
	}

	TaskHandle_t worker = nullptr;
	bool hasReadySignal = false;
	{
		CurierLock lock(_impl->mutex);
		if (!lock) {
			return CurierResult::failure(CurierStatus::InternalError, "Curier mutex lock failed");
		}
		if (_impl->lifecycle == CurierLifecycle::Uninitialized) {
			return CurierResult::success("Curier is not initialized");
		}
		worker = _impl->task.handle();
		if (worker != nullptr && xTaskGetCurrentTaskHandle() == worker) {
			return CurierResult::failure(
			    CurierStatus::Busy,
			    "Curier cannot end from its callback task"
			);
		}
		_impl->lifecycle = CurierLifecycle::Stopping;
		hasReadySignal = static_cast<bool>(_impl->workerReadySignal);
	}

	if (worker != nullptr) {
		(void)xTaskNotify(worker, kNotificationStop, eSetBits);
	}
	if (hasReadySignal && !_impl->workerReadySignal.take(ticksForTimeout(timeoutMs))) {
		return CurierResult::failure(CurierStatus::Timeout, "Curier shutdown timed out");
	}

	// The worker has published its final diagnostics and no longer accesses CurierImpl.
	// Reset the Strata task from this owner task so stack and TCB storage are reclaimed
	// before end() reports success.
	if (worker != nullptr) {
		_impl->task.reset();
	}

	CurierLock lock(_impl->mutex);
	if (!lock) {
		return CurierResult::failure(CurierStatus::InternalError, "Curier mutex lock failed");
	}
	if (_impl->queue) {
		for (CurierJobPtr &job : *_impl->queue) {
			if (job) {
				secureClear(job->payload);
				secureClear(job->subscription.auth);
				job.reset();
			}
		}
		_impl->queue.reset();
	}
	_impl->workerReadySignal.reset();
	_impl->crypto.reset();
	_impl->clearConfigSecretsLocked();
	_impl->queueHead = 0;
	_impl->queueTail = 0;
	_impl->queueCount = 0;
	_impl->inFlight = 0;
	_impl->clearJwtCacheLocked();
	_impl->lifecycle = CurierLifecycle::Uninitialized;
	return CurierResult::success("Curier stopped");
}

bool Curier::initialized() const {
	if (!_impl) {
		return false;
	}
	CurierLock lock(_impl->mutex);
	return lock && _impl->lifecycle == CurierLifecycle::Running;
}

CurierResult Curier::send(
    const CurierSubscription &subscription,
    const CurierPayload &payload,
    CurierSendCallback callback
) {
	if (!_impl) {
		return CurierResult::failure(CurierStatus::NotInitialized, "Curier is not initialized");
	}
	if (!callback) {
		return CurierResult::failure(CurierStatus::InvalidArgument, "send callback is required");
	}
	TaskHandle_t taskToNotify = nullptr;
	CurierResult enqueueResult;
	{
		CurierLock lock(_impl->mutex);
		if (!lock) {
			return CurierResult::failure(CurierStatus::InternalError, "Curier mutex lock failed");
		}
		if (_impl->lifecycle == CurierLifecycle::Stopping) {
			return CurierResult::failure(CurierStatus::Stopping, "Curier is stopping");
		}
		if (_impl->lifecycle != CurierLifecycle::Running || !_impl->crypto || !_impl->queue) {
			return CurierResult::failure(CurierStatus::NotInitialized, "Curier is not initialized");
		}
		std::string serialized;
		CurierResult payloadResult =
		    curier_internal::serializePayload(payload, _impl->config.maxPayloadBytes, serialized);
		if (!payloadResult) {
			return payloadResult;
		}
		std::string origin;
		CurierResult endpointResult = curier_internal::endpointOrigin(
		    subscription.endpoint,
		    _impl->config.maxEndpointBytes,
		    origin
		);
		if (!endpointResult) {
			return endpointResult;
		}
		CurierResult subscriptionResult = _impl->crypto->validateSubscription(subscription);
		if (!subscriptionResult) {
			return subscriptionResult;
		}
		enqueueResult = enqueueLocked(
		    *_impl,
		    subscription,
		    std::move(origin),
		    std::move(serialized),
		    std::move(callback),
		    taskToNotify
		);
	}
	if (enqueueResult && taskToNotify != nullptr) {
		(void)xTaskNotify(taskToNotify, kNotificationWork, eSetBits);
	}
	return enqueueResult;
}

CurierResult Curier::send(
    const CurierSubscription &subscription, const JsonDocument &payload, CurierSendCallback callback
) {
	if (!_impl) {
		return CurierResult::failure(CurierStatus::NotInitialized, "Curier is not initialized");
	}
	if (!callback) {
		return CurierResult::failure(CurierStatus::InvalidArgument, "send callback is required");
	}
	TaskHandle_t taskToNotify = nullptr;
	CurierResult enqueueResult;
	{
		CurierLock lock(_impl->mutex);
		if (!lock) {
			return CurierResult::failure(CurierStatus::InternalError, "Curier mutex lock failed");
		}
		if (_impl->lifecycle == CurierLifecycle::Stopping) {
			return CurierResult::failure(CurierStatus::Stopping, "Curier is stopping");
		}
		if (_impl->lifecycle != CurierLifecycle::Running || !_impl->crypto || !_impl->queue) {
			return CurierResult::failure(CurierStatus::NotInitialized, "Curier is not initialized");
		}
		std::string serialized;
		CurierResult payloadResult =
		    curier_internal::serializePayload(payload, _impl->config.maxPayloadBytes, serialized);
		if (!payloadResult) {
			return payloadResult;
		}
		std::string origin;
		CurierResult endpointResult = curier_internal::endpointOrigin(
		    subscription.endpoint,
		    _impl->config.maxEndpointBytes,
		    origin
		);
		if (!endpointResult) {
			return endpointResult;
		}
		CurierResult subscriptionResult = _impl->crypto->validateSubscription(subscription);
		if (!subscriptionResult) {
			return subscriptionResult;
		}
		enqueueResult = enqueueLocked(
		    *_impl,
		    subscription,
		    std::move(origin),
		    std::move(serialized),
		    std::move(callback),
		    taskToNotify
		);
	}
	if (enqueueResult && taskToNotify != nullptr) {
		(void)xTaskNotify(taskToNotify, kNotificationWork, eSetBits);
	}
	return enqueueResult;
}

CurierResult Curier::setTimeProvider(CurierTimeProvider provider) {
	if (!_impl) {
		return CurierResult::failure(
		    CurierStatus::AllocationFailed,
		    "Curier implementation allocation failed"
		);
	}
	if (!provider) {
		return CurierResult::failure(CurierStatus::InvalidArgument, "time provider is required");
	}
	CurierLock lock(_impl->mutex);
	if (!lock) {
		return CurierResult::failure(CurierStatus::InternalError, "Curier mutex lock failed");
	}
	_impl->timeProvider = std::move(provider);
	_impl->clockGeneration++;
	if (_impl->clockGeneration == 0) {
		_impl->clockGeneration = 1;
	}
	_impl->clearJwtCacheLocked();
	return CurierResult::success("time provider registered");
}

CurierResult Curier::clearTimeProvider() {
	if (!_impl) {
		return CurierResult::failure(
		    CurierStatus::AllocationFailed,
		    "Curier implementation allocation failed"
		);
	}
	CurierLock lock(_impl->mutex);
	if (!lock) {
		return CurierResult::failure(CurierStatus::InternalError, "Curier mutex lock failed");
	}
	_impl->timeProvider = CurierTimeProvider{};
	_impl->clockGeneration++;
	if (_impl->clockGeneration == 0) {
		_impl->clockGeneration = 1;
	}
	_impl->clearJwtCacheLocked();
	return CurierResult::success("standard system time restored");
}

CurierDiagnostics Curier::diagnostics() const {
	if (!_impl) {
		return CurierDiagnostics{};
	}
	CurierLock lock(_impl->mutex);
	if (!lock) {
		return CurierDiagnostics{};
	}
	CurierDiagnostics result = _impl->diag;
	result.queueDepth = _impl->queueCount;
	result.inFlight = _impl->inFlight;
	if (_impl->task && _impl->lifecycle == CurierLifecycle::Running) {
		result.stackHighWaterMarkBytes = _impl->task.stackHighWaterMarkBytes();
		result.stackRegion = _impl->task.stackRegion();
	}
	return result;
}

const char *Curier::statusToString(CurierStatus status) const {
	switch (status) {
	case CurierStatus::Ok:
		return "Ok";
	case CurierStatus::NotInitialized:
		return "NotInitialized";
	case CurierStatus::AlreadyInitialized:
		return "AlreadyInitialized";
	case CurierStatus::Stopping:
		return "Stopping";
	case CurierStatus::InvalidConfig:
		return "InvalidConfig";
	case CurierStatus::InvalidArgument:
		return "InvalidArgument";
	case CurierStatus::InvalidVapidSubject:
		return "InvalidVapidSubject";
	case CurierStatus::InvalidVapidKey:
		return "InvalidVapidKey";
	case CurierStatus::VapidKeyMismatch:
		return "VapidKeyMismatch";
	case CurierStatus::InvalidSubscription:
		return "InvalidSubscription";
	case CurierStatus::InvalidPayload:
		return "InvalidPayload";
	case CurierStatus::PayloadTooLarge:
		return "PayloadTooLarge";
	case CurierStatus::QueueFull:
		return "QueueFull";
	case CurierStatus::AllocationFailed:
		return "AllocationFailed";
	case CurierStatus::QueueCreateFailed:
		return "QueueCreateFailed";
	case CurierStatus::TaskCreateFailed:
		return "TaskCreateFailed";
	case CurierStatus::ClockUnavailable:
		return "ClockUnavailable";
	case CurierStatus::CryptoError:
		return "CryptoError";
	case CurierStatus::JwtError:
		return "JwtError";
	case CurierStatus::TransportError:
		return "TransportError";
	case CurierStatus::HttpError:
		return "HttpError";
	case CurierStatus::Cancelled:
		return "Cancelled";
	case CurierStatus::Busy:
		return "Busy";
	case CurierStatus::Timeout:
		return "Timeout";
	case CurierStatus::InternalError:
		return "InternalError";
	}
	return "Unknown";
}
