#include "ayu/features/plugins/plugin_trust.h"

#include "logs.h"

#include <openssl/evp.h>

#include <QtCore/QCryptographicHash>

namespace TeleForge::Plugins::Trust {
namespace {

[[nodiscard]] QByteArray HexToBytes(const QString &hex) {
	const auto cleaned = hex.trimmed();
	if (cleaned.size() % 2 != 0) {
		return {};
	}
	auto result = QByteArray(cleaned.size() / 2, Qt::Uninitialized);
	for (auto i = 0; i < result.size(); ++i) {
		auto ok = false;
		const auto byte = cleaned.mid(i * 2, 2).toUShort(&ok, 16);
		if (!ok) {
			return {};
		}
		result[i] = char(byte);
	}
	return result;
}

[[nodiscard]] QString NormalizeChannel(QString channel) {
	channel = channel.trimmed().toLower();
	if (channel.startsWith(u'@')) {
		channel = channel.mid(1);
	}
	return channel;
}

} // namespace

QByteArray DeveloperAttestMessage(
		const QString &devId,
		const QString &channelUsername,
		const QString &pubkeyHex) {
	return QString(
		u"TFDEVELOPER\n%1\n%2\n%3"_q
	).arg(
		devId.trimmed(),
		NormalizeChannel(channelUsername),
		pubkeyHex.trimmed().toLower()).toUtf8();
}

QByteArray PluginSignMessage(
		const QByteArray &sha256,
		const QString &devId,
		const QString &fileName) {
	return QString(
		u"TFPLUGIN\n%1\n%2\n%3"_q
	).arg(
		QString::fromLatin1(sha256.toHex()),
		devId.trimmed(),
		fileName.trimmed()).toUtf8();
}

QByteArray PublicKeyFromHex(const QString &hex) {
	const auto bytes = HexToBytes(hex);
	return (bytes.size() == 32) ? bytes : QByteArray();
}

bool VerifyEd25519(
		const QByteArray &publicKey32,
		const QByteArray &message,
		const QByteArray &signature64) {
	if (publicKey32.size() != 32 || signature64.size() != 64) {
		return false;
	}
	const auto key = EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519,
		nullptr,
		reinterpret_cast<const unsigned char*>(publicKey32.constData()),
		32);
	if (!key) {
		return false;
	}
	const auto guard = [=] { EVP_PKEY_free(key); };

	auto ctx = EVP_MD_CTX_new();
	if (!ctx) {
		return false;
	}
	const auto guardCtx = [=] { EVP_MD_CTX_free(ctx); };

	if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) != 1) {
		return false;
	}
	const auto ok = EVP_DigestVerify(
		ctx,
		reinterpret_cast<const unsigned char*>(signature64.constData()),
		64,
		reinterpret_cast<const unsigned char*>(message.constData()),
		message.size());
	return ok == 1;
}

bool VerifyRootDeveloperAttestation(
		const QString &devId,
		const QString &channelUsername,
		const QString &pubkeyHex,
		const QString &rootSignatureHex) {
	const auto rootPub = PublicKeyFromHex(
		QString::fromLatin1(kRootPublicKeyHex));
	const auto sig = HexToBytes(rootSignatureHex);
	const auto msg = DeveloperAttestMessage(devId, channelUsername, pubkeyHex);
	if (rootPub.isEmpty() || sig.isEmpty()) {
		return false;
	}
	const auto ok = VerifyEd25519(rootPub, msg, sig);
	if (!ok) {
		LOG(("TeleForge trust: bad root attestation for dev %1").arg(devId));
	}
	return ok;
}

bool VerifyDeveloperPluginSignature(
		const QByteArray &publicKey32,
		const QByteArray &sha256,
		const QString &devId,
		const QString &fileName,
		const QString &signatureHex) {
	if (sha256.size() != 32) {
		return false;
	}
	const auto sig = HexToBytes(signatureHex);
	const auto msg = PluginSignMessage(sha256, devId, fileName);
	return VerifyEd25519(publicKey32, msg, sig);
}

} // namespace TeleForge::Plugins::Trust
