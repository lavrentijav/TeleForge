// Copyright @Radolyn, 2026
#pragma once

#include <optional>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace TeleForge::Tools {

// What the model decided about delivering the reply, filled by control tools.
struct ReplyDirective {
	bool decided = false;      // a control tool (no_reply / send_ladder) fired
	bool shouldReply = true;   // false when the model called no_reply
	QStringList parts;         // >1 => ladder; empty => fall back to content
};

// OpenAI-style definitions of the built-in tools the model may call.
[[nodiscard]] QJsonArray BuiltinToolsJson();

[[nodiscard]] bool IsControlTool(const QString &name);

// Runs a "data" tool (get_current_time / recall_memory) and returns the text
// to feed back to the model as a role:"tool" message. Returns std::nullopt if
// `name` is not a known data tool.
[[nodiscard]] std::optional<QString> RunDataTool(
	const QString &name,
	const QJsonObject &args,
	long long peerId);

// Interprets a control tool (no_reply / send_ladder), updating `out`.
// Returns true if `name` was a control tool.
bool ApplyControlTool(
	const QString &name,
	const QJsonObject &args,
	ReplyDirective &out);

} // namespace TeleForge::Tools
