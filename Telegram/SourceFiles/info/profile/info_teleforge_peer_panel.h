/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

#include "data/data_peer.h"

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Info {
class Controller;
} // namespace Info

namespace Info::Profile {

[[nodiscard]] object_ptr<Ui::RpWidget> SetupTeleForgePeerPanel(
	not_null<Controller*> controller,
	not_null<Ui::RpWidget*> parent,
	not_null<PeerData*> peer);

} // namespace Info::Profile
