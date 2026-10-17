#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

namespace TeleForge::Sync {

[[nodiscard]] QByteArray DeriveSyncKey(
	const QByteArray &masterMaterial,
	const QByteArray &optionalExportKey = {});

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
[[nodiscard]] std::optional<QByteArray> DecryptBundle(
	const QByteArray &encrypted,
	const QByteArray &key);
[[nodiscard]] QString ExportKeyBase64(const QByteArray &key);
[[nodiscard]] std::optional<QByteArray> ImportKeyBase64(const QString &encoded);

} // namespace TeleForge::Sync
