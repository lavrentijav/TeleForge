#include "ayu/features/plugins/plugin_registry.h"

#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace TeleForge::Plugins {
namespace {

[[nodiscard]] QString StatePath() {
	return cWorkingDir() + u"tdata/teleforge_plugins.json"_q;
}

[[nodiscard]] QJsonObject LoadRoot() {
	auto file = QFile(StatePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	return doc.isObject() ? doc.object() : QJsonObject();
}

void SaveRoot(const QJsonObject &root) {
	QDir().mkpath(cWorkingDir() + u"tdata"_q);
	auto file = QFile(StatePath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
	}
}

[[nodiscard]] QJsonObject EntryForFile(
		const QJsonObject &root,
		const QString &fileName) {
	const auto plugins = root.value(u"plugins"_q).toArray();
	for (const auto v : plugins) {
		const auto o = v.toObject();
		if (o.value(u"file"_q).toString() == fileName) {
			return o;
		}
	}
	return {};
}

void UpsertEntry(const QString &fileName, const QJsonObject &patch) {
	auto root = LoadRoot();
	auto plugins = root.value(u"plugins"_q).toArray();
	auto found = false;
	for (auto i = 0; i < plugins.size(); ++i) {
		auto o = plugins.at(i).toObject();
		if (o.value(u"file"_q).toString() == fileName) {
			for (auto it = patch.begin(); it != patch.end(); ++it) {
				o.insert(it.key(), it.value());
			}
			plugins.replace(i, o);
			found = true;
			break;
		}
	}
	if (!found) {
		auto o = patch;
		o.insert(u"file"_q, fileName);
		plugins.push_back(o);
	}
	root.insert(u"plugins"_q, plugins);
	SaveRoot(root);
}

} // namespace

std::vector<InstalledPlugin> listInstalled() {
	const auto root = LoadRoot();
	const auto dir = QDir(cWorkingDir() + u"plugins"_q);
	const auto files = dir.exists()
		? dir.entryList({ u"*.py"_q }, QDir::Files)
		: QStringList();
	std::vector<InstalledPlugin> result;
	result.reserve(files.size());
	for (const auto &file : files) {
		const auto entry = EntryForFile(root, file);
		auto item = InstalledPlugin{
			.fileName = file,
			.displayName = entry.value(u"name"_q).toString().isEmpty()
				? file
				: entry.value(u"name"_q).toString(),
			.enabled = entry.contains(u"enabled"_q)
				? entry.value(u"enabled"_q).toBool(true)
				: true,
			.sourceChannel = entry.value(u"source"_q).toString(),
		};
		result.push_back(std::move(item));
	}
	return result;
}

bool isEnabled(const QString &fileName) {
	const auto entry = EntryForFile(LoadRoot(), fileName);
	if (entry.isEmpty()) {
		return true;
	}
	return entry.value(u"enabled"_q).toBool(true);
}

void setEnabled(const QString &fileName, bool enabled) {
	UpsertEntry(fileName, { { u"enabled"_q, enabled } });
}

void setSource(const QString &fileName, const QString &channel) {
	UpsertEntry(fileName, { { u"source"_q, channel } });
}

bool removeInstalledFile(const QString &fileName) {
	const auto path = QDir(cWorkingDir() + u"plugins"_q).absoluteFilePath(fileName);
	if (!QFile::exists(path)) {
		return false;
	}
	return QFile::remove(path);
}

void persist() {
	SaveRoot(LoadRoot());
}

} // namespace TeleForge::Plugins
