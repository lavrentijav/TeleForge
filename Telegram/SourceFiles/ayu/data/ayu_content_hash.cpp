#include "ayu/data/ayu_content_hash.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QString>

namespace AyuContentHash {
namespace {

[[nodiscard]] std::string Sha256Hex(const QByteArray &data) {
	return QCryptographicHash::hash(data, QCryptographicHash::Sha256)
		.toHex()
		.toStdString();
}

[[nodiscard]] QByteArray PackFields(
		long long userId,
		long long dialogId,
		int messageId,
		long long fromId,
		const std::string &text,
		int revisionDate) {
	auto payload = QByteArray();
	payload.append(QByteArray::number(userId));
	payload.append('|');
	payload.append(QByteArray::number(dialogId));
	payload.append('|');
	payload.append(QByteArray::number(messageId));
	payload.append('|');
	payload.append(QByteArray::number(fromId));
	payload.append('|');
	payload.append(text.c_str(), int(text.size()));
	payload.append('|');
	payload.append(QByteArray::number(revisionDate));
	return payload;
}

} // namespace

std::string ComputeEdited(const EditedMessage &message) {
	return Sha256Hex(PackFields(
		message.userId,
		message.dialogId,
		message.messageId,
		message.fromId,
		message.text,
		message.editDate));
}

std::string ComputeDeleted(const DeletedMessage &message) {
	return Sha256Hex(PackFields(
		message.userId,
		message.dialogId,
		message.messageId,
		message.fromId,
		message.text,
		message.date));
}

} // namespace AyuContentHash
