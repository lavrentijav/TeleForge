// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "tfc/data/entities.h"

#include <functional>

class TfDataSchemaVersion
{
public:
	int id;
	int version;
};

namespace TfDatabase {

void initialize();

void addEditedMessage(const EditedMessage &message);
std::vector<EditedMessage> getEditedMessages(TfId userId, TfId dialogId, TfId messageId, TfId minId, TfId maxId, int totalLimit);
bool hasRevisions(TfId userId, TfId dialogId, TfId messageId);

void addDeletedMessage(const DeletedMessage &message);
std::vector<DeletedMessage> getDeletedMessages(TfId userId, TfId dialogId, TfId topicId, TfId minId, TfId maxId, int totalLimit, const std::string &searchQuery = "");
bool hasDeletedMessages(TfId userId, TfId dialogId, TfId topicId);

std::vector<RegexFilter> getAllRegexFilters();
RegexFilter getById(std::vector<char> id);
std::vector<RegexFilter> getShared();
std::vector<RegexFilter> getByDialogId(TfId dialogId);
std::vector<RegexFilterGlobalExclusion> getAllFiltersExclusions();
std::vector<RegexFilter> getExcludedByDialogId(TfId dialogId);

int getCount();


void addRegexFilter(const RegexFilter &filter);
void addRegexExclusion(const RegexFilterGlobalExclusion &exclusion);

void updateRegexFilter(const RegexFilter &filter);

void deleteFilter(const std::vector<char> &id);
void deleteExclusionsByFilterId(const std::vector<char> &id);
void deleteExclusion(TfId dialogId, std::vector<char> filterId);

void deleteAllFilters();
void deleteAllExclusions();

bool hasFilters();
bool hasPerDialogFilters();


void insertOnlineEvent(const OnlineEvent &event);
[[nodiscard]] std::vector<OnlineEvent> loadOnlineEventsForUser(TfId userId, int sinceTs);
[[nodiscard]] std::optional<int> manualLastSeenForUser(TfId userId);
void upsertSpyTarget(TfId userId, bool enabled);
[[nodiscard]] bool hasSpyTargetOverride(TfId userId);
[[nodiscard]] bool isSpyTargetEnabled(TfId userId);
void purgeOnlineEventsBefore(int timestamp);

void insertGhostReadBatch(const std::vector<GhostReadMessage> &rows);
[[nodiscard]] bool hasGhostRead(TfId accountId);
[[nodiscard]] std::vector<GhostReadMessage> loadGhostReadForAccount(TfId accountId);
[[nodiscard]] std::vector<GhostReadMessage> loadGhostReadForPeer(TfId accountId, TfId peerId);
void eraseGhostReadForPeer(TfId accountId, TfId peerId);
void eraseGhostReadUpTo(
	TfId accountId,
	TfId peerId,
	int msgId,
	TfId topicId = 0);
void eraseGhostReadForAccount(TfId accountId);
void purgeGhostReadBefore(int timestamp);

}
