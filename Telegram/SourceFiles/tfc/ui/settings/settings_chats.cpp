// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "tfc/ui/settings/settings_chats.h"

#include "lang_auto.h"
#include "tfc/tf_settings.h"
#include "tfc/ui/boxes/edit_mark_box.h"
#include "tfc/ui/components/message_preview.h"
#include "tfc/ui/settings/tf_builder.h"
#include "tfc/ui/settings/settings_tf_utils.h"
#include "tfc/ui/settings/settings_main.h"
#include "core/application.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_tf_icons.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

namespace Settings {

using namespace Builder;
using namespace AyBuilder;

namespace {

void BuildStickersAndEmoji(SectionBuilder &builder, TfSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::lng_settings_stickers_emoji());

	ayu.addSettingToggle({
		.id = u"ayu/showOnlyAddedEmojisAndStickers"_q,
		.title = tr::tf_ShowOnlyAddedEmojisAndStickers(),
		.getter = &TfSettings::showOnlyAddedEmojisAndStickers,
		.setter = &TfSettings::setShowOnlyAddedEmojisAndStickers,
	});

	ayu.addCollapsibleToggle({
		.id = u"ayu/hideReactions"_q,
		.title = tr::tf_HideReactions(),
		.checkboxes = {
			NestedEntry{
				tr::tf_HideReactionsInChannels(tr::now),
				[] { return !TfSettings::getInstance().showChannelReactions(); },
				[](bool v) { TfSettings::getInstance().setShowChannelReactions(!v); }
			},
			NestedEntry{
				tr::tf_HideReactionsInGroups(tr::now),
				[] { return !TfSettings::getInstance().showGroupReactions(); },
				[](bool v) { TfSettings::getInstance().setShowGroupReactions(!v); }
			},
			NestedEntry{
				tr::tf_HideReactionsInPrivateChats(tr::now),
				[] { return !TfSettings::getInstance().showPrivateChatReactions(); },
				[](bool v) { TfSettings::getInstance().setShowPrivateChatReactions(!v); }
			}
		},
		.toggledWhenAll = false,
	});

