#include <Arduino.h>
#include <Curier.h>

#include <ctime>

Curier curier;

void setup() {
	Serial.begin(115200);

	curier.setTimeProvider([](uint64_t &epochSeconds) {
		const time_t now = std::time(nullptr);
		if (now <= 0) {
			return false;
		}
		epochSeconds = static_cast<uint64_t>(now);
		return true;
	});

	CurierConfig config;
	config.vapidConfig.subject = "mailto:notify@example.com";
	config.vapidConfig.publicKeyBase64 = "BAvapidPublicKeyBase64Url...";
	config.vapidConfig.privateKeyBase64 = "vapidPrivateKeyBase64Url...";
	config.retry.mode = CurierRetryMode::Exponential;
	config.retry.maxRetries = 4;
	config.retry.baseDelayMs = 1000;
	config.retry.maxDelayMs = 30000;
	config.retry.jitterPercent = 15;
	config.retry.respectRetryAfter = true;

	CurierResult result = curier.init(config);
	if (!result) {
		Serial.println(result.message);
	}
}

void loop() {
	delay(1000);
}
