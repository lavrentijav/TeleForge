// Copyright @Radolyn, 2026
#include "tfc/ui/settings/settings_spy.h"

#include "ayu/ayu_settings.h"
#include "tfc/features/ghost/tf_ghost_audit.h"
#include "tfc/features/spy/online_history_storage.h"
#include "tfc/features/teleforge/tf_archive_users_box.h"
#include "tfc/features/teleforge/tf_peer_archive.h"
#include "tfc/ui/settings/ayu_builder.h"
#include "tfc/ui/settings/settings_ayu_utils.h"
#include "tfc/ui/settings/settings_main.h"
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
		return tr::ayu_SpyDurationSeconds(
			tr::now,
			lt_value,
			QString::number(seconds));
	}
	if (seconds < 3600) {
		return tr::ayu_SpyDurationMinutes(
			tr::now,
			lt_value,
			QString::number(seconds / 60));
	}
	const auto hours = seconds / 3600;
	const auto mins = (seconds % 3600) / 60;
	return mins
		? tr::ayu_SpyDurationHoursMinutes(
			tr::now,
			lt_hours,
			QString::number(hours),
			lt_minutes,
			QString::number(mins))
		: tr::ayu_SpyDurationHours(
			tr::now,
			lt_hours,
			QString::number(hours));
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
		.title = tr::ayu_SpyPollIntervals(),
		.fill = [&](not_null<Ui::VerticalLayout*> inner) {
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archiveChatRefresh"_q,
				.title = tr::ayu_SpyChatRefresh(),
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
				.title = tr::ayu_SpyKnownUserOnline(),
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
				.title = tr::ayu_SpyPrivateOnline(),
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
				.title = tr::ayu_SpyPrivateProfile(),
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
				.title = tr::ayu_SpyOtherProfile(),
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
	ayu.base().addDividerText(tr::ayu_SpyPollIntervalsHint());
}

void BuildArchiveScanSettings(AyuSectionBuilder &ayu) {
	const auto &settings = AyuSettings::getInstance();

	ayu.addCollapsibleSection({
		.id = u"teleforge/archiveScan"_q,
		.title = tr::ayu_SpyArchiveScan(),
		.fill = [&](not_null<Ui::VerticalLayout*> inner) {
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveBackgroundCrawl"_q,
				.title = tr::ayu_SpyBackgroundCrawl(),
				.getter = [] {
					return AyuSettings::getInstance().archiveBackgroundCrawl();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveBackgroundCrawl(v);
				},
			});
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveScanParticipants"_q,
				.title = tr::ayu_SpyScanParticipants(),
				.getter = [] {
					return AyuSettings::getInstance().archiveScanParticipants();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveScanParticipants(v);
				},
			});
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveScanBroadcasts"_q,
				.title = tr::ayu_SpyScanBroadcasts(),
				.getter = [] {
					return AyuSettings::getInstance().archiveScanBroadcasts();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveScanBroadcasts(v);
				},
			});
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveSaveUserpics"_q,
				.title = tr::ayu_SpySaveUserpics(),
				.getter = [] {
					return AyuSettings::getInstance().archiveSaveUserpics();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveSaveUserpics(v);
				},
			});
			ayu.addToggleTo(inner, {
				.id = u"teleforge/archiveFollowLinks"_q,
				.title = tr::ayu_SpyFollowLinks(),
				.getter = [] {
					return AyuSettings::getInstance().archiveFollowLinks();
				},
				.setter = [](bool v) {
					AyuSettings::getInstance().setArchiveFollowLinks(v);
				},
			});
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archiveHistoryScanLimit"_q,
				.title = tr::ayu_SpyHistoryScanLimit(),
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
					return x ? QString::number(x) : tr::ayu_SpyLimitNone(tr::now);
				},
			});
			ayu.addSliderTo(inner, {
				.id = u"teleforge/archiveCrawlBatchSize"_q,
				.title = tr::ayu_SpyCrawlBatchSize(),
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
				.title = tr::ayu_SpyMaxParticipants(),
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
					return x ? QString::number(x) : tr::ayu_SpyLimitNone(tr::now);
				},
			});
		},
	});

	ayu.base().addSkip();
	ayu.base().addDividerText(tr::ayu_SpyArchiveScanHint());
}

