#include "ayu/ui/ghost_online_bar.h"

#include "ayu/ayu_settings.h"
#include "lang/lang_keys.h"
#include "ui/widgets/labels.h"
#include "ui/painter.h"
#include "styles/style_chat.h"
#include "styles/style_ayu_icons.h"

namespace Ayu::GhostOnlineWarn {

GhostOnlineBar::GhostOnlineBar(
		not_null<QWidget*> parent,
		not_null<Main::Session*> session)
: _wrap(parent, object_ptr<Ui::RpWidget>(parent)) {
	_wrap.hide(anim::type::instant);

	const auto widget = _wrap.entity();
	widget->resize(0, st::historyTranslateBarHeight);
	widget->setAttribute(Qt::WA_OpaquePaintEvent);

	widget->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		QPainter(widget).fillRect(clip, st::historyComposeButtonBg);
	}, widget->lifetime());

	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		widget,
		tr::ayu_GhostOnlineWarnBar(),
		st::historyTranslateLabel);
	const auto icon = Ui::CreateChild<Ui::RpWidget>(widget);
	label->setAttribute(Qt::WA_TransparentForMouseEvents);
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);
	icon->resize(st::ayuGhostIcon.size());
	icon->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(icon);
		st::ayuGhostIcon.paint(p, 0, 0, icon->width());
	}, icon->lifetime());

	const auto updateGeometry = [=] {
		const auto full = _wrap.width() - icon->width();
		const auto skip = st::semiboldFont->spacew * 2;
		const auto natural = label->textMaxWidth();
		const auto top = (_wrap.height() - label->height()) / 2;
		const auto available = full - 2 * skip;
		label->resizeToWidth(std::min(natural, available));
		label->moveToLeft((full - label->width()) / 2 + icon->width(), top);
		icon->move(
			label->x() - icon->width() - skip / 2,
			(_wrap.height() - icon->height()) / 2);
	};

	_wrap.sizeValue() | rpl::on_next([=](QSize) {
		updateGeometry();
	}, lifetime());

	AyuSettings::ghost(session).ghostModeActiveValue(
	) | rpl::on_next([=](bool active) {
		if (active) {
			_wrap.show(anim::type::normal);
		} else {
			_wrap.hide(anim::type::normal);
		}
	}, lifetime());
}

void GhostOnlineBar::raise() {
	_wrap.raise();
}

void GhostOnlineBar::move(int x, int y) {
	_wrap.move(x, y);
}

void GhostOnlineBar::resizeToWidth(int width) {
	_wrap.entity()->resizeToWidth(width);
}

int GhostOnlineBar::height() const {
	return _wrap.height();
}

rpl::producer<int> GhostOnlineBar::heightValue() const {
	return _wrap.heightValue();
}

void GhostOnlineBar::finishAnimating() {
	_wrap.finishAnimating();
}

} // namespace Ayu::GhostOnlineWarn
