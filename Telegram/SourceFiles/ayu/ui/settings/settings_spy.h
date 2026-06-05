// Copyright @Radolyn, 2026
#pragma once

#include "settings/settings_common.h"
#include "settings/settings_common_session.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

class AyuSpy : public Section<AyuSpy> {
public:
	AyuSpy(QWidget *parent, not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

	not_null<Window::SessionController*> _controller;
};

[[nodiscard]] Type AyuSpyId();

} // namespace Settings
