// Copyright @Radolyn, 2026

#include "ayu/features/teleforge/tf_archive_userpics_box.h"

#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QFile>
#include <QPainter>

namespace TeleForge::PeerArchive {
namespace {

[[nodiscard]] QString FormatTs(int ts) {
	if (!ts) {
		return QString();
	}
	return QDateTime::fromSecsSinceEpoch(ts).toString(u"dd.MM.yyyy HH:mm"_q);
}

class UserpicViewer final : public Ui::RpWidget {
public:
	UserpicViewer(
		QWidget *parent,
		const std::vector<UserpicEntry> &entries)
	: RpWidget(parent)
	, _entries(entries) {
		resize(320, 320);
	}

	[[nodiscard]] rpl::producer<QString> caption() const {
		return _caption.value();
	}

	void showIndex(int index) {
		if (_entries.empty()) {
			return;
		}
		_index = std::clamp(index, 0, int(_entries.size()) - 1);
		_image = QImage();
		const auto &entry = _entries[_index];
		if (QFile::exists(entry.localPath)) {
			_image = QImage(entry.localPath);
		}
		auto parts = QStringList{
			u"Аватар %1 из %2"_q.arg(_index + 1).arg(_entries.size()),
			FormatTs(entry.observedAt),
		};
		if (entry.removed || entry.fileMissing) {
			parts.push_back(u"удалён"_q);
		}
		_caption = parts.join(u" · "_q);
		update();
	}

	void next() {
		showIndex(_index + 1);
	}

	void prev() {
		showIndex(_index - 1);
	}

	[[nodiscard]] bool hasMultiple() const {
		return _entries.size() > 1;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.fillRect(rect(), st::boxBg->c);
		if (_image.isNull()) {
			p.setPen(st::windowSubTextFg);
			p.drawText(rect(), Qt::AlignCenter, u"Файл недоступен"_q);
		} else {
			const auto target = _image.scaled(
				size(),
				Qt::KeepAspectRatio,
				Qt::SmoothTransformation);
			const auto pos = QPoint(
				(width() - target.width()) / 2,
				(height() - target.height()) / 2);
			p.drawImage(QRect(pos, target.size()), target);
		}
		const auto &entry = _entries[_index];
		if (entry.removed || entry.fileMissing) {
			const auto badge = st::menuIconDelete;
			const auto iconSize = 22;
			const auto iconPos = QPoint(width() - iconSize - 8, 8);
			p.setBrush(st::boxBg->c);
			p.setPen(Qt::NoPen);
			p.drawEllipse(
				QRect(iconPos, QSize(iconSize + 6, iconSize + 6)));
			badge.paint(
				p,
				iconPos.x() + 3,
				iconPos.y() + 3,
				width());
		}
	}

private:
	const std::vector<UserpicEntry> _entries;
	int _index = 0;
	QImage _image;
	rpl::variable<QString> _caption;

};

} // namespace

void ShowArchiveUserpicsBox(
		not_null<Window::SessionController*> controller,
		const std::vector<UserpicEntry> &userpics) {
	if (userpics.empty()) {
		controller->showToast(u"Нет сохранённых аватаров."_q);
		return;
	}
	auto entries = userpics;

	controller->show(Box([entries = std::move(entries)](
			not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Архив аватаров"_q));
		box->setWidth(st::boxWideWidth);

		const auto viewer = box->addRow(
			object_ptr<UserpicViewer>(box, entries),
			style::margins());
		viewer->showIndex(0);

		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				viewer->caption(),
				st::boxLabel),
			st::boxRowPadding);

		const auto nav = box->addRow(
			object_ptr<Ui::RpWidget>(box));
		nav->resize(0, st::defaultBoxButton.height);

		const auto prev = Ui::CreateChild<Ui::RoundButton>(
			nav,
			rpl::single(u"←"_q),
			st::defaultBoxButton);
		const auto next = Ui::CreateChild<Ui::RoundButton>(
			nav,
			rpl::single(u"→"_q),
			st::defaultBoxButton);

		const auto layoutNav = [=] {
			const auto w = nav->width();
			const auto bw = prev->width();
			prev->move((w / 2) - bw - st::boxRowPadding.left(), 0);
			next->move((w / 2) + st::boxRowPadding.right(), 0);
			prev->setVisible(viewer->hasMultiple());
			next->setVisible(viewer->hasMultiple());
		};
		nav->widthValue() | rpl::on_next([=] { layoutNav(); }, nav->lifetime());
		layoutNav();

		prev->setClickedCallback([=] { viewer->prev(); });
		next->setClickedCallback([=] { viewer->next(); });

		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

} // namespace TeleForge::PeerArchive
