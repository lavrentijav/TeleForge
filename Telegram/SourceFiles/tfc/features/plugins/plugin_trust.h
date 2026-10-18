#pragma once

#include <QString>

namespace TeleForge::Plugins::Trust {

// TeleForge root public key (Ed25519, 32 bytes hex). Replace after `teleforge_trust.py gen-root`.
inline constexpr auto kRootPublicKeyHex =
	"2df7f5ea89dd64401ce20e26911f11d426d1e993111c4377fb3431a32a1cd4b8";

[[nodiscard]] QByteArray DeveloperAttestMessage(
	const QString &devId,
	const QString &channelUsername,
	const QString &pubkeyHex);

[[nodiscard]] QByteArray PluginSignMessage(
	const QByteArray &sha256,
	const QString &devId,
	const QString &fileName);

[[nodiscard]] bool VerifyEd25519(
	const QByteArray &publicKey32,
	const QByteArray &message,
	const QByteArray &signature64);

[[nodiscard]] bool VerifyRootDeveloperAttestation(
	const QString &devId,
	const QString &channelUsername,
	const QString &pubkeyHex,
	const QString &rootSignatureHex);

[[nodiscard]] bool VerifyDeveloperPluginSignature(
	const QByteArray &publicKey32,
	const QByteArray &sha256,
	const QString &devId,
	const QString &fileName,
	const QString &signatureHex);

[[nodiscard]] QByteArray PublicKeyFromHex(const QString &hex);

} // namespace TeleForge::Plugins::Trust
