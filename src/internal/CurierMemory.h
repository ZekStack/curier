#pragma once

#include "../Curier.h"

#include <strata/STL.h>

#include <string_view>

namespace curier_internal {

using CurierString = Strata::String;
using CurierBytes = Strata::Vector<uint8_t>;

template <typename T>
using CurierVector = Strata::Vector<T>;

struct CurierSubscriptionView {
	std::string_view endpoint;
	std::string_view p256dh;
	std::string_view auth;
};

struct CurierVapidView {
	std::string_view subject;
	std::string_view publicKeyBase64;
	std::string_view privateKeyBase64;
};

struct CurierOwnedSubscription {
	explicit CurierOwnedSubscription(Strata::Placement placement) noexcept
	    : endpoint(Strata::Allocator<char>{placement}),
	      p256dh(Strata::Allocator<char>{placement}),
	      auth(Strata::Allocator<char>{placement}) {
	}

	void assign(const CurierSubscription &source) {
		endpoint.assign(source.endpoint.data(), source.endpoint.size());
		p256dh.assign(source.p256dh.data(), source.p256dh.size());
		auth.assign(source.auth.data(), source.auth.size());
	}

	[[nodiscard]] CurierSubscriptionView view() const noexcept {
		return {
		    std::string_view(endpoint.data(), endpoint.size()),
		    std::string_view(p256dh.data(), p256dh.size()),
		    std::string_view(auth.data(), auth.size()),
		};
	}

	CurierString endpoint;
	CurierString p256dh;
	CurierString auth;
};

struct CurierRuntimeConfig {
	explicit CurierRuntimeConfig(Strata::Placement placement) noexcept
	    : vapidSubject(Strata::Allocator<char>{placement}),
	      vapidPublicKeyBase64(Strata::Allocator<char>{placement}),
	      vapidPrivateKeyBase64(Strata::Allocator<char>{placement}),
	      taskName(Strata::Allocator<char>{placement}),
	      caCertificatePem(Strata::Allocator<char>{placement}) {
	}

	void assign(const CurierConfig &source) {
		memory = source.memory;
		vapidSubject.assign(source.vapidConfig.subject.data(), source.vapidConfig.subject.size());
		vapidPublicKeyBase64.assign(
		    source.vapidConfig.publicKeyBase64.data(), source.vapidConfig.publicKeyBase64.size()
		);
		vapidPrivateKeyBase64.assign(
		    source.vapidConfig.privateKeyBase64.data(), source.vapidConfig.privateKeyBase64.size()
		);
		queueSize = source.queueSize;
		maxPayloadBytes = source.maxPayloadBytes;
		maxEndpointBytes = source.maxEndpointBytes;
		stackSize = source.stackSize;
		priority = source.priority;
		coreId = source.coreId;
		taskName.assign(source.taskName.data(), source.taskName.size());
		requestTimeoutMs = source.requestTimeoutMs;
		ttlSeconds = source.ttlSeconds;
		retry = source.retry;
		retryPolicy = source.retryPolicy;
		useTlsCertBundle = source.useTlsCertBundle;
		useGlobalCaStore = source.useGlobalCaStore;
		skipTlsCommonNameCheck = source.skipTlsCommonNameCheck;
		caCertificatePem.assign(
		    source.caCertificatePem.data(), source.caCertificatePem.size()
		);
	}

	[[nodiscard]] CurierVapidView vapidView() const noexcept {
		return {
		    std::string_view(vapidSubject.data(), vapidSubject.size()),
		    std::string_view(vapidPublicKeyBase64.data(), vapidPublicKeyBase64.size()),
		    std::string_view(vapidPrivateKeyBase64.data(), vapidPrivateKeyBase64.size()),
		};
	}

	Strata::MemoryPolicy memory{};
	CurierString vapidSubject;
	CurierString vapidPublicKeyBase64;
	CurierString vapidPrivateKeyBase64;

	size_t queueSize = 16;
	size_t maxPayloadBytes = 3993;
	size_t maxEndpointBytes = 2048;

	uint32_t stackSize = 4096;
	UBaseType_t priority = 1;
	BaseType_t coreId = tskNO_AFFINITY;
	CurierString taskName;

	uint32_t requestTimeoutMs = 10000;
	uint32_t ttlSeconds = 2419200;

	CurierRetryConfig retry;
	CurierRetryPolicy retryPolicy;

	bool useTlsCertBundle = true;
	bool useGlobalCaStore = false;
	bool skipTlsCommonNameCheck = false;
	CurierString caCertificatePem;
};

} // namespace curier_internal
