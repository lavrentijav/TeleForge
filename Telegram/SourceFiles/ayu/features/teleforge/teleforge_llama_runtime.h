#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QString>

#include <optional>
#include <vector>

namespace TeleForge {

void TeleForgeLlamaBackendInit();

void EnsureNativeChatLlamaLoadedAsync(
	const QString &modelGgufPath,
	Fn<void(bool ok, QString error)> onMainThread);

void EnsureNativeEmbedLlamaLoadedAsync(
	const QString &modelGgufPath,
	Fn<void(bool ok, QString error)> onMainThread);

void ShutdownNativeChatLlama();
void ShutdownNativeEmbedLlama();

[[nodiscard]] bool NativeChatLlamaReady();
[[nodiscard]] bool NativeEmbedLlamaReady();

/// Blocking; call only from a worker thread (mutex-serialized with other native calls).
[[nodiscard]] bool NativeChatCompleteBlocking(
	const QJsonArray &messages,
	double temperature,
	int maxTokens,
	QString *outText,
	QString *outError);

/// Blocking; call only from a worker thread.
[[nodiscard]] std::optional<std::vector<float>> NativeEmbedOneBlocking(const QString &text);

} // namespace TeleForge
