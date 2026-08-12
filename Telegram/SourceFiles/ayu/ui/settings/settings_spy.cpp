// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_spy.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/ghost/tf_ghost_audit.h"
#include "ayu/features/spy/online_history_storage.h"
#include "ayu/features/teleforge/tf_archive_users_box.h"
#include "ayu/features/teleforge/tf_peer_archive.h"
#include "ayu/ui/settings/ayu_builder.h"
#include "ayu/ui/settings/settings_ayu_utils.h"
#include "ayu/ui/settings/settings_main.h"
#include "lang_auto.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <algorithm>

namespace Settings {

using namespace Builder;
using namespace AyBuilder;

namespace {

[[nodiscard]] QString FormatDurationLabel(int seconds) {
	if (seconds < 60) {
		return QString::number(seconds) + u" сек"_q;
	}
	if (seconds < 3600) {
		return QString::number(seconds / 60) + u" мин"_q;
	}
	const auto hours = seconds / 3600;
	const auto mins = (seconds % 3600) / 60;
	return mins
		? (QString::number(hours) + u" ч "_q + QString::number(mins) + u" мин"_q)
		: (QString::number(hours) + u" ч"_q);
}

[[nodiscard]] int IndexForValue(
		int value,
		int minValue,
		int step) {
	return (value - minValue) / step;
}

void BuildPollIntervalSettings(AyuSectionBuilder &ayu) {
	const auto &settings = AyuSettings::getInstance();

	ayu.addCollapsibleSection({
		.id = u"teleforge/archivePollIntervals"_q,
		.title = rpl::single(u"Интервалы опроса"_q),
		.fill = [&](not_null<Ui::VerticalLayout*> inner) {
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archiveChatRefresh"_q,
				.title = rpl::single(u"Обновление всех чатов"_q),
				.steps = 59,
				.current = IndexForValue(
					settings.archiveChatRefreshSeconds(),
					60,
					60),
				.indexToValue = [](int index) { return 60 + index * 60; },
				.onFinalChanged = [](int seconds) {
					AyuSettings::getInstance().setArchiveChatRefreshSeconds(seconds);
				},
				.formatLabel = FormatDurationLabel,
			});
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archiveKnownUserOnline"_q,
				.title = rpl::single(u"Онлайн известных пользователей"_q),
				.steps = 40,
				.current = IndexForValue(
					settings.archiveKnownUserOnlineSeconds(),
					15,
					15),
				.indexToValue = [](int index) { return 15 + index * 15; },
				.onFinalChanged = [](int seconds) {
					AyuSettings::getInstance().setArchiveKnownUserOnlineSeconds(seconds);
				},
				.formatLabel = FormatDurationLabel,
			});
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archivePrivateOnline"_q,
				.title = rpl::single(u"Онлайн в личных чатах"_q),
				.steps = 24,
				.current = IndexForValue(
					settings.archivePrivateOnlineSeconds(),
					5,
					5),
				.indexToValue = [](int index) { return 5 + index * 5; },
				.onFinalChanged = [](int seconds) {
					AyuSettings::getInstance().setArchivePrivateOnlineSeconds(seconds);
				},
				.formatLabel = FormatDurationLabel,
			});
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archivePrivateProfile"_q,
				.title = rpl::single(u"Имя и био в личных чатах"_q),
				.steps = 119,
				.current = IndexForValue(
					settings.archivePrivateProfileSeconds(),
					60,
					60),
				.indexToValue = [](int index) { return 60 + index * 60; },
				.onFinalChanged = [](int seconds) {
					AyuSettings::getInstance().setArchivePrivateProfileSeconds(seconds);
				},
				.formatLabel = FormatDurationLabel,
			});
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archiveOtherProfile"_q,
				.title = rpl::single(u"Имя и био остальных"_q),
				.steps = 72,
				.current = IndexForValue(
					settings.archiveOtherProfileSeconds(),
					300,
					300),
				.indexToValue = [](int index) { return 300 + index * 300; },
				.onFinalChanged = [](int seconds) {
					AyuSettings::getInstance().setArchiveOtherProfileSeconds(seconds);
				},
				.formatLabel = FormatDurationLabel,
			});
		},
	});

	ayu.base().addSkip();
	ayu.base().addDividerText(rpl::single(
		u"Время «был в сети» и даты в архиве округляются до интервала опроса: "
		u"раз в минуту — до минуты, раз в 15 секунд — до 15 секунд и т.д."_q));
}

