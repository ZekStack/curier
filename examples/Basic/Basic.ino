#include <Arduino.h>
#include <Curier.h>

Curier curier;

void setup() {
	Serial.begin(115200);

	CurierVapid vapid;
	vapid.subject = "mailto:notify@example.com";
	vapid.publicKeyBase64 = "BAvapidPublicKeyBase64Url...";
	vapid.privateKeyBase64 = "vapidPrivateKeyBase64Url...";

	CurierConfig config;
	config.vapidConfig = vapid;
	config.memory.allocation = Strata::Placement::Default;
	config.memory.taskStack = Strata::Placement::PreferExternal;
	config.stackSize = 4096;
	config.coreId = tskNO_AFFINITY;
	config.queueSize = 16;
	config.maxPayloadBytes = 3993;

	CurierResult initResult = curier.init(config);
	if (!initResult) {
		Serial.println(initResult.message);
		return;
	}

	CurierSubscription subscription;
	subscription.endpoint = "https://fcm.googleapis.com/fcm/send/...";
	subscription.p256dh = "BME...";
	subscription.auth = "nsa...";

	CurierPayload payload;
	payload.title = "Hello";
	payload.body = "ESP32";
	payload.tag = "demo";
	payload.icon = "https://example.com/icon.png";

	CurierResult result = curier.send(subscription, payload, [](CurierSendResult sendResult) {
		if (!sendResult.ok()) {
			Serial
			    .printf("Push failed: %s (status %d)\n", sendResult.message, sendResult.statusCode);
			return;
		}
		Serial.printf("Push accepted (status %d)\n", sendResult.statusCode);
	});
	if (!result) {
		Serial.println(result.message);
	}
}

void loop() {
	delay(1000);
}
