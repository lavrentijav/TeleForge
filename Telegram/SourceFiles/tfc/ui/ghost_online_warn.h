#pragma once

#include "base/weak_ptr.h"
#include "base/basic_types.h"

class QWidget;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu::GhostOnlineWarn {

enum class Action {
	PinMessage,
	UnpinMessage,
	SendMessage,
	Reaction,
	PollVote,
	EditMessage,
	Forward,
	StartBot,
	ChatPin,
	Generic,
};

[[nodiscard]] bool shouldWarn(not_null<Main::Session*> session);

void warn(
	not_null<QWidget*> parent,
	not_null<Main::Session*> session,
	Action action);

void warnController(
	not_null<Window::SessionController*> controller,
	Action action);

} // namespace Ayu::GhostOnlineWarn
