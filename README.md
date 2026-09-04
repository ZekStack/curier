# Curier

Curier is an asynchronous Web Push client for Arduino ESP32 with VAPID
authentication, RFC 8291 `aes128gcm` payload encryption, bounded work queues,
and Strata-backed memory ownership.

Curier is designed for firmware that needs to enqueue browser notifications
without performing TLS, cryptography, or retry delays on the caller's task.
Curier owns Web Push orchestration and lifecycle policy while
[Strata](https://github.com/ZekStack/strata) owns memory placement and low-level
FreeRTOS storage.

[![CI](https://github.com/ZekStack/curier/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/curier/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/curier?sort=semver)](https://github.com/ZekStack/curier/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Curier?

* **Modern Web Push** - implements RFC 8291/8188 `aes128gcm`; the legacy
  `aesgcm` encoding is intentionally not supported.
* **Async ownership** - accepted subscriptions, payloads, and callbacks are
  copied into a bounded queue and processed by Curier's own worker task.
* **Consistent memory policy** - `Strata::MemoryPolicy` controls Curier-owned
  movable runtime storage and worker-stack placement.
* **Strata-owned FreeRTOS storage** - task stack/TCB, recursive mutexes, and the
  shutdown semaphore use Strata ownership primitives.
* **VAPID authentication** - validates the configured P-256 key pair and signs
  ES256 JWTs with a small per-origin cache.
* **Controlled retries** - fixed, exponential, or application-defined retry
  policies can honor `Retry-After`.
* **Runtime visibility** - diagnostics separate requested placement from the
  observed memory region.

## Dependencies

Curier `v0.2.0` requires:

* Strata `v0.1.2`
* ArduinoJson `>= 7.0.0`

### PlatformIO

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/curier.git#v0.2.0
  bblanchon/ArduinoJson@>=7.0.0

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

Curier's `library.json` pins Strata `v0.1.2`, so PlatformIO resolves it as a
transitive dependency.

### Arduino IDE

Curier and Strata are not published to Arduino Library Manager yet. Install both
repositories into the Arduino libraries directory and install ArduinoJson v7
through Library Manager:

```text
Arduino/libraries/Strata
Arduino/libraries/Curier
```

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

    config.memory.allocation = Strata::Placement::Default;
    config.memory.taskStack = Strata::Placement::PreferExternal;
    config.stackSize = 4096;
    config.coreId = tskNO_AFFINITY;
    config.queueSize = 16;
    config.maxPayloadBytes = 3993;
    config.ttlSeconds = 2419200;

    CurierResult initResult = curier.init(config);
    if (!initResult) {
        ESP_LOGE("WEBPUSH", "Curier init failed: %s", initResult.message);
        return;
    }

    CurierSubscription subscription;
    subscription.endpoint = "https://fcm.googleapis.com/fcm/send/...";
    subscription.p256dh = "BME...";
    subscription.auth = "nsa...";

    CurierPayload payload;
    payload.title = "Hello";
    payload.body = "ESP32";

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

## Memory policy

Curier uses the ZekStack-standard configuration shape:

```cpp
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

`memory.allocation` applies to Curier-owned movable runtime storage, including
queue backing, accepted subscription/payload copies, runtime configuration
strings, JWT cache, crypto working buffers, Curier-created ArduinoJson storage,
and dynamic HTTP scratch storage.

`memory.taskStack` applies to the worker stack. FreeRTOS control blocks remain
internal through Strata.

The `v0.1.0` migration mapping is:

| Curier v0.1.0 | Curier v0.2.0 |
| --- | --- |
| `CurierStackType::Auto` | `Strata::Placement::PreferExternal` |
| `CurierStackType::Internal` | `Strata::Placement::Internal` |
| `CurierStackType::Psram` | `Strata::Placement::RequireExternal` |
| `config.stackType` | `config.memory.taskStack` |

`PreferExternal` falls back to internal memory when external memory is not
available. `RequireExternal` fails instead of consuming internal memory.

Public `CurierConfig`, `CurierSubscription`, `CurierPayload`, caller-created
`JsonDocument`, and callback/provider captures remain caller-owned standard C++
objects. Curier applies its memory policy after it accepts and copies runtime
data. Allocations internal to ESP-IDF and Mbed TLS are also outside Curier's
allocator boundary.

## Diagnostics

```cpp
CurierDiagnostics diag = curier.diagnostics();
ESP_LOGI(
    "WEBPUSH",
    "stack requested=%s actual=%s",
    Strata::toString(diag.requestedStackPlacement),
    Strata::toString(diag.stackRegion)
);
```

Diagnostics include queue depth, in-flight count, high-water marks, completion
and retry counters, task stack high-water mark, general allocation placement,
requested task-stack placement, observed stack region, queue storage placement
and region, and shutdown-semaphore control region.

Requested placement and observed region are deliberately separate. A
`PreferExternal` request may validly report `Internal` after fallback.

## Lifecycle and callbacks

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
* During shutdown the worker publishes final diagnostics, gives a Strata binary
  semaphore, and suspends. `end()` resets the owned `Strata::FreeRTOS::Task`
  from the owner context, releasing its stack and TCB before runtime state is
  freed.
* If `end()` times out, Curier preserves `Stopping` and all owned state so a
  later call can finish cleanup.
* `queueSize` bounds all accepted jobs, including the active job.

## Time and retry behavior

Curier uses `std::time(nullptr)` by default. A provider may be installed before
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

See [`docs/retries.md`](docs/retries.md) for custom retry-policy details.

## Security notes

Subscriptions and JSON are validated and copied before `send()` returns.
Endpoint authorities are validated before HTTP delivery and VAPID audience
construction. Malformed ports and unbracketed IPv6 are rejected.

The VAPID private key is a device secret. Curier explicitly clears its copied
private-key storage, cached JWTs, subscription auth secrets, plaintext payload
copies, and sensitive cryptographic working buffers during normal release.
Selecting external placement means those Curier-owned buffers may reside in
external RAM; secure clearing still applies.

HTTPS verification is enabled by default through the ESP-IDF certificate bundle
when available.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Initialize Curier with Strata memory policy and send a typed payload. |
| `ArduinoJson` | Send a validated ArduinoJson v7 document. |
| `CustomTimeAndRetry` | Register a clock provider and custom retry policy. |

Start with `examples/Basic`.

## Documentation

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | VAPID, memory policy, subscription, clock, and first-send setup. |
| [`docs/api.md`](docs/api.md) | Public API, results, diagnostics, payloads, and callbacks. |
| [`docs/configuration.md`](docs/configuration.md) | Memory, queue, task, payload, TLS, TTL, and retry settings. |
| [`docs/concurrency.md`](docs/concurrency.md) | Worker ownership, callback context, and Strata-backed shutdown guarantees. |
| [`docs/retries.md`](docs/retries.md) | Default and custom retry decisions. |
| [`docs/protocol.md`](docs/protocol.md) | Supported Web Push protocol and wire behavior. |
| [`docs/security.md`](docs/security.md) | Key handling, placement, endpoint validation, TLS, and secret boundaries. |
| [`docs/memory.md`](docs/memory.md) | Strata ownership boundary and lifecycle qualification. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common setup, placement, and delivery failures. |

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Language | C++20 |
| Memory layer | Strata `v0.1.2` |
| Networking | ESP-IDF `esp_http_client` |
| Encryption | RFC 8291/8188 `aes128gcm` only |
| Authentication | VAPID ES256 |
| JSON | ArduinoJson `>= 7.0.0` |
| HTTPS | Certificate bundle, global CA store, or custom PEM |
| Task stack | Strata `Default`, `Internal`, `PreferExternal`, or `RequireExternal` |
| Exceptions | Not used by Curier APIs |
| Version | `0.2.0` |

## Release qualification

The host suite compares Curier's encrypted body byte-for-byte with the RFC 8291
Appendix A vector and independently verifies generated VAPID ES256 signatures.
Target sketches exercise queue bounds, callbacks, retries, cancellation,
timeout recovery, and repeated Internal, PreferExternal, and RequireExternal
lifecycle cleanup across ESP32, ESP32-S3, ESP32-C3, and ESP32-P4 builds.

## License

MIT - see [`LICENSE.md`](LICENSE.md) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## ZekStack

Part of the ZekStack ESP32 library stack.
