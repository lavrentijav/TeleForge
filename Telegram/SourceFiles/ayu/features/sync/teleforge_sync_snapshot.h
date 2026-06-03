#pragma once

#include <optional>

#include <QByteArray>
#include <QString>

namespace TeleForge::Sync {

enum class SyncDatabaseKind {
	TeleForge,
	AyuData,
};

[[nodiscard]] QString SnapshotPath(SyncDatabaseKind kind);
[[nodiscard]] bool CreateDatabaseSnapshot(SyncDatabaseKind kind, const QString &destPath);
[[nodiscard]] QByteArray CompressSnapshot(const QString &snapshotPath);
[[nodiscard]] std::optional<QString> DecompressToTemp(const QByteArray &compressed);
[[nodiscard]] QByteArray EncryptSnapshot(
	const QByteArray &compressed,
	const QByteArray &key);
[[nodiscard]] std::optional<QByteArray> DecryptSnapshot(
	const QByteArray &encrypted,
	const QByteArray &key);

} // namespace TeleForge::Sync
