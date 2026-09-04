#include "CurierHttp.h"

#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <utility>

extern "C" {
#include "esp_http_client.h"
}

#if __has_include(<esp_crt_bundle.h>)
extern "C" {
#include <esp_crt_bundle.h>
}
#define CURIER_HAS_CERT_BUNDLE 1
#else
#define CURIER_HAS_CERT_BUNDLE 0
#endif

namespace {

struct ResponseContext {
	std::array<char, 65> retryAfter{};
	size_t retryAfterLength = 0;
};

#ifdef CURIER_ENABLE_TEST_HOOKS
curier_internal::CurierHttpTransport testTransport;
#endif

esp_err_t handleHttpEvent(esp_http_client_event_t *event) {
	if (event == nullptr || event->user_data == nullptr) {
		return ESP_OK;
	}
	auto *context = static_cast<ResponseContext *>(event->user_data);
	if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != nullptr &&
	    event->header_value != nullptr && strcasecmp(event->header_key, "Retry-After") == 0) {
		const size_t length = std::strlen(event->header_value);
		if (length <= 64) {
			std::memcpy(context->retryAfter.data(), event->header_value, length);
			context->retryAfter[length] = '\0';
			context->retryAfterLength = length;
		}
	}
	return ESP_OK;
}

CurierSendResult sendFailure(
    CurierStatus status, const char *message, esp_err_t transportError = ESP_OK, int statusCode = 0
) {
	CurierSendResult result;
	result.result = false;
	result.status = status;
	result.message = message;
	result.transportError = transportError;
	result.statusCode = statusCode;
	return result;
}

} // namespace

namespace curier_internal {

CurierHttpResponse sendWebPushRequest(
    const CurierRuntimeConfig &config,
    CurierSubscriptionView subscription,
    std::string_view jwt,
    std::span<const uint8_t> body
) {
#ifdef CURIER_ENABLE_TEST_HOOKS
	if (testTransport) {
		return testTransport(config, subscription, jwt, body);
	}
#endif

	CurierHttpResponse response(config.memory.allocation);
	if (subscription.endpoint.empty() || jwt.empty() || body.empty() ||
	    body.size() > static_cast<size_t>(INT_MAX) ||
	    config.requestTimeoutMs > static_cast<uint32_t>(INT_MAX)) {
		response.result =
		    sendFailure(CurierStatus::InternalError, "HTTP request inputs are invalid");
		return response;
	}

	ResponseContext context;
	esp_http_client_config_t httpConfig = {};
	httpConfig.url = subscription.endpoint.data();
	httpConfig.method = HTTP_METHOD_POST;
	httpConfig.timeout_ms = static_cast<int>(config.requestTimeoutMs);
	httpConfig.buffer_size_tx = 4096;
	httpConfig.skip_cert_common_name_check = config.skipTlsCommonNameCheck;
	httpConfig.use_global_ca_store = config.useGlobalCaStore;
	httpConfig.event_handler = handleHttpEvent;
	httpConfig.user_data = &context;
	if (!config.caCertificatePem.empty()) {
		httpConfig.cert_pem = config.caCertificatePem.c_str();
	}
#if CURIER_HAS_CERT_BUNDLE
	if (config.useTlsCertBundle && !config.useGlobalCaStore && config.caCertificatePem.empty()) {
		httpConfig.crt_bundle_attach = esp_crt_bundle_attach;
	}
#else
	if (config.useTlsCertBundle && !config.useGlobalCaStore && config.caCertificatePem.empty()) {
		response.result = sendFailure(
		    CurierStatus::TransportError,
		    "ESP-IDF certificate bundle is unavailable",
		    ESP_ERR_NOT_SUPPORTED
		);
		return response;
	}
#endif

	esp_http_client_handle_t client = esp_http_client_init(&httpConfig);
	if (client == nullptr) {
		response.result =
		    sendFailure(CurierStatus::AllocationFailed, "HTTP client allocation failed");
		return response;
	}

	CurierString authorization{Strata::Allocator<char>{config.memory.allocation}};
	authorization.assign("vapid t=");
	authorization.append(jwt.data(), jwt.size());
	authorization.append(", k=");
	authorization.append(config.vapidPublicKeyBase64);

	std::array<char, 16> ttl{};
	const int ttlLength = std::snprintf(ttl.data(), ttl.size(), "%u", config.ttlSeconds);
	if (ttlLength <= 0 || static_cast<size_t>(ttlLength) >= ttl.size()) {
		esp_http_client_cleanup(client);
		response.result =
		    sendFailure(CurierStatus::InternalError, "HTTP TTL formatting failed");
		return response;
	}

	esp_err_t setupResult =
	    esp_http_client_set_header(client, "Authorization", authorization.c_str());
	if (setupResult == ESP_OK) {
		setupResult = esp_http_client_set_header(client, "TTL", ttl.data());
	}
	if (setupResult == ESP_OK) {
		setupResult = esp_http_client_set_header(client, "Content-Encoding", "aes128gcm");
	}
	if (setupResult == ESP_OK) {
		setupResult =
		    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
	}
	if (setupResult == ESP_OK) {
		setupResult = esp_http_client_set_post_field(
		    client,
		    reinterpret_cast<const char *>(body.data()),
		    static_cast<int>(body.size())
		);
	}
	if (setupResult != ESP_OK) {
		esp_http_client_cleanup(client);
		response.result =
		    sendFailure(CurierStatus::TransportError, "HTTP request setup failed", setupResult);
		return response;
	}

	const esp_err_t transportResult = esp_http_client_perform(client);
	const int statusCode = esp_http_client_get_status_code(client);
	if (context.retryAfterLength > 0) {
		response.retryAfter.assign(context.retryAfter.data(), context.retryAfterLength);
	}
	esp_http_client_cleanup(client);

	if (transportResult != ESP_OK) {
		response.result = sendFailure(
		    CurierStatus::TransportError,
		    "Web Push transport failed",
		    transportResult,
		    statusCode
		);
		return response;
	}
	if (statusCode < 200 || statusCode >= 300) {
		response.result = sendFailure(
		    CurierStatus::HttpError,
		    "push service returned an HTTP error",
		    ESP_OK,
		    statusCode
		);
		return response;
	}

	response.result.result = true;
	response.result.status = CurierStatus::Ok;
	response.result.message = "push service accepted the message";
	response.result.transportError = ESP_OK;
	response.result.statusCode = statusCode;
	return response;
}

#ifdef CURIER_ENABLE_TEST_HOOKS
void setHttpTransportForTesting(CurierHttpTransport transport) {
	testTransport = std::move(transport);
}

void clearHttpTransportForTesting() {
	testTransport = CurierHttpTransport{};
}
#endif

} // namespace curier_internal
