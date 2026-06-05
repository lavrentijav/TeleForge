// Copyright @Radolyn, 2026

#include "info/profile/info_tf_peer_archive_panel.h"

#include "ayu/features/teleforge/tf_archive_userpics_box.h"
#include "ayu/features/teleforge/tf_peer_archive.h"
#include "base/unixtime.h"
#include "data/data_peer_id.h"
#include "data/data_user.h"
#include "info/info_controller.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QFile>
#include <QGuiApplication>

namespace Info::Profile {
namespace {

[[nodiscard]] QString FormatTs(int ts) {
	if (!ts) {
		return u"—"_q;
	}
	return QDateTime::fromSecsSinceEpoch(ts).toString(u"dd.MM.yyyy HH:mm"_q);
}

void CopyText(not_null<Window::SessionController*> session, const QString &text) {
	if (text.isEmpty()) {
		return;
	}
	QGuiApplication::clipboard()->setText(text);
	session->showToast(u"Скопировано."_q);
}

void AddSummaryLine(
		not_null<Ui::VerticalLayout*> parent,
		const QString &label,
		const QString &value) {
	parent->add(
		object_ptr<Ui::FlatLabel>(
			parent,
			rpl::single(label),
			st::boxDividerLabel),
		st::boxRowPadding);
	parent->add(
		object_ptr<Ui::FlatLabel>(
			parent,
			rpl::single(value),
			st::boxLabel),
		style::margins(
			st::boxRowPadding.left(),
			0,
			st::boxRowPadding.right(),
			st::boxRowPadding.bottom() / 2));
}

void AddHistoryEntry(
		not_null<Ui::VerticalLayout*> parent,
		not_null<Window::SessionController*> session,
		const QString &title,
		const QString &subtitle) {
	const auto text = subtitle.isEmpty()
		? title
		: (title + u"\n"_q + subtitle);
	const auto row = parent->add(object_ptr<Ui::SettingsButton>(
		parent,
		rpl::single(text),
		st::infoSharedMediaButton));
	row->addClickHandler([=] { CopyText(session, title); });
}

void AddExpandableHistory(
		not_null<Ui::VerticalLayout*> parent,
		not_null<Window::SessionController*> session,
		const QString &title,
		const std::vector<std::pair<QString, QString>> &rows) {
	if (rows.empty()) {
		return;
	}

	const auto header = parent->add(object_ptr<Ui::SettingsButton>(
		parent,
		rpl::single(title),
		st::infoSharedMediaButton));
	header->toggleOn(rpl::single(false));

	const auto wrap = parent->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		parent,
		object_ptr<Ui::VerticalLayout>(parent)));
	const auto inner = wrap->entity();
	wrap->setDuration(st::infoSlideDuration)->toggleOn(header->toggledValue());

	for (const auto &[text, date] : rows) {
		AddHistoryEntry(inner, session, text, date);
	}
}

not_null<Ui::VerticalLayout*> AddCollapsibleSection(
		not_null<Ui::VerticalLayout*> parent,
		const QString &title,
		bool expandedByDefault,
		Fn<void(not_null<Ui::VerticalLayout*>)> fill) {
	const auto header = parent->add(object_ptr<Ui::SettingsButton>(
		parent,
		rpl::single(title),
		st::infoSharedMediaButton));
	header->toggleOn(rpl::single(expandedByDefault));

	const auto wrap = parent->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		parent,
		object_ptr<Ui::VerticalLayout>(parent)));
	const auto inner = wrap->entity();
	wrap->setDuration(st::infoSlideDuration)->toggleOn(header->toggledValue());
	fill(inner);
	return inner;
}

} // namespace

