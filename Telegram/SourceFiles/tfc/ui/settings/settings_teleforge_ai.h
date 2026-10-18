// This is the source code of AyuGram for Desktop.
//
// Copyright @Radolyn, 2026
#pragma once

#include "settings/settings_common.h"
#include "settings/settings_common_session.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

class TeleForgeAi : public Section<TeleForgeAi> {
public:
	TeleForgeAi(QWidget *parent, not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
};

[[nodiscard]] Type TeleForgeAiId();

} // namespace Settings
