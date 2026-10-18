#pragma once

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

namespace TeleForge::Sync {

// Pre-v2 key: a single SHA-256 over publicly derivable account material. Kept
// only so bundles written by older builds stay readable; never used for writes.
[[nodiscard]] QByteArray DeriveLegacySyncKey(const QByteArray &masterMaterial);

// v2 key: PBKDF2-HMAC-SHA256 over the random per-install secret, salted with a
// persisted random salt plus the account binding, so different accounts never
// share a key even on one install.
[[nodiscard]] QByteArray DeriveSyncKey(
	const QByteArray &secret,
	const QByteArray &optionalExportKey = {},
	const QByteArray &accountBinding = {});

// Account key: derived from a passphrase the user remembers, so any device
// where that account logs in can read the archive without moving key material
// between machines. Argon2id, salted with the account binding, so the server
// (which never sees the passphrase) cannot derive it.
[[nodiscard]] QByteArray DeriveAccountKey(
	const QByteArray &passphrase,
	const QByteArray &accountBinding);

// Parameters recorded next to encrypted data so any device can reproduce the
// key: mode tells which derivation was used, salt is public, cost is fixed.
enum class KeyMode {
	LegacyV1,
	DeviceV2,
	AccountArgon2id,
};
[[nodiscard]] QByteArray KdfSalt();
[[nodiscard]] QByteArray DeriveForMode(
	KeyMode mode,
	const QByteArray &secret,
	const QByteArray &accountBinding,
	const QByteArray &salt = {});
// 32 random bytes stored once per install (DPAPI-wrapped on Windows).
[[nodiscard]] QByteArray DeviceSecret();

// Remembers the pre-v2 key as a decrypt-only fallback while a migration is
// still possible; MarkSyncMigrationComplete() retires it for good.
void SetLegacySyncKey(const QByteArray &key);
void MarkSyncMigrationComplete();
[[nodiscard]] bool SyncMigrationComplete();

// Key protecting secret fields of the local DB (API keys, connection strings).
[[nodiscard]] QByteArray FieldCryptKey();
[[nodiscard]] QString ProtectSecret(const QString &plain);
[[nodiscard]] QString UnprotectSecret(const QString &stored);
[[nodiscard]] bool IsProtectedSecret(const QString &stored);

// Deterministic keyed tag of arbitrary data: HMAC-SHA256(key, data) as
// lowercase hex, truncated to 32 chars (128-bit). Opaque and non-reversible,
// but stable, so the remote DB can index and equality-match blinded columns
// without ever learning the real value.
[[nodiscard]] QString HmacTag(const QByteArray &data, const QByteArray &key);

// Convenience: HmacTag over the decimal form of a numeric id (chat/user/msg).
[[nodiscard]] QString HmacId(long long id, const QByteArray &key);

// Keyed orthogonal (signed-permutation) projection of an embedding vector.
// A signed permutation is an orthogonal transform, so cosine similarity and L2
// distance are preserved between any two vectors projected with the SAME key —
// server-side / cross-device search over one account's own vectors still works.
// Without the key the original component order and signs cannot be recovered.
// Deterministic: the client projects its query the same way before searching.
[[nodiscard]] std::vector<float> ProjectEmbedding(
	const std::vector<float> &vector,
	const QByteArray &key);
[[nodiscard]] QByteArray EncryptBundle(const QByteArray &plain, const QByteArray &key);

// Self-describing bundle: the header records which derivation produced the key
// and the salt used, so another device can rebuild the key from the passphrase
// without any side channel. Older bundles keep the short header and are read as
// LegacyV1/DeviceV2 depending on the key that opens them.
[[nodiscard]] QByteArray EncryptBundleEx(
	const QByteArray &plain,
	const QByteArray &key,
	KeyMode mode,
	const QByteArray &salt);

// Reads the header of a bundle without decrypting it.
[[nodiscard]] bool ReadBundleParams(
	const QByteArray &encrypted,
	KeyMode *mode,
	QByteArray *salt);

[[nodiscard]] std::optional<QByteArray> DecryptBundle(
	const QByteArray &encrypted,
	const QByteArray &key);
[[nodiscard]] QString ExportKeyBase64(const QByteArray &key);
[[nodiscard]] std::optional<QByteArray> ImportKeyBase64(const QString &encoded);

} // namespace TeleForge::Sync
