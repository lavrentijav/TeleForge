// Copyright @Radolyn, 2026
#include "ayu/features/teleforge/teleforge_tools.h"

#include "ayu/features/teleforge/teleforge_memory.h"

#include <QtCore/QDateTime>

namespace TeleForge::Tools {
namespace {

[[nodiscard]] QJsonObject Function(
		const QString &name,
		const QString &description,
		const QJsonObject &parameters) {
	return QJsonObject{
		{ QStringLiteral("type"), QStringLiteral("function") },
		{ QStringLiteral("function"), QJsonObject{
			{ QStringLiteral("name"), name },
			{ QStringLiteral("description"), description },
			{ QStringLiteral("parameters"), parameters },
		} },
	};
}

[[nodiscard]] QJsonObject NoParams() {
	return QJsonObject{
		{ QStringLiteral("type"), QStringLiteral("object") },
		{ QStringLiteral("properties"), QJsonObject{} },
	};
}

[[nodiscard]] QString RecallMemory(const QString &query, long long peerId) {
	auto settings = LoadMemorySettings(peerId);
	auto request = RetrievalRequest{
		.chatId = peerId,
		.queryText = query,
		.uniqueUserIds = {},
		.settings = settings,
	};
	const auto context = BuildMemoryContext(request);
	auto blocks = QStringList();
	for (const auto &block : {
			context.globalBlock,
			context.chatBlock,
			context.appendixBlock }) {
		if (!block.trimmed().isEmpty()) {
			blocks.push_back(block.trimmed());
		}
	}
	if (blocks.isEmpty()) {
		return QStringLiteral("No additional relevant facts were found in memory.");
	}
	return blocks.join(QStringLiteral("\n\n"));
}

} // namespace

QJsonArray BuiltinToolsJson() {
	auto tools = QJsonArray();

	tools.push_back(Function(
		QStringLiteral("get_current_time"),
		QStringLiteral(
			"Returns the current local date and time. Call this whenever the "
			"reply depends on the current time, day of week or date."),
		NoParams()));

	tools.push_back(Function(
		QStringLiteral("recall_memory"),
		QStringLiteral(
			"Retrieves additional relevant facts from the long-term memory "
			"database for a natural-language query. Use it when you need more "
			"context about the user, the chat or a topic than is already in "
			"the conversation."),
		QJsonObject{
			{ QStringLiteral("type"), QStringLiteral("object") },
			{ QStringLiteral("properties"), QJsonObject{
				{ QStringLiteral("query"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("string") },
					{ QStringLiteral("description"),
						QStringLiteral("What to look up in memory.") },
				} },
			} },
			{ QStringLiteral("required"), QJsonArray{
				QStringLiteral("query") } },
		}));

