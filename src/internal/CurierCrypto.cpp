#include "CurierCrypto.h"

#include <ArduinoJson.h>
#include <strata/arduinojson/Allocator.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

extern "C" {
#include "mbedtls/base64.h"
#include "mbedtls/cipher.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
}

namespace {

constexpr size_t kP256PublicKeyBytes = 65;
constexpr size_t kP256PrivateKeyBytes = 32;
constexpr size_t kAuthSecretBytes = 16;
constexpr size_t kSaltBytes = 16;
constexpr size_t kSharedSecretBytes = 32;
constexpr size_t kContentKeyBytes = 16;
constexpr size_t kNonceBytes = 12;
constexpr size_t kGcmTagBytes = 16;
constexpr uint32_t kRecordSize = 4096;
constexpr size_t kMaxVapidSubjectBytes = 512;

void secureZero(void *memory, size_t size) {
	volatile uint8_t *cursor = static_cast<volatile uint8_t *>(memory);
	while (size-- > 0) {
		*cursor++ = 0;
	}
}

template <typename String>
void secureClear(String &value) {
	if (!value.empty()) {
		secureZero(value.data(), value.size());
	}
	value.clear();
}

curier_internal::CurierBytes makeBytes(Strata::Placement placement) {
	return curier_internal::CurierBytes{Strata::Allocator<uint8_t>{placement}};
}

curier_internal::CurierString makeString(Strata::Placement placement) {
	return curier_internal::CurierString{Strata::Allocator<char>{placement}};
}

void appendUint32(curier_internal::CurierBytes &output, uint32_t value) {
	output.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
	output.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
	output.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
	output.push_back(static_cast<uint8_t>(value & 0xffU));
}

bool validBase64UrlShape(std::string_view input) {
	if (input.empty()) {
		return false;
	}
	bool paddingStarted = false;
	size_t paddingCount = 0;
	for (char character : input) {
		const bool alphaNumeric = (character >= 'A' && character <= 'Z') ||
		                          (character >= 'a' && character <= 'z') ||
		                          (character >= '0' && character <= '9');
		if (alphaNumeric || character == '-' || character == '_') {
			if (paddingStarted) {
				return false;
			}
			continue;
		}
		if (character == '=') {
			paddingStarted = true;
			paddingCount++;
			if (paddingCount > 2) {
				return false;
			}
			continue;
		}
		return false;
	}
	if (paddingStarted && input.size() % 4 != 0) {
		return false;
	}
	return input.size() % 4 != 1;
}

bool base64UrlDecode(
    std::string_view input,
    Strata::Placement placement,
    curier_internal::CurierBytes &output
) {
	output.clear();
	if (!validBase64UrlShape(input)) {
		return false;
	}
	curier_internal::CurierString padded = makeString(placement);
	padded.assign(input.data(), input.size());
	for (char &character : padded) {
		if (character == '-') {
			character = '+';
		} else if (character == '_') {
			character = '/';
		}
	}
	while (padded.size() % 4 != 0) {
		padded.push_back('=');
	}

	size_t required = 0;
	const int probe = mbedtls_base64_decode(
	    nullptr,
	    0,
	    &required,
	    reinterpret_cast<const uint8_t *>(padded.data()),
	    padded.size()
	);
	if (probe != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && probe != 0) {
		return false;
	}
	output.assign(required, 0);
	size_t written = 0;
	const int decoded = mbedtls_base64_decode(
	    output.data(),
	    output.size(),
	    &written,
	    reinterpret_cast<const uint8_t *>(padded.data()),
	    padded.size()
	);
	if (decoded != 0) {
		output.clear();
		return false;
	}
	output.resize(written);
	return true;
}

bool base64UrlEncode(
    const uint8_t *input,
    size_t inputSize,
    Strata::Placement placement,
    curier_internal::CurierString &output
) {
	output.clear();
	if (input == nullptr || inputSize == 0 ||
	    inputSize > (std::numeric_limits<size_t>::max() - 4) / 4 * 3) {
		return false;
	}
	curier_internal::CurierBytes encoded = makeBytes(placement);
	encoded.resize(4 * ((inputSize + 2) / 3) + 4);
	size_t written = 0;
	if (mbedtls_base64_encode(encoded.data(), encoded.size(), &written, input, inputSize) != 0) {
		return false;
	}
	output.assign(reinterpret_cast<const char *>(encoded.data()), written);
	std::replace(output.begin(), output.end(), '+', '-');
	std::replace(output.begin(), output.end(), '/', '_');
	while (!output.empty() && output.back() == '=') {
		output.pop_back();
	}
	return !output.empty();
}

bool decodePublicKey(
    std::string_view encoded,
    Strata::Placement placement,
    curier_internal::CurierBytes &key
) {
	return (encoded.size() == 87 || encoded.size() == 88) &&
	       base64UrlDecode(encoded, placement, key) && key.size() == kP256PublicKeyBytes &&
	       key[0] == 0x04;
}

bool decodePrivateKey(
    std::string_view encoded,
    Strata::Placement placement,
    curier_internal::CurierBytes &key
) {
	return (encoded.size() == 43 || encoded.size() == 44) &&
	       base64UrlDecode(encoded, placement, key) && key.size() == kP256PrivateKeyBytes;
}

bool validVapidSubject(std::string_view subject) {
	if (subject.empty() || subject.size() > kMaxVapidSubjectBytes) {
		return false;
	}
	const bool mailto = subject.rfind("mailto:", 0) == 0;
	const bool https = subject.rfind("https://", 0) == 0;
	if ((!mailto && !https) || (mailto && subject.size() == sizeof("mailto:") - 1) ||
	    (https && subject.size() == sizeof("https://") - 1)) {
		return false;
	}
	if (https) {
		const size_t authorityStart = sizeof("https://") - 1;
		const size_t authorityEnd = subject.find_first_of("/?#", authorityStart);
		const size_t end = authorityEnd == std::string_view::npos ? subject.size() : authorityEnd;
		if (end == authorityStart || subject.find('@', authorityStart) < end) {
			return false;
		}
	}
	for (char character : subject) {
		const unsigned char value = static_cast<unsigned char>(character);
		if (value <= 32 || value >= 127) {
			return false;
		}
	}
	return true;
}

bool hmacSha256(
    const uint8_t *key, size_t keySize, const uint8_t *input, size_t inputSize, uint8_t output[32]
) {
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	return info != nullptr && mbedtls_md_hmac(info, key, keySize, input, inputSize, output) == 0;
}

bool hkdfExpandSingleBlock(
    const uint8_t prk[32],
    const uint8_t *info,
    size_t infoSize,
    Strata::Placement placement,
    uint8_t output[32]
) {
	curier_internal::CurierBytes input = makeBytes(placement);
	input.reserve(infoSize + 1);
	input.insert(input.end(), info, info + infoSize);
	input.push_back(0x01);
	return hmacSha256(prk, 32, input.data(), input.size(), output);
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

CurierResult encryptWithInputs(
    mbedtls_ctr_drbg_context &random,
    Strata::Placement placement,
    std::string_view plaintext,
    curier_internal::CurierSubscriptionView subscription,
    const curier_internal::CurierEncryptionInputs &inputs,
    curier_internal::CurierBytes &body
) {
	body.clear();
	if (plaintext.empty() || plaintext.size() > 3993) {
		return CurierResult::failure(
		    CurierStatus::PayloadTooLarge,
		    "payload is empty or exceeds RFC 8291 limits"
		);
	}

	curier_internal::CurierBytes receiverPublic = makeBytes(placement);
	curier_internal::CurierBytes authSecret = makeBytes(placement);
	if (!decodePublicKey(subscription.p256dh, placement, receiverPublic) ||
	    (subscription.auth.size() != 22 && subscription.auth.size() != 24) ||
	    !base64UrlDecode(subscription.auth, placement, authSecret) ||
	    authSecret.size() != kAuthSecretBytes) {
		secureZero(authSecret.data(), authSecret.size());
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription encryption keys are invalid"
		);
	}

	mbedtls_ecp_group group;
	mbedtls_mpi senderPrivate;
	mbedtls_mpi shared;
	mbedtls_ecp_point senderPublicPoint;
	mbedtls_ecp_point receiverPublicPoint;
	mbedtls_ecp_group_init(&group);
	mbedtls_mpi_init(&senderPrivate);
	mbedtls_mpi_init(&shared);
	mbedtls_ecp_point_init(&senderPublicPoint);
	mbedtls_ecp_point_init(&receiverPublicPoint);

	std::array<uint8_t, kP256PublicKeyBytes> senderPublic{};
	std::array<uint8_t, kSharedSecretBytes> sharedSecret{};
	std::array<uint8_t, 32> prkKey{};
	std::array<uint8_t, 32> inputKeyMaterial{};
	std::array<uint8_t, 32> prk{};
	std::array<uint8_t, 32> contentKeyFull{};
	std::array<uint8_t, 32> nonceFull{};
	std::array<uint8_t, kContentKeyBytes> contentKey{};
	std::array<uint8_t, kNonceBytes> nonce{};
	bool success = false;
	size_t senderPublicSize = 0;

	do {
		if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
		    mbedtls_mpi_read_binary(
		        &senderPrivate,
		        inputs.senderPrivateKey.data(),
		        inputs.senderPrivateKey.size()
		    ) != 0 ||
		    mbedtls_ecp_check_privkey(&group, &senderPrivate) != 0 ||
		    mbedtls_ecp_mul(
		        &group,
		        &senderPublicPoint,
		        &senderPrivate,
		        &group.G,
		        mbedtls_ctr_drbg_random,
		        &random
		    ) != 0 ||
		    mbedtls_ecp_point_write_binary(
		        &group,
		        &senderPublicPoint,
		        MBEDTLS_ECP_PF_UNCOMPRESSED,
		        &senderPublicSize,
		        senderPublic.data(),
		        senderPublic.size()
		    ) != 0 ||
		    senderPublicSize != kP256PublicKeyBytes ||
		    mbedtls_ecp_point_read_binary(
		        &group,
		        &receiverPublicPoint,
		        receiverPublic.data(),
		        receiverPublic.size()
		    ) != 0 ||
		    mbedtls_ecp_check_pubkey(&group, &receiverPublicPoint) != 0 ||
		    mbedtls_ecdh_compute_shared(
		        &group,
		        &shared,
		        &receiverPublicPoint,
		        &senderPrivate,
		        mbedtls_ctr_drbg_random,
		        &random
		    ) != 0 ||
		    mbedtls_mpi_write_binary(&shared, sharedSecret.data(), sharedSecret.size()) != 0) {
			break;
		}

		curier_internal::CurierBytes keyInfo = makeBytes(placement);
		static constexpr char kWebPushInfo[] = "WebPush: info";
		keyInfo.reserve(sizeof(kWebPushInfo) + receiverPublic.size() + senderPublic.size());
		keyInfo.insert(keyInfo.end(), kWebPushInfo, kWebPushInfo + sizeof(kWebPushInfo) - 1);
		keyInfo.push_back(0x00);
		keyInfo.insert(keyInfo.end(), receiverPublic.begin(), receiverPublic.end());
		keyInfo.insert(keyInfo.end(), senderPublic.begin(), senderPublic.end());
		if (!hmacSha256(
		        authSecret.data(),
		        authSecret.size(),
		        sharedSecret.data(),
		        sharedSecret.size(),
		        prkKey.data()
		    ) ||
		    !hkdfExpandSingleBlock(
		        prkKey.data(),
		        keyInfo.data(),
		        keyInfo.size(),
		        placement,
		        inputKeyMaterial.data()
		    ) ||
		    !hmacSha256(
		        inputs.salt.data(),
		        inputs.salt.size(),
		        inputKeyMaterial.data(),
		        inputKeyMaterial.size(),
		        prk.data()
		    )) {
			break;
		}

		static constexpr char kContentKeyInfo[] = "Content-Encoding: aes128gcm";
		static constexpr char kNonceInfo[] = "Content-Encoding: nonce";
		curier_internal::CurierBytes contentInfo = makeBytes(placement);
		contentInfo.insert(
		    contentInfo.end(), kContentKeyInfo, kContentKeyInfo + sizeof(kContentKeyInfo) - 1
		);
		contentInfo.push_back(0x00);
		curier_internal::CurierBytes nonceInfo = makeBytes(placement);
		nonceInfo.insert(nonceInfo.end(), kNonceInfo, kNonceInfo + sizeof(kNonceInfo) - 1);
		nonceInfo.push_back(0x00);
		if (!hkdfExpandSingleBlock(
		        prk.data(),
		        contentInfo.data(),
		        contentInfo.size(),
		        placement,
		        contentKeyFull.data()
		    ) ||
		    !hkdfExpandSingleBlock(
		        prk.data(),
		        nonceInfo.data(),
		        nonceInfo.size(),
		        placement,
		        nonceFull.data()
		    )) {
			break;
		}
		std::copy_n(contentKeyFull.begin(), contentKey.size(), contentKey.begin());
		std::copy_n(nonceFull.begin(), nonce.size(), nonce.begin());

		curier_internal::CurierBytes record = makeBytes(placement);
		record.insert(
		    record.end(),
		    reinterpret_cast<const uint8_t *>(plaintext.data()),
		    reinterpret_cast<const uint8_t *>(plaintext.data()) + plaintext.size()
		);
		record.push_back(0x02);
		curier_internal::CurierBytes ciphertext = makeBytes(placement);
		ciphertext.resize(record.size() + kGcmTagBytes);
		size_t outputSize = 0;
		size_t written = 0;
		std::array<uint8_t, kGcmTagBytes> tag{};
		mbedtls_cipher_context_t cipher;
		mbedtls_cipher_init(&cipher);
		const mbedtls_cipher_info_t *cipherInfo =
		    mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_GCM);
		const bool encrypted =
		    cipherInfo != nullptr && mbedtls_cipher_setup(&cipher, cipherInfo) == 0 &&
		    mbedtls_cipher_setkey(&cipher, contentKey.data(), 128, MBEDTLS_ENCRYPT) == 0 &&
		    mbedtls_cipher_set_iv(&cipher, nonce.data(), nonce.size()) == 0 &&
		    mbedtls_cipher_reset(&cipher) == 0 &&
		    mbedtls_cipher_update_ad(&cipher, nullptr, 0) == 0 &&
		    mbedtls_cipher_update(
		        &cipher,
		        record.data(),
		        record.size(),
		        ciphertext.data(),
		        &written
		    ) == 0;
		if (encrypted) {
			outputSize = written;
			if (mbedtls_cipher_finish(&cipher, ciphertext.data() + outputSize, &written) != 0) {
				mbedtls_cipher_free(&cipher);
				secureZero(record.data(), record.size());
				break;
			}
			outputSize += written;
			if (mbedtls_cipher_write_tag(&cipher, tag.data(), tag.size()) != 0) {
				mbedtls_cipher_free(&cipher);
				secureZero(record.data(), record.size());
				break;
			}
		}
		mbedtls_cipher_free(&cipher);
		secureZero(record.data(), record.size());
		if (!encrypted) {
			break;
		}
		ciphertext.resize(outputSize);
		ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());

		body.reserve(inputs.salt.size() + 4 + 1 + senderPublic.size() + ciphertext.size());
		body.insert(body.end(), inputs.salt.begin(), inputs.salt.end());
		appendUint32(body, kRecordSize);
		body.push_back(static_cast<uint8_t>(senderPublic.size()));
		body.insert(body.end(), senderPublic.begin(), senderPublic.end());
		body.insert(body.end(), ciphertext.begin(), ciphertext.end());
		success = body.size() == plaintext.size() + 103;
	} while (false);

	mbedtls_ecp_point_free(&receiverPublicPoint);
	mbedtls_ecp_point_free(&senderPublicPoint);
	mbedtls_mpi_free(&shared);
	mbedtls_mpi_free(&senderPrivate);
	mbedtls_ecp_group_free(&group);
	secureZero(authSecret.data(), authSecret.size());
	secureZero(sharedSecret.data(), sharedSecret.size());
	secureZero(prkKey.data(), prkKey.size());
	secureZero(inputKeyMaterial.data(), inputKeyMaterial.size());
	secureZero(prk.data(), prk.size());
	secureZero(contentKeyFull.data(), contentKeyFull.size());
	secureZero(nonceFull.data(), nonceFull.size());
	secureZero(contentKey.data(), contentKey.size());
	secureZero(nonce.data(), nonce.size());
	if (!success) {
		body.clear();
		return CurierResult::failure(
		    CurierStatus::CryptoError,
		    "Web Push payload encryption failed"
		);
	}
	return CurierResult::success();
}

} // namespace

