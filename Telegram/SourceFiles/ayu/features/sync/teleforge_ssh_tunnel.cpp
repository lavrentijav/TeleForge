#include "ayu/features/sync/teleforge_ssh_tunnel.h"

#include "ayu/features/teleforge/teleforge_core.h"

#include "logs.h"

#include <crl/crl.h>

#include <future>
#include <memory>

#include <QtCore/QMap>
#include <QtCore/QMutex>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace TeleForge::Ssh {
namespace {

constexpr auto kConnectTimeoutMs = 15000;
constexpr auto kPollIntervalMs = 200;

struct RunningTunnel {
	std::unique_ptr<QProcess> process;
	int localPort = 0;
	bool ready = false;
};

// Every member below is touched only on the main thread (from crl::on_main
// lambdas), so it needs no locking of its own.
QMap<QString, std::shared_ptr<RunningTunnel>> &Registry() {
	static auto registry = QMap<QString, std::shared_ptr<RunningTunnel>>();
	return registry;
}

[[nodiscard]] QString KeyFor(const TunnelConfig &config) {
	return config.sshTarget
		+ QLatin1Char('|') + config.identityFile
		+ QLatin1Char('|') + config.remoteDbHost
		+ QLatin1Char('|') + QString::number(config.remoteDbPort);
}

[[nodiscard]] int PickFreePort() {
	auto probe = QTcpServer();
	if (!probe.listen(QHostAddress::LocalHost, 0)) {
		return 0;
	}
	const auto port = probe.serverPort();
	probe.close();
	return port;
}

struct ParsedSshTarget {
	QString userHost;
	int port = 22;
};

[[nodiscard]] ParsedSshTarget ParseSshTarget(const QString &target) {
	auto result = ParsedSshTarget();
	auto value = target.trimmed();
	const auto at = value.indexOf(QLatin1Char('@'));
	const auto colon = value.lastIndexOf(QLatin1Char(':'));
	if (colon > at) {
		const auto parsedPort = value.mid(colon + 1).toInt();
		if (parsedPort > 0) {
			result.port = parsedPort;
		}
		value = value.left(colon);
	}
	result.userHost = value;
	return result;
}

void PollUntilReady(
		std::shared_ptr<RunningTunnel> tunnel,
		int elapsedMs,
		Fn<void(bool ok)> done) {
	if (tunnel->process->state() == QProcess::NotRunning) {
		done(false);
		return;
	}
	auto socket = std::make_shared<QTcpSocket>();
	const auto finishOnce = std::make_shared<bool>(false);
	const auto finish = [=](bool ok) {
		if (*finishOnce) {
			return;
		}
		*finishOnce = true;
		socket->abort();
		done(ok);
	};
	QObject::connect(socket.get(), &QTcpSocket::connected, [=] {
		tunnel->ready = true;
		finish(true);
	});
	QObject::connect(
		socket.get(),
		&QTcpSocket::errorOccurred,
		[=](QAbstractSocket::SocketError) {
			if (*finishOnce) {
				return;
			}
			*finishOnce = true;
			const auto nextElapsed = elapsedMs + kPollIntervalMs;
			if (nextElapsed >= kConnectTimeoutMs) {
				done(false);
				return;
			}
			QTimer::singleShot(kPollIntervalMs, [=] {
				PollUntilReady(tunnel, nextElapsed, done);
			});
		});
	socket->connectToHost(QHostAddress::LocalHost, tunnel->localPort);
}

} // namespace

