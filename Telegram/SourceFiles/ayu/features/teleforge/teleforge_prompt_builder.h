#pragma once

#include <utility>
#include <vector>

#include <QtCore/QString>

namespace TeleForge {

struct RetrievedMemoryContext;

namespace Prompt {

struct ContextTurn {
	QString role;
	QString content;
	long long senderUserId = 0;
};

struct AssemblyOptions {
	int maxMemoryBlockChars = 12000;
	/// Personality / base instructions — first system message, before global/chat memory blocks.
	QString baseSystemPrompt;
};

struct BuiltChatPrompt {
	std::vector<std::pair<QString, QString>> messages;
};

[[nodiscard]] QString TruncateBlock(const QString &text, int maxChars);

[[nodiscard]] BuiltChatPrompt BuildOpenAiStyleMessages(
	const RetrievedMemoryContext &memory,
	const std::vector<ContextTurn> &chronologicalOldestFirst,
	const AssemblyOptions &options = {});

} // namespace Prompt
} // namespace TeleForge
