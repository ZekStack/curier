# Retry policies

## Default policy

The built-in policy can use fixed or exponential delays with symmetric jitter.
It retries only explicitly enabled categories:

* unavailable system time;
* ESP-IDF transport failures;
* HTTP 408;
* HTTP 429;
* HTTP 500 through 599.

Other HTTP responses are terminal. In particular, authentication and
subscription failures such as 400, 401, 403, 404, and 410 should be handled by
the application rather than repeatedly sent.

When enabled, `Retry-After` accepts either delta seconds or an IMF-fixdate. Its
delay is bounded by `maxDelayMs` and is not shortened by jitter.

## Fixed retries

```cpp
config.retry.mode = CurierRetryMode::Fixed;
config.retry.maxRetries = 3;
config.retry.baseDelayMs = 2000;
config.retry.maxDelayMs = 10000;
config.retry.jitterPercent = 10;
```

## Disabling retries

```cpp
config.retry.mode = CurierRetryMode::Disabled;
config.retry.maxRetries = 0;
```

## Custom policy

Set `config.retryPolicy` to replace the built-in classification and delay
calculation:

```cpp
config.retry.maxRetries = 4;
config.retry.maxDelayMs = 30000;
config.retryPolicy = [](const CurierRetryContext &context) {
    CurierRetryDecision decision;

    if (context.statusCode == 429 && context.attempts <= 4) {
        decision.retry = true;
        decision.delayMs =
            context.retryAfterMs != 0 ? context.retryAfterMs : 5000;
    }

    return decision;
};
```

The custom callback runs on Curier's worker task after a failed attempt. Keep it
short, deterministic, and independent of Curier lifecycle calls. Curier still
enforces `maxRetries` and clamps its returned delay.

## Delivery attempts

`CurierSendResult::attempts` includes the initial delivery. If encryption or
VAPID JWT creation fails before an HTTP operation, that processing pass still
counts as an attempt. Payload encryption is performed once and reused across
transport retries for the accepted job.
