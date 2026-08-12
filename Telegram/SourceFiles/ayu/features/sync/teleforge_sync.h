#pragma once

#include "main/main_session.h"

namespace TeleForge::Sync {

void initialize();
void scheduleUploadDebounced(not_null<Main::Session*> session);
void syncNow(not_null<Main::Session*> session, Fn<void(QString message)> done);
void tryDownloadOnStartup(not_null<Main::Session*> session);
// Compares the fingerprint the cloud backend stored for the last uploaded
// snapshot with the local fact set. `done` receives whether the local copy is
// behind; when it is, a download is started automatically.
void checkRemoteFreshness(
	not_null<Main::Session*> session,
	Fn<void(bool stale, QString error)> done = nullptr);
// Creates the dedicated sync chat right away (no-op if sync is disabled or the
// chat already exists). Safe to call repeatedly.
void ensureSyncChat(
	not_null<Main::Session*> session,
	Fn<void(bool ok, QString message)> done = nullptr);
void setExportKey(const QByteArray &key);

} // namespace TeleForge::Sync
