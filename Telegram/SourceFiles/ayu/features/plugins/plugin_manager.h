#pragma once

#include "ayu/features/plugins/plugin_registry.h"

#include <QStringList>

namespace TeleForge::Plugins {

void initialize();
void reloadAll();
void startPlugin(const QString &fileName);
void stopPlugin(const QString &fileName);
[[nodiscard]] QStringList loadedPluginNames();
[[nodiscard]] std::vector<InstalledPlugin> listWithRuntimeState();

} // namespace TeleForge::Plugins