void BuildArchiveScanSettings(AyuSectionBuilder &ayu) {
	const auto &settings = AyuSettings::getInstance();

	ayu.addCollapsibleSection({
		.id = u"teleforge/archiveScan"_q,
		.title = rpl::single(u"Сканирование архива"_q),
		.fill = [&](not_null<Ui::VerticalLayout*> inner) {
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveBackgroundCrawl"_q,
				.title = rpl::single(u"Фоновый обход чатов"_q),
				.getter = [] {
					return AyuSettings::getInstance().archiveBackgroundCrawl();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveBackgroundCrawl(v);
				},
			});
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveScanParticipants"_q,
				.title = rpl::single(u"Собирать участников чатов"_q),
				.getter = [] {
					return AyuSettings::getInstance().archiveScanParticipants();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveScanParticipants(v);
				},
			});
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveScanBroadcasts"_q,
				.title = rpl::single(u"Сканировать каналы (не только группы)"_q),
				.getter = [] {
					return AyuSettings::getInstance().archiveScanBroadcasts();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveScanBroadcasts(v);
				},
			});
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveSaveUserpics"_q,
				.title = rpl::single(u"Сохранять аватары на диск"_q),
				.getter = [] {
					return AyuSettings::getInstance().archiveSaveUserpics();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveSaveUserpics(v);
				},
			});
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveFollowLinks"_q,
				.title = rpl::single(u"Ловить @упоминания и t.me ссылки"_q),
				.getter = [] {
					return AyuSettings::getInstance().archiveFollowLinks();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveFollowLinks(v);
				},
			});
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archiveHistoryScanLimit"_q,
				.title = rpl::single(u"Сообщений на чат за проход"_q),
				.steps = 40,
				.current = IndexForValue(
					settings.archiveHistoryScanLimit(),
					0,
					100),
				.indexToValue = [](int index) { return index * 100; },
				.onFinalChanged = [](int value) {
					AyuSettings::getInstance().setArchiveHistoryScanLimit(value);
				},
				.formatLabel = [](int x) {
					return x ? QString::number(x) : u"без лимита"_q;
				},
			});
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archiveCrawlBatchSize"_q,
				.title = rpl::single(u"Записей в БД за один проход"_q),
				.steps = 39,
				.current = IndexForValue(
					settings.archiveCrawlBatchSize(),
					4,
					4),
				.indexToValue = [](int index) { return 4 + index * 4; },
				.onFinalChanged = [](int value) {
					AyuSettings::getInstance().setArchiveCrawlBatchSize(value);
				},
				.formatLabel = [](int x) { return QString::number(x); },
			});
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archiveMaxParticipants"_q,
				.title = rpl::single(u"Максимум участников на чат"_q),
				.steps = 40,
				.current = IndexForValue(
					settings.archiveMaxParticipants(),
					0,
					500),
				.indexToValue = [](int index) { return index * 500; },
				.onFinalChanged = [](int value) {
					AyuSettings::getInstance().setArchiveMaxParticipants(value);
				},
				.formatLabel = [](int x) {
					return x ? QString::number(x) : u"без лимита"_q;
				},
			});
		},
	});

	ayu.base().addSkip();
	ayu.base().addDividerText(rpl::single(
		u"Чем меньше «сообщений на чат» и «записей в БД за проход», тем меньше "
		u"архив нагружает интерфейс. Аватары кодируются в отдельном потоке."_q));
}