	ayu.addSectionDivider();
}

void BuildRecentStickersLimit(SectionBuilder &builder, TfSectionBuilder &ayu) {
	auto *settings = &TfSettings::getInstance();

	ayu.addSlider({
		.id = u"ayu/recentStickersCount"_q,
		.title = tr::tf_SettingsRecentStickersCount(),
		.steps = 200 + 1,
		.current = settings->recentStickersCount(),
		.indexToValue = [](int index) { return index; },
		.onChanged = nullptr,
		.onFinalChanged = [](int amount) {
			TfSettings::getInstance().setRecentStickersCount(amount);
		},
		.formatLabel = [](int amount) { return QString::number(amount); },
	});

	ayu.addSectionDivider();
}

void BuildGroupsAndChannels(SectionBuilder &builder, TfSectionBuilder &ayu) {
	auto *settings = &TfSettings::getInstance();

	builder.addSubsectionTitle(tr::tf_ChatsGroupsAndChannels());

	ayu.addChooseButton({
		.id = u"ayu/channelBottomButton"_q,
		.altIds = { u"ayu/bottomButton"_q },
		.title = tr::tf_ChannelBottomButton(),
		.boxTitle = tr::tf_ChannelBottomButton(),
		.initialSelection = static_cast<int>(settings->channelBottomButton()),
		.options = {
			tr::tf_ChannelBottomButtonHide(tr::now),
			tr::tf_ChannelBottomButtonMute(tr::now),
			tr::tf_ChannelBottomButtonDiscuss(tr::now),
		},
		.setter = [](int index) {
			TfSettings::getInstance().setChannelBottomButton(
				static_cast<ChannelBottomButton>(index));
		},
	});

	ayu.addSettingToggle({
		.id = u"ayu/quickAdminShortcuts"_q,
		.title = tr::tf_QuickAdminShortcuts(),
		.getter = &TfSettings::quickAdminShortcuts,
		.setter = &TfSettings::setQuickAdminShortcuts,
	});
	ayu.addSettingToggle({
		.id = u"ayu/showMessageShot"_q,
		.title = tr::tf_SettingsShowMessageShot(),
		.getter = &TfSettings::showMessageShot,
		.setter = &TfSettings::setShowMessageShot,
	});

	builder.addSkip();
	builder.addDividerText(tr::tf_SettingsShowMessageShotDescription());
	builder.addSkip();
}

void BuildMarks(SectionBuilder &builder, TfSectionBuilder &ayu) {
	auto *settings = &TfSettings::getInstance();
	const auto controller = builder.controller();

	builder.addSubsectionTitle(tr::lng_settings_messages());

	builder.add([=](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		return {
			.widget = object_ptr<MessagePreview>(ctx.container, controller),
			.margin = style::margins(
				0,
				st::defaultVerticalListSkip,
				0,
				st::settingsPrivacySkipTop),
		};
	});

	ayu.addSettingToggle({
		.id = u"ayu/replaceBottomInfoWithIcons"_q,
		.altIds = { u"ayu/replaceEditedWithIcon"_q },
		.title = tr::tf_ReplaceMarksWithIcons(),
		.getter = &TfSettings::replaceBottomInfoWithIcons,
		.setter = &TfSettings::setReplaceBottomInfoWithIcons,
	});

	builder.scope([&] {
		builder.addButton({
			.id = u"ayu/deletedMark"_q,
			.title = tr::tf_DeletedMarkText(),
			.st = &st::settingsButtonNoIcon,
			.label = TfSettings::getInstance().deletedMarkValue(),
			.onClick = [=] {
				auto box = Box<EditMarkBox>(
					tr::tf_DeletedMarkText(),
					settings->deletedMark(),
					QString("🧹"),
					[=](const QString &value) {
						TfSettings::getInstance().setDeletedMark(value);
					});
				Ui::show(std::move(box));
			},
		});

		builder.addButton({
			.id = u"ayu/editedMark"_q,
			.title = tr::tf_EditedMarkText(),
			.st = &st::settingsButtonNoIcon,
			.label = TfSettings::getInstance().editedMarkValue(),
			.onClick = [=] {
				auto box = Box<EditMarkBox>(
					tr::tf_EditedMarkText(),
					settings->editedMark(),
					tr::lng_edited(tr::now),
					[=](const QString &value) {
						TfSettings::getInstance().setEditedMark(value);
					});
				Ui::show(std::move(box));
			},
		});
	}, TfSettings::getInstance().replaceBottomInfoWithIconsValue()
		| rpl::map([](bool v) { return !v; }));

	ayu.addSettingToggle({
		.id = u"ayu/removeMessageTail"_q,
		.title = tr::tf_RemoveMessageTail(),
		.getter = &TfSettings::removeMessageTail,
		.setter = &TfSettings::setRemoveMessageTail,
	});

	ayu.addSettingToggle({
		.id = u"ayu/hideFastShare"_q,
		.altIds = { u"ayu/hideShareButton"_q },
		.title = tr::tf_HideShareButton(),
		.getter = &TfSettings::hideFastShare,
		.setter = &TfSettings::setHideFastShare,
	});
	ayu.addSettingToggle({
		.id = u"ayu/simpleQuotesAndReplies"_q,
		.altIds = { u"ayu/disableColorfulReplies"_q, u"ayu/replyElements"_q },
		.title = tr::tf_SimpleQuotesAndReplies(),
		.getter = &TfSettings::simpleQuotesAndReplies,
		.setter = &TfSettings::setSimpleQuotesAndReplies,
	});

	const auto semiTransparent = ayu.addSettingToggle({
		.id = u"ayu/semiTransparentDeletedMessages"_q,
		.altIds = { u"ayu/translucentDeletedMessages"_q },
		.title = tr::tf_SemiTransparentDeletedMessages(),
		.getter = &TfSettings::semiTransparentDeletedMessages,
		.setter = &TfSettings::setSemiTransparentDeletedMessages,
	});
	if (semiTransparent) {
		ayu.addBetaBadge(semiTransparent);
	}

	ayu.addSectionDivider();
}

void BuildWideMessagesMultiplier(SectionBuilder &builder, TfSectionBuilder &ayu) {
	auto *settings = &TfSettings::getInstance();

	constexpr auto kSizeAmount = 61; // (4.00 - 1.00) / 0.05 + 1
	constexpr auto kMinSize = 1.00;
	constexpr auto kStep = 0.05;

	const auto valueToIndex = [=](double value) {
		return static_cast<int>(std::round((value - kMinSize) / kStep));
	};

	const auto controller = builder.controller();
	ayu.addSlider({
		.id = u"ayu/wideMultiplier"_q,
		.title = tr::tf_SettingsWideMultiplier(),
		.steps = kSizeAmount,
		.current = valueToIndex(settings->wideMultiplier()),
		.indexToValue = [](int index) { return index; },
		.onChanged = nullptr,
		.onFinalChanged = [=](int index) {
			TfSettings::getInstance().setWideMultiplier(
				kMinSize + index * kStep);
			crl::on_main([=] {
				controller->show(Ui::MakeConfirmBox({
					.text = tr::lng_settings_need_restart(),
					.confirmed = [] { Core::Restart(); },
					.confirmText = tr::lng_settings_restart_now(),
					.cancelText = tr::lng_settings_restart_later(),
				}));
			});
		},
		.formatLabel = [=](int index) {
			return QString::number(kMinSize + index * kStep, 'f', 2);
		},
	});

	builder.addSkip();
	builder.addDividerText(tr::tf_SettingsWideMultiplierDescription());
	builder.addSkip();
}

void BuildContextMenuElements(SectionBuilder &builder, TfSectionBuilder &ayu) {
	auto *settings = &TfSettings::getInstance();

	builder.addSubsectionTitle(tr::tf_ContextMenuElementsHeader());

	const auto options = std::vector{
		tr::tf_SettingsContextMenuItemHidden(tr::now),
		tr::tf_SettingsContextMenuItemShown(tr::now),
		tr::tf_SettingsContextMenuItemExtended(tr::now),
	};

	ayu.addChooseButton({
		.id = u"ayu/showReactionsPanelInContextMenu"_q,
		.title = tr::tf_SettingsContextMenuReactionsPanel(),
		.boxTitle = tr::tf_SettingsContextMenuTitle(),
		.initialSelection = static_cast<int>(settings->showReactionsPanelInContextMenu()),
		.options = options,
		.setter = [](int i) { TfSettings::getInstance().setShowReactionsPanelInContextMenu(static_cast<ContextMenuVisibility>(i)); },
		.icon = { &st::menuIconReactions },
	});
	ayu.addChooseButton({
		.id = u"ayu/showViewsPanelInContextMenu"_q,
		.title = tr::tf_SettingsContextMenuViewsPanel(),
		.boxTitle = tr::tf_SettingsContextMenuTitle(),
		.initialSelection = static_cast<int>(settings->showViewsPanelInContextMenu()),
		.options = options,
		.setter = [](int i) { TfSettings::getInstance().setShowViewsPanelInContextMenu(static_cast<ContextMenuVisibility>(i)); },
		.icon = { &st::menuIconShowInChat },
	});
	ayu.addChooseButton({
		.id = u"ayu/showHideMessageInContextMenu"_q,
		.title = tr::tf_ContextHideMessage(),
		.boxTitle = tr::tf_SettingsContextMenuTitle(),
		.initialSelection = static_cast<int>(settings->showHideMessageInContextMenu()),
		.options = options,
		.setter = [](int i) { TfSettings::getInstance().setShowHideMessageInContextMenu(static_cast<ContextMenuVisibility>(i)); },
		.icon = { &st::menuIconClear },
	});
	ayu.addChooseButton({
		.id = u"ayu/showUserMessagesInContextMenu"_q,
		.title = tr::tf_UserMessagesMenuText(),
		.boxTitle = tr::tf_SettingsContextMenuTitle(),
		.initialSelection = static_cast<int>(settings->showUserMessagesInContextMenu()),
		.options = options,
		.setter = [](int i) { TfSettings::getInstance().setShowUserMessagesInContextMenu(static_cast<ContextMenuVisibility>(i)); },
		.icon = { &st::menuIconTTL },
	});
	ayu.addChooseButton({
		.id = u"ayu/showMessageDetailsInContextMenu"_q,
		.title = tr::tf_MessageDetailsPC(),
		.boxTitle = tr::tf_SettingsContextMenuTitle(),
		.initialSelection = static_cast<int>(settings->showMessageDetailsInContextMenu()),
		.options = options,
		.setter = [](int i) { TfSettings::getInstance().setShowMessageDetailsInContextMenu(static_cast<ContextMenuVisibility>(i)); },
		.icon = { &st::menuIconInfo },
	});
	ayu.addChooseButton({
		.id = u"ayu/showRepeatMessageInContextMenu"_q,
		.title = tr::tf_RepeatMessage(),
		.boxTitle = tr::tf_SettingsContextMenuTitle(),
		.initialSelection = static_cast<int>(settings->showRepeatMessageInContextMenu()),
		.options = options,
		.setter = [](int i) { TfSettings::getInstance().setShowRepeatMessageInContextMenu(static_cast<ContextMenuVisibility>(i)); },
		.icon = { &st::tfRepeatMenuIcon },
	});
	if (settings->filtersEnabled()) {
		ayu.addChooseButton({
			.id = u"ayu/showAddFilterInContextMenu"_q,
			.title = tr::tf_RegexFilterQuickAdd(),
			.boxTitle = tr::tf_SettingsContextMenuTitle(),
			.initialSelection = static_cast<int>(settings->showAddFilterInContextMenu()),
			.options = options,
			.setter = [](int i) { TfSettings::getInstance().setShowAddFilterInContextMenu(static_cast<ContextMenuVisibility>(i)); },
			.icon = { &st::menuIconAddToFolder },
		});
	}

	builder.addSkip();
	builder.addDividerText(tr::tf_SettingsContextMenuDescription());
	builder.addSkip();
}

void BuildMessageFieldElements(SectionBuilder &builder, TfSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::tf_MessageFieldElementsHeader());

