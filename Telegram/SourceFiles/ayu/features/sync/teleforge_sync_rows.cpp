// Copyright @Radolyn, 2026
#include "ayu/features/sync/teleforge_sync_rows.h"

#include "tfc/data/entities.h"
#include "ayu/features/sync/teleforge_sync_crypto.h"
#include "ayu/features/teleforge/teleforge_unified_db.h"

#include "logs.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>

namespace TeleForge::Sync {
namespace {

[[nodiscard]] QString Str(const std::string &v) {
	return QString::fromStdString(v);
}

[[nodiscard]] QString B64(const std::vector<char> &v) {
	return QString::fromLatin1(
		QByteArray(v.data(), int(v.size())).toBase64());
}

[[nodiscard]] std::string StdStr(const QJsonValue &v) {
	return v.toString().toStdString();
}

[[nodiscard]] bool JsonId(const QJsonValue &v, ID *out) {
	if (v.isDouble()) {
		*out = qint64(v.toDouble());
		return true;
	}
	if (v.isString()) {
		auto ok = false;
		*out = v.toString().toLongLong(&ok);
		return ok;
	}
	return false;
}

[[nodiscard]] bool JsonInt(const QJsonValue &v, int *out) {
	if (v.isDouble()) {
		*out = v.toInt();
		return true;
	}
	if (v.isString()) {
		auto ok = false;
		*out = v.toString().toInt(&ok);
		return ok;
	}
	return false;
}

[[nodiscard]] std::vector<char> Vec(const QJsonValue &v) {
	const auto b = QByteArray::fromBase64(v.toString().toLatin1());
	return std::vector<char>(b.constData(), b.constData() + b.size());
}

constexpr auto kCompressedPayloadMagic = "TFZ1";
constexpr auto kPayloadCompressionLevel = 6;

// The row payload is JSON with ids and base64 blobs, so it compresses several
// times over even with a small window. The marker keeps rows written by older
// builds readable, since only this function's output is ever unpacked.
[[nodiscard]] QByteArray PackPayload(const QByteArray &plain) {
	const auto compressed = qCompress(plain, kPayloadCompressionLevel);
	if (compressed.size() + 4 >= plain.size()) {
		return plain;
	}
	return QByteArray(kCompressedPayloadMagic, 4) + compressed;
}

[[nodiscard]] QByteArray UnpackPayload(const QByteArray &stored) {
	const auto packed = stored.size() > 4
		&& !std::memcmp(stored.constData(), kCompressedPayloadMagic, 4);
	return packed ? qUncompress(stored.mid(4)) : stored;
}

// Full, self-describing JSON serialization of a captured message. 64-bit ids
// are stored as strings so JSON's double numbers never truncate them; binary
// blobs are base64. The result is encrypted before it leaves the device.
[[nodiscard]] QByteArray SerializeMessage(const AyuMessageBase &m) {
	auto o = QJsonObject{
		{ u"userId"_q, QString::number(m.userId) },
		{ u"dialogId"_q, QString::number(m.dialogId) },
		{ u"groupedId"_q, QString::number(m.groupedId) },
		{ u"peerId"_q, QString::number(m.peerId) },
		{ u"fromId"_q, QString::number(m.fromId) },
		{ u"topicId"_q, QString::number(m.topicId) },
		{ u"messageId"_q, m.messageId },
		{ u"date"_q, m.date },
		{ u"flags"_q, m.flags },
		{ u"editDate"_q, m.editDate },
		{ u"views"_q, m.views },
		{ u"fwdFlags"_q, m.fwdFlags },
		{ u"fwdFromId"_q, QString::number(m.fwdFromId) },
		{ u"fwdName"_q, Str(m.fwdName) },
		{ u"fwdDate"_q, m.fwdDate },
		{ u"fwdPostAuthor"_q, Str(m.fwdPostAuthor) },
		{ u"postAuthor"_q, Str(m.postAuthor) },
		{ u"replyFlags"_q, m.replyFlags },
		{ u"replyMessageId"_q, m.replyMessageId },
		{ u"replyPeerId"_q, QString::number(m.replyPeerId) },
		{ u"replyTopId"_q, m.replyTopId },
		{ u"replyForumTopic"_q, m.replyForumTopic },
		{ u"replySerialized"_q, B64(m.replySerialized) },
		{ u"replyMarkupSerialized"_q, B64(m.replyMarkupSerialized) },
		{ u"entityCreateDate"_q, m.entityCreateDate },
		{ u"text"_q, Str(m.text) },
		{ u"textEntities"_q, B64(m.textEntities) },
		{ u"mediaPath"_q, Str(m.mediaPath) },
		{ u"hqThumbPath"_q, Str(m.hqThumbPath) },
		{ u"documentType"_q, m.documentType },
		{ u"documentSerialized"_q, B64(m.documentSerialized) },
		{ u"thumbsSerialized"_q, B64(m.thumbsSerialized) },
		{ u"documentAttributesSerialized"_q, B64(m.documentAttributesSerialized) },
		{ u"mimeType"_q, Str(m.mimeType) },
		{ u"contentHash"_q, Str(m.contentHash) },
	};
	return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

[[nodiscard]] std::optional<AyuMessageBase> DeserializeMessage(
		const QByteArray &json) {
	const auto o = QJsonDocument::fromJson(json).object();
	if (o.isEmpty()) {
		return std::nullopt;
	}
	auto m = AyuMessageBase();
	m.fakeId = 0;
	auto ok = true;
	ok = JsonId(o.value(u"userId"_q), &m.userId) && ok;
	ok = JsonId(o.value(u"dialogId"_q), &m.dialogId) && ok;
	ok = JsonId(o.value(u"groupedId"_q), &m.groupedId) && ok;
	ok = JsonId(o.value(u"peerId"_q), &m.peerId) && ok;
	ok = JsonId(o.value(u"fromId"_q), &m.fromId) && ok;
	ok = JsonId(o.value(u"topicId"_q), &m.topicId) && ok;
	ok = JsonInt(o.value(u"messageId"_q), &m.messageId) && ok;
	ok = JsonInt(o.value(u"date"_q), &m.date) && ok;
	ok = JsonInt(o.value(u"flags"_q), &m.flags) && ok;
	ok = JsonInt(o.value(u"editDate"_q), &m.editDate) && ok;
	ok = JsonInt(o.value(u"views"_q), &m.views) && ok;
	ok = JsonInt(o.value(u"fwdFlags"_q), &m.fwdFlags) && ok;
	ok = JsonId(o.value(u"fwdFromId"_q), &m.fwdFromId) && ok;
	m.fwdName = StdStr(o.value(u"fwdName"_q));
	ok = JsonInt(o.value(u"fwdDate"_q), &m.fwdDate) && ok;
	m.fwdPostAuthor = StdStr(o.value(u"fwdPostAuthor"_q));
	m.postAuthor = StdStr(o.value(u"postAuthor"_q));
	ok = JsonInt(o.value(u"replyFlags"_q), &m.replyFlags) && ok;
	ok = JsonInt(o.value(u"replyMessageId"_q), &m.replyMessageId) && ok;
	ok = JsonId(o.value(u"replyPeerId"_q), &m.replyPeerId) && ok;
	ok = JsonInt(o.value(u"replyTopId"_q), &m.replyTopId) && ok;
	m.replyForumTopic = o.value(u"replyForumTopic"_q).toBool();
	m.replySerialized = Vec(o.value(u"replySerialized"_q));
	m.replyMarkupSerialized = Vec(o.value(u"replyMarkupSerialized"_q));
	ok = JsonInt(o.value(u"entityCreateDate"_q), &m.entityCreateDate) && ok;
	m.text = StdStr(o.value(u"text"_q));
	m.textEntities = Vec(o.value(u"textEntities"_q));
	m.mediaPath = StdStr(o.value(u"mediaPath"_q));
	m.hqThumbPath = StdStr(o.value(u"hqThumbPath"_q));
	ok = JsonInt(o.value(u"documentType"_q), &m.documentType) && ok;
	m.documentSerialized = Vec(o.value(u"documentSerialized"_q));
	m.thumbsSerialized = Vec(o.value(u"thumbsSerialized"_q));
	m.documentAttributesSerialized = Vec(o.value(u"documentAttributesSerialized"_q));
	m.mimeType = StdStr(o.value(u"mimeType"_q));
	m.contentHash = StdStr(o.value(u"contentHash"_q));
	if (!ok) {
		LOG(("TeleForge Sync: rejecting message row with mismatched field types"));
		return std::nullopt;
	}
	return m;
}

[[nodiscard]] SyncMessageRow BuildRow(
		const AyuMessageBase &m,
		bool deleted,
		bool edited,
		const QByteArray &key) {
	auto row = SyncMessageRow{};
	row.contentHash = Str(m.contentHash);
	row.chatIdH = HmacId(m.peerId, key);
	row.accountIdH = HmacId(m.fromId, key);
	row.messageIdH = HmacId(m.messageId, key);
	row.deleted = deleted;
	row.edited = edited;
	row.date = m.date;
	row.editDate = m.editDate;
	row.payload = EncryptBundle(PackPayload(SerializeMessage(m)), key);
	return row;
}

} // namespace

std::vector<SyncMessageRow> CollectMessageRows(const QByteArray &key) {
	auto rows = std::vector<SyncMessageRow>();
	try {
		for (const auto &m : Storage::Db().get_all<DeletedMessage>()) {
			if (m.contentHash.empty()) {
				continue;
			}
			rows.push_back(BuildRow(m, true, false, key));
		}
		for (const auto &m : Storage::Db().get_all<EditedMessage>()) {
			if (m.contentHash.empty()) {
				continue;
			}
			rows.push_back(BuildRow(m, false, true, key));
		}
	} catch (const std::exception &e) {
		LOG(("TeleForge Sync: CollectMessageRows failed: %1").arg(e.what()));
	}
	return rows;
}

int ApplyMessageRows(
		const std::vector<SyncMessageRow> &rows,
		const QByteArray &key) {
	auto inserted = 0;
	try {
		auto seenDeleted = std::unordered_set<std::string>();
		auto seenEdited = std::unordered_set<std::string>();
		for (const auto &m : Storage::Db().get_all<DeletedMessage>()) {
			seenDeleted.insert(m.contentHash);
		}
		for (const auto &m : Storage::Db().get_all<EditedMessage>()) {
			seenEdited.insert(m.contentHash);
		}
		for (const auto &row : rows) {
			if (row.payload.isEmpty()) {
				continue;
			}
			const auto decrypted = DecryptBundle(row.payload, key);
			if (!decrypted) {
				continue;
			}
			const auto plain = UnpackPayload(*decrypted);
			if (plain.isEmpty()) {
				continue;
			}
			const auto parsed = DeserializeMessage(plain);
			if (!parsed || parsed->contentHash.empty()) {
				continue;
			}
			const auto &m = *parsed;
			if (row.deleted && seenDeleted.find(m.contentHash) == seenDeleted.end()) {
				auto stored = DeletedMessage();
				static_cast<AyuMessageBase&>(stored) = m;
				Storage::Db().insert(stored);
				seenDeleted.insert(m.contentHash);
				++inserted;
			} else if (row.edited && seenEdited.find(m.contentHash) == seenEdited.end()) {
				auto stored = EditedMessage();
				static_cast<AyuMessageBase&>(stored) = m;
				Storage::Db().insert(stored);
				seenEdited.insert(m.contentHash);
				++inserted;
			}
		}
	} catch (const std::exception &e) {
		LOG(("TeleForge Sync: ApplyMessageRows failed: %1").arg(e.what()));
	}
	return inserted;
}

} // namespace TeleForge::Sync
