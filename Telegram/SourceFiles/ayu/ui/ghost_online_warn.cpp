#include "ayu/ui/ghost_online_warn.h"

#include "ayu/ayu_settings.h"
#include "lang/lang_keys.h"
#include "ui/toast/toast.h"
#include "window/window_session_controller.h"

namespace Ayu::GhostOnlineWarn {
namespace {

constexpr auto kToastDuration = crl::time(4500);

[[nodiscard]] TextWithEntities textFor(Action action) {
	switch (action) {
	case Action::PinMessage:
		return { tr::ayu_GhostOnlineWarnPin(tr::now) };
	case Action::UnpinMessage:
		return { tr::ayu_GhostOnlineWarnUnpin(tr::now) };
	case Action::SendMessage:
		return { tr::ayu_GhostOnlineWarnSend(tr::now) };
	case Action::Reaction:
		return { tr::ayu_GhostOnlineWarnReaction(tr::now) };
	case Action::PollVote:
		return { tr::ayu_GhostOnlineWarnPoll(tr::now) };
	case Action::EditMessage:
		return { tr::ayu_GhostOnlineWarnEdit(tr::now) };
	case Action::Forward:
		return { tr::ayu_GhostOnlineWarnForward(tr::now) };
	case Action::StartBot:
		return { tr::ayu_GhostOnlineWarnBot(tr::now) };
	case Action::ChatPin:
		return { tr::ayu_GhostOnlineWarnChatPin(tr::now) };
	case Action::Generic:
		break;
	}
	return { tr::ayu_GhostOnlineWarnGeneric(tr::now) };
}

} // namespace

bool shouldWarn(not_null<Main::Session*> session) {
	return AyuSettings::ghost(session).isGhostModeActive();
}

void warn(
		not_null<QWidget*> parent,
		not_null<Main::Session*> session,
		Action action) {
	if (!shouldWarn(session)) {
		return;
	}
	Ui::Toast::Show(parent, {
		.text = textFor(action),
		.duration = kToastDuration,
	});
}

void warnController(
		not_null<Window::SessionController*> controller,
		Action action) {
	if (!shouldWarn(&controller->session())) {
		return;
	}
	controller->showToast(textFor(action), kToastDuration);
}

} // namespace Ayu::GhostOnlineWarn
