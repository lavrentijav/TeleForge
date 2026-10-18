// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "tfc/ui/settings/settings_appearance.h"

#include "lang_auto.h"
#include "tfc/tf_settings.h"
#include "tfc/tf_ui_settings.h"
#include "tfc/ui/boxes/font_selector.h"
#include "tfc/ui/components/avatar_corners_preview.h"
#include "tfc/ui/components/icon_picker.h"
#include "tfc/ui/settings/tf_builder.h"
#include "tfc/ui/settings/settings_main.h"
#include "core/application.h"
#include "inline_bots/bot_attach_web_view.h"
#include "main/main_session.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_tf_icons.h"
#include "styles/style_tf_styles.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/painter.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

namespace Settings {

using namespace Builder;
using namespace AyBuilder;

namespace {

bool HasDrawerBots(not_null<Window::SessionController*> controller) {
	// todo: maybe iterate through all accounts
	const auto bots = &controller->session().attachWebView();
	for (const auto &bot : bots->attachBots()) {
		if (!bot.inMainMenu || !bot.media) {
			continue;
		}
		return true;
	}
	return false;
}

void BuildAppIcon(SectionBuilder &builder, TfSectionBuilder &ayu) {
	builder.addSubsectionTitle({
		.id = u"tf/appIcon"_q,
		.title = tr::tf_AppIconHeader(),
	});

	builder.add([](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		return {
			.widget = object_ptr<IconPicker>(ctx.container),
			.margin = st::settingsButtonNoIcon.padding,
		};
	});

#ifdef Q_OS_WIN
	builder.addDivider();
	builder.addSkip();
	ayu.addSettingToggle({
		.id = u"tf/hideNotificationBadge"_q,
		.title = tr::tf_HideNotificationBadge(),
		.getter = &TfSettings::hideNotificationBadge,
		.setter = &TfSettings::setHideNotificationBadge,
	});
	builder.addSkip();
	builder.addDividerText(tr::tf_HideNotificationBadgeDescription());
	builder.addSkip();
#else
    builder.addDivider();
    builder.addSkip();
#endif
}

void BuildAvatarCorners(SectionBuilder &builder, TfSectionBuilder &ayu) {
	auto *settings = &TfSettings::getInstance();
	const auto controller = builder.controller();

	const auto mapRadius = [](int val)
	{
		if (val == 0) {
			return tr::tf_AvatarCornersSquare(tr::now).toUpper();
		} else if (val == TfUiSettings::kMaxAvatarCorners) {
			return tr::tf_AvatarCornersCircle(tr::now).toUpper();
		}
		return QString::number(val);
	};

	builder.add([=](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		const auto container = ctx.container;
		auto title = object_ptr<Ui::FlatLabel>(
			container,
			tr::tf_AvatarCorners(),
			st::defaultSubsectionTitle);
		const auto titleRaw = title.data();

		const auto badge = Ui::CreateChild<Ui::PaddingWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				settings->avatarCornersValue() | rpl::map(mapRadius),
				st::settingsPremiumNewBadge),
			st::tfBetaBadgePadding);
		badge->show();
		badge->setAttribute(Qt::WA_TransparentForMouseEvents);
		badge->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(badge);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgActive);
			const auto r = st::tfBetaBadgePadding.left();
			p.drawRoundedRect(badge->rect(), r, r);
		}, badge->lifetime());

		titleRaw->geometryValue() | rpl::on_next([=](QRect geometry) {
			badge->moveToLeft(
				geometry.x()
					+ titleRaw->textMaxWidth()
					+ st::settingsPremiumNewBadgePosition.x(),
				geometry.y()
					+ (geometry.height() - badge->height()) / 2);
		}, badge->lifetime());

		return {
			.widget = std::move(title),
			.margin = st::defaultSubsectionTitlePadding,
		};
	}, [] {
		return SearchEntry{
			.id = u"tf/avatarCorners"_q,
			.title = tr::tf_AvatarCorners(tr::now),
		};
	});

	auto *previewRaw = static_cast<AvatarCornersPreview*>(nullptr);
	builder.add([&](const Builder::WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		auto preview = object_ptr<AvatarCornersPreview>(
			ctx.container,
			controller);
		previewRaw = preview.data();
		const auto vMargin = st::settingsButtonNoIcon.padding
			- st::defaultDialogRow.padding;
		return {
			.widget = std::move(preview),
			.margin = QMargins(0, vMargin.top(), 0, vMargin.bottom()),
		};
	});

	ayu.addSlider({
		.id = u"tf/avatarCornersSlider"_q,
		.title = rpl::single(QString()),
		.showTitle = false,
		.steps = TfUiSettings::kMaxAvatarCorners + 1,
		.current = settings->avatarCorners(),
		.onChanged = [=](int val) {
			TfSettings::getInstance().setAvatarCorners(val);
			if (previewRaw) {
				previewRaw->update();
			}
		},
		.onFinalChanged = [=](int val) {
			TfSettings::getInstance().setAvatarCorners(val);
			crl::on_main([=] {
				controller->show(Ui::MakeConfirmBox({
					.text = tr::lng_settings_need_restart(),
					.confirmed = [] { Core::Restart(); },
					.confirmText = tr::lng_settings_restart_now(),
					.cancelText = tr::lng_settings_restart_later(),
				}));
			});
		},
	});

	ayu.addSettingToggle({
		.id = u"tf/singleCornerRadius"_q,
		.title = tr::tf_SingleCornerRadius(),
		.getter = &TfSettings::singleCornerRadius,
		.setter = &TfSettings::setSingleCornerRadius,
	});

	builder.addSkip();
	builder.addDividerText(tr::tf_SingleCornerRadiusDescription());
	builder.addSkip();
}

