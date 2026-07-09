/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_teleforge_peer_panel.h"

#include "ayu/features/spy/online_history_storage.h"
#include "ayu/ui/settings/settings_ayu_utils.h"
#include "ayu/features/teleforge/teleforge_storage.h"
#include "data/data_peer_id.h"
#include "data/data_user.h"
#include "info/info_controller.h"
#include "base/unixtime.h"
#include "ui/text/format_values.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <cmath>

namespace Info::Profile {
namespace {

[[nodiscard]] int ParseIntClamped(
		const QString &text,
		int lo,
		int hi,
		int fallback) {
	auto ok = false;
	const auto v = text.trimmed().toInt(&ok);
	if (!ok) {
		return fallback;
	}
	return std::clamp(v, lo, hi);
}

[[nodiscard]] double ParseDouble(const QString &text, double fallback) {
	auto ok = false;
	const auto v = text.trimmed().toDouble(&ok);
	return ok ? v : fallback;
}

void SavePeerRow(
		TeleForge::Storage::PerChatSettingsRecord r,
		long long peerStorageId,
		not_null<Window::SessionController*> controller) {
	r.peerId = peerStorageId;
	r.updatedAt = base::unixtime::now();
	TeleForge::Storage::upsertPerChatSettings(r);
	controller->showToast(u"Настройки чата сохранены."_q);
}

[[nodiscard]] bool CanConfigureTeleForgeForPeer(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return !user->isServiceUser();
	}
	return true;
}

void AddCollapsibleSpyHistory(
		not_null<Ui::VerticalLayout*> parent,
		not_null<UserData*> user) {
	const auto since = base::unixtime::now() - 7 * 86400;
	const auto events = TeleForge::Spy::loadRecentForUser(
		user->id.value,
		since);
	const auto usesApprox = TeleForge::Spy::lastseenUsesSpyApproximation(
		user->id.value);

	auto lines = QStringList();
	if (usesApprox) {
		if (const auto manual = TeleForge::Spy::manualLastSeenForUser(
				user->id.value)) {
			lines.push_back(u"Последний раз онлайн (вручную): %1"_q.arg(
				Ui::FormatDateTime(base::unixtime::parse(*manual))));
		}
	}
	for (const auto &e : events) {
		if (lines.size() >= 12) {
			break;
		}
		if (!usesApprox && e.kind == 2) {
			continue;
		}
		const auto label = (e.kind == 1)
			? u"онлайн"_q
			: (e.kind == 2 ? u"скрыт"_q : u"офлайн"_q);
		lines.push_back(u"%1 — %2"_q.arg(
			Ui::FormatDateTime(base::unixtime::parse(e.timestamp)),
			label));
	}
	if (lines.isEmpty()) {
		return;
	}

	const auto inner = ::Settings::AddCollapsibleArrowSection(
		parent,
		rpl::single(u"История онлайн (%1)"_q.arg(lines.size())),
		false).inner;

	for (const auto &line : lines) {
		inner->add(object_ptr<Ui::SettingsButton>(
			inner,
			rpl::single(line),
			st::infoSharedMediaButton));
	}
}

not_null<Ui::VerticalLayout*> AddCollapsibleSection(
		not_null<Ui::VerticalLayout*> parent,
		const QString &title,
		bool expandedByDefault,
		Fn<void(not_null<Ui::VerticalLayout*>)> fill) {
	const auto inner = ::Settings::AddCollapsibleArrowSection(
		parent,
		rpl::single(title),
		expandedByDefault).inner;
	fill(inner);
	return inner;
}

} // namespace

