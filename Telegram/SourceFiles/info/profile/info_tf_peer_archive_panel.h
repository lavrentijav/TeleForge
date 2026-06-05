// Copyright @Radolyn, 2026

#pragma once

#include "ui/rp_widget.h"

namespace Info {
class Controller;
} // namespace Info

namespace Info::Profile {

[[nodiscard]] object_ptr<Ui::RpWidget> SetupPeerArchivePanel(
	not_null<Controller*> controller,
	not_null<Ui::RpWidget*> parent,
	not_null<PeerData*> peer);

} // namespace Info::Profile
