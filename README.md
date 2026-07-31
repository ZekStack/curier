# Curier

Curier is an asynchronous Web Push client for Arduino ESP32 with VAPID
authentication, RFC 8291 `aes128gcm` payload encryption, and bounded work
queues.

Curier is designed for firmware that needs to enqueue browser notifications
without performing TLS, cryptography, or retry delays on the caller's task.

[![CI](https://github.com/ZekStack/curier/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/curier/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/curier?sort=semver)](https://github.com/ZekStack/curier/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Curier?

* **Modern Web Push** - implements RFC 8291/8188 `aes128gcm`; the legacy
  `aesgcm` encoding is intentionally not supported.
* **Async ownership** - accepted subscriptions, payloads, and callbacks are
  copied into a bounded queue and processed by Curier's own FreeRTOS task.
* **VAPID authentication** - validates the configured P-256 key pair and signs
  ES256 JWTs with a small per-origin cache.
* **Explicit results** - initialization and enqueue operations return
  `CurierResult`; terminal delivery uses `CurierSendResult`.
* **Controlled retries** - fixed, exponential, or application-defined retry
  policies can honor `Retry-After`.
* **Time integration** - uses standard system time by default, while allowing a
  provider to be registered before or after `init()`.

## Install

### PlatformIO

Curier is built for Arduino ESP32 and depends on ArduinoJson v7.

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/curier.git
  bblanchon/ArduinoJson@>=7.0.0

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

### Arduino IDE

Curier is not published to Arduino Library Manager yet.

Install it by downloading the repository ZIP or cloning it into:

```txt
Arduino/libraries/Curier
```

Install ArduinoJson v7 through Library Manager.

## Quick start

```cpp
#include <Arduino.h>
#include <Curier.h>

Curier curier;

void setup() {
    CurierConfig config;
    config.vapidConfig.subject = "mailto:notify@example.com";
    config.vapidConfig.publicKeyBase64 = "BAvapidPublicKeyBase64Url...";
    config.vapidConfig.privateKeyBase64 = "vapidPrivateKeyBase64Url...";
    config.stackType = CurierStackType::Auto;
    config.stackSize = 4096;
    config.coreId = tskNO_AFFINITY;
    config.queueSize = 16;
    config.maxPayloadBytes = 3993;
    config.ttlSeconds = 2419200;

    CurierResult initResult = curier.init(config);
    if (!initResult) {
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

    CurierResult queued = curier.send(
        subscription,
        payload,
        [](CurierSendResult result) {
            if (!result.ok()) {
                ESP_LOGE(
                    "WEBPUSH",
                    "Push failed: %s (status %d)",
                    result.message,
                    result.statusCode
                );
                return;
            }
            ESP_LOGI("WEBPUSH", "Push OK (status %d)", result.statusCode);
        }
    );

    if (!queued) {
        ESP_LOGE("WEBPUSH", "Push was not queued: %s", queued.message);
    }
}

void loop() {
    delay(1000);
}
```

## Important notes

> [!IMPORTANT]
> Curier callbacks run on Curier's worker task. Keep callbacks short and protect
> application state shared with other tasks.

* A successful `send()` means the job was accepted, not that the push service
  accepted it. The callback reports the terminal outcome.
* Every accepted job receives exactly one callback before a successful `end()`
  returns. Queued and retrying jobs are completed as `Cancelled` during
  shutdown.
* Calling `end()` from Curier's callback returns `Busy`. Destroying the Curier
  instance from that callback is a fatal programming error; schedule destruction
  on another application task.
* A successful `end()` means the worker task has been deleted with the matching
  normal or capability-aware API and all owned runtime state has been released.
  A timeout preserves the `Stopping` state so a later call can finish cleanup.
* `queueSize` bounds all accepted jobs, including the active job.
* Subscriptions and JSON are validated and copied before `send()` returns.
* Subscription endpoint authorities are validated before the HTTP request and
  VAPID audience are constructed. Malformed ports and unbracketed IPv6 are
  rejected.
* The default clock is `std::time(nullptr)`. Configure system time before
  delivery, for example with Tempo, SNTP, or another clock owner.
* HTTPS verification is enabled by default through the ESP-IDF certificate
  bundle when that feature is available.
* A VAPID private key is a device secret. Do not log it or include it in public
  firmware repositories.
* Callbacks, time providers, and retry policies must not throw exceptions.

## Time and retry behavior

Registering a time provider is optional. The provider may be installed before
or after initialization:

```cpp
curier.setTimeProvider([](uint64_t &epochSeconds) {
    epochSeconds = static_cast<uint64_t>(std::time(nullptr));
    return epochSeconds > 0;
});
```

Changing or clearing the provider invalidates cached VAPID JWTs. The provider
runs on the worker task and must be thread-safe and non-blocking.

The default retry policy uses bounded exponential backoff with jitter for clock
unavailability, transport errors, HTTP 408, HTTP 429, and HTTP 5xx responses.
Permanent HTTP failures such as 400, 401, 403, and 404 are not retried.

```cpp
config.retry.mode = CurierRetryMode::Exponential;
config.retry.maxRetries = 5;
config.retry.baseDelayMs = 1500;
config.retry.maxDelayMs = 15000;
config.retry.jitterPercent = 20;
config.retry.respectRetryAfter = true;
```

See [`docs/retries.md`](docs/retries.md) for a custom policy example.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Initialize Curier and send a typed payload. |
| `ArduinoJson` | Send a validated ArduinoJson v7 document. |
| `CustomTimeAndRetry` | Register a clock provider and custom retry policy. |

Start with:

```txt
examples/Basic
```

## Documentation

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | VAPID, subscription, clock, and first-send setup. |
| [`docs/api.md`](docs/api.md) | Public API, result types, payloads, and callbacks. |
| [`docs/configuration.md`](docs/configuration.md) | Queue, task, payload, TLS, TTL, and retry settings. |
| [`docs/concurrency.md`](docs/concurrency.md) | Worker ownership, callback context, and shutdown guarantees. |
| [`docs/retries.md`](docs/retries.md) | Default and custom retry decisions. |
| [`docs/protocol.md`](docs/protocol.md) | Supported Web Push protocol and wire behavior. |
| [`docs/security.md`](docs/security.md) | Key handling, endpoint validation, TLS, and secret boundaries. |
| [`docs/memory.md`](docs/memory.md) | Queue ownership, transient allocations, and stack qualification. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common setup and delivery failures. |

## API overview

```cpp
CurierResult init(const CurierConfig &config);
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
CurierResult end(uint32_t timeoutMs = 5000);
CurierDiagnostics diagnostics() const;
```

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Language | C++20 |
| Networking | ESP-IDF `esp_http_client` |
| Encryption | RFC 8291/8188 `aes128gcm` only |
| Authentication | VAPID ES256 |
| JSON | ArduinoJson `>= 7.0.0` |
| HTTPS | Certificate bundle, global CA store, or custom PEM |
| Task stack | Internal RAM, PSRAM where supported, or automatic fallback |
| Exceptions | Not used by Curier |
| Version | `0.1.0` |

## Configuration

The defaults are intentionally bounded:

```cpp
CurierConfig config;
config.queueSize = 16;
config.maxPayloadBytes = 3993;
config.maxEndpointBytes = 2048;
config.stackSize = 4096;
config.priority = 1;
config.coreId = tskNO_AFFINITY;
config.stackType = CurierStackType::Auto;
config.requestTimeoutMs = 10000;
config.ttlSeconds = 2419200;
```

`maxPayloadBytes` cannot exceed the 3993-byte single-record limit used by
Curier. Encrypted bodies use a 4096-byte RFC 8188 record size.

## Error handling

```cpp
CurierResult queued = curier.send(subscription, payload, onPushComplete);
if (!queued) {
    ESP_LOGE("WEBPUSH", "%s", queued.message);
}
```

Transport and delivery details are separate in the terminal result:

* `result.status` describes the Curier-level outcome.
* `result.transportError` contains the ESP-IDF transport error.
* `result.statusCode` contains the HTTP response code when available.
* `result.attempts` is the number of delivery attempts.

## Release qualification

The host suite compares Curier's encrypted body byte-for-byte with the RFC 8291
Appendix A vector and independently verifies generated VAPID ES256 signatures.
Target sketches exercise queue bounds, callbacks, retries, cancellation,
timeout recovery, and repeated Internal, Auto, and PSRAM lifecycle cleanup.
Run the target sketches on the production board before tagging a release.

## License

MIT - see [`LICENSE.md`](LICENSE.md) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## ZekStack

Part of the ZekStack ESP32 library stack.

ZekStack libraries are designed to provide small, reusable building blocks for
ESP32 applications.
