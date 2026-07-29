#include <Arduino.h>
#include <ArduinoJson.h>
#include <Curier.h>

Curier curier;

void setup() {
	Serial.begin(115200);

	CurierConfig config;
	config.vapidConfig.subject = "mailto:notify@example.com";
	config.vapidConfig.publicKeyBase64 = "BAvapidPublicKeyBase64Url...";
	config.vapidConfig.privateKeyBase64 = "vapidPrivateKeyBase64Url...";

	CurierResult initResult = curier.init(config);
	if (!initResult) {
		Serial.println(initResult.message);
		return;
	}

	CurierSubscription subscription;
	subscription.endpoint = "https://fcm.googleapis.com/fcm/send/...";
	subscription.p256dh = "BME...";
	subscription.auth = "nsa...";

	JsonDocument document;
	document["title"] = "Hello";
	document["body"] = "ESP32";
	document["tag"] = "demo";

	CurierResult result = curier.send(subscription, document, [](CurierSendResult sendResult) {
		Serial.printf("Push result: %s (status %d)\n", sendResult.message, sendResult.statusCode);
	});
	if (!result) {
		Serial.println(result.message);
	}
}

void loop() {
	delay(1000);
}