void BuildSpyEssentials(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle(tr::ayu_SpyEssentialsHeader());

	ayu.addToggle({
		.id = u"teleforge/spyMode"_q,
		.altIds = { u"ayu/spyMode"_q },
		.title = rpl::single(u"Режим шпиона (глобально)"_q),
		.getter = [] {
			return TeleForge::Spy::spyModeGloballyEnabled();
		},
		.setter = [](bool v) {
			TeleForge::Spy::setSpyModeGloballyEnabled(v);
		},
	});

	builder.addSkip();
	builder.addDividerText(rpl::single(
		u"Отслеживает онлайн-статус пользователей и сохраняет историю. "
		u"Глобальный режим включает отслеживание для всех; в профиле пользователя "
		u"можно задать исключение или включить отдельно при выключенном глобальном режиме."_q));

	ayu.addSlider({
		.id = u"teleforge/spyRetentionDays"_q,
		.title = rpl::single(u"Хранить историю онлайна (дней)"_q),
		.steps = 365,
		.current = TeleForge::Spy::retentionDays() - 1,
		.indexToValue = [](int index) { return index + 1; },
		.onFinalChanged = [](int days) {
			TeleForge::Spy::setRetentionDays(days);
			TeleForge::Spy::purgeOldEvents();
		},
		.formatLabel = [](int x) { return QString::number(x); },
	});

	ayu.addToggle({
		.id = u"teleforge/peerArchive"_q,
		.title = rpl::single(u"Архив пользователей TeleForge"_q),
		.getter = [] {
			return AyuSettings::getInstance().peerArchiveEnabled();
		},
		.setter = [](bool v) {
			TeleForge::PeerArchive::setArchiveEnabled(v);
		},
	});

	builder.addButton({
		.id = u"teleforge/archiveUsersBrowse"_q,
		.title = rpl::single(u"Просмотреть всех пользователей"_q),
		.onClick = [=] {
			TeleForge::PeerArchive::ShowArchiveUsersBox(controller);
		},
	});

	builder.addSkip();
	builder.addDividerText(rpl::single(
		u"Сохраняет имена, юзернеймы, био, аватары (локально) и чаты, где встречался пользователь. "
		u"Имена, юзернеймы и био синхронизируются между устройствами; файлы аватаров — нет. "
		u"Удалённые аватары помечаются значком корзины. По умолчанию выключено."_q));

	BuildPollIntervalSettings(ayu);
	BuildArchiveScanSettings(ayu);

	ayu.addSectionDivider();
	builder.addSubsectionTitle(rpl::single(u"Сохранение сообщений"_q));

	ayu.addSettingToggle({
		.id = u"ayu/saveDeletedMessages"_q,
		.title = tr::ayu_SaveDeletedMessages(),
		.getter = &AyuSettings::saveDeletedMessages,
		.setter = &AyuSettings::setSaveDeletedMessages,
	});
	ayu.addSettingToggle({
		.id = u"ayu/saveMessagesHistory"_q,
		.title = tr::ayu_SaveMessagesHistory(),
		.getter = &AyuSettings::saveMessagesHistory,
		.setter = &AyuSettings::setSaveMessagesHistory,
	});
	ayu.addSettingToggle({
		.id = u"teleforge/showDeletedInChat"_q,
		.title = rpl::single(u"Показывать удалённые прямо в чате"_q),
		.getter = &AyuSettings::showDeletedInChat,
		.setter = &AyuSettings::setShowDeletedInChat,
	});
	ayu.addSlider({
		.id = u"teleforge/deletedRestoreLimit"_q,
		.title = rpl::single(u"Восстанавливать удалённых на чат"_q),
		.steps = 40,
		.current = AyuSettings::getInstance().deletedRestoreLimit() / 50,
		.indexToValue = [](int index) { return index * 50; },
		.onFinalChanged = [](int value) {
			AyuSettings::getInstance().setDeletedRestoreLimit(value);
		},
		.formatLabel = [](int x) {
			return x ? QString::number(x) : u"выкл"_q;
		},
	});

	builder.addSkip();
	builder.addDividerText(rpl::single(
		u"Сохранённые удалённые сообщения подставляются обратно в историю чата "
		u"при его открытии, на своё место по дате и с пометкой удаления. "
		u"Восстанавливается только текст; медиа остаётся в отдельном разделе."_q));

	ayu.addSectionDivider();
	builder.addSubsectionTitle(rpl::single(u"Диагностика призрака"_q));

	ayu.addSettingToggle({
		.id = u"teleforge/ghostPassthroughMentions"_q,
		.title = rpl::single(
			u"Отмечать упоминания и реакции прочитанными в призраке"_q),
		.getter = &AyuSettings::ghostPassthroughMentions,
		.setter = &AyuSettings::setGhostPassthroughMentions,
	});

	builder.addButton({
		.id = u"teleforge/ghostWakeSignals"_q,
		.title = rpl::single(u"Журнал сигналов пробуждения"_q),
		.onClick = [=] {
			auto lines = TeleForge::Ghost::recentWakeSignals();
			if (lines.isEmpty()) {
				controller->showToast(
					u"Пока ни один сигнал не уходил на сервер."_q);
				return;
			}
			std::reverse(lines.begin(), lines.end());
			controller->show(Ui::MakeInformBox({
				.text = lines.mid(0, 40).join('\n'),
				.title = rpl::single(u"Сигналы пробуждения"_q),
			}));
		},
	});

	builder.addSkip();
	builder.addDividerText(rpl::single(
		u"Каждый запрос, который сервер может засчитать за активность аккаунта, "
		u"записывается сюда, пока призрак включён. Отключение сквозного чтения "
		u"упоминаний убирает последний путь, по которому сообщение может "
		u"«проскочить» призрака."_q));

	ayu.addSectionDivider();

	ayu.addSettingToggle({
		.id = u"ayu/saveForBots"_q,
		.title = tr::ayu_MessageSavingSaveForBots(),
		.getter = &AyuSettings::saveForBots,
		.setter = &AyuSettings::setSaveForBots,
	});

	builder.addSkip();
	builder.addDividerText(tr::ayu_DeleteStubTextDescription());
	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto field = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					tr::ayu_DeleteStubText(),
					TextWithTags{
						AyuSettings::getInstance().deleteStubText(),
					}),
				st::boxRowPadding);
			field->changes(
			) | rpl::on_next([=] {
				AyuSettings::getInstance().setDeleteStubText(
					field->getLastText().trimmed());
			}, field->lifetime());
		}, [](const SearchContext &) {});
	});
}

const auto kMeta = BuildHelper({
	.id = AyuSpy::Id(),
	.parentId = AyuMain::Id(),
	.title = u"Шпион"_q,
	.icon = &st::menuIconShowAll,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	builder.addSkip();
	BuildSpyEssentials(builder, ayu);
	builder.addSkip();
});

} // namespace

rpl::producer<QString> AyuSpy::title() {
	return rpl::single(u"Шпион"_q);
}

AyuSpy::AyuSpy(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller)
, _controller(controller) {
	setupContent();
}

void AyuSpy::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuSpyId() {
	return AyuSpy::Id();
}

} // namespace Settings
