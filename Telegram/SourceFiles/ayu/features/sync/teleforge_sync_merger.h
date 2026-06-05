#pragma once

#include "ayu/features/sync/teleforge_sync_snapshot.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <vector>

namespace TeleForge::Sync {

struct EncryptedDbShard {
	SyncDatabaseKind kind = SyncDatabaseKind::Data;
	QByteArray encrypted;
	QString fileName;
	QString sha256Hex;
	int dateFrom = 0;
	int dateTo = 0;
};

[[nodiscard]] std::vector<EncryptedDbShard> BuildEncryptedDatabaseShards(
	const QByteArray &syncKey);
[[nodiscard]] bool ApplyEncryptedDatabaseShard(
	SyncDatabaseKind kind,
	const QByteArray &encrypted,
	const QByteArray &syncKey);

} // namespace TeleForge::Sync
