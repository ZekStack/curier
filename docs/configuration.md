# Configuration

## Memory policy

Curier `v0.2.0` uses the ZekStack-standard `Strata::MemoryPolicy`:

```cpp
CurierConfig config;
config.memory.allocation = Strata::Placement::Default;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

| Field | Default | Meaning |
| --- | --- | --- |
| `memory.allocation` | `Default` | Placement for Curier-owned movable runtime storage. |
| `memory.taskStack` | `PreferExternal` | Placement for the Curier worker-task stack. |

`memory.allocation` covers Curier-owned runtime copies and working storage such
as queue backing, accepted subscriptions and serialized payloads, JWT cache,
crypto buffers, Curier-created ArduinoJson documents, and HTTP scratch strings.
The internal implementation object, FreeRTOS control blocks, mutex control
storage, and shutdown semaphore control storage remain internal for safety.

`memory.taskStack` accepts the shared Strata placement vocabulary:

* `Default` uses the backend default.
* `Internal` requires internal memory.
* `PreferExternal` prefers external RAM and falls back to internal memory.
* `RequireExternal` fails initialization instead of consuming internal memory
  when an external stack cannot be allocated.

The `v0.1.0` mapping is:

| Curier v0.1.0 | Curier v0.2.0 |
| --- | --- |
| `CurierStackType::Auto` | `Strata::Placement::PreferExternal` |
| `CurierStackType::Internal` | `Strata::Placement::Internal` |
| `CurierStackType::Psram` | `Strata::Placement::RequireExternal` |
| `stackType` | `memory.taskStack` |

## Delivery and queue limits

| Field | Default | Meaning |
| --- | ---: | --- |
| `queueSize` | 16 | Maximum accepted jobs, including the active job; range 1 through 256. |
| `maxPayloadBytes` | 3993 | Maximum serialized plaintext bytes. |
| `maxEndpointBytes` | 2048 | Maximum accepted subscription endpoint bytes. |
| `requestTimeoutMs` | 10000 | Per-attempt ESP-IDF HTTP timeout. |
| `ttlSeconds` | 2419200 | Web Push `TTL` header value. |

`maxPayloadBytes` must be from 1 through 3993. The maximum is derived from one
4096-byte RFC 8188 encrypted record and Curier's record overhead. Web Push TTL
values must not exceed 2,147,483,648 seconds.

## Worker task

| Field | Default | Meaning |
| --- | ---: | --- |
| `stackSize` | 4096 | FreeRTOS task stack size in bytes on ESP-IDF. |
| `priority` | 1 | Worker priority. |
| `coreId` | `tskNO_AFFINITY` | Worker core affinity. |
| `taskName` | `curier-task` | FreeRTOS task name. |

Stack size must be at least 1024 bytes and aligned to `sizeof(StackType_t)`.
Use `CurierDiagnostics::stackHighWaterMarkBytes` to qualify the configured size
under real TLS, cryptography, payload, retry, and callback workloads.

## Retry settings

| Field | Default |
| --- | ---: |
| `retry.mode` | `Exponential` |
| `retry.maxRetries` | 5 |
| `retry.baseDelayMs` | 1500 |
| `retry.maxDelayMs` | 15000 |
| `retry.jitterPercent` | 20 |
| `retry.respectRetryAfter` | true |
| `retry.retryClockUnavailable` | true |
| `retry.retryTransportErrors` | true |
| `retry.retryHttp408` | true |
| `retry.retryHttp429` | true |
| `retry.retryHttp5xx` | true |

`maxRetries` counts retries after the first attempt. A value of five permits up
to six total attempts. Curier caps the configured value at 20.

## TLS

| Field | Default | Meaning |
| --- | ---: | --- |
| `useTlsCertBundle` | true | Attach the ESP-IDF certificate bundle when available. |
| `useGlobalCaStore` | false | Use the ESP-IDF global CA store. |
| `skipTlsCommonNameCheck` | false | Disable host-name verification. |
| `caCertificatePem` | empty | Custom CA certificate PEM. |

Do not set both `useGlobalCaStore` and `caCertificatePem`. At least one trust
source must remain selected. A custom certificate takes precedence over the
default bundle; the global CA store takes precedence over the bundle.
`skipTlsCommonNameCheck` weakens endpoint authentication and should remain false
in production.