namespace curier_internal {

struct CurierCrypto::State {
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context random;
	bool initialized = false;

	State() noexcept {
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&random);
	}

	~State() noexcept {
		mbedtls_ctr_drbg_free(&random);
		mbedtls_entropy_free(&entropy);
	}
};

CurierCrypto::CurierCrypto(Strata::Placement placement) noexcept : _placement(placement) {
}

CurierResult CurierCrypto::init() {
	if (!_state) {
		_state = Strata::makeUnique<State>(_placement);
	}
	if (!_state) {
		return CurierResult::failure(
		    CurierStatus::AllocationFailed,
		    "crypto state allocation failed"
		);
	}
	if (_state->initialized) {
		return CurierResult::success();
	}
	static constexpr char kPersonalization[] = "curier-drbg";
	const int result = mbedtls_ctr_drbg_seed(
	    &_state->random,
	    mbedtls_entropy_func,
	    &_state->entropy,
	    reinterpret_cast<const uint8_t *>(kPersonalization),
	    sizeof(kPersonalization) - 1
	);
	if (result != 0) {
		return CurierResult::failure(
		    CurierStatus::CryptoError,
		    "crypto random generator initialization failed"
		);
	}
	_state->initialized = true;
	return CurierResult::success();
}

CurierResult CurierCrypto::validateSubscription(CurierSubscriptionView subscription) {
	if (subscription.endpoint.empty() || subscription.p256dh.empty() || subscription.auth.empty()) {
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription endpoint and keys are required"
		);
	}
	CurierBytes publicKey = makeBytes(_placement);
	if (!decodePublicKey(subscription.p256dh, _placement, publicKey)) {
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription p256dh key is invalid"
		);
	}
	CurierBytes authSecret = makeBytes(_placement);
	if ((subscription.auth.size() != 22 && subscription.auth.size() != 24) ||
	    !base64UrlDecode(subscription.auth, _placement, authSecret) ||
	    authSecret.size() != kAuthSecretBytes) {
		secureZero(authSecret.data(), authSecret.size());
		return CurierResult::failure(
		    CurierStatus::InvalidSubscription,
		    "subscription auth secret is invalid"
		);
	}
	secureZero(authSecret.data(), authSecret.size());
	return CurierResult::success();
}

