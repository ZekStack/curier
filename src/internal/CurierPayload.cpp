#include "CurierPayload.h"

#include <strata/arduinojson/Allocator.h>

#include <cstring>

namespace {

bool knownPayloadKey(const char *key) {
	return std::strcmp(key, "title") == 0 || std::strcmp(key, "body") == 0 ||
	       std::strcmp(key, "tag") == 0 || std::strcmp(key, "icon") == 0 ||
	       std::strcmp(key, "badge") == 0 || std::strcmp(key, "image") == 0 ||
	       std::strcmp(key, "data") == 0 || std::strcmp(key, "actions") == 0 ||
	       std::strcmp(key, "renotify") == 0 || std::strcmp(key, "requireInteraction") == 0 ||
	       std::strcmp(key, "silent") == 0 || std::strcmp(key, "timestamp") == 0;
}

bool knownActionKey(const char *key) {
	return std::strcmp(key, "action") == 0 || std::strcmp(key, "title") == 0 ||
	       std::strcmp(key, "icon") == 0 || std::strcmp(key, "navigate") == 0;
}

bool nonEmptyString(JsonVariantConst value) {
	return value.is<const char *>() && value.as<const char *>() != nullptr &&
	       value.as<const char *>()[0] != '\0';
}

CurierResult validateAction(JsonObjectConst action) {
	for (JsonPairConst pair : action) {
		if (!knownActionKey(pair.key().c_str())) {
			return CurierResult::failure(
			    CurierStatus::InvalidPayload,
			    "payload action contains an unknown field"
			);
		}
	}
	if (!nonEmptyString(action["action"]) || !nonEmptyString(action["title"])) {
		return CurierResult::failure(
		    CurierStatus::InvalidPayload,
		    "payload action and title must be non-empty strings"
		);
	}
	for (const char *key : {"icon", "navigate"}) {
		JsonVariantConst value = action[key];
		if (!value.isNull() && !value.is<const char *>()) {
			return CurierResult::failure(
			    CurierStatus::InvalidPayload,
			    "payload action optional fields must be strings"
			);
		}
	}
	return CurierResult::success();
}

class CurierStringWriter {
  public:
	explicit CurierStringWriter(curier_internal::CurierString &target) : _target(target) {
	}

	size_t write(uint8_t value) {
		_target.push_back(static_cast<char>(value));
		return 1;
	}

	size_t write(const uint8_t *buffer, size_t size) {
		if (buffer == nullptr || size == 0) {
			return 0;
		}
		_target.append(reinterpret_cast<const char *>(buffer), size);
		return size;
	}

