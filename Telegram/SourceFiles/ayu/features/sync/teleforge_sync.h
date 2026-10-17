#pragma once

#include "main/main_session.h"

namespace TeleForge::Sync {

void initialize();
void scheduleUploadDebounced(not_null<Main::Session*> session);
void syncNow(not_null<Main::Session*> session, Fn<void(QString message)> done);
// Pushes the entire local store to the configured cloud backend right now
// (upload only, no download/merge). Intended for an explicit "export local DB"
// action in the settings UI. `onProgress(done, total)` fires on the main thread
// as rows are uploaded (DB backends only).
void uploadNow(
	not_null<Main::Session*> session,
	Fn<void(QString message)> done,
	Fn<void(int done, int total)> onProgress = nullptr);
void tryDownloadOnStartup(not_null<Main::Session*> session);
// Creates the dedicated sync chat right away (no-op if sync is disabled or the
// chat already exists). Safe to call repeatedly.
void ensureSyncChat(
	not_null<Main::Session*> session,
	Fn<void(bool ok, QString message)> done = nullptr);
void setExportKey(const QByteArray &key);

} // namespace TeleForge::Sync