	ayu.addSettingToggle({
		.id = u"ayu/showAttachButtonInMessageField"_q,
		.title = tr::tf_MessageFieldElementAttach(),
		.getter = &TfSettings::showAttachButtonInMessageField,
		.setter = &TfSettings::setShowAttachButtonInMessageField,
		.icon = { &st::messageFieldAttachIcon },
	});
	ayu.addSettingToggle({
		.id = u"ayu/showCommandsButtonInMessageField"_q,
		.title = tr::tf_MessageFieldElementCommands(),
		.getter = &TfSettings::showCommandsButtonInMessageField,
		.setter = &TfSettings::setShowCommandsButtonInMessageField,
		.icon = { &st::messageFieldCommandsIcon },
	});
	ayu.addSettingToggle({
		.id = u"ayu/showAutoDeleteButtonInMessageField"_q,
		.title = tr::tf_MessageFieldElementTTL(),
		.getter = &TfSettings::showAutoDeleteButtonInMessageField,
		.setter = &TfSettings::setShowAutoDeleteButtonInMessageField,
		.icon = { &st::messageFieldTTLIcon },
	});
	ayu.addSettingToggle({
		.id = u"ayu/showEmojiButtonInMessageField"_q,
		.title = tr::tf_MessageFieldElementEmoji(),
		.getter = &TfSettings::showEmojiButtonInMessageField,
		.setter = &TfSettings::setShowEmojiButtonInMessageField,
		.icon = { &st::messageFieldEmojiIcon },
	});
	ayu.addSettingToggle({
		.id = u"ayu/showMicrophoneButtonInMessageField"_q,
		.title = tr::tf_MessageFieldElementVoice(),
		.getter = &TfSettings::showMicrophoneButtonInMessageField,
		.setter = &TfSettings::setShowMicrophoneButtonInMessageField,
		.icon = { &st::messageFieldVoiceIcon },
	});

