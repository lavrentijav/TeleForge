#pragma once

#include "data/data_channel.h"
#include "main/main_session.h"

#include <QByteArray>

namespace TeleForge::Sync {

void EnsureSyncChannel(
	not_null<Main::Session*> session,
	Fn<void(bool ok, QString error)> done);

[[nodiscard]] ChannelData *ResolveSyncChannel(not_null<Main::Session*> session);
[[nodiscard]] bool IsSyncChannel(not_null<Main::Session*> session, PeerId peerId);

void RunSyncUpload(
	not_null<Main::Session*> session,
	const QByteArray &syncKey,
	Fn<void(bool ok, QString error)> done);
void RunSyncDownload(
	not_null<Main::Session*> session,
	const QByteArray &syncKey,
	Fn<void(bool ok, QString error)> done);

} // namespace TeleForge::Sync
