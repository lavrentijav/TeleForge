#include "ayu/features/sync/teleforge_sync_crypto.h"

#include "base/openssl_help.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <QtCore/QCryptographicHash>

namespace TeleForge::Sync {
namespace {

const auto kMagic = QByteArray("TFSYNC1\0", 8);
constexpr auto kNonceSize = 12;
constexpr auto kTagSize = 16;

[[nodiscard]] QByteArray HkdfSha256(
		const QByteArray &ikm,
		const QByteArray &salt,
		const QByteArray &info) {
	auto data = salt + ikm + info;
	return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

} // namespace

QByteArray DeriveSyncKey(
		const QByteArray &masterMaterial,
		const QByteArray &optionalExportKey) {
	if (!optionalExportKey.isEmpty()) {
		return HkdfSha256(
			optionalExportKey,
			QByteArray("TeleForge-Sync-Export"),
			QByteArray("v1"));
	}
	return HkdfSha256(
		masterMaterial,
		QByteArray("TeleForge-Sync-v1"),
		QByteArray("account"));
}

QByteArray EncryptBundle(const QByteArray &plain, const QByteArray &key) {
	if (key.size() < 32) {
		return {};
	}

	auto nonce = QByteArray(kNonceSize, Qt::Uninitialized);
	RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), kNonceSize);

	auto ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		return {};
	}
	auto out = QByteArray();
	out.reserve(kMagic.size() + kNonceSize + plain.size() + kTagSize + 16);
	out.append(kMagic);
	out.append(nonce);

	auto ciphertext = QByteArray(plain.size() + 16, Qt::Uninitialized);
	auto tag = QByteArray(kTagSize, Qt::Uninitialized);
	int len = 0;
	int total = 0;

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
		|| EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1
		|| EVP_EncryptInit_ex(
			ctx,
			nullptr,
			nullptr,
			reinterpret_cast<const unsigned char*>(key.constData()),
			reinterpret_cast<const unsigned char*>(nonce.constData())) != 1
		|| EVP_EncryptUpdate(
			ctx,
			reinterpret_cast<unsigned char*>(ciphertext.data()),
			&len,
			reinterpret_cast<const unsigned char*>(plain.constData()),
			plain.size()) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return {};
	}
	total = len;
	if (EVP_EncryptFinal_ex(
			ctx,
			reinterpret_cast<unsigned char*>(ciphertext.data()) + len,
			&len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return {};
	}
	total += len;
	if (EVP_CIPHER_CTX_ctrl(
			ctx,
			EVP_CTRL_GCM_GET_TAG,
			kTagSize,
			tag.data()) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return {};
	}
	EVP_CIPHER_CTX_free(ctx);

	ciphertext.resize(total);
	out.append(ciphertext);
	out.append(tag);
	return out;
}

std::optional<QByteArray> DecryptBundle(
		const QByteArray &encrypted,
		const QByteArray &key) {
	if (encrypted.size() < kMagic.size() + kNonceSize + kTagSize + 1) {
		return std::nullopt;
	}
	if (!encrypted.startsWith(kMagic)) {
		return std::nullopt;
	}
	const auto nonce = encrypted.mid(kMagic.size(), kNonceSize);
	const auto tag = encrypted.right(kTagSize);
	const auto ciphertext = encrypted.mid(
		kMagic.size() + kNonceSize,
		encrypted.size() - kMagic.size() - kNonceSize - kTagSize);

	auto ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		return std::nullopt;
	}
	auto plain = QByteArray(ciphertext.size(), Qt::Uninitialized);
	int len = 0;
	int total = 0;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
		|| EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1
		|| EVP_DecryptInit_ex(
			ctx,
			nullptr,
			nullptr,
			reinterpret_cast<const unsigned char*>(key.constData()),
			reinterpret_cast<const unsigned char*>(nonce.constData())) != 1
		|| EVP_DecryptUpdate(
			ctx,
			reinterpret_cast<unsigned char*>(plain.data()),
			&len,
			reinterpret_cast<const unsigned char*>(ciphertext.constData()),
			ciphertext.size()) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return std::nullopt;
	}
	total = len;
	if (EVP_CIPHER_CTX_ctrl(
			ctx,
			EVP_CTRL_GCM_SET_TAG,
			kTagSize,
			const_cast<char*>(tag.data())) != 1
		|| EVP_DecryptFinal_ex(
			ctx,
			reinterpret_cast<unsigned char*>(plain.data()) + len,
			&len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return std::nullopt;
	}
	total += len;
	EVP_CIPHER_CTX_free(ctx);
	plain.resize(total);
	return plain;
}

QString ExportKeyBase64(const QByteArray &key) {
	return QString::fromLatin1(key.toBase64());
}

std::optional<QByteArray> ImportKeyBase64(const QString &encoded) {
	const auto bytes = QByteArray::fromBase64(encoded.trimmed().toLatin1());
	if (bytes.size() < 16) {
		return std::nullopt;
	}
	return bytes;
}

} // namespace TeleForge::Sync
