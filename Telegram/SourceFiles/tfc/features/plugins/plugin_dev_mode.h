#pragma once

namespace TeleForge::Plugins::DevMode {

// Seconds to wait after all acknowledgement checkboxes before the final switch unlocks.
inline constexpr auto kCooldownSeconds = 30;

[[nodiscard]] bool allowUnverifiedInstalls();
void setAllowUnverifiedInstalls(bool enabled);

} // namespace TeleForge::Plugins::DevMode
