#pragma once

#include "main/main_session.h"

#include <QByteArray>
#include <QString>

namespace TeleForge::Sync {

// PostgreSQL cloud backend for TeleForge sync. Uses a proper relational schema
// (teleforge_messages + teleforge_events + teleforge_embeddings): one row per
// captured message with an encrypted, text-sized payload; ids are blinded with
// HMAC so they can be indexed without leaking real values, and tenants are
// isolated both by the opaque tenant id and by per-account encryption.
//
// The local SQLite unified DB always remains the source of truth; Postgres is
// a best-effort replication target. Uploads/downloads that fail (offline /
// unreachable) leave the local state untouched and are retried by the scheduler.

// Opaque, stable tenant id for the account (SHA-256 hex of account identity).
[[nodiscard]] QString PgTenantId(not_null<Main::Session*> session);

void RunPgUpload(
	not_null<Main::Session*> session,
	const QByteArray &syncKey,
	Fn<void(bool ok, QString error)> done,
	Fn<void(int done, int total)> onProgress = nullptr);
void RunPgDownload(
	not_null<Main::Session*> session,
	const QByteArray &syncKey,
	Fn<void(bool ok, QString error)> done);

// Connectivity check for the settings UI. Runs off the main thread.
void PgTestConnection(
	const QString &connString,
	Fn<void(bool ok, QString error)> done);

} // namespace TeleForge::Sync
