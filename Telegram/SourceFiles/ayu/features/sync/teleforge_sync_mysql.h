#pragma once

#include "main/main_session.h"

#include <QByteArray>
#include <QString>

namespace TeleForge::Sync {

// MySQL / MariaDB cloud backend for TeleForge sync. Mirrors the PostgreSQL
// backend (teleforge_sync_pg): it stores the same encrypted, compressed
// snapshot bundle in a shared table keyed by an opaque per-account tenant id,
// so tenants sharing one database stay cryptographically isolated. The local
// SQLite unified DB always remains the source of truth.
//
// The connection string reuses the cloud connection-string field and is parsed
// as space/';'-separated key=value tokens, e.g.:
//   host=127.0.0.1 port=3306 user=root password=secret dbname=teleforge

void RunMySqlUpload(
	not_null<Main::Session*> session,
	const QByteArray &syncKey,
	Fn<void(bool ok, QString error)> done);
void RunMySqlDownload(
	not_null<Main::Session*> session,
	const QByteArray &syncKey,
	Fn<void(bool ok, QString error)> done);

// Connectivity check for the settings UI. Runs off the main thread.
void MySqlTestConnection(
	const QString &connString,
	Fn<void(bool ok, QString error)> done);

} // namespace TeleForge::Sync
