#include <Arduino.h>
#include <Curier.h>

Curier curier;
bool passed = true;

void expect(bool condition, const char *message) {
	if (!condition) {
		passed = false;
		Serial.println(message);
	}
}

CurierConfig testConfig() {
	CurierConfig config;
	config.vapidConfig.subject = "mailto:test@example.com";
	config.vapidConfig.publicKeyBase64 =
	    "BAQrcGSCN0uGkDRKNe9p_5Uvlc4dPOZsRFLpuUplt8tlUzM4XmsHHFnLrb6CqYRgH3f2XyVJJlJrbKksBFtU_fw";
	config.vapidConfig.privateKeyBase64 = "Qu8KQ-mRYC-ACZNsrftkSEaMv4qFAP9b-6Q7tK3lZX4";
	config.queueSize = 2;
	config.stackSize = 6144;
	config.stackType = CurierStackType::Internal;
	config.retry.mode = CurierRetryMode::Disabled;
	return config;
}
void setup() {
	Serial.begin(115200);

	CurierResult providerResult = curier.setTimeProvider([](uint64_t &epochSeconds) {
		epochSeconds = 1750000000;
		return true;
	});
	expect(providerResult.result, "provider registration before init failed");

	CurierResult initialized = curier.init(testConfig());
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

	initialized = curier.init(testConfig());
	expect(initialized.result, "reinitialization failed");
	expect(curier.end(5000).result, "second shutdown failed");

	Serial.println(passed ? "Curier lifecycle smoke passed" : "Curier lifecycle smoke failed");
}

void loop() {
	delay(1000);
}