void BuildAppearance(SectionBuilder &builder, TfSectionBuilder &ayu) {
	auto *settings = &TfSettings::getInstance();

	builder.addSubsectionTitle(tr::tf_CategoryAppearance());

	ayu.addSettingToggle({
		.id = u"tf/materialSwitches"_q,
		.altIds = { u"tf/newSwitchStyle"_q },
		.title = tr::tf_MaterialSwitches(),
		.getter = &TfSettings::materialSwitches,
		.setter = &TfSettings::setMaterialSwitches,
	});
	ayu.addSettingToggle({
		.id = u"tf/disableCustomBackgrounds"_q,
		.altIds = { u"tf/customThemes"_q },
		.title = tr::tf_DisableCustomBackgrounds(),
		.getter = &TfSettings::disableCustomBackgrounds,
		.setter = &TfSettings::setDisableCustomBackgrounds,
	});

	ayu.addSettingToggle({
		.id = u"tf/hidePremiumStatuses"_q,
		.title = tr::tf_HidePremiumStatuses(),
		.getter = &TfSettings::hidePremiumStatuses,
		.setter = &TfSettings::setHidePremiumStatuses,
	});

	const auto controller = builder.controller();
	builder.addButton({
		.id = u"tf/monoFont"_q,
		.title = tr::tf_MonospaceFont(),
		.st = &st::settingsButtonNoIcon,
		.label = rpl::single(
			settings->monoFont().isEmpty()
				? tr::tf_FontDefault(tr::now)
				: settings->monoFont()),
		.onClick = [=] {
			TfUi::FontSelectorBox::Show(
				controller,
				[=](const QString &font) {
					TfSettings::getInstance().setMonoFont(font);
				});
		},
	});

	ayu.addSectionDivider();
}

void BuildChatFolders(SectionBuilder &builder, TfSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::tf_ChatFoldersHeader());

	ayu.addSettingToggle({
		.id = u"tf/hideNotificationCounters"_q,
		.altIds = { u"tf/tabCounter"_q },
		.title = tr::tf_HideNotificationCounters(),
		.getter = &TfSettings::hideNotificationCounters,
		.setter = &TfSettings::setHideNotificationCounters,
	});
	ayu.addSettingToggle({
		.id = u"tf/hideAllChatsFolder"_q,
		.altIds = { u"tf/hideAllChats"_q },
		.title = tr::tf_HideAllChats(),
		.getter = &TfSettings::hideAllChatsFolder,
		.setter = &TfSettings::setHideAllChatsFolder,
	});

	ayu.addSectionDivider();
}

void BuildTrayElements(SectionBuilder &builder, TfSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::tf_TrayElementsHeader());

	ayu.addSettingToggle({
		.id = u"tf/showGhostToggleInTray"_q,
		.title = tr::tf_EnableGhostModeTray(),
		.getter = &TfSettings::showGhostToggleInTray,
		.setter = &TfSettings::setShowGhostToggleInTray,
	});

#if defined Q_OS_WIN || defined Q_OS_MAC
	ayu.addSettingToggle({
		.id = u"tf/showStreamerToggleInTray"_q,
		.title = tr::tf_EnableStreamerModeTray(),
		.getter = &TfSettings::showStreamerToggleInTray,
		.setter = &TfSettings::setShowStreamerToggleInTray,
	});
#endif

	ayu.addSectionDivider();
}