	tools.push_back(Function(
		QStringLiteral("send_ladder"),
		QStringLiteral(
			"Sends the reply as several separate consecutive messages (a "
			"\"ladder\") instead of one. Provide the messages in order; each "
			"array item becomes its own message."),
		QJsonObject{
			{ QStringLiteral("type"), QStringLiteral("object") },
			{ QStringLiteral("properties"), QJsonObject{
				{ QStringLiteral("messages"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("array") },
					{ QStringLiteral("items"), QJsonObject{
						{ QStringLiteral("type"), QStringLiteral("string") } } },
					{ QStringLiteral("description"),
						QStringLiteral("Ordered messages to send one by one.") },
				} },
			} },
			{ QStringLiteral("required"), QJsonArray{
				QStringLiteral("messages") } },
		}));

	tools.push_back(Function(
		QStringLiteral("no_reply"),
		QStringLiteral(
			"Explicitly decide NOT to answer. Call this when the best action "
			"is to stay silent (nothing to add, not addressed to the owner, "
			"spam, etc.)."),
		NoParams()));

	tools.push_back(Function(
		QStringLiteral("reply_to"),
		QStringLiteral(
			"Sends a message as a reply to a specific earlier message, "
			"identified by its numeric message id. Use when the answer is "
			"directed at one particular message rather than the whole chat."),
		QJsonObject{
			{ QStringLiteral("type"), QStringLiteral("object") },
			{ QStringLiteral("properties"), QJsonObject{
				{ QStringLiteral("message_id"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("integer") },
					{ QStringLiteral("description"),
						QStringLiteral("Id of the message to reply to.") },
				} },
				{ QStringLiteral("text"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("string") },
					{ QStringLiteral("description"),
						QStringLiteral("The reply text.") },
				} },
			} },
			{ QStringLiteral("required"), QJsonArray{
				QStringLiteral("message_id"),
				QStringLiteral("text") } },
		}));

	tools.push_back(Function(
		QStringLiteral("typing"),
		QStringLiteral(
			"Shows the \"typing...\" indicator in the chat for a few seconds. "
			"Call it before a reply to look more natural."),
		NoParams()));

	tools.push_back(Function(
		QStringLiteral("mark_read"),
		QStringLiteral(
			"Marks the current chat as read (all incoming messages seen)."),
		NoParams()));

	tools.push_back(Function(
		QStringLiteral("set_reaction"),
		QStringLiteral(
			"Adds an emoji reaction to a specific message by its numeric id. "
			"Use a single standard emoji the chat allows."),
		QJsonObject{
			{ QStringLiteral("type"), QStringLiteral("object") },
			{ QStringLiteral("properties"), QJsonObject{
				{ QStringLiteral("message_id"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("integer") },
					{ QStringLiteral("description"),
						QStringLiteral("Id of the message to react to.") },
				} },
				{ QStringLiteral("emoji"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("string") },
					{ QStringLiteral("description"),
						QStringLiteral("A single emoji, e.g. \xF0\x9F\x91\x8D.") },
				} },
			} },
			{ QStringLiteral("required"), QJsonArray{
				QStringLiteral("message_id"),
				QStringLiteral("emoji") } },
		}));

	tools.push_back(Function(
		QStringLiteral("edit_message"),
		QStringLiteral(
			"Edits the text of one of the account owner's own messages, "
			"identified by its numeric id."),
		QJsonObject{
			{ QStringLiteral("type"), QStringLiteral("object") },
			{ QStringLiteral("properties"), QJsonObject{
				{ QStringLiteral("message_id"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("integer") },
					{ QStringLiteral("description"),
						QStringLiteral("Id of your message to edit.") },
				} },
				{ QStringLiteral("text"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("string") },
					{ QStringLiteral("description"),
						QStringLiteral("New message text.") },
				} },
			} },
			{ QStringLiteral("required"), QJsonArray{
				QStringLiteral("message_id"),
				QStringLiteral("text") } },
		}));

	tools.push_back(Function(
		QStringLiteral("delete_message"),
		QStringLiteral(
			"Deletes one of the account owner's own messages for everyone, "
			"identified by its numeric id."),
		QJsonObject{
			{ QStringLiteral("type"), QStringLiteral("object") },
			{ QStringLiteral("properties"), QJsonObject{
				{ QStringLiteral("message_id"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("integer") },
					{ QStringLiteral("description"),
						QStringLiteral("Id of your message to delete.") },
				} },
			} },
			{ QStringLiteral("required"), QJsonArray{
				QStringLiteral("message_id") } },
		}));

	tools.push_back(Function(
		QStringLiteral("send_sticker"),
		QStringLiteral(
			"Sends a sticker matching the given emoji, chosen from the "
			"account owner's recent and favorite stickers. Fails if none of "
			"them matches that emoji."),
		QJsonObject{
			{ QStringLiteral("type"), QStringLiteral("object") },
			{ QStringLiteral("properties"), QJsonObject{
				{ QStringLiteral("emoji"), QJsonObject{
					{ QStringLiteral("type"), QStringLiteral("string") },
					{ QStringLiteral("description"),
						QStringLiteral("Emoji the sticker represents.") },
				} },
			} },
			{ QStringLiteral("required"), QJsonArray{
				QStringLiteral("emoji") } },
		}));

	return tools;
}

bool IsControlTool(const QString &name) {
	return name == QStringLiteral("send_ladder")
		|| name == QStringLiteral("no_reply");
}

std::optional<QString> RunDataTool(
		const QString &name,
		const QJsonObject &args,
		long long peerId) {
	if (name == QStringLiteral("get_current_time")) {
		const auto now = QDateTime::currentDateTime();
		return now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss dddd"));
	}
	if (name == QStringLiteral("recall_memory")) {
		const auto query = args.value(QStringLiteral("query")).toString();
		if (query.trimmed().isEmpty()) {
			return QStringLiteral("recall_memory requires a non-empty query.");
		}
		return RecallMemory(query, peerId);
	}
	return std::nullopt;
}

bool ApplyControlTool(
		const QString &name,
		const QJsonObject &args,
		ReplyDirective &out) {
	if (name == QStringLiteral("no_reply")) {
		out.decided = true;
		out.shouldReply = false;
		out.parts.clear();
		return true;
	}
	if (name == QStringLiteral("send_ladder")) {
		out.decided = true;
		out.shouldReply = true;
		out.parts.clear();
		for (const auto &value : args.value(QStringLiteral("messages")).toArray()) {
			const auto text = value.toString().trimmed();
			if (!text.isEmpty()) {
				out.parts.push_back(text);
			}
		}
		return true;
	}
	return false;
}

} // namespace TeleForge::Tools