int EnsureTunnelBlocking(const TunnelConfig &config, QString &error) {
	if (!config.enabled) {
		return 0;
	}
	if (config.sshTarget.trimmed().isEmpty()
		|| config.remoteDbHost.trimmed().isEmpty()
		|| config.remoteDbPort <= 0) {
		error = u"Не заданы параметры SSH-туннеля (адрес входа или БД)."_q;
		return 0;
	}

	auto promise = std::make_shared<std::promise<std::pair<int, QString>>>();
	auto future = promise->get_future();

	crl::on_main([=] {
		const auto key = KeyFor(config);
		auto &registry = Registry();
		auto tunnel = registry.value(key);
		if (tunnel
			&& tunnel->process
			&& tunnel->process->state() == QProcess::NotRunning) {
			registry.remove(key);
			tunnel = nullptr;
		}
		if (tunnel && tunnel->ready) {
			promise->set_value({ tunnel->localPort, QString() });
			return;
		}

		if (!tunnel) {
			const auto localPort = PickFreePort();
			if (!localPort) {
				promise->set_value({
					0,
					u"Не удалось выделить локальный порт для туннеля."_q,
				});
				return;
			}
			const auto target = ParseSshTarget(config.sshTarget);
			auto args = QStringList{
				u"-N"_q,
				u"-L"_q,
				u"127.0.0.1:%1:%2:%3"_q
					.arg(localPort)
					.arg(config.remoteDbHost)
					.arg(config.remoteDbPort),
				target.userHost,
				u"-p"_q, QString::number(target.port),
				u"-o"_q, u"StrictHostKeyChecking=accept-new"_q,
				u"-o"_q, u"ExitOnForwardFailure=yes"_q,
				u"-o"_q, u"ServerAliveInterval=30"_q,
				u"-o"_q, u"BatchMode=yes"_q,
			};
			if (!config.identityFile.trimmed().isEmpty()) {
				args << u"-i"_q << config.identityFile.trimmed();
			}

			tunnel = std::make_shared<RunningTunnel>();
			tunnel->localPort = localPort;
			tunnel->process = std::make_unique<QProcess>();
			LOG(("TeleForge SSH tunnel: starting for %1 -> %2:%3 (local %4)")
				.arg(config.sshTarget, config.remoteDbHost)
				.arg(config.remoteDbPort)
				.arg(localPort));
			tunnel->process->start(u"ssh"_q, args);
			registry.insert(key, tunnel);
		}

		PollUntilReady(tunnel, 0, [=](bool ok) {
			if (ok) {
				promise->set_value({ tunnel->localPort, QString() });
				return;
			}
			const auto stderrText = QString::fromUtf8(
				tunnel->process->readAllStandardError()).trimmed();
			registry.remove(key);
			tunnel->process->kill();
			LOG(("TeleForge SSH tunnel: failed: %1").arg(stderrText));
			promise->set_value({
				0,
				stderrText.isEmpty()
					? u"Не удалось установить SSH-туннель (тайм-аут или ssh недоступен)."_q
					: stderrText,
			});
		});
	});

	// PollUntilReady always resolves within kConnectTimeoutMs on the main
	// thread; this extra margin only guards against the main thread being
	// stuck elsewhere (e.g. shutdown), so the worker thread is never blocked
	// forever.
	const auto status = future.wait_for(
		std::chrono::milliseconds(kConnectTimeoutMs + 5000));
	if (status != std::future_status::ready) {
		error = u"SSH-туннель не ответил вовремя."_q;
		return 0;
	}
	const auto [port, err] = future.get();
	error = err;
	return port;
}

void ShutdownAllTunnels() {
	crl::on_main([] {
		for (const auto &tunnel : Registry()) {
			if (tunnel->process) {
				tunnel->process->kill();
				tunnel->process->waitForFinished(1000);
			}
		}
		Registry().clear();
	});
}

QString RewriteConnStringHostPort(
		const QString &connString,
		const QString &newHost,
		int newPort) {
	const auto tokens = connString.split(
		QRegularExpression(u"[\\s;]+"_q),
		Qt::SkipEmptyParts);
	auto out = QStringList();
	auto sawHost = false;
	auto sawPort = false;
	for (const auto &token : tokens) {
		const auto eq = token.indexOf(QLatin1Char('='));
		if (eq <= 0) {
			out << token;
			continue;
		}
		const auto key = token.left(eq).trimmed().toLower();
		if (key == u"host"_q || key == u"server"_q) {
			out << (u"host="_q + newHost);
			sawHost = true;
		} else if (key == u"port"_q) {
			out << (u"port="_q + QString::number(newPort));
			sawPort = true;
		} else {
			out << token;
		}
	}
	if (!sawHost) {
		out << (u"host="_q + newHost);
	}
	if (!sawPort) {
		out << (u"port="_q + QString::number(newPort));
	}
	return out.join(QLatin1Char(' '));
}

ConnTarget ExtractConnHostPort(const QString &connString, int defaultPort) {
	auto result = ConnTarget{ .host = u"127.0.0.1"_q, .port = defaultPort };
	const auto tokens = connString.split(
		QRegularExpression(u"[\\s;]+"_q),
		Qt::SkipEmptyParts);
	for (const auto &token : tokens) {
		const auto eq = token.indexOf(QLatin1Char('='));
		if (eq <= 0) {
			continue;
		}
		const auto key = token.left(eq).trimmed().toLower();
		const auto value = token.mid(eq + 1).trimmed();
		if (key == u"host"_q || key == u"server"_q) {
			if (!value.isEmpty()) {
				result.host = value;
			}
		} else if (key == u"port"_q) {
			const auto parsed = value.toInt();
			if (parsed > 0) {
				result.port = parsed;
			}
		}
	}
	return result;
}

QString ApplyTunnelIfConfigured(
		const QString &connString,
		int defaultPort,
		QString &error) {
	const auto core = LoadPersonalityCore();
	if (!core || !core->sshTunnelEnabled) {
		return connString;
	}
	const auto target = ExtractConnHostPort(connString, defaultPort);
	const auto localPort = EnsureTunnelBlocking({
		.enabled = true,
		.sshTarget = core->sshTunnelTarget,
		.identityFile = core->sshTunnelIdentityFile,
		.remoteDbHost = target.host,
		.remoteDbPort = target.port,
	}, error);
	if (!localPort) {
		return QString();
	}
	return RewriteConnStringHostPort(connString, u"127.0.0.1"_q, localPort);
}

} // namespace TeleForge::Ssh