CurierResult CurierCrypto::validateSubscription(const CurierSubscription &subscription) {
	return validateSubscription(CurierSubscriptionView{
	    subscription.endpoint,
	    subscription.p256dh,
	    subscription.auth,
	});
}

CurierResult CurierCrypto::validateVapid(CurierVapidView vapid) {
	if (!validVapidSubject(vapid.subject)) {
		return CurierResult::failure(
		    CurierStatus::InvalidVapidSubject,
		    "VAPID subject must be a bounded mailto or HTTPS URI"
		);
	}
	CurierBytes publicKey = makeBytes(_placement);
	CurierBytes privateKey = makeBytes(_placement);
	if (!decodePublicKey(vapid.publicKeyBase64, _placement, publicKey) ||
	    !decodePrivateKey(vapid.privateKeyBase64, _placement, privateKey)) {
		secureZero(privateKey.data(), privateKey.size());
		return CurierResult::failure(
		    CurierStatus::InvalidVapidKey,
		    "VAPID keys must be unpadded P-256 base64url values"
		);
	}
	CurierResult initialized = init();
	if (!initialized) {
		secureZero(privateKey.data(), privateKey.size());
		return initialized;
	}

	mbedtls_ecp_group group;
	mbedtls_mpi scalar;
	mbedtls_ecp_point derived;
	mbedtls_ecp_group_init(&group);
	mbedtls_mpi_init(&scalar);
	mbedtls_ecp_point_init(&derived);
	bool valid = false;
	std::array<uint8_t, kP256PublicKeyBytes> encoded{};
	size_t encodedSize = 0;
	do {
		if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
		    mbedtls_mpi_read_binary(&scalar, privateKey.data(), privateKey.size()) != 0 ||
		    mbedtls_ecp_check_privkey(&group, &scalar) != 0 ||
		    mbedtls_ecp_mul(
		        &group,
		        &derived,
		        &scalar,
		        &group.G,
		        mbedtls_ctr_drbg_random,
		        &_state->random
		    ) != 0 ||
		    mbedtls_ecp_point_write_binary(
		        &group,
		        &derived,
		        MBEDTLS_ECP_PF_UNCOMPRESSED,
		        &encodedSize,
		        encoded.data(),
		        encoded.size()
		    ) != 0) {
			break;
		}
		valid = encodedSize == publicKey.size() &&
		        std::memcmp(encoded.data(), publicKey.data(), encodedSize) == 0;
	} while (false);

	mbedtls_ecp_point_free(&derived);
	mbedtls_mpi_free(&scalar);
	mbedtls_ecp_group_free(&group);
	secureZero(privateKey.data(), privateKey.size());
	secureZero(encoded.data(), encoded.size());
	if (!valid) {
		return CurierResult::failure(
		    CurierStatus::VapidKeyMismatch,
		    "VAPID public key does not match private key"
		);
	}
	return CurierResult::success();
}

