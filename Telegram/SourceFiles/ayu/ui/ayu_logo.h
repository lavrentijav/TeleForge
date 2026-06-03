// Copyright @Radolyn, 2026
#pragma once

#define ICON(name, value) const auto name##_ICON = QStringLiteral(value)

namespace TeleForgeAssets {

ICON(DEFAULT, "default");

void loadAppIco();
QString appIcoPath();

QImage loadPreview(const QString &name);

QString currentAppLogoName();
QImage currentAppLogo();
QImage currentAppLogoPad();

} // namespace TeleForgeAssets
