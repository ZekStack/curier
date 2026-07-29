# Troubleshooting

## `InvalidVapidKey`

The public key must decode to a 65-byte uncompressed P-256 point beginning with
`0x04`. The private key must decode to a 32-byte P-256 scalar.

## `VapidKeyMismatch`

The configured public and private keys are individually shaped correctly but
do not form one key pair. Generate or provision them together.

## `ClockUnavailable`

Curier received zero or failure from the custom provider, or
`std::time(nullptr)` did not return a positive Unix timestamp. Complete Tempo or
SNTP synchronization before sending, or retain clock retries long enough for
startup sync.

## HTTP 401 or 403

Check the VAPID subject, system time, key pair, and whether the browser
subscription was created with the same public key. These responses are not
retried by default.

## HTTP 404 or 410

The subscription is normally expired or no longer valid. Remove it from
application storage and ask the browser to subscribe again.

## `QueueFull`

`queueSize` includes both queued and active work. Increase the limit only after
measuring memory and expected burst behavior, or apply backpressure in the
caller.

## `PayloadTooLarge`

Measure the serialized JSON, not the in-memory ArduinoJson document. Curier
allows at most 3993 plaintext bytes and may be configured lower.

## TLS transport failure

Verify network state, DNS, system time, certificate-bundle support, and the
selected CA configuration. Inspect `CurierSendResult::transportError` with
`esp_err_to_name()`.

## Shutdown timeout

The active HTTP operation has not returned yet. Curier remains in `Stopping`
and retains its owned state. Call `end()` again after the configured request
timeout. Do not destroy the instance from its callback.

## Stack pressure

Exercise real VAPID, TLS, payload, retry, and callback paths, then inspect:

```cpp
CurierDiagnostics diag = curier.diagnostics();
ESP_LOGI("WEBPUSH", "stack HWM: %u", diag.stackHighWaterMarkBytes);
```

Raise `stackSize` if the measured safety margin is too small for the target and
SDK configuration.