CurierResult CurierCrypto::validateVapid(const CurierVapid &vapid) {
	return validateVapid(CurierVapidView{
	    vapid.subject,
	    vapid.publicKeyBase64,
	    vapid.privateKeyBase64,
	});
}

CurierResult CurierCrypto::encrypt(
    std::string_view plaintext, CurierSubscriptionView subscription, CurierBytes &body
) {
	CurierResult initialized = init();
	if (!initialized) {
		return initialized;
	}

	CurierEncryptionInputs inputs;
	mbedtls_ecp_group group;
	mbedtls_mpi privateKey;
	mbedtls_ecp_point publicKey;
	mbedtls_ecp_group_init(&group);
	mbedtls_mpi_init(&privateKey);
	mbedtls_ecp_point_init(&publicKey);
	bool generated = false;
	do {
		if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
		    mbedtls_ecp_gen_keypair(
		        &group,
		        &privateKey,
		        &publicKey,
		        mbedtls_ctr_drbg_random,
		        &_state->random
		    ) != 0 ||
		    mbedtls_mpi_write_binary(
		        &privateKey,
		        inputs.senderPrivateKey.data(),
		        inputs.senderPrivateKey.size()
		    ) != 0 ||
		    mbedtls_ctr_drbg_random(&_state->random, inputs.salt.data(), inputs.salt.size()) != 0) {
			break;
		}
		generated = true;
	} while (false);
	mbedtls_ecp_point_free(&publicKey);
	mbedtls_mpi_free(&privateKey);
	mbedtls_ecp_group_free(&group);
	if (!generated) {
		secureZero(inputs.senderPrivateKey.data(), inputs.senderPrivateKey.size());
		secureZero(inputs.salt.data(), inputs.salt.size());
		return CurierResult::failure(
		    CurierStatus::CryptoError,
		    "Web Push encryption inputs could not be generated"
		);
	}

	CurierResult result =
	    ::encryptWithInputs(_state->random, _placement, plaintext, subscription, inputs, body);
	secureZero(inputs.senderPrivateKey.data(), inputs.senderPrivateKey.size());
	secureZero(inputs.salt.data(), inputs.salt.size());
	return result;
}

