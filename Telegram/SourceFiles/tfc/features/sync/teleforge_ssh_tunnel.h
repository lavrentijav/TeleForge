#pragma once

#include <QtCore/QString>

namespace TeleForge::Ssh {

struct TunnelConfig {
	bool enabled = false;
	/// ssh login target, "user@host" or "user@host:port" (bastion machine;
	/// port defaults to 22 when omitted).
	QString sshTarget;
	/// Optional private key path; empty = ssh's own default (agent/~/.ssh).
	QString identityFile;
	/// Address of the database as seen from the bastion machine.
	QString remoteDbHost;
	int remoteDbPort = 0;
};

/// Ensures a local-forwarding `ssh -L` tunnel to (remoteDbHost, remoteDbPort)
/// via `sshTarget` is up, reusing an already-running tunnel for the same
/// target. Blocks the calling thread until the forwarded port accepts
/// connections or a timeout elapses — only call this from a worker thread
/// (e.g. inside crl::async), never from the main/UI thread, since the wait is
/// satisfied by work posted back to the main thread.
/// Returns the local port on success, or 0 with `error` filled on failure.
[[nodiscard]] int EnsureTunnelBlocking(const TunnelConfig &config, QString &error);

/// Kills every tunnel process started by this session. Safe to call from any
/// thread; call on app shutdown so ssh subprocesses do not outlive the app.
void ShutdownAllTunnels();

/// Rewrites the `host=`/`port=` tokens of a libpq/MySQL-style space-or-
/// semicolon-separated key=value connection string, appending them if
/// missing. Other tokens (user, password, dbname, ...) pass through as-is.
[[nodiscard]] QString RewriteConnStringHostPort(
	const QString &connString,
	const QString &newHost,
	int newPort);

/// Reads the current `host=`/`port=` tokens out of the same connection
/// string format `RewriteConnStringHostPort` writes.
struct ConnTarget {
	QString host;
	int port = 0;
};
[[nodiscard]] ConnTarget ExtractConnHostPort(
	const QString &connString,
	int defaultPort);

/// Reads TeleForge's stored SSH-tunnel settings and, if enabled, ensures the
/// tunnel to whatever host/port `connString` currently points at is up, then
/// returns `connString` rewritten to target the local forwarded port. Returns
/// `connString` unchanged when the tunnel is disabled. Returns an empty
/// string with `error` filled when the tunnel is enabled but could not be
/// established. Only call from a worker thread — see EnsureTunnelBlocking.
[[nodiscard]] QString ApplyTunnelIfConfigured(
	const QString &connString,
	int defaultPort,
	QString &error);

} // namespace TeleForge::Ssh
