#include "tfc/features/plugins/plugin_tool_registry.h"

#include "tfc/features/plugins/plugin_runner.h"

namespace TeleForge::Plugins {

QJsonArray PluginToolsForInference() {
	return PluginRunner::toolsJson();
}

} // namespace TeleForge::Plugins
