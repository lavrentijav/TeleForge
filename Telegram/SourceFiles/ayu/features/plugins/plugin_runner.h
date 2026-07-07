#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>

namespace TeleForge::Plugins {

// Owns the embedded Python interpreter on a dedicated background thread so a
// slow or unstable plugin never blocks the TeleForge UI thread. All Python
// execution (init, loading, event dispatch) happens on that thread; native API
// calls that touch application state marshal back to the main thread.
class PluginRunner {
public:
	static void ensureStarted();

	// Re-execute the given enabled plugin files (clears prior registrations).
	static void reloadEnabled(const QStringList &paths);

	// Execute a single plugin file on the plugin thread.
	static void loadPlugin(const QString &path);

	// Fire-and-forget: dispatch a core event to subscribed plugin hooks.
	static void fireEvent(const QString &name, const QString &payloadJson);

	// Invoke a plugin-registered menu item by id (fire-and-forget).
	static void invokeMenuItem(const QString &itemId);

	// Cached plugin menu items as a JSON array of {id, title}. Main-thread safe.
	[[nodiscard]] static QString menuItemsJson();

	// Clears every plugin registration (does not stop the interpreter thread).
	static void unloadAll();

	// Stops the interpreter thread. Call once at application shutdown.
	static void shutdown();

	[[nodiscard]] static QJsonArray toolsJson();
	static QString callTool(
		const QString &name,
		const QString &argumentsJson);
};

} // namespace TeleForge::Plugins
