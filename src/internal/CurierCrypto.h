#pragma once

#include "CurierMemory.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace curier_internal {

struct CurierEncryptionInputs {
	std::array<uint8_t, 32> senderPrivateKey{};
	std::array<uint8_t, 16> salt{};
};

class CurierCrypto {
  public:
	explicit CurierCrypto(Strata::Placement placement = Strata::Placement::Default) noexcept;
	~CurierCrypto() noexcept = default;

	CurierCrypto(const CurierCrypto &) = delete;
	CurierCrypto &operator=(const CurierCrypto &) = delete;

	CurierResult init();

	CurierResult validateVapid(CurierVapidView vapid);
	CurierResult validateVapid(const CurierVapid &vapid);

	CurierResult validateSubscription(CurierSubscriptionView subscription);
	CurierResult validateSubscription(const CurierSubscription &subscription);

	CurierResult encrypt(
	    std::string_view plaintext,
	    CurierSubscriptionView subscription,
	    CurierBytes &body
	);

	CurierResult createVapidJwt(
	    CurierVapidView vapid,
	    std::string_view audience,
	    uint64_t nowEpochSeconds,
	    uint32_t lifetimeSeconds,
	    CurierString &jwt,
	    uint64_t &expiresAt
	);

	CurierResult encryptWithInputsForTesting(
	    const std::string &plaintext,
	    const CurierSubscription &subscription,
	    const CurierEncryptionInputs &inputs,
	    std::vector<uint8_t> &body
	);
	CurierResult createVapidJwt(
	    const CurierVapid &vapid,
	    const std::string &audience,
	    uint64_t nowEpochSeconds,
	    uint32_t lifetimeSeconds,
	    std::string &jwt,
	    uint64_t &expiresAt
	);

	static bool base64UrlDecode(const std::string &input, std::vector<uint8_t> &output);
	static bool base64UrlEncode(const uint8_t *input, size_t inputSize, std::string &output);

  private:
	struct State;
	Strata::Placement _placement = Strata::Placement::Default;
	Strata::UniquePtr<State> _state;
};

} // namespace curier_internal
