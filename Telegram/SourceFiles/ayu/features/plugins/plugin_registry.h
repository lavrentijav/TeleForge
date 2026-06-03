#pragma once

#include <QString>
#include <QStringList>
#include <vector>

namespace TeleForge::Plugins {

struct InstalledPlugin {
	QString fileName;
	QString displayName;
	bool enabled = true;
	QString sourceChannel;
	bool loaded = false;
};

[[nodiscard]] std::vector<InstalledPlugin> listInstalled();
[[nodiscard]] bool isEnabled(const QString &fileName);
void setEnabled(const QString &fileName, bool enabled);
void setSource(const QString &fileName, const QString &channel);
bool removeInstalledFile(const QString &fileName);
void persist();

} // namespace TeleForge::Plugins
