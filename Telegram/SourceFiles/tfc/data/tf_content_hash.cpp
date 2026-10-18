#include "tfc/data/tf_content_hash.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QByteArray>

#include <array>

namespace TfContentHash {
namespace {

constexpr auto kSeparator = char('|');

void AppendNumber(QByteArray &out, long long value) {
	auto magnitude = static_cast<unsigned long long>(value);
	if (value < 0) {
		out.append('-');
		magnitude = 0 - magnitude;
	}
	auto digits = std::array<char, 20>();
	auto count = 0;
	do {
		digits[count++] = char('0' + (magnitude % 10));
		magnitude /= 10;
	} while (magnitude);
	while (count > 0) {
		out.append(digits[--count]);
	}
}

[[nodiscard]] std::string ComputeKey(
		long long accountId,
		long long dialogId,
		int messageId,
		long long authorId,
		const std::string &text,
		int revisionDate) {
	auto payload = QByteArray();
	AppendNumber(payload, accountId);
	payload.append(kSeparator);
	AppendNumber(payload, dialogId);
	payload.append(kSeparator);
	AppendNumber(payload, messageId);
	payload.append(kSeparator);
	AppendNumber(payload, authorId);
	payload.append(kSeparator);
	payload.append(text.data(), int(text.size()));
	payload.append(kSeparator);
	AppendNumber(payload, revisionDate);

	return QCryptographicHash::hash(
		payload,
		QCryptographicHash::Sha256
	).toHex().toStdString();
}

} // namespace

std::string ComputeEdited(const EditedMessage &message) {
	return ComputeKey(
		message.userId,
		message.dialogId,
		message.messageId,
		message.fromId,
		message.text,
		message.editDate);
}

std::string ComputeDeleted(const DeletedMessage &message) {
	return ComputeKey(
		message.userId,
		message.dialogId,
		message.messageId,
		message.fromId,
		message.text,
		message.date);
}

} // namespace TfContentHash
