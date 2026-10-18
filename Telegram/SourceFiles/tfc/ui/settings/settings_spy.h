// Copyright @Radolyn, 2026
#pragma once

#include "settings/settings_common.h"
#include "settings/settings_common_session.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

class TfSpy : public Section<TfSpy> {
public:
	TfSpy(QWidget *parent, not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

	not_null<Window::SessionController*> _controller;
};

[[nodiscard]] Type TfSpyId();

} // namespace Settings
