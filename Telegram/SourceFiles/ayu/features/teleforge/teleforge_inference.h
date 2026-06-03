#pragma once

#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_lm_studio.h"

#include "base/basic_types.h"

#include <vector>

#include <QtCore/QJsonArray>
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

/// Asks the main chat model (temperature ~0) to split `messageText` into one fact per line,
/// then invokes `onFacts` with non-empty lines. On failure or empty parse, calls `onFallback`.
void RequestMemoryFactSplit(
	QString messageText,
	Fn<void(QStringList facts)> onFacts,
	Fn<void()> onFallback);

} // namespace TeleForge
