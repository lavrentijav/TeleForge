// Copyright @Radolyn, 2026

#include "ayu/features/teleforge/tf_archive_users_box.h"

#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QFile>
#include <QPainter>
#include <QPainterPath>

namespace TeleForge::PeerArchive {
namespace {

constexpr auto kRowHeight = 56;
constexpr auto kAvatarSize = 42;

class ArchivedUserListRow final : public Ui::RippleButton {
public:
	ArchivedUserListRow(
		QWidget *parent,
		const ArchivedUserRow &row)
	: RippleButton(parent, st::defaultRippleAnimation)
	, _row(row) {
		if (QFile::exists(_row.userpicPath)) {
			_image = QImage(_row.userpicPath);
		}
		resize(0, kRowHeight);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.fillRect(rect(), st::boxBg->c);

		const auto left = st::boxRowPadding.left();
		const auto avatarRect = QRect(
			left,
			(height() - kAvatarSize) / 2,
			kAvatarSize,
			kAvatarSize);

		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawEllipse(avatarRect);

		if (!_image.isNull()) {
			p.save();
			QPainterPath clip;
			clip.addEllipse(avatarRect);
			p.setClipPath(clip);
			const auto scaled = _image.scaled(
				avatarRect.size(),
				Qt::KeepAspectRatioByExpanding,
				Qt::SmoothTransformation);
			const auto pos = QPoint(
				avatarRect.x() + (avatarRect.width() - scaled.width()) / 2,
				avatarRect.y() + (avatarRect.height() - scaled.height()) / 2);
			p.drawImage(pos, scaled);
			p.restore();
		} else {
			p.setPen(st::windowSubTextFg);
			const auto letter = _row.displayName.isEmpty()
				? u"?"_q
				: _row.displayName.left(1).toUpper();
			p.drawText(avatarRect, Qt::AlignCenter, letter);
		}

		if (_row.deletedUserpics > 0) {
			const auto badge = st::menuIconDelete;
			const auto iconSize = 16;
			const auto iconPos = QPoint(
				avatarRect.right() - iconSize + 2,
				avatarRect.bottom() - iconSize + 2);
			p.setBrush(st::boxBg->c);
			p.setPen(Qt::NoPen);
			p.drawEllipse(
				QRect(iconPos, QSize(iconSize + 4, iconSize + 4)));
			badge.paint(
				p,
				iconPos.x() + 2,
				iconPos.y() + 2,
				width());
		}

		const auto textLeft = avatarRect.right() + st::boxRowPadding.left();
		const auto textWidth = width() - textLeft - st::boxRowPadding.right();
		const auto titleRect = QRect(
			textLeft,
			(height() / 2) - st::boxLabel.style.font->height,
			textWidth,
			st::boxLabel.style.font->height);
		const auto subtitleRect = QRect(
			textLeft,
			height() / 2,
			textWidth,
			st::boxLabel.style.font->height);

		p.setPen(st::boxTextFg);
		p.setFont(st::boxLabel.style.font);
		p.drawText(
			titleRect,
			Qt::AlignLeft | Qt::AlignVCenter,
			_row.displayName);

		p.setPen(st::windowSubTextFg);
		p.drawText(
			subtitleRect,
			Qt::AlignLeft | Qt::AlignVCenter,
			_row.subtitle);

		RippleButton::paintRipple(p, 0, 0);
	}

private:
	ArchivedUserRow _row;
	QImage _image;

};

[[nodiscard]] QString FormatCountLabel(
		const QString &query,
		int count) {
	if (query.trimmed().isEmpty()) {
		return u"Всего: %1"_q.arg(count);
	}
	return u"Найдено: %1"_q.arg(count);
}

void RebuildList(
		not_null<Ui::VerticalLayout*> list,
		not_null<Ui::FlatLabel*> counter,
		not_null<Window::SessionController*> controller,
		const QString &query) {
	const auto rows = searchArchivedUsers(&controller->session(), query);
	counter->setText(FormatCountLabel(query, int(rows.size())));
	while (list->count() > 0) {
		delete list->widgetAt(0);
	}
	if (rows.empty()) {
		list->add(
			object_ptr<Ui::FlatLabel>(
				list,
				rpl::single(query.isEmpty()
					? u"Архив пуст. Включите «Архив пользователей» и подождите сбор данных."_q
					: u"Ничего не найдено."_q),
				st::boxLabel),
			st::boxRowPadding);
		return;
	}
	for (const auto &row : rows) {
		const auto button = list->add(
			object_ptr<ArchivedUserListRow>(list, row));
		button->setClickedCallback([=] {
			const auto peerId = DeserializePeerId(
				static_cast<quint64>(row.peerId));
			const auto peer = controller->session().data().peer(peerId);
			const auto user = peer ? peer->asUser() : nullptr;
			if (!user) {
				controller->showToast(
					u"Пользователь не загружен в этом клиенте."_q);
				return;
			}
			controller->showPeerInfo(user);
		});
	}
}

} // namespace

void ShowArchiveUsersBox(
		not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Архив пользователей"_q));
		box->setWidth(st::boxWideWidth);
		box->setMaxHeight(st::boxMaxListHeight);

		const auto search = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				Ui::InputField::Mode::SingleLine,
				rpl::single(u"Поиск по имени, @username, био…"_q),
				TextWithTags{}),
			st::boxRowPadding);

		const auto counter = box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(QString()),
				st::boxDividerLabel),
			st::boxRowPadding);

		const auto list = box->addRow(
			object_ptr<Ui::VerticalLayout>(box));

		const auto refill = [=] {
			RebuildList(
				list,
				counter,
				controller,
				search->getLastText());
		};
		refill();

		search->changes(
		) | rpl::on_next([=] { refill(); }, search->lifetime());

		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
		box->setFocusCallback([=] { search->setFocusFast(); });
	}));
}

} // namespace TeleForge::PeerArchive
