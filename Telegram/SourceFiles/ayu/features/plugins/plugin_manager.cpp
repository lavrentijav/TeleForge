#include "ayu/features/plugins/plugin_manager.h"

#include "ayu/features/plugins/plugin_runner.h"
#include "settings.h"

#include <QtCore/QDir>

namespace TeleForge::Plugins {
namespace {

auto g_loaded = QStringList();

[[nodiscard]] QString PluginsDir() {
	return cWorkingDir() + u"plugins"_q;
}

} // namespace

void initialize() {
	reloadAll();
}

void reloadAll() {
	PluginRunner::unloadAll();
	g_loaded.clear();
	const auto dir = QDir(PluginsDir());
	if (!dir.exists()) {
		dir.mkpath(QString());
		return;
	}
	for (const auto &entry : dir.entryList({ u"*.py"_q }, QDir::Files)) {
		if (!isEnabled(entry)) {
			continue;
		}
		const auto path = dir.absoluteFilePath(entry);
		if (PluginRunner::loadPlugin(path)) {
			g_loaded.push_back(entry);
		}
	}
}

void startPlugin(const QString &fileName) {
	setEnabled(fileName, true);
	const auto path = QDir(PluginsDir()).absoluteFilePath(fileName);
	if (!g_loaded.contains(fileName) && PluginRunner::loadPlugin(path)) {
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
