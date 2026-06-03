#include "ayu/features/plugins/plugin_catalog_store.h"

#include "ayu/features/plugins/plugin_catalog.h"
#include "ayu/features/plugins/plugin_dev_mode.h"
#include "ayu/features/plugins/plugin_developers_store.h"
#include "ayu/features/plugins/plugin_manager.h"
#include "ayu/features/plugins/plugin_registry.h"
#include "ayu/features/plugins/plugin_trust.h"
#include <crl/crl.h>
#include "logs.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <memory>

namespace TeleForge::Plugins {
namespace {

auto g_cached = QVector<CatalogEntry>();

[[nodiscard]] bool IsHttpsUrl(const QString &url) {
	const auto parsed = QUrl(url.trimmed());
	return parsed.isValid()
		&& parsed.scheme() == u"https"_q
		&& !parsed.host().isEmpty();
}

[[nodiscard]] QString SafeFileName(QString name) {
	name = name.trimmed();
	if (name.isEmpty()) {
		return {};
	}
	name = QFileInfo(name).fileName();
	if (!name.endsWith(u".py"_q, Qt::CaseInsensitive)
		&& !name.endsWith(u".plugin"_q, Qt::CaseInsensitive)) {
		if (name.endsWith(u".py.txt"_q)) {
			name.chop(4);
		} else {
			name += u".py"_q;
		}
	}
	return name;
}

[[nodiscard]] QVector<CatalogEntry> ParseCatalogText(const QByteArray &raw) {
	auto entries = QVector<CatalogEntry>();
	const auto lines = QString::fromUtf8(raw).split(u'\n');
	for (auto line : lines) {
		line = line.trimmed();
		if (line.isEmpty() || line.startsWith(u'#')) {
			continue;
		}
		const auto parts = line.split(u'|');
		if (parts.size() < 3) {
			continue;
		}
		const auto fileName = SafeFileName(parts[0]);
		const auto title = parts.size() > 1
			? parts[1].trimmed()
			: fileName;
		const auto downloadUrl = parts.size() > 2
			? parts[2].trimmed()
			: QString();
		const auto devId = parts.size() > 3 ? parts[3].trimmed() : QString();
		const auto pluginSig = parts.size() > 4
			? parts[4].trimmed().toLower()
			: QString();
		const auto source = parts.size() > 5
			? parts[5].trimmed()
			: (devId.isEmpty() ? u"Каталог TeleForge"_q : devId);
		if (fileName.isEmpty() || !IsHttpsUrl(downloadUrl)) {
			continue;
		}
		entries.push_back({
			.fileName = fileName,
			.title = title.isEmpty() ? fileName : title,
			.downloadUrl = downloadUrl,
			.devId = devId,
			.pluginSignatureHex = pluginSig,
			.sourceLabel = source,
		});
	}
	return entries;
}

void SaveCatalogCache(const QByteArray &raw) {
	const auto path = cWorkingDir() + u"tdata/teleforge_plugins_catalog.txt"_q;
	QDir().mkpath(cWorkingDir() + u"tdata"_q);
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(raw);
	}
}

[[nodiscard]] QByteArray LoadCatalogCache() {
	const auto path = cWorkingDir() + u"tdata/teleforge_plugins_catalog.txt"_q;
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return file.readAll();
}

void DownloadBytes(
		const QUrl &url,
		Fn<void(QByteArray, QString)> done) {
	const auto manager = new QNetworkAccessManager();
	const auto request = QNetworkRequest(url);
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
	manager->get(request);
}

} // namespace

QVector<CatalogEntry> cachedCatalog() {
	return g_cached;
}

void clearCatalogCache() {
	g_cached.clear();
}

void fetchCatalog(CatalogCallback done) {
	const auto url = QUrl(QString::fromLatin1(Catalog::kCatalogUrl));
	if (!IsHttpsUrl(url.toString())) {
		done({}, u"Некорректный URL каталога."_q);
		return;
	}
	DownloadBytes(url, [=](QByteArray body, QString error) {
		crl::on_main([=, body = std::move(body), error = std::move(error)]() mutable {
			if (!error.isEmpty() && body.isEmpty()) {
				const auto cached = LoadCatalogCache();
				if (!cached.isEmpty()) {
					g_cached = ParseCatalogText(cached);
					done(g_cached, u"Офлайн: показан сохранённый каталог."_q);
					return;
				}
				done({}, u"Не удалось загрузить каталог: %1"_q.arg(error));
				return;
			}
			SaveCatalogCache(body);
			g_cached = ParseCatalogText(body);
			const auto msg = g_cached.isEmpty()
				? u"Каталог пуст или неверный формат."_q
				: QString();
			done(g_cached, msg);
		});
	});
}

[[nodiscard]] bool VerifyPluginPayload(
		const CatalogEntry &entry,
		const QByteArray &body,
		QString &error) {
	if (DevMode::allowUnverifiedInstalls()) {
		return true;
	}
	if (entry.devId.isEmpty() || entry.pluginSignatureHex.isEmpty()) {
		error = u"В каталоге нет подписи разработчика."_q;
		return false;
	}
	const auto dev = findDeveloper(entry.devId);
	if (!dev) {
		error = u"Разработчик не в списке удостоверенных."_q;
		return false;
	}
	const auto pub = Trust::PublicKeyFromHex(dev->pubkeyHex);
	const auto sha = QCryptographicHash::hash(body, QCryptographicHash::Sha256);
	if (!Trust::VerifyDeveloperPluginSignature(
			pub,
			sha,
			entry.devId,
			entry.fileName,
			entry.pluginSignatureHex)) {
		error = u"Подпись плагина не совпадает с ключом разработчика."_q;
		return false;
	}
	return true;
}

void installCatalogEntry(const CatalogEntry &entry, DoneCallback done) {
	if (!IsHttpsUrl(entry.downloadUrl)) {
		done(false, u"Недопустимая ссылка на плагин."_q);
		return;
	}
	const auto fileName = SafeFileName(entry.fileName);
	if (fileName.isEmpty()) {
		done(false, u"Некорректное имя файла."_q);
		return;
	}
	DownloadBytes(QUrl(entry.downloadUrl), [=](QByteArray body, QString error) {
		crl::on_main([=, body = std::move(body), error = std::move(error)]() mutable {
			if (!error.isEmpty() || body.isEmpty()) {
				done(false, u"Скачивание не удалось: %1"_q.arg(error));
				return;
			}
			auto verifyError = QString();
			if (!VerifyPluginPayload(entry, body, verifyError)) {
				done(false, verifyError);
				return;
			}
			const auto pluginsDir = cWorkingDir() + u"plugins"_q;
			QDir().mkpath(pluginsDir);
			const auto dest = QDir(pluginsDir).absoluteFilePath(fileName);
			auto file = QFile(dest);
			if (!file.open(QIODevice::WriteOnly)) {
				done(false, u"Не удалось записать %1"_q.arg(fileName));
				return;
			}
			file.write(body);
			file.close();
			setSource(fileName, entry.sourceLabel);
			setEnabled(fileName, true);
			reloadAll();
			const auto suffix = DevMode::allowUnverifiedInstalls()
				? u" (режим разработчика, без проверки)"_q
				: u" (подпись проверена)"_q;
			done(true, u"Установлено%1: %2"_q.arg(suffix, fileName));
		});
	});
}

} // namespace TeleForge::Plugins
