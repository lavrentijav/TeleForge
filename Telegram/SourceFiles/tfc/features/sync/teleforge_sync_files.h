#pragma once

#include "data/data_channel.h"
#include "main/main_session.h"

#include <QByteArray>
#include <QString>

namespace TeleForge::Sync {

struct SyncDocumentRef {
	int64 id = 0;
	uint64 accessHash = 0;
	QByteArray fileReference;
	int32 dcId = 0;
	int64 size = 0;
	QString sha256Hex;
};

void UploadEncryptedDocument(
	not_null<Main::Session*> session,
	not_null<ChannelData*> channel,
	const QByteArray &payload,
	const QString &fileName,
	Fn<void(bool ok, SyncDocumentRef ref, QString error)> done);

void DownloadEncryptedDocument(
	not_null<Main::Session*> session,
	const SyncDocumentRef &ref,
	Fn<void(QByteArray payload, QString error)> done);

} // namespace TeleForge::Sync
