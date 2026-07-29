# Getting started

Curier needs four things before it can deliver a notification:

1. An active network connection with working DNS and verified HTTPS.
2. A valid VAPID P-256 key pair and contact subject.
3. A browser Push API subscription.
4. Valid Unix system time.

## VAPID configuration

Configure the same VAPID public key that the browser used when creating its
subscription. Curier accepts uncompressed 65-byte P-256 public keys and 32-byte
private scalars encoded as base64url. Padding is accepted but not required.

```cpp
CurierVapid vapid;
vapid.subject = "mailto:notify@example.com";
vapid.publicKeyBase64 = "...";
vapid.privateKeyBase64 = "...";

CurierConfig config;
config.vapidConfig = vapid;
```

`init()` decodes both keys, validates the curve values, and verifies that the
public key was derived from the private key.

## Clock setup

Curier calls `std::time(nullptr)` by default. If Tempo or SNTP configures the
system clock, no Curier-specific integration is needed. Enqueueing is allowed
before sync; delivery reports `ClockUnavailable` and follows the configured
retry policy until a clock is available or retries are exhausted.

A custom provider can be installed before or after `init()`:

```cpp
curier.setTimeProvider([](uint64_t &epochSeconds) {
    epochSeconds = readApplicationEpoch();
    return epochSeconds != 0;
});
```

## Initialization

```cpp
Curier curier;
CurierConfig config;
config.vapidConfig = vapid;
config.queueSize = 16;
config.maxPayloadBytes = 3993;
config.stackType = CurierStackType::Auto;
config.stackSize = 4096;

CurierResult result = curier.init(config);
if (!result) {
    ESP_LOGE("WEBPUSH", "Curier init failed: %s", result.message);
}
```

The default stack size is a starting point, not a universal runtime
qualification. Inspect `diagnostics().stackHighWaterMarkBytes` under real TLS,
payload, and retry workloads.

## Subscription and send

The browser subscription supplies `endpoint`, `p256dh`, and `auth`. Treat all
three as untrusted external input. Curier validates their scheme, bounds, and
decoded key sizes before queue ownership is published.

```cpp
CurierSubscription subscription;
subscription.endpoint = endpointFromBrowser;
subscription.p256dh = p256dhFromBrowser;
subscription.auth = authFromBrowser;

CurierPayload payload;
payload.title = "Alarm";
payload.body = "Input 4 changed";

CurierResult queued = curier.send(subscription, payload, onComplete);
```

The subscription and serialized payload only need to remain valid until
`send()` returns.

## Shutdown

```cpp
CurierResult stopped = curier.end(5000);
```

Shutdown cancels queued or retrying work and waits for the active HTTP operation
to return through its configured request timeout. If `end()` times out, Curier
remains in `Stopping`; call `end()` again later to finish cleanup.