void BuildSpyEssentials(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle(tr::ayu_SpyEssentialsHeader());

	ayu.addToggle({
		.id = u"teleforge/spyMode"_q,
		.altIds = { u"ayu/spyMode"_q },
		.title = tr::ayu_SpyModeGlobal(),
		.getter = [] {
			return TeleForge::Spy::spyModeGloballyEnabled();
		},
		.setter = [](bool v) {
			TeleForge::Spy::setSpyModeGloballyEnabled(v);
		},
	});

	builder.addSkip();
	builder.addDividerText(tr::ayu_SpyModeGlobalHint());

	ayu.addSlider({
		.id = u"teleforge/spyRetentionDays"_q,
		.title = tr::ayu_SpyRetentionDays(),
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
		.title = tr::ayu_SpyPeerArchive(),
		.getter = [] {
			return AyuSettings::getInstance().peerArchiveEnabled();
		},
		.setter = [](bool v) {
			TeleForge::PeerArchive::setArchiveEnabled(v);
		},
	});

	builder.addButton({
		.id = u"teleforge/archiveUsersBrowse"_q,
		.title = tr::ayu_SpyBrowseUsers(),
		.onClick = [=] {
			TeleForge::PeerArchive::ShowArchiveUsersBox(controller);
		},
	});

	builder.addSkip();
	builder.addDividerText(tr::ayu_SpyPeerArchiveHint());

	BuildPollIntervalSettings(ayu);
	BuildArchiveScanSettings(ayu);

	ayu.addSectionDivider();
	builder.addSubsectionTitle(tr::ayu_SpyMessageSaving());

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
		.title = tr::ayu_SpyShowDeletedInChat(),
		.getter = &AyuSettings::showDeletedInChat,
		.setter = &AyuSettings::setShowDeletedInChat,
	});
	ayu.addSlider({
		.id = u"teleforge/deletedRestoreLimit"_q,
		.title = tr::ayu_SpyDeletedRestoreLimit(),
		.steps = 40,
		.current = AyuSettings::getInstance().deletedRestoreLimit() / 50,
		.indexToValue = [](int index) { return index * 50; },
		.onFinalChanged = [](int value) {
			AyuSettings::getInstance().setDeletedRestoreLimit(value);
		},
		.formatLabel = [](int x) {
			return x ? QString::number(x) : tr::ayu_SpyOff(tr::now);
		},
	});

	builder.addSkip();
	builder.addDividerText(tr::ayu_SpyDeletedHint());

	ayu.addSectionDivider();
	builder.addSubsectionTitle(tr::ayu_SpyGhostDiagnostics());

	ayu.addSettingToggle({
		.id = u"teleforge/ghostPassthroughMentions"_q,
		.title = tr::ayu_SpyPassthroughMentions(),
		.getter = &AyuSettings::ghostPassthroughMentions,
		.setter = &AyuSettings::setGhostPassthroughMentions,
	});

	builder.addButton({
		.id = u"teleforge/ghostWakeSignals"_q,
		.title = tr::ayu_SpyWakeSignals(),
		.onClick = [=] {
			auto lines = TeleForge::Ghost::recentWakeSignals();
			if (lines.isEmpty()) {
				controller->showToast(tr::ayu_SpyNoWakeSignals(tr::now));
				return;
			}
			std::reverse(lines.begin(), lines.end());
			controller->show(Ui::MakeInformBox({
				.text = lines.mid(0, 40).join('\n'),
				.title = tr::ayu_SpyWakeSignalsTitle(tr::now),
			}));
		},
	});

	builder.addSkip();
	builder.addDividerText(tr::ayu_SpyWakeSignalsHint());

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
	.title = &tr::ayu_CategorySpy,
	.icon = &st::menuIconShowAll,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	builder.addSkip();
	BuildSpyEssentials(builder, ayu);
	builder.addSkip();
});

} // namespace

rpl::producer<QString> AyuSpy::title() {
	return tr::ayu_CategorySpy();
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