CurierResult CurierCrypto::encryptWithInputsForTesting(
    const std::string &plaintext,
    const CurierSubscription &subscription,
    const CurierEncryptionInputs &inputs,
    std::vector<uint8_t> &body
) {
	CurierResult initialized = init();
	if (!initialized) {
		return initialized;
	}
	CurierBytes placedBody = makeBytes(_placement);
	CurierResult result = ::encryptWithInputs(
	    _state->random,
	    _placement,
	    plaintext,
	    CurierSubscriptionView{subscription.endpoint, subscription.p256dh, subscription.auth},
	    inputs,
	    placedBody
	);
	if (result) {
		body.assign(placedBody.begin(), placedBody.end());
	} else {
		body.clear();
	}
	return result;
}

CurierResult CurierCrypto::createVapidJwt(
    CurierVapidView vapid,
    std::string_view audience,
    uint64_t nowEpochSeconds,
    uint32_t lifetimeSeconds,
    CurierString &jwt,
    uint64_t &expiresAt
) {
	jwt.clear();
	expiresAt = 0;
	if (nowEpochSeconds == 0 || lifetimeSeconds == 0 || lifetimeSeconds > 86400 ||
	    audience.empty()) {
		return CurierResult::failure(
		    CurierStatus::JwtError,
		    "VAPID JWT time or audience is invalid"
		);
	}
	CurierResult initialized = init();
	if (!initialized) {
		return initialized;
	}
	CurierBytes privateKey = makeBytes(_placement);
	if (!decodePrivateKey(vapid.privateKeyBase64, _placement, privateKey)) {
		return CurierResult::failure(CurierStatus::InvalidVapidKey, "VAPID private key is invalid");
	}
	expiresAt = nowEpochSeconds + lifetimeSeconds;
	if (expiresAt < nowEpochSeconds) {
		secureZero(privateKey.data(), privateKey.size());
		expiresAt = 0;
		return CurierResult::failure(CurierStatus::JwtError, "VAPID JWT expiration overflow");
	}

	CurierString audienceText = makeString(_placement);
	audienceText.assign(audience.data(), audience.size());
	CurierString subjectText = makeString(_placement);
	subjectText.assign(vapid.subject.data(), vapid.subject.size());

	static constexpr char kHeader[] = R"({"alg":"ES256","typ":"JWT"})";
	Strata::ArduinoJson::Allocator jsonAllocator{_placement};
	JsonDocument payload{&jsonAllocator};
	payload["aud"] = audienceText.c_str();
	payload["exp"] = expiresAt;
	payload["sub"] = subjectText.c_str();
	if (payload.overflowed()) {
		secureZero(privateKey.data(), privateKey.size());
		return CurierResult::failure(CurierStatus::AllocationFailed, "VAPID JWT JSON allocation failed");
	}
	CurierString payloadJson = makeString(_placement);
	CurierStringWriter writer(payloadJson);
	if (serializeJson(payload, writer) == 0) {
		secureZero(privateKey.data(), privateKey.size());
		return CurierResult::failure(
		    CurierStatus::JwtError,
		    "VAPID JWT payload serialization failed"
		);
	}

	CurierString encodedHeader = makeString(_placement);
	CurierString encodedPayload = makeString(_placement);
	if (!base64UrlEncode(
	        reinterpret_cast<const uint8_t *>(kHeader),
	        sizeof(kHeader) - 1,
	        _placement,
	        encodedHeader
	    ) ||
	    !base64UrlEncode(
	        reinterpret_cast<const uint8_t *>(payloadJson.data()),
	        payloadJson.size(),
	        _placement,
	        encodedPayload
	    )) {
		secureZero(privateKey.data(), privateKey.size());
		return CurierResult::failure(CurierStatus::JwtError, "VAPID JWT base64url encoding failed");
	}
	CurierString signingInput = makeString(_placement);
	signingInput.reserve(encodedHeader.size() + 1 + encodedPayload.size());
	signingInput.append(encodedHeader);
	signingInput.push_back('.');
	signingInput.append(encodedPayload);

	std::array<uint8_t, 32> hash{};
	const mbedtls_md_info_t *hashInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (hashInfo == nullptr || mbedtls_md(
	                               hashInfo,
	                               reinterpret_cast<const uint8_t *>(signingInput.data()),
	                               signingInput.size(),
	                               hash.data()
	                           ) != 0) {
		secureZero(privateKey.data(), privateKey.size());
		return CurierResult::failure(CurierStatus::JwtError, "VAPID JWT hashing failed");
	}

	mbedtls_ecp_group group;
	mbedtls_mpi scalar;
	mbedtls_mpi signatureR;
	mbedtls_mpi signatureS;
	mbedtls_ecp_group_init(&group);
	mbedtls_mpi_init(&scalar);
	mbedtls_mpi_init(&signatureR);
	mbedtls_mpi_init(&signatureS);
	std::array<uint8_t, 64> signature{};
	bool signedToken = false;
	do {
		if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
		    mbedtls_mpi_read_binary(&scalar, privateKey.data(), privateKey.size()) != 0 ||
		    mbedtls_ecp_check_privkey(&group, &scalar) != 0 ||
		    mbedtls_ecdsa_sign_det_ext(
		        &group,
		        &signatureR,
		        &signatureS,
		        &scalar,
		        hash.data(),
		        hash.size(),
		        MBEDTLS_MD_SHA256,
		        mbedtls_ctr_drbg_random,
		        &_state->random
		    ) != 0 ||
		    mbedtls_mpi_write_binary(&signatureR, signature.data(), 32) != 0 ||
		    mbedtls_mpi_write_binary(&signatureS, signature.data() + 32, 32) != 0) {
			break;
		}
		CurierString encodedSignature = makeString(_placement);
		if (!base64UrlEncode(
		        signature.data(), signature.size(), _placement, encodedSignature
		    )) {
			break;
		}
		jwt.reserve(signingInput.size() + 1 + encodedSignature.size());
		jwt.assign(signingInput);
		jwt.push_back('.');
		jwt.append(encodedSignature);
		signedToken = true;
	} while (false);

	mbedtls_mpi_free(&signatureS);
	mbedtls_mpi_free(&signatureR);
	mbedtls_mpi_free(&scalar);
	mbedtls_ecp_group_free(&group);
	secureZero(privateKey.data(), privateKey.size());
	secureZero(signature.data(), signature.size());
	secureZero(hash.data(), hash.size());
	secureClear(payloadJson);
	secureClear(signingInput);
	if (!signedToken) {
		jwt.clear();
		expiresAt = 0;
		return CurierResult::failure(CurierStatus::JwtError, "VAPID JWT signing failed");
	}
	return CurierResult::success();
}

