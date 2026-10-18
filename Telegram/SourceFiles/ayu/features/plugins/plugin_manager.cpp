#include "ayu/features/plugins/plugin_manager.h"

#include "ayu/features/plugins/plugin_dev_mode.h"
#include "ayu/features/plugins/plugin_developers_store.h"
#include "ayu/features/plugins/plugin_registry.h"
#include "ayu/features/plugins/plugin_runner.h"
#include "ayu/features/plugins/plugin_trust.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>

namespace TeleForge::Plugins {
namespace {

auto g_loaded = QStringList();

[[nodiscard]] QString PluginsDir() {
	return cWorkingDir() + u"plugins"_q;
}

[[nodiscard]] bool VerifyInstalled(const QString &fileName) {
	const auto recorded = signature(fileName);
	if (!recorded) {
		return DevMode::allowUnverifiedInstalls();
	}
	const auto path = PluginFilePath(fileName);
	auto file = QFile(path);
	if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto digest = QCryptographicHash::hash(
		file.readAll(),
		QCryptographicHash::Sha256);
	if (digest.toHex() != recorded->sha256Hex.toLatin1().toLower()) {
		LOG(("TeleForge plugin: %1 changed after install").arg(fileName));
		return false;
	}
	const auto dev = findDeveloper(recorded->devId);
	if (!dev) {
		LOG(("TeleForge plugin: %1 is unknown developer %2")
			.arg(fileName, recorded->devId));
		return false;
	}
	return Trust::VerifyDeveloperPluginSignature(
		Trust::PublicKeyFromHex(dev->pubkeyHex),
		digest,
		recorded->devId,
		fileName,
		recorded->signatureHex);
}

} // namespace

void initialize() {
	reloadAll();
}

void reloadAll() {
	g_loaded.clear();
	const auto dir = QDir(PluginsDir());
	if (!dir.exists()) {
		dir.mkpath(QString());
		PluginRunner::reloadEnabled({});
		return;
	}
	auto paths = QStringList();
	for (const auto &entry : dir.entryList({ u"*.py"_q }, QDir::Files)) {
		if (!isEnabled(entry)) {
			continue;
		}
		if (!VerifyInstalled(entry)) {
			LOG(("TeleForge plugin: %1 has no verified signature, not executed")
				.arg(entry));
			continue;
		}
		const auto path = PluginFilePath(entry);
		if (path.isEmpty()) {
			continue;
		}
		paths.push_back(path);
		g_loaded.push_back(entry);
	}
	PluginRunner::reloadEnabled(paths);
}

void startPlugin(const QString &fileName) {
	setEnabled(fileName, true);
	const auto path = PluginFilePath(fileName);
	if (path.isEmpty()) {
		return;
	}
	if (!g_loaded.contains(fileName)) {
		if (!VerifyInstalled(fileName)) {
			LOG(("TeleForge plugin: %1 has no verified signature, not started")
				.arg(fileName));
			return;
		}
		PluginRunner::loadPlugin(path);
		g_loaded.push_back(fileName);
	}
}

void stopPlugin(const QString &fileName) {
	setEnabled(fileName, false);
	g_loaded.removeAll(fileName);
	// Python modules stay in the interpreter until full reload; disable prevents re-run.
}

QStringList loadedPluginNames() {
	return g_loaded;
}

std::vector<InstalledPlugin> listWithRuntimeState() {
	auto list = listInstalled();
	for (auto &plugin : list) {
		plugin.loaded = g_loaded.contains(plugin.fileName);
	}
	return list;
}

} // namespace TeleForge::Plugins
