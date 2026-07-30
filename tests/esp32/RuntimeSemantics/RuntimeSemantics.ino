#include <Arduino.h>
#include <Curier.h>
#include <internal/CurierHttp.h>

#include <atomic>
#include <functional>

extern "C" {
#include "freertos/semphr.h"
}

namespace {

std::atomic<bool> passed{true};

void expect(bool condition, const char *message) {
	if (!condition) {
		passed.store(false);
		Serial.println(message);
	}
}

void expectResult(const CurierResult &result, const char *message) {
	expect(result.result, message);
}

bool waitUntil(const std::function<bool()> &condition, uint32_t timeoutMs = 5000) {
	const uint32_t started = millis();
	while (!condition()) {
		if (millis() - started >= timeoutMs) {
			return false;
		}
		delay(1);
	}
	return true;
}

CurierConfig config(size_t queueSize = 2) {
	CurierConfig value;
	value.vapidConfig.subject = "mailto:test@example.com";
	value.vapidConfig.publicKeyBase64 =
	    "BAQrcGSCN0uGkDRKNe9p_5Uvlc4dPOZsRFLpuUplt8tlUzM4XmsHHFnLrb6CqYRgH3f2XyVJJlJrbKksBFtU_fw";
	value.vapidConfig.privateKeyBase64 = "Qu8KQ-mRYC-ACZNsrftkSEaMv4qFAP9b-6Q7tK3lZX4";
	value.queueSize = queueSize;
	value.stackSize = 8192;
	value.stackType = CurierStackType::Internal;
	value.retry.mode = CurierRetryMode::Disabled;
	return value;
}

CurierSubscription subscription() {
	CurierSubscription value;
	value.endpoint = "https://push.example.net/push/test";
	value.p256dh =
	    "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4";
	value.auth = "BTBZMqHH6r4Tts7J_aSIgg";
	return value;
}

CurierPayload payload(const char *body) {
	CurierPayload value;
	value.title = "Runtime";
	value.body = body;
	return value;
}

curier_internal::CurierHttpResponse httpSuccess() {
	curier_internal::CurierHttpResponse response;
	response.result.result = true;
	response.result.status = CurierStatus::Ok;
	response.result.message = "test accepted";
	response.result.statusCode = 201;
	return response;
}

curier_internal::CurierHttpResponse httpFailure(int statusCode, const char *retryAfter = nullptr) {
	curier_internal::CurierHttpResponse response;
	response.result.result = false;
	response.result.status = CurierStatus::HttpError;
	response.result.message = "test HTTP failure";
	response.result.statusCode = statusCode;
	if (retryAfter != nullptr) {
		response.retryAfter = retryAfter;
	}
	return response;
}

void installClock(Curier &curier) {
	const CurierResult result = curier.setTimeProvider([](uint64_t &epochSeconds) {
		epochSeconds = 1750000000;
		return true;
	});
	expectResult(result, "time provider registration failed");
}

void testQueueBoundAndExactlyOnce() {
	Curier curier;
	installClock(curier);
	SemaphoreHandle_t entered = xSemaphoreCreateBinary();
	SemaphoreHandle_t release = xSemaphoreCreateBinary();
	std::atomic<int> callbacks{0};

	curier_internal::setHttpTransportForTesting(
	    [entered, release](
	        const CurierConfig &,
	        const CurierSubscription &,
	        const std::string &,
	        const std::vector<uint8_t> &
	    ) {
		    xSemaphoreGive(entered);
		    xSemaphoreTake(release, portMAX_DELAY);
		    return httpSuccess();
	    }
	);

	expectResult(curier.init(config(1)), "queue test init failed");
	const CurierResult firstQueued =
	    curier.send(subscription(), payload("first"), [&callbacks](CurierSendResult result) {
		    expect(result.ok(), "first callback did not succeed");
		    callbacks.fetch_add(1);
	    });
	expectResult(firstQueued, "first send was not queued");
	expect(xSemaphoreTake(entered, pdMS_TO_TICKS(5000)) == pdTRUE, "transport did not start");

	const CurierResult rejected =
	    curier.send(subscription(), payload("second"), [&callbacks](CurierSendResult) {
		    callbacks.fetch_add(100);
	    });
	expect(!rejected && rejected.status == CurierStatus::QueueFull, "active job was not bounded");
	xSemaphoreGive(release);
	expect(
	    waitUntil([&curier, &callbacks]() {
		    return callbacks.load() == 1 && curier.diagnostics().completed == 1;
	    }),
	    "callback or completion diagnostics were not exactly once"
	);
	expectResult(curier.end(5000), "queue test shutdown failed");

	vSemaphoreDelete(release);
	vSemaphoreDelete(entered);
	curier_internal::clearHttpTransportForTesting();
}

void testCallbackRequeueAndBusyEnd() {
	Curier curier;
	installClock(curier);
	std::atomic<int> callbacks{0};
	std::atomic<CurierStatus> callbackEndStatus{CurierStatus::InternalError};
	curier_internal::setHttpTransportForTesting(
	    [](const CurierConfig &, const CurierSubscription &, const std::string &, const std::vector<uint8_t> &) {
		    return httpSuccess();
	    }
	);

	expectResult(curier.init(config(2)), "requeue test init failed");
	const CurierResult firstQueued = curier.send(
	    subscription(),
	    payload("first"),
	    [&curier, &callbacks, &callbackEndStatus](CurierSendResult first) {
		    expect(first.ok(), "requeue first callback failed");
		    callbackEndStatus.store(curier.end(0).status);
		    const CurierResult queued = curier.send(
		        subscription(),
		        payload("second"),
		        [&callbacks](CurierSendResult second) {
			        expect(second.ok(), "requeue second callback failed");
			        callbacks.fetch_add(1);
		        }
		    );
		    expectResult(queued, "callback could not enqueue with available capacity");
		    callbacks.fetch_add(1);
	    }
	);
	expectResult(firstQueued, "requeue first send failed");
	expect(waitUntil([&callbacks]() { return callbacks.load() == 2; }), "requeue callbacks timed out");
	expect(callbackEndStatus.load() == CurierStatus::Busy, "callback end() did not return Busy");
	expectResult(curier.end(5000), "requeue test shutdown failed");
	curier_internal::clearHttpTransportForTesting();
}

void testRetryAndCancellation() {
	Curier curier;
	installClock(curier);
	std::atomic<int> attempts{0};
	std::atomic<int> callbacks{0};
	CurierSendResult terminal;

	curier_internal::setHttpTransportForTesting(
	    [&attempts](
	        const CurierConfig &,
	        const CurierSubscription &,
	        const std::string &,
	        const std::vector<uint8_t> &
	    ) {
		    const int attempt = attempts.fetch_add(1) + 1;
		    return attempt < 3 ? httpFailure(503) : httpSuccess();
	    }
	);
	CurierConfig retryConfig = config();
	retryConfig.retry.mode = CurierRetryMode::Fixed;
	retryConfig.retry.maxRetries = 3;
	retryConfig.retry.baseDelayMs = 1;
	retryConfig.retry.maxDelayMs = 1;
	retryConfig.retry.jitterPercent = 0;
	expectResult(curier.init(retryConfig), "retry test init failed");
	const CurierResult retryQueued =
	    curier.send(subscription(), payload("retry"), [&callbacks, &terminal](CurierSendResult result) {
		    terminal = result;
		    callbacks.fetch_add(1);
	    });
	expectResult(retryQueued, "retry send failed");
	expect(waitUntil([&callbacks]() { return callbacks.load() == 1; }), "retry callback timed out");
	expect(terminal.ok() && terminal.attempts == 3, "retry terminal result is incorrect");
	expect(curier.diagnostics().retried == 2, "retry diagnostics are incorrect");
	expectResult(curier.end(5000), "retry test shutdown failed");

	attempts.store(0);
	callbacks.store(0);
	terminal = CurierSendResult{};
	curier_internal::setHttpTransportForTesting(
	    [&attempts](
	        const CurierConfig &,
	        const CurierSubscription &,
	        const std::string &,
	        const std::vector<uint8_t> &
	    ) {
		    attempts.fetch_add(1);
		    return httpFailure(503);
	    }
	);
	retryConfig.retry.baseDelayMs = 60000;
	retryConfig.retry.maxDelayMs = 60000;
	expectResult(curier.init(retryConfig), "cancellation test init failed");
	const CurierResult cancellationQueued =
	    curier.send(subscription(), payload("cancel"), [&callbacks, &terminal](CurierSendResult result) {
		    terminal = result;
		    callbacks.fetch_add(1);
	    });
	expectResult(cancellationQueued, "cancellation send failed");
	expect(waitUntil([&attempts]() { return attempts.load() == 1; }), "cancellation attempt timed out");
	const uint32_t started = millis();
	expectResult(curier.end(5000), "retry cancellation shutdown failed");
	expect(millis() - started < 2000, "retry wait did not wake promptly for shutdown");
	expect(callbacks.load() == 1, "cancelled callback was not exactly once");
	expect(terminal.status == CurierStatus::Cancelled, "retry cancellation status is incorrect");
	curier_internal::clearHttpTransportForTesting();
}

void testShutdownTimeoutRecovery() {
	Curier curier;
	installClock(curier);
	SemaphoreHandle_t entered = xSemaphoreCreateBinary();
	SemaphoreHandle_t release = xSemaphoreCreateBinary();
	std::atomic<int> callbacks{0};
	CurierSendResult terminal;

	curier_internal::setHttpTransportForTesting(
	    [entered, release](
	        const CurierConfig &,
	        const CurierSubscription &,
	        const std::string &,
	        const std::vector<uint8_t> &
	    ) {
		    xSemaphoreGive(entered);
		    xSemaphoreTake(release, portMAX_DELAY);
		    return httpSuccess();
	    }
	);
	expectResult(curier.init(config()), "timeout test init failed");
	const CurierResult timeoutQueued =
	    curier.send(subscription(), payload("timeout"), [&callbacks, &terminal](CurierSendResult result) {
		    terminal = result;
		    callbacks.fetch_add(1);
	    });
	expectResult(timeoutQueued, "timeout send failed");
	expect(xSemaphoreTake(entered, pdMS_TO_TICKS(5000)) == pdTRUE, "timeout transport did not start");
	const CurierResult timedOut = curier.end(1);
	expect(!timedOut && timedOut.status == CurierStatus::Timeout, "end() did not preserve timeout state");
	expect(!curier.initialized(), "Stopping state was reported as initialized");
	xSemaphoreGive(release);
	expectResult(curier.end(5000), "second end() did not finish cleanup");
	expect(callbacks.load() == 1, "timeout recovery callback was not exactly once");
	expect(terminal.status == CurierStatus::Cancelled, "timeout recovery did not cancel active work");

	vSemaphoreDelete(release);
	vSemaphoreDelete(entered);
	curier_internal::clearHttpTransportForTesting();
}

} // namespace

void setup() {
	Serial.begin(115200);
	testQueueBoundAndExactlyOnce();
	testCallbackRequeueAndBusyEnd();
	testRetryAndCancellation();
	testShutdownTimeoutRecovery();
	Serial.println(passed.load() ? "Curier runtime semantics passed" : "Curier runtime semantics failed");
}

void loop() {
	delay(1000);
}
