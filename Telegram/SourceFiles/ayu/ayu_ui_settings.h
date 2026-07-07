// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// NOTE(TeleForge): this header was reconstructed because the original
// ayu_ui_settings.h was referenced by several source files but never
// committed to the repository (the build failed with C1083: cannot open
// 'ayu/ayu_ui_settings.h'). It is a lightweight UI-layer cache of a few
// appearance settings, synced from AyuSettings (see ayu_infra.cpp /
// ayu_settings.cpp) and read by the userpic renderer. If the original file
// resurfaces, prefer it and verify kMaxAvatarCorners.
#pragma once

#include <QtCore/QString>

namespace AyuUiSettings {

// Avatar corner "roundness": 0 = square, kMaxAvatarCorners = full circle.
// The renderer packs the value as a 5-bit field (`corners & 0x1F`), so the
// maximum is 31.
inline constexpr auto kMaxAvatarCorners = 31;

namespace details {

[[nodiscard]] inline int &avatarCornersStorage() {
	static auto value = 0;
	return value;
}

[[nodiscard]] inline QString &monoFontStorage() {
	static auto value = QString();
	return value;
}

[[nodiscard]] inline bool &materialSwitchesStorage() {
	static auto value = true;
	return value;
}

[[nodiscard]] inline double &wideMultiplierStorage() {
	static auto value = 1.0;
	return value;
}

} // namespace details

[[nodiscard]] inline int getAvatarCorners() {
	return details::avatarCornersStorage();
}

inline void setAvatarCorners(int value) {
	details::avatarCornersStorage() = value;
}

[[nodiscard]] inline QString getMonoFont() {
	return details::monoFontStorage();
}

inline void setMonoFont(const QString &value) {
	details::monoFontStorage() = value;
}

[[nodiscard]] inline bool getMaterialSwitches() {
	return details::materialSwitchesStorage();
}

inline void setMaterialSwitches(bool value) {
	details::materialSwitchesStorage() = value;
}

[[nodiscard]] inline double getWideMultiplier() {
	return details::wideMultiplierStorage();
}

inline void setWideMultiplier(double value) {
	details::wideMultiplierStorage() = value;
}

} // namespace AyuUiSettings