object_ptr<Ui::RpWidget> SetupPeerArchivePanel(
		not_null<Controller*> controller,
		not_null<Ui::RpWidget*> parent,
		not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	if (!user || user->isSelf() || user->isServiceUser()) {
		return nullptr;
	}

	auto wrap = object_ptr<Ui::VerticalLayout>(parent);
	const auto outer = wrap.get();
	const auto session = controller->parentController();
	const auto peerStorageId = static_cast<long long>(SerializePeerId(peer->id));

	if (TeleForge::PeerArchive::archiveEnabled()) {
		TeleForge::PeerArchive::noteUserObserved(user, {
			.kind = TeleForge::PeerArchive::ObservationSource::Kind::Profile,
		});
	}

	const auto snapshot = TeleForge::PeerArchive::loadProfile(peerStorageId);

	const auto header = outer->add(object_ptr<Ui::SettingsButton>(
		outer,
		rpl::single(u"Архив TeleForge"_q),
		st::infoSharedMediaButton));
	header->toggleOn(rpl::single(false));

	const auto bodyWrap = outer->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		outer,
		object_ptr<Ui::VerticalLayout>(outer)));
	const auto inner = bodyWrap->entity();
	bodyWrap->setDuration(st::infoSlideDuration)->toggleOn(header->toggledValue());

	Ui::AddSkip(inner);

	AddCollapsibleSection(inner, u"Сводка"_q, false, [&](
			not_null<Ui::VerticalLayout*> section) {
		const auto firstContext = [&] {
			if (!snapshot.firstChatId) {
				return QString();
			}
			const auto source = snapshot.firstSource.isEmpty()
				? u"?"_q
				: snapshot.firstSource;
			if (snapshot.firstMessageId) {
				return u"чат "_q
					+ QString::number(snapshot.firstChatId)
					+ u", msg "_q
					+ QString::number(snapshot.firstMessageId)
					+ u" ("_q
					+ source
					+ u')';
			}
			return u"чат "_q
				+ QString::number(snapshot.firstChatId)
				+ u" ("_q
				+ source
				+ u')';
		}();
		AddSummaryLine(section, u"Первое появление"_q, FormatTs(snapshot.firstSeenAt));
		if (!firstContext.isEmpty()) {
			AddSummaryLine(section, u"Контекст"_q, firstContext);
		}
		AddSummaryLine(section, u"Последнее"_q, FormatTs(snapshot.lastSeenAt));
		AddSummaryLine(
			section,
			u"Общих чатов (вы в них)"_q,
			QString::number(snapshot.sharedGroupsCount));
	});

	auto usernames = std::vector<std::pair<QString, QString>>();
	for (const auto &entry : snapshot.usernames) {
		usernames.push_back({
			u'@' + entry.username,
			FormatTs(entry.observedAt),
		});
	}
	AddExpandableHistory(
		inner,
		session,
		usernames.size()
			? u"Юзернеймы, %1"_q.arg(usernames.size())
			: u"Юзернеймы"_q,
		usernames);

	auto names = std::vector<std::pair<QString, QString>>();
	for (const auto &entry : snapshot.names) {
		names.push_back({
			entry.firstName + ' ' + entry.lastName,
			FormatTs(entry.observedAt),
		});
	}
	AddExpandableHistory(
		inner,
		session,
		names.size() ? u"Имена, %1"_q.arg(names.size()) : u"Имена"_q,
		names);

	auto bios = std::vector<std::pair<QString, QString>>();
	for (const auto &entry : snapshot.bios) {
		bios.push_back({
			entry.bio,
			FormatTs(entry.observedAt),
		});
	}
	AddExpandableHistory(
		inner,
		session,
		bios.size() ? u"О себе, %1"_q.arg(bios.size()) : u"О себе"_q,
		bios);

	const auto currentPhotoId = static_cast<long long>(user->userpicPhotoId());
	auto existingUserpics = std::vector<TeleForge::PeerArchive::UserpicEntry>();
	existingUserpics.reserve(snapshot.userpics.size());
	for (const auto &entry : snapshot.userpics) {
		auto copy = entry;
		copy.fileMissing = !QFile::exists(entry.localPath);
		copy.removed = currentPhotoId && entry.photoId != currentPhotoId;
		existingUserpics.push_back(copy);
	}
	if (!existingUserpics.empty()) {
		const auto viewBtn = inner->add(object_ptr<Ui::SettingsButton>(
			inner,
			rpl::single(u"Аватары (%1) — Просмотреть"_q.arg(existingUserpics.size())),
			st::infoSharedMediaButton));
		viewBtn->addClickHandler([=] {
			TeleForge::PeerArchive::ShowArchiveUserpicsBox(
				session,
				existingUserpics);
		});
	}

	auto chats = std::vector<std::pair<QString, QString>>();
	for (const auto &entry : snapshot.chats) {
		const auto suffix = entry.selfInChat ? u" · вы в чате"_q : QString();
		const auto icon = TeleForge::PeerArchive::chatUserpicArchivePath(entry.chatId);
		const auto iconMark = icon.isEmpty() ? QString() : u"[иконка] "_q;
		chats.push_back({
			iconMark + entry.chatTitle + suffix,
			FormatTs(entry.firstSeenAt) + u" — "_q + FormatTs(entry.lastSeenAt),
		});
	}
	AddExpandableHistory(
		inner,
		session,
		chats.size() ? u"Чаты, %1"_q.arg(chats.size()) : u"Чаты"_q,
		chats);

	const auto refresh = inner->add(object_ptr<Ui::SettingsButton>(
		inner,
		rpl::single(u"Обновить данные чатов"_q),
		st::infoSharedMediaButton));
	refresh->addClickHandler([=] {
		TeleForge::PeerArchive::refreshMembershipFromApi(
			&session->session(),
			user,
			[=] { session->showToast(u"Данные чатов обновлены."_q); });
	});

	Ui::AddSkip(inner);

	return wrap;
}

} // namespace Info::Profile
