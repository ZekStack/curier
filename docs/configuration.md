# Configuration

## Delivery and queue limits

| Field | Default | Meaning |
| --- | ---: | --- |
| `queueSize` | 16 | Maximum accepted jobs, including the active job; range 1 through 256. |
| `maxPayloadBytes` | 3993 | Maximum serialized plaintext bytes. |
| `maxEndpointBytes` | 2048 | Maximum accepted subscription endpoint bytes. |
| `requestTimeoutMs` | 10000 | Per-attempt ESP-IDF HTTP timeout. |
| `ttlSeconds` | 2419200 | Web Push `TTL` header value. |

`maxPayloadBytes` must be from 1 through 3993. The maximum is derived from one
4096-byte RFC 8188 encrypted record and Curier's record overhead.

Web Push TTL values must not exceed 2,147,483,648 seconds.

## Worker task

| Field | Default | Meaning |
| --- | ---: | --- |
| `stackSize` | 4096 | FreeRTOS task stack size in bytes on ESP-IDF. |
| `priority` | 1 | Worker priority. |
| `coreId` | `tskNO_AFFINITY` | Worker core affinity. |
| `stackType` | `Auto` | Internal, PSRAM, or automatic stack placement. |
| `taskName` | `curier-task` | FreeRTOS task name. |

`CurierStackType::Auto` first attempts a PSRAM stack when ESP-IDF supports
capability-aware task creation and PSRAM is available. It falls back to
internal memory. `Psram` is strict and fails initialization when an external
stack cannot be created.

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
