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

## Configuration and memory

`CurierConfig` exposes the shared Strata memory policy:

```cpp
Strata::MemoryPolicy memory{
    .allocation = Strata::Placement::Default,
    .taskStack = Strata::Placement::PreferExternal,
};
```

`memory.allocation` controls Curier-owned movable runtime allocations.
`memory.taskStack` controls the worker stack. See
[`configuration.md`](configuration.md) and [`memory.md`](memory.md) for the
ownership boundary and migration mapping from `v0.1.0`.

## Lifecycle

```cpp
CurierResult init(const CurierConfig &config = CurierConfig());
CurierResult end(uint32_t timeoutMs = 5000);
bool initialized() const;
```

`init()` validates the complete configuration, Strata memory policy, and VAPID
key pair before starting the worker. Repeated initialization returns
`AlreadyInitialized`.

A successful `end()` means the externally-owned Strata task has been reset from
the owner context and Curier's queue, semaphore, crypto state, JWT cache, and
runtime configuration have been released.

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

Both overloads apply the same payload schema and size limit. Caller-owned
subscription/payload objects remain standard C++/ArduinoJson objects. After
validation, Curier copies accepted data into storage backed by
`config.memory.allocation`.

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

Diagnostics retain the existing queue/completion/retry counters and stack
high-water mark and add Strata placement visibility:

```cpp
Strata::Placement allocationPlacement;
Strata::Placement requestedStackPlacement;
Strata::Region stackRegion;
Strata::Placement queueStoragePlacement;
Strata::Region queueStorageRegion;
Strata::Region shutdownSignalControlRegion;
```

Requested placement and observed region are intentionally separate. For
example, `PreferExternal` may legitimately report an internal region after
fallback on a target without usable external RAM.

## Status strings

```cpp
const char *statusToString(CurierStatus status) const;
```

The returned string is static and does not require caller ownership.
