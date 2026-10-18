// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "tfc/tf_settings.h"
#include "tfc/features/sync/teleforge_sync_crypto.h"

#include "lang_auto.h"
#include "tray.h"
#include "tfc/tf_ui_settings.h"
#include "tfc/tf_worker.h"
#include "tfc/features/ghost/tf_ghost_read.h"
#include "tfc/ui/tf_logo.h"
#include "core/application.h"
#include "main/main_account.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "features/filters/filters_cache_controller.h"
#include "features/translator/tf_translator.h"
#include "history/history.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "rpl/combine.h"
#include "ui/tf_userpic.h"
#include "window/window_controller.h"

#include <exception>
#include <fstream>
#include <QApplication>

using json = nlohmann::json;

namespace {

std::string getSettingsPath() {
	return (cWorkingDir() + u"tdata/ayu_settings.json"_q).toStdString();
}

void repaintApp() {
	for (QWidget *widget : QApplication::allWidgets()) {
		widget->update();
	}
}

rpl::lifetime lifetime; // idk reactivity dies when placed in `GhostModeAccountSettings` as field

} // namespace

GhostModeAccountSettings::GhostModeAccountSettings() {
	rpl::combine(
		_sendReadMessages.value(),
		_sendReadStories.value(),
		_sendOnlinePackets.value(),
		_sendUploadProgress.value(),
		_sendOfflinePacketAfterOnline.value()
	) | rpl::on_next([=](
			bool readMsg,
			bool readStories,
			bool online,
			bool upload,
			bool offline) {
		// WHY: a single suppressed leak already hides the user, so the mode
		// must not stay reported as off while the network is ghosted.
		_ghostModeActive = !readMsg
			|| !readStories
			|| !online
			|| !upload
			|| offline;
	}, lifetime);
}

