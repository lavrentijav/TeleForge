#include "ayu/features/plugins/plugin_developers_store.h"

#include "ayu/features/plugins/plugin_catalog.h"
#include "ayu/features/plugins/plugin_trust.h"
#include <crl/crl.h>
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace TeleForge::Plugins {
namespace {

auto g_developers = QVector<TrustedDeveloper>();

[[nodiscard]] QString NormalizeUsername(QString username) {
	username = username.trimmed().toLower();
	if (username.startsWith(u'@')) {
		username = username.mid(1);
	}
	return username;
}

[[nodiscard]] QVector<TrustedDeveloper> ParseDevelopersText(const QByteArray &raw) {
	auto result = QVector<TrustedDeveloper>();
	for (auto line : QString::fromUtf8(raw).split(u'\n')) {
		line = line.trimmed();
		if (line.isEmpty() || line.startsWith(u'#')) {
			continue;
		}
		const auto parts = line.split(u'|');
		if (parts.size() < 5) {
			continue;
		}
		auto dev = TrustedDeveloper{
			.devId = parts[0].trimmed(),
			.channelUsername = NormalizeUsername(parts[1]),
			.title = parts[2].trimmed(),
			.pubkeyHex = parts[3].trimmed().toLower(),
			.rootSignatureHex = parts[4].trimmed().toLower(),
		};
		dev.rootVerified = Trust::VerifyRootDeveloperAttestation(
			dev.devId,
			dev.channelUsername,
			dev.pubkeyHex,
			dev.rootSignatureHex);
		if (dev.rootVerified) {
			result.push_back(std::move(dev));
		}
	}
	return result;
}

void SaveCache(const QByteArray &raw) {
	QDir().mkpath(cWorkingDir() + u"tdata"_q);
	auto file = QFile(cWorkingDir() + u"tdata/teleforge_developers.txt"_q);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(raw);
	}
}

[[nodiscard]] QByteArray LoadCache() {
	auto file = QFile(cWorkingDir() + u"tdata/teleforge_developers.txt"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return file.readAll();
}

void Download(const QUrl &url, Fn<void(QByteArray, QString)> done) {
	const auto manager = new QNetworkAccessManager();
	QObject::connect(manager, &QNetworkAccessManager::finished, manager,
		[=](QNetworkReply *reply) {
			reply->deleteLater();
			manager->deleteLater();
			if (reply->error() != QNetworkReply::NoError) {
				done({}, reply->errorString());
				return;
			}
			done(reply->readAll(), {});
		});
	manager->get(QNetworkRequest(url));
}

} // namespace

QVector<TrustedDeveloper> cachedTrustedDevelopers() {
	return g_developers;
}

const TrustedDeveloper *findDeveloper(const QString &devId) {
	const auto id = devId.trimmed();
	for (const auto &dev : g_developers) {
		if (dev.devId == id) {
			return &dev;
		}
	}
	return nullptr;
}

bool isTrustedChannel(const QString &username) {
	const auto normalized = NormalizeUsername(username);
	for (const auto &dev : g_developers) {
		if (dev.channelUsername == normalized) {
			return true;
		}
	}
	return false;
}

void fetchTrustedDevelopers(DevelopersCallback done) {
	const auto url = QUrl(QString::fromLatin1(Catalog::kDevelopersUrl));
	Download(url, [=](QByteArray body, QString error) {
		crl::on_main([=, body = std::move(body), error = std::move(error)]() mutable {
			if (!error.isEmpty() && body.isEmpty()) {
				const auto cached = LoadCache();
				if (!cached.isEmpty()) {
					g_developers = ParseDevelopersText(cached);
					done(g_developers, u"Офлайн: сохранённый список разработчиков."_q);
					return;
				}
				done({}, u"Не удалось загрузить developers.txt: %1"_q.arg(error));
				return;
			}
			SaveCache(body);
			g_developers = ParseDevelopersText(body);
			const auto msg = g_developers.isEmpty()
				? u"Нет удостоверенных разработчиков (проверьте подписи корня)."_q
				: QString();
			done(g_developers, msg);
		});
	});
}

} // namespace TeleForge::Plugins
