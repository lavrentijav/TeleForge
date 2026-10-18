#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace TeleForge {

[[nodiscard]] QString TeleForgeModelsDirectory();
void TeleForgeEnsureModelsDirectoryExists();
[[nodiscard]] QStringList TeleForgeListGgufInModelsDirectory();

} // namespace TeleForge
