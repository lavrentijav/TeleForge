#pragma once

#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_lm_studio.h"

#include "base/basic_types.h"

#include <vector>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace TeleForge {

struct InferenceTurn {
	QString role;
	QString content;
	long long senderUserId = 0;
};

struct InferenceParams {
	long long peerId = 0;
	QString retrievalQuery;
	std::vector<InferenceTurn> chronologicalTurns;
	LmStudioRequestOptions lmOptions;
};

struct PreparedChatCompletion {
	QJsonArray messages;
	LmStudioRequestOptions lmOptions;
};

// Result of a tool-aware reply generation.
struct TeleForgeReplyOutcome {
	bool shouldReply = true;  // false when the model called the no_reply tool
	QStringList parts;        // messages to deliver; >1 means "ladder"
	QString error;            // non-empty => generation failed
};

void ApplyPersonalityEndpoints(const PersonalityCore &core);

void ApplyEndpointsFromStorage();

[[nodiscard]] PreparedChatCompletion PrepareTeleForgeCompletion(
	const InferenceParams &params);

void RequestTeleForgeCompletion(
	const InferenceParams &params,
	LmStudioBridge::SuccessCallback onSuccess,
	LmStudioBridge::ErrorCallback onError = {});

void RequestTeleForgeCompletionAsync(
	InferenceParams params,
	LmStudioBridge::SuccessCallback onSuccess,
	LmStudioBridge::ErrorCallback onError = {});

/// Executes a chat action tool the model called (e.g. reply_to). Receives the
/// tool name and parsed arguments; returns the result string fed back to the
/// model, or a null QString if the tool is unknown/unavailable.
using ToolActionExecutor = Fn<QString(const QString &name, const QJsonObject &args)>;

/// Tool-aware reply generation. Runs the built-in data/control tools
/// (get_current_time, recall_memory, send_ladder, no_reply) plus any plugin
/// tools in a bounded loop. Non-data/non-control tools are routed to
/// `onAction` (when provided), which performs real chat actions. Reports the
/// final delivery decision via `onDone`.
void RequestTeleForgeReply(
	const InferenceParams &params,
	Fn<void(TeleForgeReplyOutcome)> onDone,
	ToolActionExecutor onAction = nullptr);

/// Asks the main chat model (temperature ~0) to split `messageText` into one fact per line,
/// then invokes `onFacts` with non-empty lines. On failure or empty parse, calls `onFallback`.
void RequestMemoryFactSplit(
	QString messageText,
	Fn<void(QStringList facts)> onFacts,
	Fn<void()> onFallback);

} // namespace TeleForge
