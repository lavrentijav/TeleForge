#include "ayu/features/teleforge/teleforge_history_bridge.h"

#include "ayu/features/teleforge/teleforge_inference.h"
#include "ayu/features/teleforge/teleforge_memory.h"

#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"

#include <limits>

#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

namespace TeleForge {
namespace {

void UpsertMemoryWithLlmFactSplit(
		MemoryUpsertRequest base,
		const QString &fullText) {
	RequestMemoryFactSplit(fullText, [=](QStringList facts) {
		constexpr auto kMaxFacts = 24;
		auto n = 0;
		for (const auto &fact : facts) {
			if (++n > kMaxFacts) {
				break;
			}
			auto r = base;
			r.title = fact.left(120);
			r.summary = fact.left(4000);
			r.factType = base.factType + QStringLiteral("_fact");
			(void)UpsertMemory(r);
		}
	}, [=] {
		(void)UpsertMemory(base);
	});
}


[[nodiscard]] std::optional<int> ApiMsgIdToStorage(MsgId id) {
	constexpr auto kMin = std::numeric_limits<int>::min();
	constexpr auto kMax = std::numeric_limits<int>::max();
	if (!id || id.bare < kMin || id.bare > kMax) {
		return std::nullopt;
	}
	return static_cast<int>(id.bare);
}

[[nodiscard]] bool IsTrivialCommandOrShort(const QString &t) {
	if (t.size() < 12) {
		return true;
	}
	const auto trimmed = t.trimmed();
	if (trimmed.startsWith('/') || trimmed.startsWith('!')) {
		return true;
	}
	return false;
}

[[nodiscard]] bool LooksLikeMemoryContent(const QString &t) {
	if (IsTrivialCommandOrShort(t)) {
		return false;
	}
	static const auto rx = QRegularExpression(
		QStringLiteral(
			"\\b("
			"I prefer|I like|I don't|I never|I always|I am |I'm |I work|my name|remember that|"
			"я люблю|я предпочитаю|я не хочу|я всегда|я никогда|зовут|называюсь|"
			"мне нравится|не забудь|запомни|аллерг|работаю в|живу в"
			")\\b"),
		QRegularExpression::CaseInsensitiveOption);
	return (t.size() >= 60) || rx.match(t).hasMatch();
}

[[nodiscard]] bool LooksLikeSelfStatement(const QString &t) {
	static const auto rx = QRegularExpression(
		QStringLiteral(
			"\\b("
			"^я |я люблю|я ненавижу|я предпочитаю|мне нравится|мой |моя |мои |"
			"^I |I like|I hate|I prefer|I'm |I am |my |I don't|I never|I always"
			")"),
		QRegularExpression::CaseInsensitiveOption);
	return rx.match(t.trimmed()).hasMatch();
}

[[nodiscard]] QString TurnLabel(not_null<HistoryItem*> item) {
	if (item->out()) {
		return QStringLiteral("You");
	}
	return item->author()->name();
}

[[nodiscard]] QString PlainMessageText(not_null<HistoryItem*> item) {
	return item->originalText().text;
}

} // namespace

std::vector<InferenceTurn> CollectInferenceTurns(
		not_null<History*> history,
		int maxTurns) {
	auto reversed = std::vector<InferenceTurn>();
	reversed.reserve(maxTurns);

	for (auto bi = int(history->blocks.size()) - 1;
			bi >= 0 && int(reversed.size()) < maxTurns;
			--bi) {
		const auto &block = history->blocks[bi];
		for (auto mi = int(block->messages.size()) - 1;
				mi >= 0 && int(reversed.size()) < maxTurns;
				--mi) {
			const auto item = block->messages[mi]->data();
			if (item->isService() || item->isEmpty()) {
				continue;
			}
			const auto text = PlainMessageText(item).trimmed();
			if (text.isEmpty()) {
				continue;
			}
			auto turn = InferenceTurn{
				.role = QStringLiteral("user"),
				.content = TurnLabel(item) + QStringLiteral(": ") + text,
				.senderUserId = 0,
			};
			const auto &dataSession = item->history()->owner();
			if (item->out()) {
				turn.senderUserId = static_cast<long long>(
					SerializePeerId(dataSession.session().userPeerId()));
			} else {
				turn.senderUserId = static_cast<long long>(
					SerializePeerId(item->from()->id));
			}
			reversed.push_back(std::move(turn));
		}
	}
	std::reverse(reversed.begin(), reversed.end());
	return reversed;
}

QString BuildRetrievalQueryFromTurns(
		const std::vector<InferenceTurn> &chronologicalTurns,
		int lastN) {
	auto parts = QStringList();
	const auto n = std::max(1, lastN);
	const auto start = std::max(0, int(chronologicalTurns.size()) - n);
	for (auto i = start; i != int(chronologicalTurns.size()); ++i) {
		parts.push_back(chronologicalTurns[i].content);
	}
	return parts.join('\n');
}

void MaybeIngestHistoryItem(not_null<HistoryItem*> item) {
	if (item->isService() || item->isEmpty() || item->out() || item->isLocal()) {
		return;
	}
	const auto text = PlainMessageText(item).trimmed();
	if (!LooksLikeMemoryContent(text)) {
		return;
	}
	const auto chatPeerId = static_cast<long long>(
		SerializePeerId(item->history()->peer->id));
	const auto settings = LoadMemorySettings(chatPeerId);
	if (!settings.writeEnabled) {
		return;
	}

	auto req = MemoryUpsertRequest{
		.sourcePeerId = chatPeerId,
		.sourceMessageId = ApiMsgIdToStorage(item->id),
		.title = text.left(120),
		.summary = text.left(4000),
		.details = QString(),
		.factType = QStringLiteral("auto_incoming"),
		.basePriority = 5.,
		.stabilityScore = 5.,
	};

	if (item->from()->isUser()) {
		req.scope = MemoryScope::User;
		req.userId = static_cast<long long>(SerializePeerId(item->from()->id));
	} else {
		req.scope = MemoryScope::Chat;
		req.chatId = chatPeerId;
	}
	UpsertMemoryWithLlmFactSplit(req, text);
}

void IngestOutgoingChatText(
		long long chatPeerStorageId,
		long long selfUserStorageId,
		const QString &plainText) {
	const auto text = plainText.trimmed();
	if (!LooksLikeMemoryContent(text)) {
		return;
	}
	const auto settings = LoadMemorySettings(chatPeerStorageId);
	if (!settings.writeEnabled) {
		return;
	}

	auto req = MemoryUpsertRequest{
		.sourcePeerId = chatPeerStorageId,
		.title = text.left(120),
		.summary = text.left(4000),
		.details = QString(),
		.factType = QStringLiteral("auto_outgoing"),
		.basePriority = 5.,
		.stabilityScore = LooksLikeSelfStatement(text) ? 7.5 : 5.,
	};

	if (LooksLikeSelfStatement(text)) {
		req.scope = MemoryScope::User;
		req.userId = selfUserStorageId;
	} else {
		req.scope = MemoryScope::Chat;
		req.chatId = chatPeerStorageId;
	}
	UpsertMemoryWithLlmFactSplit(req, text);
}

} // namespace TeleForge