object_ptr<Ui::RpWidget> SetupTeleForgePeerPanel(
		not_null<Controller*> controller,
		not_null<Ui::RpWidget*> parent,
		not_null<PeerData*> peer) {
	if (!CanConfigureTeleForgeForPeer(peer)) {
		return nullptr;
	}

	const auto session = controller->parentController();
	const auto peerStorageId = static_cast<long long>(
		SerializePeerId(peer->id));

	auto wrap = object_ptr<Ui::VerticalLayout>(parent);
	const auto outer = wrap.get();

	const auto inner = ::Settings::AddCollapsibleArrowSection(
		outer,
		rpl::single(u"TeleForge"_q),
		false).inner;

	Ui::AddSkip(inner);

	const auto mkToggle = [&](
			not_null<Ui::VerticalLayout*> container,
			const QString &label,
			Fn<bool()> initial,
			Fn<void(bool)> onChange) {
		const auto btn = container->add(object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(label),
			st::infoSharedMediaButton));
		btn->toggleOn(rpl::single(initial()));
		btn->toggledValue(
		) | rpl::on_next(std::move(onChange), btn->lifetime());
	};

	AddCollapsibleSection(inner, u"ИИ и память"_q, false, [&](
			not_null<Ui::VerticalLayout*> section) {
		section->add(
			object_ptr<Ui::FlatLabel>(
				section,
				rpl::single(
					u"Параметры для этого диалога; если для чата нет своей записи в базе, "
					u"используются общие значения из Настройки → TeleForge → ИИ и память."_q),
				st::boxDividerLabel),
			st::boxRowPadding);
		mkToggle(
		section,
		u"Ответы ИИ (Ctrl+Shift+M)"_q,
		[=] {
			return TeleForge::Storage::effectivePerChatSettings(
				peerStorageId).aiAnswer;
		},
		[=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(peerStorageId);
			p.aiAnswer = v;
			SavePeerRow(p, peerStorageId, session);
		});
	mkToggle(
		section,
		u"Читать память в промпт"_q,
		[=] {
			return TeleForge::Storage::effectivePerChatSettings(
				peerStorageId).memoryReadEnabled;
		},
		[=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(peerStorageId);
			p.memoryReadEnabled = v;
			SavePeerRow(p, peerStorageId, session);
		});
	mkToggle(
		section,
		u"Записывать память из сообщений"_q,
		[=] {
			return TeleForge::Storage::effectivePerChatSettings(
				peerStorageId).memoryWriteEnabled;
		},
		[=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(peerStorageId);
			p.memoryWriteEnabled = v;
			SavePeerRow(p, peerStorageId, session);
		});
	mkToggle(
		section,
		u"Доступ в интернет (заготовка)"_q,
		[=] {
			return TeleForge::Storage::effectivePerChatSettings(
				peerStorageId).webAccess;
		},
		[=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(peerStorageId);
			p.webAccess = v;
			SavePeerRow(p, peerStorageId, session);
		});
	mkToggle(
		section,
		u"Доступ к календарю (заготовка)"_q,
		[=] {
			return TeleForge::Storage::effectivePerChatSettings(
				peerStorageId).calendarAccess;
		},
		[=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(peerStorageId);
			p.calendarAccess = v;
			SavePeerRow(p, peerStorageId, session);
		});
	mkToggle(
		section,
		u"Агент ПК (заготовка)"_q,
		[=] {
			return TeleForge::Storage::effectivePerChatSettings(
				peerStorageId).pcAgent;
		},
		[=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(peerStorageId);
			p.pcAgent = v;
			SavePeerRow(p, peerStorageId, session);
		});
	});

	if (const auto user = peer->asUser()) {
		AddCollapsibleSection(inner, u"Режим шпиона"_q, false, [&](
				not_null<Ui::VerticalLayout*> section) {
			mkToggle(
				section,
				u"Отслеживать онлайн этого пользователя"_q,
				[=] {
					return TeleForge::Spy::isSpyEnabledForUser(user->id.value);
				},
				[=](bool v) {
					TeleForge::Spy::setSpyTargetEnabled(user->id.value, v);
					session->showToast(v
						? u"Отслеживание включено."_q
						: u"Отслеживание выключено."_q);
				});
			AddCollapsibleSpyHistory(section, user);
		});
	}

	const auto r = TeleForge::Storage::effectivePerChatSettings(peerStorageId);

	auto addNumRow = [&](
			not_null<Ui::VerticalLayout*> container,
			const QString &label,
			const QString &value) {
		container->add(
			object_ptr<Ui::FlatLabel>(container, label, st::boxDividerLabel),
			st::boxRowPadding);
		return container->add(
			object_ptr<Ui::InputField>(
				container,
				st::defaultInputField,
				Ui::InputField::Mode::SingleLine,
				rpl::single(QString()),
				TextWithTags{ value }),
			st::boxRowPadding);
	};

	AddCollapsibleSection(inner, u"Параметры памяти"_q, false, [&](
			not_null<Ui::VerticalLayout*> section) {
	const auto fGk = addNumRow(
		section,
		u"Глобальная память, top-K"_q,
		QString::number(r.globalMemoryTopK));
	const auto fCk = addNumRow(
		section,
		u"Память чата, top-K"_q,
		QString::number(r.chatMemoryTopK));
	const auto fUk = addNumRow(
		section,
		u"Память пользователя, top-K"_q,
		QString::number(r.userMemoryTopK));
	const auto fAk = addNumRow(
		section,
		u"Семантическое приложение, top-K"_q,
		QString::number(r.memoryAppendixTopK));
	const auto fRx = addNumRow(
		section,
		u"Число сводок (X)"_q,
		QString::number(r.recentSummaryLimit));
	const auto fSy = addNumRow(
		section,
		u"Число стабильных фактов (Y)"_q,
		QString::number(r.stableFactsLimit));
	const auto fDec = addNumRow(
		section,
		u"Линейное затухание в день"_q,
		QString::number(r.memoryDecayPerDay, 'g', 4));
	const auto fMax = addNumRow(
		section,
		u"Max chars на блок памяти в промпте"_q,
		QString::number(r.maxMemoryBlockChars));

	section->add(
		object_ptr<Ui::FlatLabel>(
			section,
			u"Белый список папок (JSON)"_q,
			st::boxDividerLabel),
		st::boxRowPadding);
	const auto fWl = section->add(
		object_ptr<Ui::InputField>(
			section,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(QString()),
			TextWithTags{ QString::fromStdString(r.directoryWhitelistJson) }),
		st::boxRowPadding);
	fWl->setMaxHeight(st::defaultInputField.heightMin * 4);

	const auto saveNums = section->add(
		object_ptr<Ui::SettingsButton>(
			section,
			rpl::single(u"Сохранить числа и whitelist"_q),
			st::settingsButtonNoIcon));
	saveNums->setClickedCallback([=] {
		auto next = TeleForge::Storage::effectivePerChatSettings(peerStorageId);
		next.globalMemoryTopK = ParseIntClamped(
			fGk->getLastText(),
			1,
			64,
			next.globalMemoryTopK);
		next.chatMemoryTopK = ParseIntClamped(
			fCk->getLastText(),
			1,
			64,
			next.chatMemoryTopK);
		next.userMemoryTopK = ParseIntClamped(
			fUk->getLastText(),
			1,
			32,
			next.userMemoryTopK);
		next.memoryAppendixTopK = ParseIntClamped(
			fAk->getLastText(),
			0,
			64,
			next.memoryAppendixTopK);
		next.recentSummaryLimit = ParseIntClamped(
			fRx->getLastText(),
			0,
			64,
			next.recentSummaryLimit);
		next.stableFactsLimit = ParseIntClamped(
			fSy->getLastText(),
			0,
			64,
			next.stableFactsLimit);
		next.memoryDecayPerDay = std::max(
			0.,
			ParseDouble(fDec->getLastText(), next.memoryDecayPerDay));
		next.maxMemoryBlockChars = ParseIntClamped(
			fMax->getLastText(),
			256,
			500000,
			next.maxMemoryBlockChars);
		next.directoryWhitelistJson = fWl->getLastText().trimmed().toStdString();
		SavePeerRow(next, peerStorageId, session);
	});

	const auto reset = section->add(
		object_ptr<Ui::SettingsButton>(
			section,
			rpl::single(u"Сбросить: использовать общие настройки"_q),
			st::settingsButtonNoIcon));
	reset->setClickedCallback([=] {
		TeleForge::Storage::removePerChatSettings(peerStorageId);
		session->showToast(u"Для чата снова действуют общие настройки TeleForge."_q);
	});
	});

	Ui::AddSkip(inner);

	return wrap;
}

} // namespace Info::Profile
