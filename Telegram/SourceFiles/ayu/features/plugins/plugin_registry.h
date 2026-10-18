#pragma once

#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

namespace TeleForge::Plugins {

struct InstalledPlugin {
	QString fileName;
	QString displayName;
	bool enabled = true;
	QString sourceChannel;
	bool loaded = false;
	bool verified = false;
};

// Install-time record that lets the loader re-check a plugin before executing
// it: the digest of the exact bytes plus the developer that signed them.
struct PluginSignature {
	QString sha256Hex;
	QString devId;
	QString signatureHex;
};

// Reduces an untrusted name (channel document name, catalog field, UI input) to
// a bare *.py file name; empty when nothing safe is left.
[[nodiscard]] QString SafePluginFileName(const QString &name);

// Absolute path of a plugin inside the plugins directory, or empty when the
// name would escape it.
[[nodiscard]] QString PluginFilePath(const QString &fileName);

[[nodiscard]] std::vector<InstalledPlugin> listInstalled();
[[nodiscard]] bool isEnabled(const QString &fileName);
void setEnabled(const QString &fileName, bool enabled);
void setSource(const QString &fileName, const QString &channel);
void setSignature(const QString &fileName, const PluginSignature &signature);
[[nodiscard]] std::optional<PluginSignature> signature(
	const QString &fileName);
bool removeInstalledFile(const QString &fileName);
void persist();

} // namespace TeleForge::Plugins
