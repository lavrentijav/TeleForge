// Copyright @Radolyn, 2026
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <vector>

namespace TeleForge::Sync {

// One replicated message (a locally captured deleted or edited message). Ids
// are blinded with HMAC so the remote DB can index/match without learning them;
// `payload` is the AES-GCM encrypted, JSON-serialized full message record.
struct SyncMessageRow {
	QString contentHash;   // stable cross-device merge key (already a hash)
	QString chatIdH;       // HMAC(peerId)
	QString accountIdH;    // HMAC(fromId — the sender)
	QString messageIdH;    // HMAC(messageId)
	bool deleted = false;  // came from the DeletedMessage table
	bool edited = false;   // came from the EditedMessage table
	long long date = 0;
	long long editDate = 0;
	QByteArray payload;
};

// Reads the local DeletedMessage + EditedMessage tables and builds encrypted,
// id-blinded rows. MUST run on the main thread (touches the SQLite unified DB).
[[nodiscard]] std::vector<SyncMessageRow> CollectMessageRows(
	const QByteArray &key);

// Decrypts rows and merges them into the local unified DB (idempotent, keyed on
// contentHash — never overwrites, only inserts missing). Returns how many new
// rows were inserted. MUST run on the main thread. The remote DB is only a
// replica: a failure here never affects data already stored locally.
int ApplyMessageRows(
	const std::vector<SyncMessageRow> &rows,
	const QByteArray &key);

} // namespace TeleForge::Sync
