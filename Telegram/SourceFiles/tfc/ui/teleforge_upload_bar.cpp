// Copyright @Radolyn, 2026
#include "tfc/ui/teleforge_upload_bar.h"

#include "core/application.h"
#include "mainwidget.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "styles/style_widgets.h"

#include <QtCore/QPointer>
#include <QtGui/QPainter>

#include <algorithm>

namespace TeleForge::UI {
namespace {

struct State {
	QPointer<Ui::RpWidget> widget;
	float value = 0.f;
};

[[nodiscard]] State &G() {
	static auto state = State();
	return state;
}

[[nodiscard]] int BarHeight() {
	return st::normalFont->height;
}

} // namespace

void ShowUploadProgress(int done, int total) {
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	const auto controller = window->sessionController();
	if (!controller) {
		return;
	}
	const auto content = controller->content();
	QWidget *parent = content;
	const auto columnWidth = std::max(1, content->dialogsWidth());
	auto &g = G();
	if (!g.widget || g.widget->parentWidget() != parent) {
		if (g.widget) {
			g.widget->deleteLater();
		}
		const auto bar = Ui::CreateChild<Ui::RpWidget>(parent);
		g.widget = bar;
		bar->resize(columnWidth, BarHeight());
		bar->paintRequest(
		) | rpl::on_next([bar](QRect) {
			auto p = QPainter(bar);
			const auto w = bar->width();
			const auto h = bar->height();
			const auto value = std::clamp(G().value, 0.f, 1.f);
			p.fillRect(0, 0, w, h, st::windowBgOver);
			const auto filled = int(w * value);
			if (filled > 0) {
				p.fillRect(0, 0, filled, h, st::windowBgActive);
			}
			p.setFont(st::normalFont);
			p.setPen(st::windowFgActive);
			p.drawText(
				QRect(0, 0, w, h),
				Qt::AlignCenter,
				QString::number(int(value * 100)) + u"%"_q);
		}, bar->lifetime());
	}
	g.value = (total > 0) ? (float(done) / float(total)) : 1.f;
	const auto bar = g.widget.data();
	bar->setGeometry(
		0,
		parent->height() - BarHeight(),
		columnWidth,
		BarHeight());
	bar->raise();
	bar->show();
	bar->update();
}

void HideUploadProgress() {
	auto &g = G();
	if (g.widget) {
		g.widget->hide();
		g.widget->deleteLater();
		g.widget = nullptr;
	}
	g.value = 0.f;
}

} // namespace TeleForge::UI
