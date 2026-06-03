#include "ayu/features/teleforge/teleforge_paths.h"

#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QStringList>

namespace TeleForge {

QString TeleForgeModelsDirectory() {
	return QDir::cleanPath(cWorkingDir() + QStringLiteral("models"));
}

void TeleForgeEnsureModelsDirectoryExists() {
	QDir().mkpath(TeleForgeModelsDirectory());
}

QStringList TeleForgeListGgufInModelsDirectory() {
	const auto root = TeleForgeModelsDirectory();
	QDir dir(root);
	if (!dir.exists()) {
		return {};
	}
	const auto names = dir.entryList(
		QStringList{ QStringLiteral("*.gguf") },
		QDir::Files,
		QDir::Name);
	auto out = QStringList();
	out.reserve(names.size());
	for (const auto &n : names) {
		out.push_back(QDir::cleanPath(root + QLatin1Char('/') + n));
	}
	return out;
}

} // namespace TeleForge
