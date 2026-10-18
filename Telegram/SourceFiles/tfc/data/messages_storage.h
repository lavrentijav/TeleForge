// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "tfc/data/entities.h"

namespace TfMessages {

void addEditedMessage(not_null<HistoryItem *> item);
void snapshotEditsBeforeDelete(not_null<HistoryItem*> item);
std::vector<TfMessageBase> getEditedMessages(not_null<HistoryItem*> item, TfId minId, TfId maxId, int totalLimit);
bool hasRevisions(not_null<HistoryItem*> item);

void addDeletedMessage(not_null<HistoryItem*> item);
std::vector<TfMessageBase> getDeletedMessages(not_null<PeerData*> peer, TfId topicId, TfId minId, TfId maxId, int totalLimit, const QString &searchQuery = QString());
bool hasDeletedMessages(not_null<PeerData*> peer, TfId topicId);
std::vector<TfMessageBase> buildDeletedMessageTimeline(not_null<PeerData*> peer, const TfMessageBase &deletedMessage, int totalLimit = 128);

}
