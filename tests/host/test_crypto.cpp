#include "Curier.h"
#include "internal/CurierCrypto.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
}

namespace {

std::vector<uint8_t> decode(const std::string &value) {
	std::vector<uint8_t> decoded;
	assert(curier_internal::CurierCrypto::base64UrlDecode(value, decoded));
	return decoded;
}

std::string encode(const std::vector<uint8_t> &value) {
	std::string encoded;
	assert(curier_internal::CurierCrypto::base64UrlEncode(value.data(), value.size(), encoded));
	return encoded;
}

std::array<std::string, 3> splitJwt(const std::string &jwt) {
	const size_t first = jwt.find('.');
	const size_t second = jwt.find('.', first + 1);
	assert(first != std::string::npos);
	assert(second != std::string::npos);
	assert(jwt.find('.', second + 1) == std::string::npos);
	return {
	    jwt.substr(0, first),
	    jwt.substr(first + 1, second - first - 1),
	    jwt.substr(second + 1),
	};
}

void verifyEs256(
    const std::string &signingInput,
    const std::string &signatureBase64,
    const std::string &publicKeyBase64
) {
	const std::vector<uint8_t> signature = decode(signatureBase64);
	const std::vector<uint8_t> publicKey = decode(publicKeyBase64);
	assert(signature.size() == 64);
	assert(publicKey.size() == 65);

	std::array<uint8_t, 32> hash{};
	const mbedtls_md_info_t *hashInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	assert(hashInfo != nullptr);
	assert(
	    mbedtls_md(
	        hashInfo,
	        reinterpret_cast<const uint8_t *>(signingInput.data()),
	        signingInput.size(),
	        hash.data()
	    ) == 0
	);

	mbedtls_ecp_group group;
	mbedtls_ecp_point point;
	mbedtls_mpi r;
	mbedtls_mpi s;
	mbedtls_ecp_group_init(&group);
	mbedtls_ecp_point_init(&point);
	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&s);

	assert(mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0);
	assert(mbedtls_ecp_point_read_binary(&group, &point, publicKey.data(), publicKey.size()) == 0);
	assert(mbedtls_ecp_check_pubkey(&group, &point) == 0);
	assert(mbedtls_mpi_read_binary(&r, signature.data(), 32) == 0);
	assert(mbedtls_mpi_read_binary(&s, signature.data() + 32, 32) == 0);
	assert(mbedtls_ecdsa_verify(&group, hash.data(), hash.size(), &point, &r, &s) == 0);

	mbedtls_mpi_free(&s);
	mbedtls_mpi_free(&r);
	mbedtls_ecp_point_free(&point);
	mbedtls_ecp_group_free(&group);
}

void testRfc8291Vector() {
	curier_internal::CurierCrypto crypto;
	CurierSubscription subscription;
	subscription.endpoint = "https://push.example.net/push/test";
	subscription.p256dh =
	    "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4";
	subscription.auth = "BTBZMqHH6r4Tts7J_aSIgg";

	curier_internal::CurierEncryptionInputs inputs;
	const std::vector<uint8_t> senderPrivate =
	    decode("yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw");
	const std::vector<uint8_t> salt = decode("DGv6ra1nlYgDCS1FRnbzlw");
	assert(senderPrivate.size() == inputs.senderPrivateKey.size());
	assert(salt.size() == inputs.salt.size());
	std::copy(senderPrivate.begin(), senderPrivate.end(), inputs.senderPrivateKey.begin());
	std::copy(salt.begin(), salt.end(), inputs.salt.begin());

	std::vector<uint8_t> body;
	const CurierResult encrypted = crypto.encryptWithInputsForTesting(
	    "When I grow up, I want to be a watermelon",
	    subscription,
	    inputs,
	    body
	);
	assert(encrypted);
	// RFC 8291 errata 5230 corrects the example Content-Length from 145 to 144.
	assert(body.size() == 144);

	const std::string expected = "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27ml"
	                             "mlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPT"
	                             "pK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN";
	assert(encode(body) == expected);
}

void testVapidJwt() {
	curier_internal::CurierCrypto crypto;
	CurierVapid vapid;
	vapid.subject = "mailto:test@example.com";
	vapid.publicKeyBase64 =
	    "BAQrcGSCN0uGkDRKNe9p_5Uvlc4dPOZsRFLpuUplt8tlUzM4XmsHHFnLrb6CqYRgH3f2XyVJJlJrbKksBFtU_fw";
	vapid.privateKeyBase64 = "Qu8KQ-mRYC-ACZNsrftkSEaMv4qFAP9b-6Q7tK3lZX4";
	assert(crypto.validateVapid(vapid));

	constexpr uint64_t now = 1750000000;
	constexpr uint32_t lifetime = 3600;
	std::string jwt;
	uint64_t expiresAt = 0;
	assert(crypto.createVapidJwt(vapid, "https://push.example.com", now, lifetime, jwt, expiresAt));
	assert(expiresAt == now + lifetime);

	const auto parts = splitJwt(jwt);
	const std::vector<uint8_t> headerBytes = decode(parts[0]);
	const std::vector<uint8_t> payloadBytes = decode(parts[1]);
	const std::string header(headerBytes.begin(), headerBytes.end());
	const std::string payload(payloadBytes.begin(), payloadBytes.end());
	assert(header == R"({"alg":"ES256","typ":"JWT"})");
	assert(payload.find(R"("aud":"https://push.example.com")") != std::string::npos);
	assert(payload.find(R"("exp":1750003600)") != std::string::npos);
	assert(payload.find(R"("sub":"mailto:test@example.com")") != std::string::npos);
	verifyEs256(parts[0] + "." + parts[1], parts[2], vapid.publicKeyBase64);

	CurierVapid mismatch = vapid;
	mismatch.publicKeyBase64 =
	    "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4";
	const CurierResult mismatchResult = crypto.validateVapid(mismatch);
	assert(!mismatchResult);
	assert(mismatchResult.status == CurierStatus::VapidKeyMismatch);

	assert(!crypto.createVapidJwt(vapid, "https://push.example.com", now, 86401, jwt, expiresAt));
	assert(!crypto.createVapidJwt(
	    vapid,
	    "https://push.example.com",
	    std::numeric_limits<uint64_t>::max() - 10,
	    3600,
	    jwt,
	    expiresAt
	));
}

} // namespace

int main() {
	testRfc8291Vector();
	testVapidJwt();
	return 0;
}
