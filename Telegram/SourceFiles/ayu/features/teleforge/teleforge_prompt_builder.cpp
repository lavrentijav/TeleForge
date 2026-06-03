#include "ayu/features/teleforge/teleforge_prompt_builder.h"

#include "ayu/features/teleforge/teleforge_memory.h"

namespace TeleForge::Prompt {
namespace {

[[nodiscard]] QString TrimBlock(const QString &block, int maxChars) {
	if (block.isEmpty() || maxChars <= 0) {
		return QString();
	}
	if (block.size() <= maxChars) {
		return block;
	}
	return block.left(maxChars - 1) + QChar(0x2026);
}

} // namespace

QString TruncateBlock(const QString &text, int maxChars) {
	return TrimBlock(text, maxChars);
}

BuiltChatPrompt BuildOpenAiStyleMessages(
		const RetrievedMemoryContext &memory,
		const std::vector<ContextTurn> &chronologicalOldestFirst,
		const AssemblyOptions &options) {
	auto result = BuiltChatPrompt();

	const auto base = TrimBlock(options.baseSystemPrompt, options.maxMemoryBlockChars);
	if (!base.isEmpty()) {
		result.messages.push_back({ QStringLiteral("system"), base });
	}

	const auto g = TrimBlock(memory.globalBlock, options.maxMemoryBlockChars);
	if (!g.isEmpty()) {
		result.messages.push_back({ QStringLiteral("system"), g });
	}
	const auto c = TrimBlock(memory.chatBlock, options.maxMemoryBlockChars);
	if (!c.isEmpty()) {
		result.messages.push_back({ QStringLiteral("system"), c });
	}

	auto userPrefaces = memory.firstMessageUserBlocks;
	for (const auto &turn : chronologicalOldestFirst) {
		auto content = turn.content;
		if (turn.senderUserId != 0 && userPrefaces.contains(turn.senderUserId)) {
			const auto block = userPrefaces.take(turn.senderUserId);
			if (!block.isEmpty()) {
				content = TrimBlock(block, options.maxMemoryBlockChars)
					+ QStringLiteral("\n\n")
					+ content;
			}
		}
		auto role = turn.role;
		if (role != QStringLiteral("system")
			&& role != QStringLiteral("user")
			&& role != QStringLiteral("assistant")) {
			role = QStringLiteral("user");
		}
		result.messages.push_back({ std::move(role), std::move(content) });
	}

	const auto appendix = TrimBlock(memory.appendixBlock, options.maxMemoryBlockChars);
	if (!appendix.isEmpty()) {
		result.messages.push_back({ QStringLiteral("system"), appendix });
	}

	return result;
}

} // namespace TeleForge::Prompt
