#pragma once

#include <QByteArray>
#include <QString>

namespace TeleForge::Sync {

[[nodiscard]] QByteArray DeriveSyncKey(
	const QByteArray &masterMaterial,
	const QByteArray &optionalExportKey = {});
[[nodiscard]] QByteArray EncryptBundle(const QByteArray &plain, const QByteArray &key);
[[nodiscard]] std::optional<QByteArray> DecryptBundle(
	const QByteArray &encrypted,
	const QByteArray &key);
[[nodiscard]] QString ExportKeyBase64(const QByteArray &key);
[[nodiscard]] std::optional<QByteArray> ImportKeyBase64(const QString &encoded);

} // namespace TeleForge::Sync
