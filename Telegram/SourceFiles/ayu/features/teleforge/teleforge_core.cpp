#include "ayu/features/teleforge/teleforge_core.h"

#include "ayu/features/teleforge/teleforge_inference.h"
#include "ayu/features/teleforge/teleforge_storage.h"

#include <algorithm>

#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>
#include <QtCore/QSysInfo>
#include <QtCore/QTextStream>
#include <QtCore/QUrl>

namespace TeleForge {
namespace {

[[nodiscard]] double ClampWeight(double value) {
	return std::clamp(value, 0., 10.);
}

[[nodiscard]] double SafeAverage(double sum, int count, double fallback) {
	return count > 0 ? (sum / count) : fallback;
}

[[nodiscard]] int CountMatches(
		const QString &text,
		const QRegularExpression &pattern) {
	auto total = 0;
	auto it = pattern.globalMatch(text);
	while (it.hasNext()) {
		it.next();
		++total;
	}
	return total;
}

[[nodiscard]] QString BuildSystemPrompt(const PersonalityWeights &weights) {
	QStringList parts;
	parts.push_back("You mirror the account owner instead of sounding like a generic assistant.");
	parts.push_back(weights.brevity >= 7.
		? "Prefer short, direct replies."
		: "Balance concise answers with enough detail to stay useful.");
	parts.push_back(weights.creativity >= 7.
		? "When appropriate, vary phrasing and structure to avoid repetitive output."
		: "Favor predictable, technically clear phrasing.");
	parts.push_back(weights.aggression >= 7.
		? "Use a firm, high-pressure tone when the context calls for it."
		: "Keep tone controlled and professional.");
	parts.push_back(weights.emojis >= 6.
		? "Emoji use is acceptable when it matches the chat."
		: "Avoid decorative emoji unless the chat already uses them.");
	parts.push_back(weights.toxicity >= 6.
		? "Do not sanitize the owner's sharp phrasing, but keep it coherent."
		: "Avoid insults and unnecessary hostility.");
	parts.push_back("Preserve markdown for code, commands, and links.");
	return parts.join(' ');
}

[[nodiscard]] Storage::PersonalityCoreRecord ToRecord(const PersonalityCore &core) {
	return {
		.singletonId = 1,
		.aggression = core.weights.aggression,
		.brevity = core.weights.brevity,
		.emojis = core.weights.emojis,
		.toxicity = core.weights.toxicity,
		.creativity = core.weights.creativity,
		.systemPrompt = core.systemPrompt.toStdString(),
		.sourceDevice = core.sourceDevice.toStdString(),
		.updatedAt = static_cast<int>(core.updatedAt.toSecsSinceEpoch()),
		.embeddingEndpointUrl = core.embeddingEndpointUrl.toStdString(),
		.embeddingModelId = core.embeddingModelId.toStdString(),
		.lmStudioBaseUrl = core.lmStudioBaseUrl.toStdString(),
		.chatModelPath = core.chatModelPath.toStdString(),
		.chatModelId = core.chatModelId.toStdString(),
		.openAiApiKey = core.openAiApiKey.toStdString(),
		.chatContextMessages = core.chatContextMessages,
		.memorySyncEnabled = core.memorySyncEnabled,
		.rerankEndpointUrl = core.rerankEndpointUrl.toStdString(),
		.rerankModelId = core.rerankModelId.toStdString(),
		.rerankModelPath = core.rerankModelPath.toStdString(),
	};
}

[[nodiscard]] PersonalityCore FromRecord(const Storage::PersonalityCoreRecord &record) {
	return {
		.weights = {
			.aggression = record.aggression,
			.brevity = record.brevity,
			.emojis = record.emojis,
			.toxicity = record.toxicity,
			.creativity = record.creativity,
		},
		.systemPrompt = QString::fromStdString(record.systemPrompt),
		.sourceDevice = QString::fromStdString(record.sourceDevice),
		.updatedAt = QDateTime::fromSecsSinceEpoch(record.updatedAt),
		.embeddingEndpointUrl = QString::fromStdString(record.embeddingEndpointUrl),
		.embeddingModelId = QString::fromStdString(record.embeddingModelId),
		.lmStudioBaseUrl = QString::fromStdString(record.lmStudioBaseUrl),
		.chatModelPath = QString::fromStdString(record.chatModelPath),
		.chatModelId = QString::fromStdString(record.chatModelId),
		.openAiApiKey = QString::fromStdString(record.openAiApiKey),
		.chatContextMessages = record.chatContextMessages > 0
			? record.chatContextMessages
			: 40,
		.memorySyncEnabled = record.memorySyncEnabled,
		.rerankEndpointUrl = QString::fromStdString(record.rerankEndpointUrl),
		.rerankModelId = QString::fromStdString(record.rerankModelId),
		.rerankModelPath = QString::fromStdString(record.rerankModelPath),
	};
}

} // namespace

PersonalityCore DefaultPersonalityCore() {
	auto core = PersonalityCore();
	core.weights = {
		.aggression = 3.5,
		.brevity = 6.5,
		.emojis = 1.0,
		.toxicity = 0.5,
		.creativity = 5.0,
	};
	core.sourceDevice = QSysInfo::machineHostName();
	core.updatedAt = QDateTime::currentDateTimeUtc();
	core.systemPrompt = BuildSystemPrompt(core.weights);
	return core;
}

PersonalityCore BuildPersonalityCoreFromMessages(const std::vector<QString> &messages) {
	if (messages.empty()) {
		return DefaultPersonalityCore();
	}

	const auto emojiPattern = QRegularExpression("[\\x{1F300}-\\x{1FAFF}]");
	const auto toxicPattern = QRegularExpression(
		"\\b(fuck|shit|bitch|idiot|moron|stupid|damn)\\b",
		QRegularExpression::CaseInsensitiveOption);
	const auto codePattern = QRegularExpression("(```|`[^`]+`|\\b(cmake|git|npm|cargo|python|powershell|bash)\\b|[A-Za-z]:\\\\)");

	auto charsTotal = 0.;
	auto shortMessages = 0.;
	auto exclamations = 0.;
	auto uppercaseChars = 0.;
	auto letterChars = 0.;
	auto emojiCount = 0.;
	auto toxicCount = 0.;
	auto creativeSignals = 0.;

	for (const auto &message : messages) {
		charsTotal += message.size();
		if (message.size() <= 40) {
			++shortMessages;
		}
		exclamations += message.count('!');
		emojiCount += CountMatches(message, emojiPattern);
		toxicCount += CountMatches(message, toxicPattern);
		if (codePattern.match(message).hasMatch()) {
			++creativeSignals;
		}
		if (message.contains('\n') || message.contains("->") || message.contains(':')) {
			++creativeSignals;
		}
		for (const auto ch : message) {
			if (ch.isLetter()) {
				++letterChars;
				if (ch.isUpper()) {
					++uppercaseChars;
				}
			}
		}
	}

	const auto count = static_cast<int>(messages.size());
	const auto averageLength = SafeAverage(charsTotal, count, 32.);
	const auto shortRatio = SafeAverage(shortMessages, count, 0.5);
	const auto uppercaseRatio = SafeAverage(uppercaseChars, letterChars, 0.);
	const auto exclamationRatio = SafeAverage(exclamations, count, 0.);
	const auto emojiRatio = SafeAverage(emojiCount, count, 0.);
	const auto toxicRatio = SafeAverage(toxicCount, count, 0.);
	const auto creativityRatio = SafeAverage(creativeSignals, count, 0.);

	PersonalityCore core;
	core.weights.aggression = ClampWeight(2. + (uppercaseRatio * 30.) + (exclamationRatio * 2.5));
	core.weights.brevity = ClampWeight(9. - std::min(averageLength, 180.) / 24. + (shortRatio * 2.));
	core.weights.emojis = ClampWeight(emojiRatio * 3.);
	core.weights.toxicity = ClampWeight(toxicRatio * 5.);
	core.weights.creativity = ClampWeight(3. + (creativityRatio * 6.) + (averageLength > 90. ? 1. : 0.));
	core.sourceDevice = QSysInfo::machineHostName();
	core.updatedAt = QDateTime::currentDateTimeUtc();
	core.systemPrompt = BuildSystemPrompt(core.weights);
	return core;
}

std::optional<PersonalityCore> LoadPersonalityCore() {
	if (const auto record = Storage::loadPersonalityCore()) {
		return FromRecord(*record);
	}
	return std::nullopt;
}

void PersistPersonalityCore(const PersonalityCore &core) {
	Storage::upsertPersonalityCore(ToRecord(core));
	ExportPersonalityCoreSnapshot(core, DefaultSnapshotPath());
	ApplyPersonalityEndpoints(core);
}

QString SerializePersonalityCore(const PersonalityCore &core) {
	QStringList lines;
	lines.push_back("teleforge_personality_core_v1");
	lines.push_back("updated_at=" + QString::number(core.updatedAt.toSecsSinceEpoch()));
	lines.push_back("source_device=" + QString::fromUtf8(
		core.sourceDevice.toUtf8().toPercentEncoding()));
	lines.push_back("aggression=" + QString::number(core.weights.aggression, 'f', 2));
	lines.push_back("brevity=" + QString::number(core.weights.brevity, 'f', 2));
	lines.push_back("emojis=" + QString::number(core.weights.emojis, 'f', 2));
	lines.push_back("toxicity=" + QString::number(core.weights.toxicity, 'f', 2));
	lines.push_back("creativity=" + QString::number(core.weights.creativity, 'f', 2));
	lines.push_back("system_prompt=" + QString::fromUtf8(
		core.systemPrompt.toUtf8().toPercentEncoding()));
	if (!core.embeddingEndpointUrl.isEmpty()) {
		lines.push_back("embedding_endpoint=" + QString::fromUtf8(
			core.embeddingEndpointUrl.toUtf8().toPercentEncoding()));
	}
	if (!core.embeddingModelId.isEmpty()) {
		lines.push_back("embedding_model=" + QString::fromUtf8(
			core.embeddingModelId.toUtf8().toPercentEncoding()));
	}
	if (!core.lmStudioBaseUrl.isEmpty()) {
		lines.push_back("lm_base_url=" + QString::fromUtf8(
			core.lmStudioBaseUrl.toUtf8().toPercentEncoding()));
	}
	if (!core.chatModelPath.isEmpty()) {
		lines.push_back("chat_model_path=" + QString::fromUtf8(
			core.chatModelPath.toUtf8().toPercentEncoding()));
	}
	if (!core.chatModelId.isEmpty()) {
		lines.push_back("chat_model_id=" + QString::fromUtf8(
			core.chatModelId.toUtf8().toPercentEncoding()));
	}
	if (!core.openAiApiKey.isEmpty()) {
		lines.push_back("openai_api_key=" + QString::fromUtf8(
			core.openAiApiKey.toUtf8().toPercentEncoding()));
	}
	if (core.chatContextMessages > 0) {
		lines.push_back("chat_context_messages=" + QString::number(core.chatContextMessages));
	}
	lines.push_back(
		"memory_sync=" + QString::number(core.memorySyncEnabled ? 1 : 0));
	if (!core.rerankEndpointUrl.isEmpty()) {
		lines.push_back("rerank_endpoint=" + QString::fromUtf8(
			core.rerankEndpointUrl.toUtf8().toPercentEncoding()));
	}
	if (!core.rerankModelId.isEmpty()) {
		lines.push_back("rerank_model=" + QString::fromUtf8(
			core.rerankModelId.toUtf8().toPercentEncoding()));
	}
	if (!core.rerankModelPath.isEmpty()) {
		lines.push_back("rerank_model_path=" + QString::fromUtf8(
			core.rerankModelPath.toUtf8().toPercentEncoding()));
	}
	return lines.join('\n');
}

std::optional<PersonalityCore> ParsePersonalityCore(const QString &serialized) {
	const auto lines = serialized.split('\n', Qt::SkipEmptyParts);
	if (lines.isEmpty() || lines.front().trimmed() != "teleforge_personality_core_v1") {
		return std::nullopt;
	}

	auto core = DefaultPersonalityCore();
	for (auto i = 1; i != lines.size(); ++i) {
		const auto line = lines[i];
		const auto index = line.indexOf('=');
		if (index <= 0) {
			continue;
		}
		const auto key = line.left(index).trimmed();
		const auto value = line.mid(index + 1).trimmed();
		if (key == "updated_at") {
			core.updatedAt = QDateTime::fromSecsSinceEpoch(value.toLongLong());
		} else if (key == "source_device") {
			core.sourceDevice = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "aggression") {
			core.weights.aggression = value.toDouble();
		} else if (key == "brevity") {
			core.weights.brevity = value.toDouble();
		} else if (key == "emojis") {
			core.weights.emojis = value.toDouble();
		} else if (key == "toxicity") {
			core.weights.toxicity = value.toDouble();
		} else if (key == "creativity") {
			core.weights.creativity = value.toDouble();
		} else if (key == "system_prompt") {
			core.systemPrompt = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "embedding_endpoint") {
			core.embeddingEndpointUrl = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "embedding_model") {
			core.embeddingModelId = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "lm_base_url") {
			core.lmStudioBaseUrl = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "chat_model_path") {
			core.chatModelPath = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "chat_model_id") {
			core.chatModelId = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "openai_api_key") {
			core.openAiApiKey = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "chat_context_messages") {
			core.chatContextMessages = value.toInt();
		} else if (key == "memory_sync") {
			core.memorySyncEnabled = (value.toInt() != 0);
		} else if (key == "rerank_endpoint") {
			core.rerankEndpointUrl = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "rerank_model") {
			core.rerankModelId = QUrl::fromPercentEncoding(value.toUtf8());
		} else if (key == "rerank_model_path") {
			core.rerankModelPath = QUrl::fromPercentEncoding(value.toUtf8());
		}
	}

	core.weights.aggression = ClampWeight(core.weights.aggression);
	core.weights.brevity = ClampWeight(core.weights.brevity);
	core.weights.emojis = ClampWeight(core.weights.emojis);
	core.weights.toxicity = ClampWeight(core.weights.toxicity);
	core.weights.creativity = ClampWeight(core.weights.creativity);
	if (core.chatContextMessages <= 0) {
		core.chatContextMessages = 40;
	} else {
		core.chatContextMessages = std::clamp(core.chatContextMessages, 4, 128);
	}
	return core;
}

bool ExportPersonalityCoreSnapshot(
		const PersonalityCore &core,
		const QString &path) {
	QSaveFile file(path.isEmpty() ? DefaultSnapshotPath() : path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return false;
	}

	QTextStream stream(&file);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	stream.setEncoding(QStringConverter::Utf8);
#else // Qt >= 6.0.0
	stream.setCodec("UTF-8");
#endif // Qt < 6.0.0
	stream << SerializePersonalityCore(core);
	return file.commit();
}

QString DefaultSnapshotPath() {
	return "./tdata/personality_core.tforge";
}

} // namespace TeleForge
