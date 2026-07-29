#include "CurierHttp.h"

#include <climits>
#include <cstring>
#include <strings.h>

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
	std::string retryAfter;
};

esp_err_t handleHttpEvent(esp_http_client_event_t *event) {
	if (event == nullptr || event->user_data == nullptr) {
		return ESP_OK;
	}
	auto *context = static_cast<ResponseContext *>(event->user_data);
	if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != nullptr &&
	    event->header_value != nullptr && strcasecmp(event->header_key, "Retry-After") == 0 &&
	    std::strlen(event->header_value) <= 64) {
		context->retryAfter = event->header_value;
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
    const CurierConfig &config,
    const CurierSubscription &subscription,
    const std::string &jwt,
    const std::vector<uint8_t> &body
) {
	CurierHttpResponse response;
	if (subscription.endpoint.empty() || jwt.empty() || body.empty() ||
	    body.size() > static_cast<size_t>(INT_MAX) ||
	    config.requestTimeoutMs > static_cast<uint32_t>(INT_MAX)) {
		response.result =
		    sendFailure(CurierStatus::InternalError, "HTTP request inputs are invalid");
		return response;
	}

	ResponseContext context;
	esp_http_client_config_t httpConfig = {};
	httpConfig.url = subscription.endpoint.c_str();
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

	const std::string authorization =
	    "vapid t=" + jwt + ", k=" + config.vapidConfig.publicKeyBase64;
	const std::string ttl = std::to_string(config.ttlSeconds);
	esp_err_t setupResult =
	    esp_http_client_set_header(client, "Authorization", authorization.c_str());
	if (setupResult == ESP_OK) {
		setupResult = esp_http_client_set_header(client, "TTL", ttl.c_str());
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
	response.retryAfter = std::move(context.retryAfter);
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

} // namespace curier_internal