  private:
	curier_internal::CurierString &_target;
};

CurierResult validateAndSerialize(
    JsonVariantConst payload,
    size_t maxPayloadBytes,
    curier_internal::CurierString &json
) {
	json.clear();
	if (!payload.is<JsonObjectConst>()) {
		return CurierResult::failure(CurierStatus::InvalidPayload, "payload must be a JSON object");
	}
	JsonObjectConst object = payload.as<JsonObjectConst>();
	for (JsonPairConst pair : object) {
		if (!knownPayloadKey(pair.key().c_str())) {
			return CurierResult::failure(
			    CurierStatus::InvalidPayload,
			    "payload contains an unknown field"
			);
		}
	}
	if (!nonEmptyString(object["title"])) {
		return CurierResult::failure(
		    CurierStatus::InvalidPayload,
		    "payload title must be a non-empty string"
		);
	}
	if (!nonEmptyString(object["body"])) {
		return CurierResult::failure(
		    CurierStatus::InvalidPayload,
		    "payload body must be a non-empty string"
		);
	}
	for (const char *key : {"tag", "icon", "badge", "image"}) {
		JsonVariantConst value = object[key];
		if (!value.isNull() && !value.is<const char *>()) {
			return CurierResult::failure(
			    CurierStatus::InvalidPayload,
			    "payload optional text fields must be strings"
			);
		}
	}
	for (const char *key : {"renotify", "requireInteraction", "silent"}) {
		JsonVariantConst value = object[key];
		if (!value.isNull() && !value.is<bool>()) {
			return CurierResult::failure(
			    CurierStatus::InvalidPayload,
			    "payload optional flags must be booleans"
			);
		}
	}
	JsonVariantConst timestamp = object["timestamp"];
	if (!timestamp.isNull() && !timestamp.is<int64_t>() && !timestamp.is<uint64_t>()) {
		return CurierResult::failure(
		    CurierStatus::InvalidPayload,
		    "payload timestamp must be an integer"
		);
	}
	JsonVariantConst data = object["data"];
	if (!data.isNull() && !data.is<JsonObjectConst>() && !data.is<JsonArrayConst>()) {
		return CurierResult::failure(
		    CurierStatus::InvalidPayload,
		    "payload data must be an object or array"
		);
	}
	JsonVariantConst actions = object["actions"];
	if (!actions.isNull()) {
		if (!actions.is<JsonArrayConst>()) {
			return CurierResult::failure(
			    CurierStatus::InvalidPayload,
			    "payload actions must be an array"
			);
		}
		for (JsonVariantConst action : actions.as<JsonArrayConst>()) {
			if (!action.is<JsonObjectConst>()) {
				return CurierResult::failure(
				    CurierStatus::InvalidPayload,
				    "payload action must be an object"
				);
			}
			CurierResult actionResult = validateAction(action.as<JsonObjectConst>());
			if (!actionResult) {
				return actionResult;
			}
		}
	}

	const size_t measured = measureJson(payload);
	if (measured == 0) {
		return CurierResult::failure(CurierStatus::InvalidPayload, "payload serialization failed");
	}
	if (measured > maxPayloadBytes) {
		return CurierResult::failure(
		    CurierStatus::PayloadTooLarge,
		    "payload exceeds configured size limit"
		);
	}
	json.reserve(measured);
	CurierStringWriter writer(json);
	if (serializeJson(payload, writer) != measured) {
		json.clear();
		return CurierResult::failure(CurierStatus::InvalidPayload, "payload serialization failed");
	}
	return CurierResult::success();
}

} // namespace

namespace curier_internal {

CurierResult serializePayload(
    const CurierPayload &payload,
    size_t maxPayloadBytes,
    Strata::Placement placement,
    CurierString &json
) {
	Strata::ArduinoJson::Allocator allocator{placement};
	JsonDocument document{&allocator};
	document["title"] = payload.title;
	document["body"] = payload.body;
	if (payload.tag.has_value()) {
		document["tag"] = payload.tag->c_str();
	}
	if (payload.icon.has_value()) {
		document["icon"] = payload.icon->c_str();
	}
	if (payload.badge.has_value()) {
		document["badge"] = payload.badge->c_str();
	}
	if (payload.image.has_value()) {
		document["image"] = payload.image->c_str();
	}
	if (payload.hasData) {
		document["data"] = payload.data.as<JsonVariantConst>();
	}
	if (!payload.actions.empty()) {
		JsonArray actions = document["actions"].to<JsonArray>();
		for (const CurierAction &action : payload.actions) {
			JsonObject value = actions.add<JsonObject>();
			value["action"] = action.action;
			value["title"] = action.title;
			if (action.icon.has_value()) {
				value["icon"] = action.icon->c_str();
			}
			if (action.navigate.has_value()) {
				value["navigate"] = action.navigate->c_str();
			}
		}
	}
	if (payload.renotify.has_value()) {
		document["renotify"] = *payload.renotify;
	}
	if (payload.requireInteraction.has_value()) {
		document["requireInteraction"] = *payload.requireInteraction;
	}
	if (payload.silent.has_value()) {
		document["silent"] = *payload.silent;
	}
	if (payload.timestamp.has_value()) {
		document["timestamp"] = *payload.timestamp;
	}
	if (document.overflowed()) {
		return CurierResult::failure(
		    CurierStatus::AllocationFailed,
		    "payload JSON allocation failed"
		);
	}
	return validateAndSerialize(document.as<JsonVariantConst>(), maxPayloadBytes, json);
}

CurierResult
serializePayload(const JsonDocument &payload, size_t maxPayloadBytes, CurierString &json) {
	return validateAndSerialize(payload.as<JsonVariantConst>(), maxPayloadBytes, json);
}

} // namespace curier_internal
