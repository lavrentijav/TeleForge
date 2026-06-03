#include "ayu/features/plugins/plugin_dev_mode.h"

#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace TeleForge::Plugins::DevMode {
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

} // namespace

bool allowUnverifiedInstalls() {
	return LoadRoot().value(u"allowUnverifiedInstalls"_q).toBool(false);
}

void setAllowUnverifiedInstalls(const bool enabled) {
	auto root = LoadRoot();
	root.insert(u"allowUnverifiedInstalls"_q, enabled);
	SaveRoot(root);
}

} // namespace TeleForge::Plugins::DevMode
