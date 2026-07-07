// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// NOTE(TeleForge): sourced from lavrentijav/lib_ui (ayu/ayu_ui_settings.h);
// the fork's Telegram code depends on it but the pinned lib_ui submodule
// predates it, so it lives here in the Telegram target instead.
#pragma once

#include <QtCore/QString>

namespace AyuUiSettings {

inline constexpr int kMaxAvatarCorners = 23;

void setMonoFont(QString newFont);
QString getMonoFont();

void setWideMultiplier(double val);

bool isWideMultiplied();
int getWideMultiplied(int width, double mult);

void setMaterialSwitches(bool val);
bool isMaterialSwitches();

void setAvatarCorners(int val);
int getAvatarCorners();

}
