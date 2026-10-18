// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "tfc/ui/settings/settings_general.h"

#include "lang_auto.h"
#include "tfc/tf_settings.h"
#include "tfc/ui/settings/tf_builder.h"
#include "tfc/ui/settings/settings_tf_utils.h"
#include "tfc/ui/settings/settings_main.h"
#include "core/application.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

namespace Settings {

using namespace Builder;
using namespace AyBuilder;

namespace {

void BuildTranslator(SectionBuilder &builder, TfSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::lng_translate_settings_subtitle());

	auto *settings = &TfSettings::getInstance();

	const auto options = std::vector{
		QString("Telegram"),
		QString("Google"),
		QString("Yandex")
	};

	const auto getIndex = [=](TranslationProvider val) {
		return static_cast<int>(val);
	};

	auto currentVal = TfSettings::getInstance().translationProviderValue()
		| rpl::map(getIndex)
		| rpl::map([=](int val) { return options[val]; });

	const auto button = builder.addButton({
		.id = u"tf/translationProvider"_q,
		.title = tr::tf_TranslationProvider(),
		.st = &st::settingsButtonNoIcon,
		.label = std::move(currentVal),
		.onClick = [=] {
			if (const auto controller = Core::App().activeWindow()->sessionController()) {
				controller->show(Box(
					[=](not_null<Ui::GenericBox*> box) {
						const auto save = [=](int index) {
							TfSettings::getInstance().setTranslationProvider(
								static_cast<TranslationProvider>(index));
						};
						SingleChoiceBox(box, {
							.title = tr::tf_TranslationProvider(),
							.options = options,
							.initialSelection = getIndex(settings->translationProvider()),
							.callback = save,
						});
					}));
			}
		},
	});
	if (button) {
		ayu.addBetaBadge(button);
	}
}

void BuildShowPeerId(SectionBuilder &builder) {
	auto *settings = &TfSettings::getInstance();

	const auto options = std::vector{
		QString(tr::tf_SettingsShowID_Hide(tr::now)),
		QString("Telegram API"),
		QString("Bot API")
	};

	auto currentVal = TfSettings::getInstance().showPeerIdValue()
		| rpl::map([=](PeerIdDisplay val) {
			return options[static_cast<int>(val)];
		});

	const auto controller = builder.controller();
	builder.addButton({
		.id = u"tf/showPeerId"_q,
		.altIds = { u"tf/showIdAndDc"_q },
		.title = tr::tf_SettingsShowID(),
		.st = &st::settingsButtonNoIcon,
		.label = std::move(currentVal),
		.onClick = [=] {
			controller->show(Box(
				[=](not_null<Ui::GenericBox*> box) {
					const auto save = [=](int index) {
						TfSettings::getInstance().setShowPeerId(
							static_cast<PeerIdDisplay>(index));
					};
					SingleChoiceBox(box, {
						.title = tr::tf_SettingsShowID(),
						.options = options,
						.initialSelection = static_cast<int>(settings->showPeerId()),
						.callback = save,
					});
				}));
		},
	});
}

void BuildQoLToggles(SectionBuilder &builder, TfSectionBuilder &ayu) {
	auto *settings = &TfSettings::getInstance();

	BuildTranslator(builder, ayu);
	ayu.addSectionDivider();

	builder.addSubsectionTitle(tr::tf_CategoryGeneral());

	ayu.addSettingToggle({
		.id = u"tf/disableStories"_q,
		.altIds = { u"tf/hideStories"_q },
		.title = tr::tf_DisableStories(),
		.getter = &TfSettings::disableStories,
		.setter = &TfSettings::setDisableStories,
	});

	ayu.addSettingToggle({
		.id = u"tf/disableOpenLinkWarning"_q,
		.title = tr::tf_DisableOpenLinkWarning(),
		.getter = &TfSettings::disableOpenLinkWarning,
		.setter = &TfSettings::setDisableOpenLinkWarning,
	});

	ayu.addCollapsibleToggle({
		.id = u"tf/similarChannels"_q,
		.title = tr::tf_DisableSimilarChannels(),
		.checkboxes = {
			NestedEntry{
				tr::tf_CollapseSimilarChannels(tr::now),
				[] { return TfSettings::getInstance().collapseSimilarChannels(); },
				[](bool v) { TfSettings::getInstance().setCollapseSimilarChannels(v); }
			},
			NestedEntry{
				tr::tf_HideSimilarChannelsTab(tr::now),
				[] { return TfSettings::getInstance().hideSimilarChannels(); },
				[](bool v) { TfSettings::getInstance().setHideSimilarChannels(v); }
			}
		},
		.toggledWhenAll = true,
	});

	ayu.addSettingToggle({
		.id = u"tf/disableNotificationsDelay"_q,
		.title = tr::tf_DisableNotificationsDelay(),
		.getter = &TfSettings::disableNotificationsDelay,
		.setter = &TfSettings::setDisableNotificationsDelay,
	});

	ayu.addSectionDivider();

	const auto controller = builder.controller();
	const auto zalgoButton = builder.addButton({
		.id = u"tf/filterZalgo"_q,
		.title = tr::tf_FilterZalgo(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(settings->filterZalgo()),
	});
	if (zalgoButton) {
		zalgoButton->toggledValue(
		) | rpl::filter(
			[=](bool enabled) {
				return (enabled != settings->filterZalgo());
			}
		) | on_next(
			[=](bool enabled) {
				TfSettings::getInstance().setFilterZalgo(enabled);
				controller->show(Ui::MakeConfirmBox({
					.text = tr::lng_settings_need_restart(),
					.confirmed = [] { Core::Restart(); },
					.confirmText = tr::lng_settings_restart_now(),
					.cancelText = tr::lng_settings_restart_later(),
				}));
			},
			zalgoButton->lifetime());
		ayu.addBetaBadge(zalgoButton);
	}

	ayu.addSettingToggle({
		.id = u"tf/improveLinkPreviews"_q,
		.title = tr::tf_ImproveLinkPreviews(),
		.getter = &TfSettings::improveLinkPreviews,
		.setter = &TfSettings::setImproveLinkPreviews,
	});
	ayu.addSettingToggle({
		.id = u"tf/showMessageSeconds"_q,
		.altIds = { u"tf/formatTimeWithSeconds"_q },
		.title = tr::tf_SettingsShowMessageSeconds(),
		.getter = &TfSettings::showMessageSeconds,
		.setter = &TfSettings::setShowMessageSeconds,
	});

	ayu.addSettingToggle({
		.id = u"tf/localPremium"_q,
		.title = tr::tf_LocalPremium(),
		.getter = &TfSettings::localPremium,
		.setter = &TfSettings::setLocalPremium,
	});
	ayu.addSettingToggle({
		.id = u"tf/disableAds"_q,
		.title = tr::tf_DisableAds(),
		.getter = &TfSettings::disableAds,
		.setter = &TfSettings::setDisableAds,
	});

	BuildShowPeerId(builder);

	ayu.addSectionDivider();

	builder.addSubsectionTitle(tr::tf_WebviewTitle());

	ayu.addSettingToggle({
		.id = u"tf/spoofWebviewAsAndroid"_q,
		.title = tr::tf_SettingsSpoofWebviewAsAndroid(),
		.getter = &TfSettings::spoofWebviewAsAndroid,
		.setter = &TfSettings::setSpoofWebviewAsAndroid,
	});

	ayu.addCollapsibleToggle({
		.id = u"tf/biggerWindow"_q,
		.title = tr::tf_SettingsBiggerWindow(),
		.checkboxes = {
			NestedEntry{
				tr::tf_SettingsIncreaseWebviewHeight(tr::now),
				[] { return TfSettings::getInstance().increaseWebviewHeight(); },
				[](bool v) { TfSettings::getInstance().setIncreaseWebviewHeight(v); }
			},
			NestedEntry{
				tr::tf_SettingsIncreaseWebviewWidth(tr::now),
				[] { return TfSettings::getInstance().increaseWebviewWidth(); },
				[](bool v) { TfSettings::getInstance().setIncreaseWebviewWidth(v); }
			}
		},
		.toggledWhenAll = false,
	});

	ayu.addSectionDivider();

	builder.addSubsectionTitle(tr::tf_ConfirmationsTitle());

	ayu.addSettingToggle({
		.id = u"tf/stickerConfirmation"_q,
		.title = tr::tf_StickerConfirmation(),
		.getter = &TfSettings::stickerConfirmation,
		.setter = &TfSettings::setStickerConfirmation,
	});
	ayu.addSettingToggle({
		.id = u"tf/gifConfirmation"_q,
		.title = tr::tf_GIFConfirmation(),
		.getter = &TfSettings::gifConfirmation,
		.setter = &TfSettings::setGifConfirmation,
	});
	ayu.addSettingToggle({
		.id = u"tf/voiceConfirmation"_q,
		.title = tr::tf_VoiceConfirmation(),
		.getter = &TfSettings::voiceConfirmation,
		.setter = &TfSettings::setVoiceConfirmation,
	});
}

const auto kMeta = BuildHelper({
	.id = TfGeneral::Id(),
	.parentId = TfMain::Id(),
	.title = &tr::tf_CategoryGeneral,
	.icon = &st::menuIconShowAll,
}, [](SectionBuilder &builder) {
	auto ayu = TfSectionBuilder(builder);

	builder.addSkip();
	BuildQoLToggles(builder, ayu);
	builder.addSkip();
});

} // namespace

rpl::producer<QString> TfGeneral::title() {
	return tr::tf_CategoryGeneral();
}

TfGeneral::TfGeneral(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void TfGeneral::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type TfGeneralId() {
	return TfGeneral::Id();
}

} // namespace Settings
