#include <Arduino.h>
#include <Curier.h>

extern "C" {
#include "esp_heap_caps.h"
}

Curier curier;
bool passed = true;

constexpr size_t LifecycleCycles = 128;
constexpr size_t HeapToleranceBytes = 512;

void expect(bool condition, const char *message) {
	if (!condition) {
		passed = false;
		Serial.println(message);
	}
}

CurierConfig testConfig(Strata::Placement stackPlacement) {
	CurierConfig config;
	config.vapidConfig.subject = "mailto:test@example.com";
	config.vapidConfig.publicKeyBase64 =
	    "BAQrcGSCN0uGkDRKNe9p_5Uvlc4dPOZsRFLpuUplt8tlUzM4XmsHHFnLrb6CqYRgH3f2XyVJJlJrbKksBFtU_fw";
	config.vapidConfig.privateKeyBase64 = "Qu8KQ-mRYC-ACZNsrftkSEaMv4qFAP9b-6Q7tK3lZX4";
	config.queueSize = 2;
	config.stackSize = 6144;
	config.memory.allocation = Strata::Placement::Default;
	config.memory.taskStack = stackPlacement;
	config.retry.mode = CurierRetryMode::Disabled;
	return config;
}

bool runLifecycleCycles(Strata::Placement stackPlacement, const char *label) {
	CurierConfig config = testConfig(stackPlacement);

	// Warm up Mbed TLS and standard-library allocations before taking the baseline.
	if (!curier.init(config) || !curier.end(5000)) {
		Serial.printf("%s warm-up failed\n", label);
		return false;
	}

	const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	const size_t psramBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

	for (size_t cycle = 0; cycle < LifecycleCycles; ++cycle) {
		CurierResult initialized = curier.init(config);
		if (!initialized) {
			Serial.printf("%s init failed at cycle %u: %s\n", label, cycle, initialized.message);
			return false;
		}
		CurierDiagnostics diagnostics = curier.diagnostics();
		if (diagnostics.requestedStackPlacement != stackPlacement) {
			Serial.printf("%s requested placement mismatch at cycle %u\n", label, cycle);
			return false;
		}
		CurierResult stopped = curier.end(5000);
		if (!stopped) {
			Serial.printf("%s end failed at cycle %u: %s\n", label, cycle, stopped.message);
			return false;
		}
		if (curier.initialized()) {
			Serial.printf("%s remained initialized at cycle %u\n", label, cycle);
			return false;
		}
	}

	const size_t internalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	const size_t psramAfter = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	Serial.printf(
	    "%s lifecycle: internal %u -> %u, psram %u -> %u\n",
	    label,
	    internalBefore,
	    internalAfter,
	    psramBefore,
	    psramAfter
	);

	if (internalAfter + HeapToleranceBytes < internalBefore) {
		Serial.printf("%s leaked internal heap\n", label);
		return false;
	}
	if (psramBefore > 0 && psramAfter + HeapToleranceBytes < psramBefore) {
		Serial.printf("%s leaked PSRAM heap\n", label);
		return false;
	}
	return true;
}

void setup() {
	Serial.begin(115200);

	CurierResult providerResult = curier.setTimeProvider([](uint64_t &epochSeconds) {
		epochSeconds = 1750000000;
		return true;
	});
	expect(providerResult.result, "provider registration before init failed");

	CurierResult initialized = curier.init(testConfig(Strata::Placement::Internal));
	expect(initialized.result, "initialization failed");
	expect(curier.initialized(), "initialized state was not published");

	providerResult = curier.setTimeProvider([](uint64_t &epochSeconds) {
		epochSeconds = 1750000001;
		return true;
	});
	expect(providerResult.result, "provider replacement after init failed");

	CurierSubscription invalidSubscription;
	invalidSubscription.endpoint = "http://example.com/push";
	invalidSubscription.p256dh = "invalid";
	invalidSubscription.auth = "invalid";

	CurierPayload payload;
	payload.title = "Lifecycle";
	payload.body = "Smoke";

	CurierResult rejected =
	    curier.send(invalidSubscription, payload, [](CurierSendResult) { passed = false; });
	expect(
	    !rejected && rejected.status == CurierStatus::InvalidSubscription,
	    "invalid subscription was not rejected before queueing"
	);

	expect(curier.end(5000).result, "shutdown failed");
	expect(!curier.initialized(), "shutdown state was not published");

	expect(
	    runLifecycleCycles(Strata::Placement::Internal, "internal"),
	    "internal lifecycle stress failed"
	);
	expect(
	    runLifecycleCycles(Strata::Placement::PreferExternal, "prefer-external"),
	    "prefer-external lifecycle stress failed"
	);

	if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
		expect(
		    runLifecycleCycles(Strata::Placement::RequireExternal, "require-external"),
		    "required-external lifecycle stress failed"
		);
	}

	Serial.println(passed ? "Curier lifecycle smoke passed" : "Curier lifecycle smoke failed");
}

void loop() {
	delay(1000);
}
