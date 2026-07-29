#include <Arduino.h>
#include <ArduinoJson.h>
#include <Curier.h>

Curier curier;

void setup() {
	CurierConfig config;
	config.stackType = CurierStackType::Auto;
	config.retry.mode = CurierRetryMode::Exponential;

	JsonDocument document;
	document["title"] = "compile";
	document["body"] = "smoke";

	(void)curier.setTimeProvider([](uint64_t &epochSeconds) {
		epochSeconds = 1710000000;
		return true;
	});
	(void)curier.clearTimeProvider();
	(void)curier.diagnostics();
}

void loop() {
	delay(1000);
}
