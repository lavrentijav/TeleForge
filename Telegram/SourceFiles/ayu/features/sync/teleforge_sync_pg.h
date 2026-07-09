#pragma once

#include "main/main_session.h"

#include <QByteArray>
#include <QString>

namespace TeleForge::Sync {

// PostgreSQL cloud backend for TeleForge sync. Stores the same encrypted,
// compressed snapshot bundle the Telegram-chat transport uses, but in a
// shared Postgres table keyed by an opaque per-account tenant id. Because the
// payload is encrypted with the account-bound sync key, tenants sharing one
// database cannot read each other's data (crypto isolation), which satisfies
// the multi-tenant "разные пользаки не ломятся друг к другу" requirement.
//
// The local SQLite unified DB always remains the source of truth; Postgres is
// a best-effort replication target. Uploads that fail (offline / unreachable)
// leave the local state untouched and are retried by the debounced scheduler.

// Opaque, stable tenant id for the account (SHA-256 hex of account identity).
[[nodiscard]] QString PgTenantId(not_null<Main::Session*> session);

void RunPgUpload(
	not_null<Main::Session*> session,
	const QByteArray &syncKey,
	Fn<void(bool ok, QString error)> done);
void RunPgDownload(
	not_null<Main::Session*> session,
	const QByteArray &syncKey,
	Fn<void(bool ok, QString error)> done);

// Connectivity check for the settings UI. Runs off the main thread.
void PgTestConnection(
	const QString &connString,
	Fn<void(bool ok, QString error)> done);

} // namespace TeleForge::Sync