void BuildDrawerElements(SectionBuilder &builder, TfSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::tf_DrawerElementsHeader());

	ayu.addSettingToggle({
		.id = u"tf/showMyProfileInDrawer"_q,
		.title = tr::lng_menu_my_profile(),
		.getter = &TfSettings::showMyProfileInDrawer,
		.setter = &TfSettings::setShowMyProfileInDrawer,
		.icon = { &st::menuIconProfile },
	});

	const auto controller = builder.controller();
	if (controller && HasDrawerBots(controller)) {
		ayu.addSettingToggle({
			.id = u"tf/showBotsInDrawer"_q,
			.title = tr::lng_filters_type_bots(),
			.getter = &TfSettings::showBotsInDrawer,
			.setter = &TfSettings::setShowBotsInDrawer,
			.icon = { &st::menuIconBot },
		});
	}

	ayu.addSettingToggle({
		.id = u"tf/showNewGroupInDrawer"_q,
		.title = tr::lng_create_group_title(),
		.getter = &TfSettings::showNewGroupInDrawer,
		.setter = &TfSettings::setShowNewGroupInDrawer,
		.icon = { &st::menuIconGroups },
	});
	ayu.addSettingToggle({
		.id = u"tf/showNewChannelInDrawer"_q,
		.title = tr::lng_create_channel_title(),
		.getter = &TfSettings::showNewChannelInDrawer,
		.setter = &TfSettings::setShowNewChannelInDrawer,
		.icon = { &st::menuIconChannel },
	});
	ayu.addSettingToggle({
		.id = u"tf/showContactsInDrawer"_q,
		.title = tr::lng_menu_contacts(),
		.getter = &TfSettings::showContactsInDrawer,
		.setter = &TfSettings::setShowContactsInDrawer,
		.icon = { &st::menuIconUserShow },
	});
	ayu.addSettingToggle({
		.id = u"tf/showCallsInDrawer"_q,
		.title = tr::lng_menu_calls(),
		.getter = &TfSettings::showCallsInDrawer,
		.setter = &TfSettings::setShowCallsInDrawer,
		.icon = { &st::menuIconPhone },
	});
	ayu.addSettingToggle({
		.id = u"tf/showSavedMessagesInDrawer"_q,
		.title = tr::lng_saved_messages(),
		.getter = &TfSettings::showSavedMessagesInDrawer,
		.setter = &TfSettings::setShowSavedMessagesInDrawer,
		.icon = { &st::menuIconSavedMessages },
	});
	ayu.addSettingToggle({
		.id = u"tf/showLReadToggleInDrawer"_q,
		.title = tr::tf_LReadMessages(),
		.getter = &TfSettings::showLReadToggleInDrawer,
		.setter = &TfSettings::setShowLReadToggleInDrawer,
		.icon = { &st::tfLReadMenuIcon },
	});
	ayu.addSettingToggle({
		.id = u"tf/showSReadToggleInDrawer"_q,
		.title = tr::tf_SReadMessages(),
		.getter = &TfSettings::showSReadToggleInDrawer,
		.setter = &TfSettings::setShowSReadToggleInDrawer,
		.icon = { &st::tfSReadMenuIcon },
	});
	ayu.addSettingToggle({
		.id = u"tf/showNightModeToggleInDrawer"_q,
		.title = tr::lng_menu_night_mode(),
		.getter = &TfSettings::showNightModeToggleInDrawer,
		.setter = &TfSettings::setShowNightModeToggleInDrawer,
		.icon = { &st::menuIconNightMode },
	});
	ayu.addSettingToggle({
		.id = u"tf/showGhostToggleInDrawer"_q,
		.title = tr::tf_GhostModeToggle(),
		.getter = &TfSettings::showGhostToggleInDrawer,
		.setter = &TfSettings::setShowGhostToggleInDrawer,
		.icon = { &st::tfGhostIcon },
	});

#if defined Q_OS_WIN || defined Q_OS_MAC
	ayu.addSettingToggle({
		.id = u"tf/showStreamerToggleInDrawer"_q,
		.title = tr::tf_StreamerModeToggle(),
		.getter = &TfSettings::showStreamerToggleInDrawer,
		.setter = &TfSettings::setShowStreamerToggleInDrawer,
		.icon = { &st::tfStreamerModeMenuIcon },
	});
#endif

	builder.addSkip();
}

const auto kMeta = BuildHelper({
	.id = TfAppearance::Id(),
	.parentId = TfMain::Id(),
	.title = &tr::tf_CategoryAppearance,
	.icon = &st::menuIconPalette,
}, [](SectionBuilder &builder) {
	auto ayu = TfSectionBuilder(builder);

	builder.addSkip();
	BuildAvatarCorners(builder, ayu);
	BuildAppearance(builder, ayu);
	BuildChatFolders(builder, ayu);
	BuildTrayElements(builder, ayu);
	BuildDrawerElements(builder, ayu);
	builder.addSkip();
});

} // namespace

rpl::producer<QString> TfAppearance::title() {
	return tr::tf_CategoryAppearance();
}

TfAppearance::TfAppearance(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void TfAppearance::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type TfAppearanceId() {
	return TfAppearance::Id();
}

} // namespace Settings