	ayu.addSectionDivider();
}

void BuildMessageFieldPopups(SectionBuilder &builder, TfSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::tf_MessageFieldPopupsHeader());

	ayu.addSettingToggle({
		.id = u"ayu/showAttachPopup"_q,
		.title = tr::tf_MessageFieldElementAttach(),
		.getter = &TfSettings::showAttachPopup,
		.setter = &TfSettings::setShowAttachPopup,
		.icon = { &st::messageFieldAttachIcon },
	});
	ayu.addSettingToggle({
		.id = u"ayu/showEmojiPopup"_q,
		.title = tr::tf_MessageFieldElementEmoji(),
		.getter = &TfSettings::showEmojiPopup,
		.setter = &TfSettings::setShowEmojiPopup,
		.icon = { &st::messageFieldEmojiIcon },
	});
}

const auto kMeta = BuildHelper({
	.id = TfChats::Id(),
	.parentId = TfMain::Id(),
	.title = &tr::tf_CategoryChats,
	.icon = &st::menuIconChatBubble,
}, [](SectionBuilder &builder) {
	auto ayu = TfSectionBuilder(builder);

	builder.addSkip();
	BuildStickersAndEmoji(builder, ayu);
	BuildRecentStickersLimit(builder, ayu);
	BuildGroupsAndChannels(builder, ayu);
	BuildMarks(builder, ayu);
	BuildWideMessagesMultiplier(builder, ayu);
	BuildContextMenuElements(builder, ayu);
	BuildMessageFieldElements(builder, ayu);
	BuildMessageFieldPopups(builder, ayu);
	builder.addSkip();
});

} // namespace

rpl::producer<QString> TfChats::title() {
	return tr::tf_CategoryChats();
}

TfChats::TfChats(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void TfChats::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type TfChatsId() {
	return TfChats::Id();
}

} // namespace Settings
