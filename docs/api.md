# API

## Result types

`CurierResult` is the common result for initialization, shutdown, queue
submission, and time-provider changes.

```cpp
struct CurierResult {
    bool result;
    CurierStatus status;
    const char *message;

    explicit operator bool() const;
};
```

`CurierSendResult` extends that common result for terminal delivery:

```cpp
struct CurierSendResult : CurierResult {
    esp_err_t transportError;
    int statusCode;
    uint8_t attempts;

    bool ok() const;
};
```

`statusCode` is zero when no HTTP response was received. `attempts` includes the
first attempt.

## Lifecycle

```cpp
CurierResult init(const CurierConfig &config = CurierConfig());
CurierResult end(uint32_t timeoutMs = 5000);
bool initialized() const;
```

`init()` validates the complete configuration and VAPID key pair before
starting the worker. Repeated initialization returns `AlreadyInitialized`.

## Sending

```cpp
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
```

Both overloads apply the same payload schema and size limit. Unknown JSON fields
are rejected so payload behavior does not silently diverge between typed and
ArduinoJson callers.

Required JSON fields:

* `title`: non-empty string
* `body`: non-empty string

Supported optional fields:

* `tag`, `icon`, `badge`, `image`
* `data` as an object or array
* `actions`, with `action`, `title`, optional `icon`, and optional `navigate`
* `renotify`, `requireInteraction`, `silent`
* integer `timestamp`

## Time provider

```cpp
using CurierTimeProvider = std::function<bool(uint64_t &epochSeconds)>;

CurierResult setTimeProvider(CurierTimeProvider provider);
CurierResult clearTimeProvider();
```

The provider may be changed in any lifecycle state. Replacement is atomic from
the worker's point of view and invalidates the VAPID JWT cache.

## Retry policy

```cpp
using CurierRetryPolicy =
    std::function<CurierRetryDecision(const CurierRetryContext &context)>;
```

The callback receives the completed attempt, transport and HTTP details, and a
parsed `Retry-After` delay when available. Return `{false, 0}` to terminate or
`{true, delayMs}` to retry. Delays are clamped to
`config.retry.maxDelayMs`.

## Diagnostics

```cpp
CurierDiagnostics diagnostics() const;
```

Diagnostics include current queue depth, accepted in-flight count, high-water
mark, completion counters, retry count, cancellation count, task stack
high-water mark, and requested/actual task stack memory.

## Status strings

```cpp
const char *statusToString(CurierStatus status) const;
```

The returned string is static and does not require caller ownership.
