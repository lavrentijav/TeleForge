#pragma once

#include <QJsonArray>
#include <QString>

namespace TeleForge::Plugins {

class PluginRunner {
public:
	static bool loadPlugin(const QString &path);
	static void unloadAll();
	[[nodiscard]] static QJsonArray toolsJson();
	[[nodiscard]] static QString callTool(
		const QString &name,
		const QString &argumentsJson);
};

} // namespace TeleForge::Plugins