CurierResult CurierCrypto::createVapidJwt(
    const CurierVapid &vapid,
    const std::string &audience,
    uint64_t nowEpochSeconds,
    uint32_t lifetimeSeconds,
    std::string &jwt,
    uint64_t &expiresAt
) {
	CurierString placedJwt = makeString(_placement);
	CurierResult result = createVapidJwt(
	    CurierVapidView{vapid.subject, vapid.publicKeyBase64, vapid.privateKeyBase64},
	    audience,
	    nowEpochSeconds,
	    lifetimeSeconds,
	    placedJwt,
	    expiresAt
	);
	if (result) {
		jwt.assign(placedJwt.data(), placedJwt.size());
	} else {
		jwt.clear();
	}
	secureClear(placedJwt);
	return result;
}

bool CurierCrypto::base64UrlDecode(const std::string &input, std::vector<uint8_t> &output) {
	CurierBytes placed = makeBytes(Strata::Placement::Default);
	if (!::base64UrlDecode(input, Strata::Placement::Default, placed)) {
		output.clear();
		return false;
	}
	output.assign(placed.begin(), placed.end());
	return true;
}

bool CurierCrypto::base64UrlEncode(const uint8_t *input, size_t inputSize, std::string &output) {
	CurierString placed = makeString(Strata::Placement::Default);
	if (!::base64UrlEncode(input, inputSize, Strata::Placement::Default, placed)) {
		output.clear();
		return false;
	}
	output.assign(placed.data(), placed.size());
	return true;
}

} // namespace curier_internal
