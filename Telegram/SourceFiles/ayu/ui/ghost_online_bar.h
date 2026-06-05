#pragma once

#include "ui/rp_widget.h"
#include "ui/wrap/slide_wrap.h"
#include "rpl/lifetime.h"
#include "base/basic_types.h"

namespace Main {
class Session;
} // namespace Main

namespace Ayu::GhostOnlineWarn {

class GhostOnlineBar final {
public:
	GhostOnlineBar(
		not_null<QWidget*> parent,
		not_null<Main::Session*> session);

	void raise();
	void move(int x, int y);
	void resizeToWidth(int width);
	[[nodiscard]] int height() const;
	[[nodiscard]] rpl::producer<int> heightValue() const;
	void finishAnimating();

	[[nodiscard]] rpl::lifetime &lifetime() {
		return _wrap.lifetime();
	}

private:
	Ui::SlideWrap<Ui::RpWidget> _wrap;
};

} // namespace Ayu::GhostOnlineWarn