void GhostModeAccountSettings::setSendReadMessages(bool val) {
	if (_sendReadMessages.current() == val) return;
	_sendReadMessages = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setSendReadStories(bool val) {
	if (_sendReadStories.current() == val) return;
	_sendReadStories = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setSendOnlinePackets(bool val) {
	if (_sendOnlinePackets.current() == val) return;
	_sendOnlinePackets = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setSendUploadProgress(bool val) {
	if (_sendUploadProgress.current() == val) return;
	_sendUploadProgress = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setSendOfflinePacketAfterOnline(bool val) {
	if (_sendOfflinePacketAfterOnline.current() == val) return;
	_sendOfflinePacketAfterOnline = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setMarkReadAfterAction(bool val) {
	if (_markReadAfterAction.current() == val) return;
	_markReadAfterAction = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setUseScheduledMessages(bool val) {
	if (_useScheduledMessages.current() == val) return;
	_useScheduledMessages = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setSendWithoutSound(bool val) {
	if (_sendWithoutSound.current() == val) return;
	_sendWithoutSound = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setGhostModeEnabled(bool val) {
	if (!_sendReadMessagesLocked.current()) _sendReadMessages = !val;
	if (!_sendReadStoriesLocked.current()) _sendReadStories = !val;
	if (!_sendOnlinePacketsLocked.current()) _sendOnlinePackets = !val;
	if (!_sendUploadProgressLocked.current()) _sendUploadProgress = !val;
	if (!_sendOfflinePacketAfterOnlineLocked.current()) _sendOfflinePacketAfterOnline = val;
	TfSettings::save();

	if (val) {
		if (const auto window = Core::App().activeWindow()) {
			if (const auto session = window->maybeSession()) {
				TfWorker::scheduleOfflinePacket(session);
			}
		}
	} else {
		for (const auto &entry : Core::App().domain().accounts()) {
			if (const auto session = entry.account->maybeSession()) {
				TeleForge::GhostRead::scheduleFlush(session);
			}
		}
	}
}

void GhostModeAccountSettings::setSendReadMessagesLocked(bool val) {
	if (_sendReadMessagesLocked.current() == val) return;
	_sendReadMessagesLocked = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setSendReadStoriesLocked(bool val) {
	if (_sendReadStoriesLocked.current() == val) return;
	_sendReadStoriesLocked = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setSendOnlinePacketsLocked(bool val) {
	if (_sendOnlinePacketsLocked.current() == val) return;
	_sendOnlinePacketsLocked = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setSendUploadProgressLocked(bool val) {
	if (_sendUploadProgressLocked.current() == val) return;
	_sendUploadProgressLocked = val;
	TfSettings::save();
}

void GhostModeAccountSettings::setSendOfflinePacketAfterOnlineLocked(bool val) {
	if (_sendOfflinePacketAfterOnlineLocked.current() == val) return;
	_sendOfflinePacketAfterOnlineLocked = val;
	TfSettings::save();
}

void to_json(nlohmann::json &j, const GhostModeAccountSettings &s) {
	j = nlohmann::json{
		{"sendReadMessages", s._sendReadMessages.current()},
		{"sendReadStories", s._sendReadStories.current()},
		{"sendOnlinePackets", s._sendOnlinePackets.current()},
		{"sendUploadProgress", s._sendUploadProgress.current()},
		{"sendOfflinePacketAfterOnline", s._sendOfflinePacketAfterOnline.current()},
		{"markReadAfterAction", s._markReadAfterAction.current()},
		{"useScheduledMessages", s._useScheduledMessages.current()},
		{"sendWithoutSound", s._sendWithoutSound.current()},
		{"sendReadMessagesLocked", s._sendReadMessagesLocked.current()},
		{"sendReadStoriesLocked", s._sendReadStoriesLocked.current()},
		{"sendOnlinePacketsLocked", s._sendOnlinePacketsLocked.current()},
		{"sendUploadProgressLocked", s._sendUploadProgressLocked.current()},
		{"sendOfflinePacketAfterOnlineLocked", s._sendOfflinePacketAfterOnlineLocked.current()}
	};
}

void from_json(const nlohmann::json &j, GhostModeAccountSettings &s) {
	s._sendReadMessages = j.value("sendReadMessages", true);
	s._sendReadStories = j.value("sendReadStories", true);
	s._sendOnlinePackets = j.value("sendOnlinePackets", true);
	s._sendUploadProgress = j.value("sendUploadProgress", true);
	s._sendOfflinePacketAfterOnline = j.value("sendOfflinePacketAfterOnline", false);
	s._markReadAfterAction = j.value("markReadAfterAction", true);
	s._useScheduledMessages = j.value("useScheduledMessages", false);
	s._sendWithoutSound = j.value("sendWithoutSound", false);
	s._sendReadMessagesLocked = j.value("sendReadMessagesLocked", false);
	s._sendReadStoriesLocked = j.value("sendReadStoriesLocked", false);
	s._sendOnlinePacketsLocked = j.value("sendOnlinePacketsLocked", false);
	s._sendUploadProgressLocked = j.value("sendUploadProgressLocked", false);
	s._sendOfflinePacketAfterOnlineLocked = j.value("sendOfflinePacketAfterOnlineLocked", false);
}

void MessageShotSettings::setShowBackground(bool val) {
	if (_showBackground.current() == val) return;
	_showBackground = val;
	TfSettings::save();
}

void MessageShotSettings::setShowDate(bool val) {
	if (_showDate.current() == val) return;
	_showDate = val;
	TfSettings::save();
}

void MessageShotSettings::setShowReactions(bool val) {
	if (_showReactions.current() == val) return;
	_showReactions = val;
	TfSettings::save();
}

void MessageShotSettings::setShowColorfulReplies(bool val) {
	if (_showColorfulReplies.current() == val) return;
	_showColorfulReplies = val;
	TfSettings::save();
}

void MessageShotSettings::setRevealSpoilers(bool val) {
	if (_revealSpoilers.current() == val) return;
	_revealSpoilers = val;
	TfSettings::save();
}

bool MessageShotSettings::isCloudThemeEmpty() const {
	return !_cloudThemeId.current()
		&& !_cloudThemeAccessHash.current()
		&& !_cloudThemeDocumentId.current()
		&& _cloudThemeTitle.current().isEmpty();
}

void MessageShotSettings::clearCloudThemeData() {
	_cloudThemeId = uint64(0);
	_cloudThemeAccessHash = uint64(0);
	_cloudThemeDocumentId = uint64(0);
	_cloudThemeTitle = QString();
	_cloudThemeAccountId = uint64(0);
}

void MessageShotSettings::setEmbeddedTheme(int type, uint32 accentColor) {
	if (_embeddedThemeType.current() == type
		&& _embeddedThemeAccentColor.current() == accentColor
		&& isCloudThemeEmpty()) {
		return;
	}
	_embeddedThemeType = type;
	_embeddedThemeAccentColor = accentColor;
	clearCloudThemeData();
	TfSettings::save();
}

void MessageShotSettings::setCloudTheme(uint64 accountId, uint64 id, uint64 accessHash, uint64 documentId, const QString &title) {
	if (_embeddedThemeType.current() == -1
		&& _embeddedThemeAccentColor.current() == 0
		&& _cloudThemeAccountId.current() == accountId
		&& _cloudThemeId.current() == id
		&& _cloudThemeAccessHash.current() == accessHash
		&& _cloudThemeDocumentId.current() == documentId
		&& _cloudThemeTitle.current() == title) {
		return;
	}
	_embeddedThemeType = -1;
	_embeddedThemeAccentColor = uint32(0);
	_cloudThemeAccountId = accountId;
	_cloudThemeId = id;
	_cloudThemeAccessHash = accessHash;
	_cloudThemeDocumentId = documentId;
	_cloudThemeTitle = title;
	TfSettings::save();
}

void MessageShotSettings::clearTheme() {
	if (_embeddedThemeType.current() == -1
		&& _embeddedThemeAccentColor.current() == 0
		&& isCloudThemeEmpty()) {
		return;
	}
	_embeddedThemeType = -1;
	_embeddedThemeAccentColor = uint32(0);
	clearCloudThemeData();
	TfSettings::save();
}

void to_json(nlohmann::json &j, const MessageShotSettings &s) {
	j = nlohmann::json{
		{"showBackground", s._showBackground.current()},
		{"showDate", s._showDate.current()},
		{"showReactions", s._showReactions.current()},
		{"showColorfulReplies", s._showColorfulReplies.current()},
		{"revealSpoilers", s._revealSpoilers.current()},
		{"embeddedThemeType", s._embeddedThemeType.current()},
		{"embeddedThemeAccentColor", s._embeddedThemeAccentColor.current()},
		{"cloudThemeId", s._cloudThemeId.current()},
		{"cloudThemeAccessHash", s._cloudThemeAccessHash.current()},
		{"cloudThemeDocumentId", s._cloudThemeDocumentId.current()},
		{"cloudThemeTitle", s._cloudThemeTitle.current()},
		{"cloudThemeAccountId", s._cloudThemeAccountId.current()},
	};
}

void from_json(const nlohmann::json &j, MessageShotSettings &s) {
	s._showBackground = j.value("showBackground", true);
	s._showDate = j.value("showDate", false);
	s._showReactions = j.value("showReactions", false);
	s._showColorfulReplies = j.value("showColorfulReplies", true);
	s._revealSpoilers = j.value("revealSpoilers", true);
	s._embeddedThemeType = j.value("embeddedThemeType", j.value("themeType", -1));
	s._embeddedThemeAccentColor = j.value("embeddedThemeAccentColor", j.value("themeAccentColor", uint32(0)));
	s._cloudThemeId = j.value("cloudThemeId", uint64(0));
	s._cloudThemeAccessHash = j.value("cloudThemeAccessHash", uint64(0));
	s._cloudThemeDocumentId = j.value("cloudThemeDocumentId", uint64(0));
	s._cloudThemeTitle = j.value("cloudThemeTitle", QString());
	s._cloudThemeAccountId = j.value("cloudThemeAccountId", uint64(0));
}

TfSettings::TfSettings()
: _appIcon(TeleForgeAssets::DEFAULT_ICON)
, _editedMark(Core::IsAppLaunched() ? tr::lng_edited(tr::now) : QString("edited")) {
}

TfSettings &TfSettings::getInstance() {
	static TfSettings instance;
	return instance;
}

void TfSettings::load() {
	std::ifstream file(getSettingsPath());
	if (!file.good()) {
		return;
	}

	auto &settings = getInstance();

	try {
		json p;
		file >> p;
		file.close();

		if (!p.contains("ghostModeSettings")) {
			p["ghostModeSettings"] = nlohmann::json::object({
				{"0", {
					{"sendReadMessages", p.value("sendReadMessages", true)},
					{"sendReadStories", p.value("sendReadStories", true)},
					{"sendOnlinePackets", p.value("sendOnlinePackets", true)},
					{"sendUploadProgress", p.value("sendUploadProgress", true)},
					{"sendOfflinePacketAfterOnline", p.value("sendOfflinePacketAfterOnline", false)},
					{"markReadAfterAction", p.value("markReadAfterAction", true)},
					{"useScheduledMessages", p.value("useScheduledMessages", false)},
					{"sendWithoutSound", p.value("sendWithoutSound", false)}
				}}
			});
			p["useGlobalGhostMode"] = true;

			LOG(("TeleForgeSettings: migrated ghost mode settings to per-account format"));
		}

		try {
			from_json(p, settings);
		} catch (...) {
			LOG(("TeleForgeSettings: failed to parse settings file"));
		}
	} catch (...) {
		LOG(("TeleForgeSettings: failed to read settings file (not json-like)"));
	}

	if (cGhost()) {
		auto &ghost = TfSettings::ghost();
		ghost._sendReadMessages = false;
		ghost._sendReadStories = false;
		ghost._sendOnlinePackets = false;
		ghost._sendUploadProgress = false;
		ghost._sendOfflinePacketAfterOnline = true;
	}

	settings.validate();
}

void TfSettings::save() {
	auto &settings = getInstance();
	json p = settings;

	std::ofstream file;
	file.open(getSettingsPath());
	file << p.dump(4);
	file.close();
}

void TfSettings::reset() {
	getInstance() = TfSettings();
	save();
}

GhostModeAccountSettings &TfSettings::ghost(not_null<Main::Session*> session) {
	return ghost(session->userId().bare);
}

GhostModeAccountSettings &TfSettings::ghost(uint64 userId) {
	auto &settings = getInstance();
	auto overriddenId = settings.getOverriddenGhostUserId(userId);

	auto it = settings._ghostAccounts.find(overriddenId);
	if (it == settings._ghostAccounts.end()) {
		auto account = std::make_unique<GhostModeAccountSettings>();
		it = settings._ghostAccounts.emplace(overriddenId, std::move(account)).first;
	}

	return *it->second;
}

GhostModeAccountSettings &TfSettings::ghost() {
	if (const auto window = Core::App().activeWindow()) {
		if (const auto session = window->maybeSession()) {
			return ghost(session);
		}
	}
	return ghost(0);
}

void TfSettings::setUseGlobalGhostMode(bool val) {
	if (_useGlobalGhostMode.current() == val) return;
	_useGlobalGhostMode = val;
	save();
}

void TfSettings::addShadowBan(int64 id) {
	if (_shadowBanIds.insert(id).second) {
		FiltersCacheController::rebuildCache();
		FiltersCacheController::fireUpdate();
		save();
	}
}

void TfSettings::removeShadowBan(int64 id) {
	if (_shadowBanIds.erase(id) > 0) {
		FiltersCacheController::rebuildCache();
		FiltersCacheController::fireUpdate();
		save();
	}
}

bool TfSettings::isUnreadBadgeExcluded(uint64 peerSerialized) const {
	return _unreadBadgeExcludedPeers.contains(peerSerialized);
}

void TfSettings::setUnreadBadgeExcluded(uint64 peerSerialized, bool excluded) {
	const auto changed = excluded
		? _unreadBadgeExcludedPeers.insert(peerSerialized).second
		: (_unreadBadgeExcludedPeers.erase(peerSerialized) > 0);
	if (!changed) {
		return;
	}
	save();
	if (const auto session = Core::App().maybePrimarySession()) {
		const auto peerId = PeerId(PeerIdHelper{ peerSerialized });
		if (const auto peer = session->data().peerLoaded(peerId)) {
			session->data().history(peer)->updateChatListEntry();
		}
	}
	Core::App().tray().updateIconCounters();
	Core::App().refreshApplicationIcon();
	Core::App().domain().notifyUnreadBadgeChanged();
}

void TfSettings::toggleUnreadBadgeExcluded(uint64 peerSerialized) {
	setUnreadBadgeExcluded(peerSerialized, !isUnreadBadgeExcluded(peerSerialized));
}

void TfSettings::validate() {
	TfSettings defaults;
	auto modified = false;

	auto validateRange = [&](auto &var, auto min, auto max, const auto &defaultVar) {
		if (var.current() < min || var.current() > max) {
			var = defaultVar.current();
			modified = true;
		}
	};

	auto validateEnum = [&](auto &var, const auto &defaultVar) {
		auto intVal = static_cast<int>(var.current());
		if (intVal < 0 || intVal > 2) {
			var = defaultVar.current();
			modified = true;
		}
	};

	validateEnum(_showPeerId, defaults._showPeerId);
	validateEnum(_channelBottomButton, defaults._channelBottomButton);
	validateEnum(_showReactionsPanelInContextMenu, defaults._showReactionsPanelInContextMenu);
	validateEnum(_showViewsPanelInContextMenu, defaults._showViewsPanelInContextMenu);
	validateEnum(_showHideMessageInContextMenu, defaults._showHideMessageInContextMenu);
	validateEnum(_showUserMessagesInContextMenu, defaults._showUserMessagesInContextMenu);
	validateEnum(_showMessageDetailsInContextMenu, defaults._showMessageDetailsInContextMenu);
	validateEnum(_showRepeatMessageInContextMenu, defaults._showRepeatMessageInContextMenu);
	validateEnum(_showAddFilterInContextMenu, defaults._showAddFilterInContextMenu);

	validateEnum(_translationProvider, defaults._translationProvider);

	validateRange(_wideMultiplier, 0.5, 4.0, defaults._wideMultiplier);
	validateRange(_recentStickersCount, 1, 200, defaults._recentStickersCount);
	validateRange(_avatarCorners, 0, TfUiSettings::kMaxAvatarCorners, defaults._avatarCorners);

	const auto embeddedType = _messageShotSettings._embeddedThemeType.current();
	auto embeddedTypeValid = (embeddedType == -1) || (embeddedType >= 0 && embeddedType <= 3); // from Window::Theme::EmbeddedType::DayBlue to Window::Theme::EmbeddedType::NightGreen
	if (!embeddedTypeValid) {
		_messageShotSettings._embeddedThemeType = defaults._messageShotSettings._embeddedThemeType.current();
		_messageShotSettings._embeddedThemeAccentColor = defaults._messageShotSettings._embeddedThemeAccentColor.current();
		modified = true;
	}

	if (modified) {
		save();
	}
}

void TfSettings::setSaveDeletedMessages(bool val) {
	if (_saveDeletedMessages.current() == val) return;
	_saveDeletedMessages = val;
	save();
}

void TfSettings::setSaveMessagesHistory(bool val) {
	if (_saveMessagesHistory.current() == val) return;
	_saveMessagesHistory = val;
	save();
}

void TfSettings::setSaveForBots(bool val) {
	if (_saveForBots.current() == val) return;
	_saveForBots = val;
	save();
}

void TfSettings::setFiltersEnabled(bool val) {
	if (_filtersEnabled.current() == val) return;
	_filtersEnabled = val;
	save();
}

void TfSettings::setFiltersEnabledInChats(bool val) {
	if (_filtersEnabledInChats.current() == val) return;
	_filtersEnabledInChats = val;
	save();
}

void TfSettings::setHideFromBlocked(bool val) {
	if (_hideFromBlocked.current() == val) return;
	_hideFromBlocked = val;
	save();
}

void TfSettings::setSemiTransparentDeletedMessages(bool val) {
	if (_semiTransparentDeletedMessages.current() == val) return;
	_semiTransparentDeletedMessages = val;
	save();
}

void TfSettings::setDisableAds(bool val) {
	if (_disableAds.current() == val) return;
	_disableAds = val;
	save();
}

void TfSettings::setDisableStories(bool val) {
	if (_disableStories.current() == val) return;
	_disableStories = val;
	save();
}

void TfSettings::setDisableCustomBackgrounds(bool val) {
	if (_disableCustomBackgrounds.current() == val) return;
	_disableCustomBackgrounds = val;
	save();
}

void TfSettings::setHidePremiumStatuses(bool val) {
	if (_hidePremiumStatuses.current() == val) return;
	_hidePremiumStatuses = val;
	save();
}

void TfSettings::setShowOnlyAddedEmojisAndStickers(bool val) {
	if (_showOnlyAddedEmojisAndStickers.current() == val) return;
	_showOnlyAddedEmojisAndStickers = val;
	save();
}

void TfSettings::setCollapseSimilarChannels(bool val) {
	if (_collapseSimilarChannels.current() == val) return;
	_collapseSimilarChannels = val;
	save();
}

void TfSettings::setHideSimilarChannels(bool val) {
	if (_hideSimilarChannels.current() == val) return;
	_hideSimilarChannels = val;
	save();
}

void TfSettings::setWideMultiplier(double val) {
	if (_wideMultiplier.current() == val) return;
	_wideMultiplier = val;
	// doesn't work because it should be set before style::StartManager()
	// TfUiSettings::setWideMultiplier(val);
	// repaintApp();
	save();
}

void TfSettings::setSpoofWebviewAsAndroid(bool val) {
	if (_spoofWebviewAsAndroid.current() == val) return;
	_spoofWebviewAsAndroid = val;
	save();
}

void TfSettings::setDisableOpenLinkWarning(bool val) {
	if (_disableOpenLinkWarning.current() == val) return;
	_disableOpenLinkWarning = val;
	save();
}

void TfSettings::setIncreaseWebviewHeight(bool val) {
	if (_increaseWebviewHeight.current() == val) return;
	_increaseWebviewHeight = val;
	save();
}

void TfSettings::setIncreaseWebviewWidth(bool val) {
	if (_increaseWebviewWidth.current() == val) return;
	_increaseWebviewWidth = val;
	save();
}

void TfSettings::setMaterialSwitches(bool val) {
	if (_materialSwitches.current() == val) return;
	_materialSwitches = val;
	TfUiSettings::setMaterialSwitches(val);
	repaintApp();
	save();
}

void TfSettings::setRemoveMessageTail(bool val) {
	if (_removeMessageTail.current() == val) return;
	_removeMessageTail = val;
	save();
}

void TfSettings::setDisableNotificationsDelay(bool val) {
	if (_disableNotificationsDelay.current() == val) return;
	_disableNotificationsDelay = val;
	save();
}

void TfSettings::setLocalPremium(bool val) {
	if (_localPremium.current() == val) return;
	_localPremium = val;
	save();
}

void TfSettings::setShowChannelReactions(bool val) {
	if (_showChannelReactions.current() == val) return;
	_showChannelReactions = val;
	save();
}

void TfSettings::setShowGroupReactions(bool val) {
	if (_showGroupReactions.current() == val) return;
	_showGroupReactions = val;
	save();
}

void TfSettings::setShowPrivateChatReactions(bool val) {
	if (_showPrivateChatReactions.current() == val) return;
	_showPrivateChatReactions = val;
	save();
}

void TfSettings::setAppIcon(const QString &val) {
	if (_appIcon.current() == val) return;
	_appIcon = val;
	save();
}

void TfSettings::setSimpleQuotesAndReplies(bool val) {
	if (_simpleQuotesAndReplies.current() == val) return;
	_simpleQuotesAndReplies = val;
	save();
}

void TfSettings::setHideFastShare(bool val) {
	if (_hideFastShare.current() == val) return;
	_hideFastShare = val;
	save();
}

void TfSettings::setReplaceBottomInfoWithIcons(bool val) {
	if (_replaceBottomInfoWithIcons.current() == val) return;
	_replaceBottomInfoWithIcons = val;
	save();
}

void TfSettings::setDeletedMark(const QString &val) {
	if (_deletedMark.current() == val) return;
	_deletedMark = val;
	save();
}

void TfSettings::setEditedMark(const QString &val) {
	if (_editedMark.current() == val) return;
	_editedMark = val;
	save();
}

void TfSettings::setRecentStickersCount(int val) {
	if (_recentStickersCount.current() == val) return;
	_recentStickersCount = val;
	save();
}

void TfSettings::setShowReactionsPanelInContextMenu(ContextMenuVisibility val) {
	if (_showReactionsPanelInContextMenu.current() == val) return;
	_showReactionsPanelInContextMenu = val;
	save();
}

void TfSettings::setShowViewsPanelInContextMenu(ContextMenuVisibility val) {
	if (_showViewsPanelInContextMenu.current() == val) return;
	_showViewsPanelInContextMenu = val;
	save();
}

void TfSettings::setShowHideMessageInContextMenu(ContextMenuVisibility val) {
	if (_showHideMessageInContextMenu.current() == val) return;
	_showHideMessageInContextMenu = val;
	save();
}

void TfSettings::setShowUserMessagesInContextMenu(ContextMenuVisibility val) {
	if (_showUserMessagesInContextMenu.current() == val) return;
	_showUserMessagesInContextMenu = val;
	save();
}

void TfSettings::setShowMessageDetailsInContextMenu(ContextMenuVisibility val) {
	if (_showMessageDetailsInContextMenu.current() == val) return;
	_showMessageDetailsInContextMenu = val;
	save();
}

void TfSettings::setShowRepeatMessageInContextMenu(ContextMenuVisibility val) {
	if (_showRepeatMessageInContextMenu.current() == val) return;
	_showRepeatMessageInContextMenu = val;
	save();
}

void TfSettings::setShowAddFilterInContextMenu(ContextMenuVisibility val) {
	if (_showAddFilterInContextMenu.current() == val) return;
	_showAddFilterInContextMenu = val;
	save();
}

void TfSettings::setShowAttachButtonInMessageField(bool val) {
	if (_showAttachButtonInMessageField.current() == val) return;
	_showAttachButtonInMessageField = val;
	save();
}

void TfSettings::setShowCommandsButtonInMessageField(bool val) {
	if (_showCommandsButtonInMessageField.current() == val) return;
	_showCommandsButtonInMessageField = val;
	save();
}

void TfSettings::setShowEmojiButtonInMessageField(bool val) {
	if (_showEmojiButtonInMessageField.current() == val) return;
	_showEmojiButtonInMessageField = val;
	save();
}

void TfSettings::setShowMicrophoneButtonInMessageField(bool val) {
	if (_showMicrophoneButtonInMessageField.current() == val) return;
	_showMicrophoneButtonInMessageField = val;
	save();
}

void TfSettings::setShowAutoDeleteButtonInMessageField(bool val) {
	if (_showAutoDeleteButtonInMessageField.current() == val) return;
	_showAutoDeleteButtonInMessageField = val;
	save();
}

void TfSettings::setShowAttachPopup(bool val) {
	if (_showAttachPopup.current() == val) return;
	_showAttachPopup = val;
	save();
}

void TfSettings::setShowEmojiPopup(bool val) {
	if (_showEmojiPopup.current() == val) return;
	_showEmojiPopup = val;
	save();
}

void TfSettings::setShowMyProfileInDrawer(bool val) {
	if (_showMyProfileInDrawer.current() == val) return;
	_showMyProfileInDrawer = val;
	save();
}

void TfSettings::setShowBotsInDrawer(bool val) {
	if (_showBotsInDrawer.current() == val) return;
	_showBotsInDrawer = val;
	save();
}

void TfSettings::setShowNewGroupInDrawer(bool val) {
	if (_showNewGroupInDrawer.current() == val) return;
	_showNewGroupInDrawer = val;
	save();
}

void TfSettings::setShowNewChannelInDrawer(bool val) {
	if (_showNewChannelInDrawer.current() == val) return;
	_showNewChannelInDrawer = val;
	save();
}

void TfSettings::setShowContactsInDrawer(bool val) {
	if (_showContactsInDrawer.current() == val) return;
	_showContactsInDrawer = val;
	save();
}

void TfSettings::setShowCallsInDrawer(bool val) {
	if (_showCallsInDrawer.current() == val) return;
	_showCallsInDrawer = val;
	save();
}

void TfSettings::setShowSavedMessagesInDrawer(bool val) {
	if (_showSavedMessagesInDrawer.current() == val) return;
	_showSavedMessagesInDrawer = val;
	save();
}

void TfSettings::setShowLReadToggleInDrawer(bool val) {
	if (_showLReadToggleInDrawer.current() == val) return;
	_showLReadToggleInDrawer = val;
	save();
}

void TfSettings::setShowSReadToggleInDrawer(bool val) {
	if (_showSReadToggleInDrawer.current() == val) return;
	_showSReadToggleInDrawer = val;
	save();
}

void TfSettings::setShowNightModeToggleInDrawer(bool val) {
	if (_showNightModeToggleInDrawer.current() == val) return;
	_showNightModeToggleInDrawer = val;
	save();
}

void TfSettings::setShowGhostToggleInDrawer(bool val) {
	if (_showGhostToggleInDrawer.current() == val) return;
	_showGhostToggleInDrawer = val;
	save();
}

void TfSettings::setShowStreamerToggleInDrawer(bool val) {
	if (_showStreamerToggleInDrawer.current() == val) return;
	_showStreamerToggleInDrawer = val;
	save();
}

void TfSettings::setShowGhostToggleInTray(bool val) {
	if (_showGhostToggleInTray.current() == val) return;
	_showGhostToggleInTray = val;
	save();
}

void TfSettings::setShowStreamerToggleInTray(bool val) {
	if (_showStreamerToggleInTray.current() == val) return;
	_showStreamerToggleInTray = val;
	save();
}

void TfSettings::setMonoFont(const QString &val) {
	if (_monoFont.current() == val) return;
	_monoFont = val;
	// doesn't work because `static const auto family = ...`
	// TfUiSettings::setMonoFont(val);
	// repaintApp();
	save();
}

void TfSettings::setHideNotificationCounters(bool val) {
	if (_hideNotificationCounters.current() == val) return;
	_hideNotificationCounters = val;
	save();
}

void TfSettings::setHideNotificationBadge(bool val) {
	if (_hideNotificationBadge.current() == val) return;
	_hideNotificationBadge = val;
	Core::App().refreshApplicationIcon();
	Core::App().tray().updateIconCounters();
	Core::App().domain().notifyUnreadBadgeChanged();
	save();
}

void TfSettings::setHideAllChatsFolder(bool val) {
	if (_hideAllChatsFolder.current() == val) return;
	_hideAllChatsFolder = val;
	save();
}

void TfSettings::setChannelBottomButton(ChannelBottomButton val) {
	if (_channelBottomButton.current() == val) return;
	_channelBottomButton = val;
	save();
}

void TfSettings::setQuickAdminShortcuts(bool val) {
	if (_quickAdminShortcuts.current() == val) return;
	_quickAdminShortcuts = val;
	save();
}

void TfSettings::setShowPeerId(PeerIdDisplay val) {
	if (_showPeerId.current() == val) return;
	_showPeerId = val;
	save();
}

void TfSettings::setShowMessageSeconds(bool val) {
	if (_showMessageSeconds.current() == val) return;
	_showMessageSeconds = val;
	save();
}

void TfSettings::setShowMessageShot(bool val) {
	if (_showMessageShot.current() == val) return;
	_showMessageShot = val;
	save();
}

void TfSettings::setFilterZalgo(bool val) {
	if (_filterZalgo.current() == val) return;
	_filterZalgo = val;
	save();
}

void TfSettings::setStickerConfirmation(bool val) {
	if (_stickerConfirmation.current() == val) return;
	_stickerConfirmation = val;
	save();
}

void TfSettings::setGifConfirmation(bool val) {
	if (_gifConfirmation.current() == val) return;
	_gifConfirmation = val;
	save();
}

void TfSettings::setVoiceConfirmation(bool val) {
	if (_voiceConfirmation.current() == val) return;
	_voiceConfirmation = val;
	save();
}

void TfSettings::setTranslationProvider(TranslationProvider val) {
	if (_translationProvider.current() == val) return;
	_translationProvider = val;
	Ayu::Translator::TranslateManager::currentInstance()->resetCache();
	save();
}

void TfSettings::setAiTranslationEnabled(bool val) {
	if (_aiTranslationEnabled.current() == val) return;
	_aiTranslationEnabled = val;
	save();
}

void TfSettings::setAiCompressionEnabled(bool val) {
	if (_aiCompressionEnabled.current() == val) return;
	_aiCompressionEnabled = val;
	save();
}

void TfSettings::setDeleteStubText(const QString &val) {
	if (_deleteStubText.current() == val) return;
	_deleteStubText = val;
	save();
}

void TfSettings::setSpyModeGloballyEnabled(bool val) {
	if (_spyModeGloballyEnabled.current() == val) return;
	_spyModeGloballyEnabled = val;
	save();
}

void TfSettings::setSpyRetentionDays(int val) {
	const auto clamped = std::clamp(val, 1, 365);
	if (_spyRetentionDays.current() == clamped) return;
	_spyRetentionDays = clamped;
	save();
}

void TfSettings::setPeerArchiveEnabled(bool val) {
	if (_peerArchiveEnabled.current() == val) return;
	_peerArchiveEnabled = val;
	save();
}

void TfSettings::setArchiveChatRefreshSeconds(int val) {
	const auto clamped = std::clamp(val, 60, 3600);
	if (_archiveChatRefreshSeconds.current() == clamped) return;
	_archiveChatRefreshSeconds = clamped;
	save();
}

void TfSettings::setArchiveKnownUserOnlineSeconds(int val) {
	const auto clamped = std::clamp(val, 15, 600);
	if (_archiveKnownUserOnlineSeconds.current() == clamped) return;
	_archiveKnownUserOnlineSeconds = clamped;
	save();
}

void TfSettings::setArchivePrivateOnlineSeconds(int val) {
	const auto clamped = std::clamp(val, 5, 120);
	if (_archivePrivateOnlineSeconds.current() == clamped) return;
	_archivePrivateOnlineSeconds = clamped;
	save();
}

void TfSettings::setArchivePrivateProfileSeconds(int val) {
	const auto clamped = std::clamp(val, 60, 7200);
	if (_archivePrivateProfileSeconds.current() == clamped) return;
	_archivePrivateProfileSeconds = clamped;
	save();
}

void TfSettings::setArchiveOtherProfileSeconds(int val) {
	const auto clamped = std::clamp(val, 300, 21600);
	if (_archiveOtherProfileSeconds.current() == clamped) return;
	_archiveOtherProfileSeconds = clamped;
	save();
}

void TfSettings::setArchiveHistoryScanLimit(int val) {
	const auto clamped = std::clamp(val, 0, 20000);
	if (_archiveHistoryScanLimit.current() == clamped) return;
	_archiveHistoryScanLimit = clamped;
	save();
}

void TfSettings::setArchiveCrawlBatchSize(int val) {
	const auto clamped = std::clamp(val, 1, 200);
	if (_archiveCrawlBatchSize.current() == clamped) return;
	_archiveCrawlBatchSize = clamped;
	save();
}

void TfSettings::setArchiveMaxParticipants(int val) {
	const auto clamped = std::clamp(val, 0, 200000);
	if (_archiveMaxParticipants.current() == clamped) return;
	_archiveMaxParticipants = clamped;
	save();
}

void TfSettings::setArchiveScanParticipants(bool val) {
	if (_archiveScanParticipants.current() == val) return;
	_archiveScanParticipants = val;
	save();
}

void TfSettings::setArchiveScanBroadcasts(bool val) {
	if (_archiveScanBroadcasts.current() == val) return;
	_archiveScanBroadcasts = val;
	save();
}

void TfSettings::setArchiveSaveUserpics(bool val) {
	if (_archiveSaveUserpics.current() == val) return;
	_archiveSaveUserpics = val;
	save();
}

void TfSettings::setArchiveFollowLinks(bool val) {
	if (_archiveFollowLinks.current() == val) return;
	_archiveFollowLinks = val;
	save();
}

void TfSettings::setArchiveBackgroundCrawl(bool val) {
	if (_archiveBackgroundCrawl.current() == val) return;
	_archiveBackgroundCrawl = val;
	save();
}

QString TfSettings::syncArchivePassphrase() const {
	const auto stored = _syncArchivePassphrase.current();
	return stored.isEmpty()
		? QString()
		: TeleForge::Sync::UnprotectSecret(stored);
}

void TfSettings::setSyncArchivePassphrase(const QString &value) {
	const auto stored = value.isEmpty()
		? QString()
		: TeleForge::Sync::ProtectSecret(value);
	if (_syncArchivePassphrase.current() == stored) {
		return;
	}
	_syncArchivePassphrase = stored;
	TfSettings::save();
}
void TfSettings::setGhostPassthroughMentions(bool val) {
	if (_ghostPassthroughMentions.current() == val) return;
	_ghostPassthroughMentions = val;
	save();
}

void TfSettings::setShowDeletedInChat(bool val) {
	if (_showDeletedInChat.current() == val) return;
	_showDeletedInChat = val;
	save();
}

void TfSettings::setDeletedRestoreLimit(int val) {
	const auto clamped = std::clamp(val, 0, 5000);
	if (_deletedRestoreLimit.current() == clamped) return;
	_deletedRestoreLimit = clamped;
	save();
}

void TfSettings::setAdaptiveCoverColor(bool val) {
	if (_adaptiveCoverColor.current() == val) return;
	_adaptiveCoverColor = val;
	save();
}

void TfSettings::setImproveLinkPreviews(bool val) {
	if (_improveLinkPreviews.current() == val) return;
	_improveLinkPreviews = val;
	save();
}

void TfSettings::setCrashReporting(bool val) {
	if (_crashReporting.current() == val) return;
	_crashReporting = val;
	save();
}

void TfSettings::setAvatarCorners(int val) {
	if (_avatarCorners.current() == val) return;
	_avatarCorners = val;
	TfUiSettings::setAvatarCorners(val);
	save();
}

void TfSettings::setSingleCornerRadius(bool val) {
	if (_singleCornerRadius.current() == val) return;
	_singleCornerRadius = val;
	repaintApp();
	save();
}

void to_json(nlohmann::json &j, const TfSettings &s) {
	std::map<std::string, GhostModeAccountSettings> ghostAccounts;
	for (const auto &[key, value] : s._ghostAccounts) {
		ghostAccounts[std::to_string(key)] = std::move(*value);
	}

	j = nlohmann::json{
		{"ghostModeSettings", ghostAccounts},
		{"useGlobalGhostMode", s._useGlobalGhostMode.current()},
		{"saveDeletedMessages", s._saveDeletedMessages.current()},
		{"showDeletedInChat", s._showDeletedInChat.current()},
		{"ghostPassthroughMentions", s._ghostPassthroughMentions.current()},
		{"syncArchivePassphrase", s._syncArchivePassphrase.current()},
		{"deletedRestoreLimit", s._deletedRestoreLimit.current()},
		{"saveMessagesHistory", s._saveMessagesHistory.current()},
		{"saveForBots", s._saveForBots.current()},
		{"shadowBanIds", s._shadowBanIds},
		{"unreadBadgeExcluded", s._unreadBadgeExcludedPeers},
		{"filtersEnabled", s._filtersEnabled.current()},
		{"filtersEnabledInChats", s._filtersEnabledInChats.current()},
		{"hideFromBlocked", s._hideFromBlocked.current()},
		{"semiTransparentDeletedMessages", s._semiTransparentDeletedMessages.current()},
		{"disableAds", s._disableAds.current()},
		{"disableStories", s._disableStories.current()},
		{"disableCustomBackgrounds", s._disableCustomBackgrounds.current()},
		{"showOnlyAddedEmojisAndStickers", s._showOnlyAddedEmojisAndStickers.current()},
		{"collapseSimilarChannels", s._collapseSimilarChannels.current()},
		{"hideSimilarChannels", s._hideSimilarChannels.current()},
		{"disableOpenLinkWarning", s._disableOpenLinkWarning.current()},
		{"wideMultiplier", s._wideMultiplier.current()},
		{"spoofWebviewAsAndroid", s._spoofWebviewAsAndroid.current()},
		{"increaseWebviewHeight", s._increaseWebviewHeight.current()},
		{"increaseWebviewWidth", s._increaseWebviewWidth.current()},
		{"materialSwitches", s._materialSwitches.current()},
		{"removeMessageTail", s._removeMessageTail.current()},
		{"disableNotificationsDelay", s._disableNotificationsDelay.current()},
		{"localPremium", s._localPremium.current()},
		{"showChannelReactions", s._showChannelReactions.current()},
		{"showGroupReactions", s._showGroupReactions.current()},
		{"showPrivateChatReactions", s._showPrivateChatReactions.current()},
		{"appIcon", s._appIcon.current()},
		{"simpleQuotesAndReplies", s._simpleQuotesAndReplies.current()},
		{"hideFastShare", s._hideFastShare.current()},
		{"replaceBottomInfoWithIcons", s._replaceBottomInfoWithIcons.current()},
		{"deletedMark", s._deletedMark.current()},
		{"editedMark", s._editedMark.current()},
		{"recentStickersCount", s._recentStickersCount.current()},
		{"showReactionsPanelInContextMenu", s._showReactionsPanelInContextMenu.current()},
		{"showViewsPanelInContextMenu", s._showViewsPanelInContextMenu.current()},
		{"showHideMessageInContextMenu", s._showHideMessageInContextMenu.current()},
		{"showUserMessagesInContextMenu", s._showUserMessagesInContextMenu.current()},
		{"showMessageDetailsInContextMenu", s._showMessageDetailsInContextMenu.current()},
		{"showRepeatMessageInContextMenu", s._showRepeatMessageInContextMenu.current()},
		{"showAddFilterInContextMenu", s._showAddFilterInContextMenu.current()},
		{"showAttachButtonInMessageField", s._showAttachButtonInMessageField.current()},
		{"showCommandsButtonInMessageField", s._showCommandsButtonInMessageField.current()},
		{"showEmojiButtonInMessageField", s._showEmojiButtonInMessageField.current()},
		{"showMicrophoneButtonInMessageField", s._showMicrophoneButtonInMessageField.current()},
		{"showAutoDeleteButtonInMessageField", s._showAutoDeleteButtonInMessageField.current()},
		{"showAttachPopup", s._showAttachPopup.current()},
		{"showEmojiPopup", s._showEmojiPopup.current()},
		{"showMyProfileInDrawer", s._showMyProfileInDrawer.current()},
		{"showBotsInDrawer", s._showBotsInDrawer.current()},
		{"showNewGroupInDrawer", s._showNewGroupInDrawer.current()},
		{"showNewChannelInDrawer", s._showNewChannelInDrawer.current()},
		{"showContactsInDrawer", s._showContactsInDrawer.current()},
		{"showCallsInDrawer", s._showCallsInDrawer.current()},
		{"showSavedMessagesInDrawer", s._showSavedMessagesInDrawer.current()},
		{"showLReadToggleInDrawer", s._showLReadToggleInDrawer.current()},
		{"showSReadToggleInDrawer", s._showSReadToggleInDrawer.current()},
		{"showNightModeToggleInDrawer", s._showNightModeToggleInDrawer.current()},
		{"showGhostToggleInDrawer", s._showGhostToggleInDrawer.current()},
		{"showStreamerToggleInDrawer", s._showStreamerToggleInDrawer.current()},
		{"showGhostToggleInTray", s._showGhostToggleInTray.current()},
		{"showStreamerToggleInTray", s._showStreamerToggleInTray.current()},
		{"monoFont", s._monoFont.current()},
		{"hideNotificationCounters", s._hideNotificationCounters.current()},
		{"hideNotificationBadge", s._hideNotificationBadge.current()},
		{"hideAllChatsFolder", s._hideAllChatsFolder.current()},
		{"channelBottomButton", s._channelBottomButton.current()},
		{"quickAdminShortcuts", s._quickAdminShortcuts.current()},
		{"showPeerId", s._showPeerId.current()},
		{"showMessageSeconds", s._showMessageSeconds.current()},
		{"showMessageShot", s._showMessageShot.current()},
		{"filterZalgo", s._filterZalgo.current()},
		{"stickerConfirmation", s._stickerConfirmation.current()},
		{"gifConfirmation", s._gifConfirmation.current()},
		{"voiceConfirmation", s._voiceConfirmation.current()},
		{"translationProvider", s._translationProvider.current()},
		{"aiTranslationEnabled", s._aiTranslationEnabled.current()},
		{"aiCompressionEnabled", s._aiCompressionEnabled.current()},
		{"deleteStubText", s._deleteStubText.current()},
		{"spyModeGloballyEnabled", s._spyModeGloballyEnabled.current()},
		{"spyRetentionDays", s._spyRetentionDays.current()},
		{"peerArchiveEnabled", s._peerArchiveEnabled.current()},
		{"archiveChatRefreshSeconds", s._archiveChatRefreshSeconds.current()},
		{"archiveKnownUserOnlineSeconds", s._archiveKnownUserOnlineSeconds.current()},
		{"archivePrivateOnlineSeconds", s._archivePrivateOnlineSeconds.current()},
		{"archivePrivateProfileSeconds", s._archivePrivateProfileSeconds.current()},
		{"archiveOtherProfileSeconds", s._archiveOtherProfileSeconds.current()},
		{"archiveHistoryScanLimit", s._archiveHistoryScanLimit.current()},
		{"archiveCrawlBatchSize", s._archiveCrawlBatchSize.current()},
		{"archiveMaxParticipants", s._archiveMaxParticipants.current()},
		{"archiveScanParticipants", s._archiveScanParticipants.current()},
		{"archiveScanBroadcasts", s._archiveScanBroadcasts.current()},
		{"archiveSaveUserpics", s._archiveSaveUserpics.current()},
		{"archiveFollowLinks", s._archiveFollowLinks.current()},
		{"archiveBackgroundCrawl", s._archiveBackgroundCrawl.current()},
		{"adaptiveCoverColor", s._adaptiveCoverColor.current()},
		{"improveLinkPreviews", s._improveLinkPreviews.current()},
		{"crashReporting", s._crashReporting.current()},
		{"avatarCorners", s._avatarCorners.current()},
		{"singleCornerRadius", s._singleCornerRadius.current()},
		{"messageShotSettings", s._messageShotSettings}
	};
}

namespace {

template <typename T>
[[nodiscard]] T ReadField(const json &j, const char *key, const T &fallback) {
	try {
		return j.value(key, fallback);
	} catch (const std::exception &e) {
		LOG(("TeleForgeSettings: bad field %1: %2")
			.arg(QString::fromUtf8(key), QString::fromUtf8(e.what())));
		return fallback;
	}
}

} // namespace

void from_json(const nlohmann::json &j, TfSettings &s) {
	TfSettings defaults;

	if (j.contains("ghostModeSettings") && j["ghostModeSettings"].is_object()) {
		s._ghostAccounts.clear();
		for (auto &[key, value] : j["ghostModeSettings"].items()) {
			auto account = std::make_unique<GhostModeAccountSettings>();
			try {
				const auto id = std::stoull(key);
				value.get_to(*account);
				s._ghostAccounts[id] = std::move(account);
			} catch (const std::exception &e) {
				LOG(("TeleForgeSettings: bad ghost account %1: %2")
					.arg(QString::fromStdString(key), QString::fromUtf8(e.what())));
			}
		}
	}

	s._useGlobalGhostMode = ReadField(j, "useGlobalGhostMode", defaults._useGlobalGhostMode.current());
	s._saveDeletedMessages = ReadField(j, "saveDeletedMessages", defaults._saveDeletedMessages.current());
	s._showDeletedInChat = ReadField(j, "showDeletedInChat", defaults._showDeletedInChat.current());
	s._ghostPassthroughMentions = ReadField(j, "ghostPassthroughMentions", defaults._ghostPassthroughMentions.current());
	// Kept exactly as stored: decrypting here would run the crypto layer while
	// the settings are still loading, before the working directory is known.
	s._syncArchivePassphrase = ReadField(
		j,
		"syncArchivePassphrase",
		defaults._syncArchivePassphrase.current());
	s._deletedRestoreLimit = ReadField(j, "deletedRestoreLimit", defaults._deletedRestoreLimit.current());
	s._saveMessagesHistory = ReadField(j, "saveMessagesHistory", defaults._saveMessagesHistory.current());
	s._saveForBots = ReadField(j, "saveForBots", defaults._saveForBots.current());
	s._shadowBanIds = ReadField(j, "shadowBanIds", defaults._shadowBanIds);
	s._unreadBadgeExcludedPeers = ReadField(j,
		"unreadBadgeExcluded",
		defaults._unreadBadgeExcludedPeers);
	s._filtersEnabled = ReadField(j, "filtersEnabled", defaults._filtersEnabled.current());
	s._filtersEnabledInChats = ReadField(j, "filtersEnabledInChats", defaults._filtersEnabledInChats.current());
	s._hideFromBlocked = ReadField(j, "hideFromBlocked", defaults._hideFromBlocked.current());
	s._semiTransparentDeletedMessages = ReadField(j, "semiTransparentDeletedMessages", defaults._semiTransparentDeletedMessages.current());
	s._disableAds = ReadField(j, "disableAds", defaults._disableAds.current());
	s._disableStories = ReadField(j, "disableStories", defaults._disableStories.current());
	s._disableCustomBackgrounds = ReadField(j, "disableCustomBackgrounds", defaults._disableCustomBackgrounds.current());
	s._showOnlyAddedEmojisAndStickers = ReadField(j, "showOnlyAddedEmojisAndStickers", defaults._showOnlyAddedEmojisAndStickers.current());
	s._collapseSimilarChannels = ReadField(j, "collapseSimilarChannels", defaults._collapseSimilarChannels.current());
	s._hideSimilarChannels = ReadField(j, "hideSimilarChannels", defaults._hideSimilarChannels.current());
	s._disableOpenLinkWarning = ReadField(j, "disableOpenLinkWarning", defaults._disableOpenLinkWarning.current());
	s._wideMultiplier = ReadField(j, "wideMultiplier", defaults._wideMultiplier.current());
	s._spoofWebviewAsAndroid = ReadField(j, "spoofWebviewAsAndroid", defaults._spoofWebviewAsAndroid.current());
	s._increaseWebviewHeight = ReadField(j, "increaseWebviewHeight", defaults._increaseWebviewHeight.current());
	s._increaseWebviewWidth = ReadField(j, "increaseWebviewWidth", defaults._increaseWebviewWidth.current());
	s._materialSwitches = ReadField(j, "materialSwitches", defaults._materialSwitches.current());
	s._removeMessageTail = ReadField(j, "removeMessageTail", defaults._removeMessageTail.current());
	s._disableNotificationsDelay = ReadField(j, "disableNotificationsDelay", defaults._disableNotificationsDelay.current());
	s._localPremium = ReadField(j, "localPremium", defaults._localPremium.current());
	s._showChannelReactions = ReadField(j, "showChannelReactions", defaults._showChannelReactions.current());
	s._showGroupReactions = ReadField(j, "showGroupReactions", defaults._showGroupReactions.current());
	s._showPrivateChatReactions = ReadField(j, "showPrivateChatReactions", defaults._showPrivateChatReactions.current());
	s._appIcon = TeleForgeAssets::DEFAULT_ICON;
	s._simpleQuotesAndReplies = ReadField(j, "simpleQuotesAndReplies", defaults._simpleQuotesAndReplies.current());
	s._hideFastShare = ReadField(j, "hideFastShare", defaults._hideFastShare.current());
	s._replaceBottomInfoWithIcons = ReadField(j, "replaceBottomInfoWithIcons", defaults._replaceBottomInfoWithIcons.current());
	s._deletedMark = ReadField(j, "deletedMark", defaults._deletedMark.current());
	s._editedMark = ReadField(j, "editedMark", defaults._editedMark.current());
	s._recentStickersCount = ReadField(j, "recentStickersCount", defaults._recentStickersCount.current());
	s._showReactionsPanelInContextMenu = ReadField(j, "showReactionsPanelInContextMenu", defaults._showReactionsPanelInContextMenu.current());
	s._showViewsPanelInContextMenu = ReadField(j, "showViewsPanelInContextMenu", defaults._showViewsPanelInContextMenu.current());
	s._showHideMessageInContextMenu = ReadField(j, "showHideMessageInContextMenu", defaults._showHideMessageInContextMenu.current());
	s._showUserMessagesInContextMenu = ReadField(j, "showUserMessagesInContextMenu", defaults._showUserMessagesInContextMenu.current());
	s._showMessageDetailsInContextMenu = ReadField(j, "showMessageDetailsInContextMenu", defaults._showMessageDetailsInContextMenu.current());
	s._showRepeatMessageInContextMenu = ReadField(j, "showRepeatMessageInContextMenu", defaults._showRepeatMessageInContextMenu.current());
	s._showAddFilterInContextMenu = ReadField(j, "showAddFilterInContextMenu", defaults._showAddFilterInContextMenu.current());
	s._showAttachButtonInMessageField = ReadField(j, "showAttachButtonInMessageField", defaults._showAttachButtonInMessageField.current());
	s._showCommandsButtonInMessageField = ReadField(j, "showCommandsButtonInMessageField", defaults._showCommandsButtonInMessageField.current());
	s._showEmojiButtonInMessageField = ReadField(j, "showEmojiButtonInMessageField", defaults._showEmojiButtonInMessageField.current());
	s._showMicrophoneButtonInMessageField = ReadField(j, "showMicrophoneButtonInMessageField", defaults._showMicrophoneButtonInMessageField.current());
	s._showAutoDeleteButtonInMessageField = ReadField(j, "showAutoDeleteButtonInMessageField", defaults._showAutoDeleteButtonInMessageField.current());
	s._showAttachPopup = ReadField(j, "showAttachPopup", defaults._showAttachPopup.current());
	s._showEmojiPopup = ReadField(j, "showEmojiPopup", defaults._showEmojiPopup.current());
	s._showMyProfileInDrawer = ReadField(j, "showMyProfileInDrawer", defaults._showMyProfileInDrawer.current());
	s._showBotsInDrawer = ReadField(j, "showBotsInDrawer", defaults._showBotsInDrawer.current());
	s._showNewGroupInDrawer = ReadField(j, "showNewGroupInDrawer", defaults._showNewGroupInDrawer.current());
	s._showNewChannelInDrawer = ReadField(j, "showNewChannelInDrawer", defaults._showNewChannelInDrawer.current());
	s._showContactsInDrawer = ReadField(j, "showContactsInDrawer", defaults._showContactsInDrawer.current());
	s._showCallsInDrawer = ReadField(j, "showCallsInDrawer", defaults._showCallsInDrawer.current());
	s._showSavedMessagesInDrawer = ReadField(j, "showSavedMessagesInDrawer", defaults._showSavedMessagesInDrawer.current());
	s._showLReadToggleInDrawer = ReadField(j, "showLReadToggleInDrawer", defaults._showLReadToggleInDrawer.current());
	s._showSReadToggleInDrawer = ReadField(j, "showSReadToggleInDrawer", defaults._showSReadToggleInDrawer.current());
	s._showNightModeToggleInDrawer = ReadField(j, "showNightModeToggleInDrawer", defaults._showNightModeToggleInDrawer.current());
	s._showGhostToggleInDrawer = ReadField(j, "showGhostToggleInDrawer", defaults._showGhostToggleInDrawer.current());
	s._showStreamerToggleInDrawer = ReadField(j, "showStreamerToggleInDrawer", defaults._showStreamerToggleInDrawer.current());
	s._showGhostToggleInTray = ReadField(j, "showGhostToggleInTray", defaults._showGhostToggleInTray.current());
	s._showStreamerToggleInTray = ReadField(j, "showStreamerToggleInTray", defaults._showStreamerToggleInTray.current());
	s._monoFont = ReadField(j, "monoFont", defaults._monoFont.current());
	s._hideNotificationCounters = ReadField(j, "hideNotificationCounters", defaults._hideNotificationCounters.current());
	s._hideNotificationBadge = ReadField(j, "hideNotificationBadge", defaults._hideNotificationBadge.current());
	s._hideAllChatsFolder = ReadField(j, "hideAllChatsFolder", defaults._hideAllChatsFolder.current());
	s._channelBottomButton = ReadField(j, "channelBottomButton", defaults._channelBottomButton.current());
	s._quickAdminShortcuts = ReadField(j, "quickAdminShortcuts", defaults._quickAdminShortcuts.current());
	s._showPeerId = ReadField(j, "showPeerId", defaults._showPeerId.current());
	s._showMessageSeconds = ReadField(j, "showMessageSeconds", defaults._showMessageSeconds.current());
	s._showMessageShot = ReadField(j, "showMessageShot", defaults._showMessageShot.current());
	s._filterZalgo = ReadField(j, "filterZalgo", defaults._filterZalgo.current());
	s._stickerConfirmation = ReadField(j, "stickerConfirmation", defaults._stickerConfirmation.current());
	s._gifConfirmation = ReadField(j, "gifConfirmation", defaults._gifConfirmation.current());
	s._voiceConfirmation = ReadField(j, "voiceConfirmation", defaults._voiceConfirmation.current());
	s._translationProvider = ReadField(j, "translationProvider", defaults._translationProvider.current());
	s._aiTranslationEnabled = ReadField(j, "aiTranslationEnabled", defaults._aiTranslationEnabled.current());
	s._aiCompressionEnabled = ReadField(j, "aiCompressionEnabled", defaults._aiCompressionEnabled.current());
	s._deleteStubText = ReadField(j, "deleteStubText", defaults._deleteStubText.current());
	s._spyModeGloballyEnabled = ReadField(j, "spyModeGloballyEnabled", defaults._spyModeGloballyEnabled.current());
	s._spyRetentionDays = ReadField(j, "spyRetentionDays", defaults._spyRetentionDays.current());
	s._peerArchiveEnabled = ReadField(j, "peerArchiveEnabled", defaults._peerArchiveEnabled.current());
	s._archiveChatRefreshSeconds = ReadField(j,
		"archiveChatRefreshSeconds",
		defaults._archiveChatRefreshSeconds.current());
	s._archiveKnownUserOnlineSeconds = ReadField(j,
		"archiveKnownUserOnlineSeconds",
		defaults._archiveKnownUserOnlineSeconds.current());
	s._archivePrivateOnlineSeconds = ReadField(j,
		"archivePrivateOnlineSeconds",
		defaults._archivePrivateOnlineSeconds.current());
	s._archivePrivateProfileSeconds = ReadField(j,
		"archivePrivateProfileSeconds",
		defaults._archivePrivateProfileSeconds.current());
	s._archiveOtherProfileSeconds = ReadField(j,
		"archiveOtherProfileSeconds",

		defaults._archiveOtherProfileSeconds.current());

	// Slider positions used to be stored where seconds belong, which left every
	// polling interval at its minimum. Anything outside the documented range now
	// falls back to the default instead of sticking.
	const auto repairInterval = [](rpl::variable<int> &value, int low, int high, int fallback) {
		if (value.current() < low || value.current() > high) {
			value = fallback;
		}
	};
	repairInterval(s._archiveChatRefreshSeconds, 60, 3600, defaults._archiveChatRefreshSeconds.current());
	repairInterval(s._archiveKnownUserOnlineSeconds, 15, 600, defaults._archiveKnownUserOnlineSeconds.current());
	repairInterval(s._archivePrivateOnlineSeconds, 5, 120, defaults._archivePrivateOnlineSeconds.current());
	repairInterval(s._archivePrivateProfileSeconds, 60, 7200, defaults._archivePrivateProfileSeconds.current());
	repairInterval(s._archiveOtherProfileSeconds, 300, 21600, defaults._archiveOtherProfileSeconds.current());
	s._archiveHistoryScanLimit = ReadField(j,
		"archiveHistoryScanLimit",
		defaults._archiveHistoryScanLimit.current());
	s._archiveCrawlBatchSize = ReadField(j,
		"archiveCrawlBatchSize",
		defaults._archiveCrawlBatchSize.current());
	s._archiveMaxParticipants = ReadField(j,
		"archiveMaxParticipants",
		defaults._archiveMaxParticipants.current());
	s._archiveScanParticipants = ReadField(j,
		"archiveScanParticipants",
		defaults._archiveScanParticipants.current());
	s._archiveScanBroadcasts = ReadField(j,
		"archiveScanBroadcasts",
		defaults._archiveScanBroadcasts.current());
	s._archiveSaveUserpics = ReadField(j,
		"archiveSaveUserpics",
		defaults._archiveSaveUserpics.current());
	s._archiveFollowLinks = ReadField(j,
		"archiveFollowLinks",
		defaults._archiveFollowLinks.current());
	s._archiveBackgroundCrawl = ReadField(j,
		"archiveBackgroundCrawl",
		defaults._archiveBackgroundCrawl.current());
	s._adaptiveCoverColor = ReadField(j, "adaptiveCoverColor", defaults._adaptiveCoverColor.current());
	s._improveLinkPreviews = ReadField(j, "improveLinkPreviews", defaults._improveLinkPreviews.current());
	s._crashReporting = ReadField(j, "crashReporting", defaults._crashReporting.current());
	s._avatarCorners = ReadField(j, "avatarCorners", defaults._avatarCorners.current());
	s._singleCornerRadius = ReadField(j, "singleCornerRadius", defaults._singleCornerRadius.current());

	if (j.contains("messageShotSettings") && j["messageShotSettings"].is_object()) {
		try {
			j["messageShotSettings"].get_to(s._messageShotSettings);
		} catch (const std::exception &e) {
			LOG(("TeleForgeSettings: bad message shot settings: %1")
				.arg(QString::fromUtf8(e.what())));
		}
	}
}
