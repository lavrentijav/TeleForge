// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/translator/ayu_translate_actions.h"

#include "api/api_transcribes.h"
#include "apiwrap.h"
#include "ayu/ayu_settings.h"
#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_lm_studio.h"
#include "boxes/translate_box.h"
#include "boxes/translate_box_content.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/ui_integration.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_context_menu.h"
#include "history/view/history_view_element.h"
#include "history/view/media/history_view_media.h"
#include "lang/translate_provider.h"
#include "lang_auto.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "spellcheck/platform/platform_language.h"
#include "styles/style_menu_icons.h"
#include "ui/boxes/choose_language_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include <QtCore/QFileInfo>

namespace Ayu::Translator {
namespace {

constexpr auto kInlineQuoteLimit = 400;

[[nodiscard]] bool PremiumTranslateAllowed(not_null<Main::Session*> session) {
	return session->premium();
}

[[nodiscard]] LanguageId FallbackOfferFrom(LanguageId to) {
	constexpr auto kFallbacks = std::array{
		LanguageId{ QLocale::English },
		LanguageId{ QLocale::Russian },
		LanguageId{ QLocale::Ukrainian },
		LanguageId{ QLocale::German },
		LanguageId{ QLocale::French },
		LanguageId{ QLocale::Spanish },
	};
	for (const auto &candidate : kFallbacks) {
		if (candidate != to) {
			return candidate;
		}
	}
	return LanguageId{ QLocale::English };
}

[[nodiscard]] LanguageId GuessLanguageFromHistory(not_null<History*> history) {
#ifndef TDESKTOP_DISABLE_SPELLCHECK
	auto counts = base::flat_map<LanguageId, int>();
	constexpr auto kScanLimit = 40;
	auto scanned = 0;
	for (auto blockIt = history->blocks.rbegin()
		; blockIt != history->blocks.rend() && scanned < kScanLimit
		; ++blockIt) {
		for (auto msgIt = (*blockIt)->messages.rbegin()
			; msgIt != (*blockIt)->messages.rend() && scanned < kScanLimit
			; ++msgIt) {
			const auto item = (*msgIt)->data();
			if (!item
				|| item->isService()
				|| !item->isRegular()
				|| item->isOnlyEmojiAndSpaces()) {
				continue;
			}
			const auto &text = item->originalText().text;
			if (text.isEmpty()) {
				continue;
			}
			++scanned;
			const auto id = Platform::Language::Recognize(text);
			if (id.known()) {
				++counts[id];
			}
		}
	}
	if (!counts.empty()) {
		using namespace base;
		constexpr auto p = &flat_multi_map_pair_type<LanguageId, int>::second;
		return ranges::max_element(counts, ranges::less(), p)->first;
	}
#endif // TDESKTOP_DISABLE_SPELLCHECK
	return {};
}

[[nodiscard]] LanguageId ResolveOfferFrom(
		not_null<History*> history,
		LanguageId to) {
	if (const auto offered = history->translateOfferedFrom()) {
		if (offered != to) {
			return offered;
		}
	}
	if (const auto guessed = GuessLanguageFromHistory(history)) {
		if (guessed != to) {
			return guessed;
		}
	}
	return FallbackOfferFrom(to);
}

void EnableChatTranslation(not_null<History*> history) {
	using Flag = PeerData::TranslationFlag;
	if (history->peer->translationFlag() == Flag::Disabled) {
		history->peer->saveTranslationDisabled(false);
	}
}

[[nodiscard]] TextWithEntities TruncateQuote(TextWithEntities quote) {
	if (quote.text.size() <= kInlineQuoteLimit) {
		return quote;
	}
	quote.text = quote.text.left(kInlineQuoteLimit - 3) + u"..."_q;
	quote.entities.clear();
	return quote;
}

[[nodiscard]] TextWithEntities FormatQuotedOverlay(
		const TextWithEntities &quote,
		const TextWithEntities &body) {
	const auto truncated = TruncateQuote(quote);
	const auto quoteLength = truncated.text.size();
	const auto separator = u"\n\n"_q;
	const auto bodyOffset = quoteLength + separator.size();

	auto result = TextWithEntities{
		.text = truncated.text + separator + body.text,
	};
	result.entities.push_back({
		EntityType::Blockquote,
		0,
		int(quoteLength),
	});
	for (const auto &entity : body.entities) {
		result.entities.push_back({
			entity.type(),
			entity.offset() + int(bodyOffset),
			entity.length(),
			entity.data(),
		});
	}
	return result;
}

void ApplyInlineOverlay(
		not_null<HistoryItem*> item,
		TextWithEntities overlay,
		LanguageId languageId = {}) {
	item->history()->session().api().transcribes().applyLocalSummary(
		item,
		std::move(overlay),
		languageId);
}

void ShowInlineLoading(
		not_null<HistoryItem*> item,
		const TextWithEntities &original) {
	ApplyInlineOverlay(
		item,
		FormatQuotedOverlay(
			original,
			TextWithEntities{ tr::lng_contacts_loading(tr::now) }));
}

void RunAiCompletionInline(
		not_null<HistoryItem*> item,
		const TextWithEntities &original,
		const QString &systemPrompt) {
	ShowInlineLoading(item, original);
	const auto personality = TeleForge::LoadPersonalityCore()
		.value_or(TeleForge::DefaultPersonalityCore());
	auto options = TeleForge::LmStudioRequestOptions{};
	options.model = personality.chatModelId.trimmed();
	options.temperature = 0.2;
	options.maxTokens = 2048;
	TeleForge::LmStudioBridge::instance().requestCompletion(
		systemPrompt,
		original.text,
		[=](const QString &reply) {
			if (const auto current = item->history()->owner().message(item->fullId())) {
				ApplyInlineOverlay(
					current,
					FormatQuotedOverlay(
						original,
						TextWithEntities{ .text = reply.trimmed() }));
			}
		},
		[=](const QString &) {
			if (const auto current = item->history()->owner().message(item->fullId())) {
				ApplyInlineOverlay(
					current,
					FormatQuotedOverlay(
						original,
						TextWithEntities{ u"Translation failed"_q }));
			}
		},
		options);
}

void ForceTranslateMessageInline(
		not_null<HistoryItem*> item,
		TextWithEntities text) {
	const auto history = item->history();
	const auto to = Ui::ChooseTranslateTo(history);
	const auto provider = Ui::CreateTranslateProvider(&history->session());
	const auto request = Ui::PrepareTranslateProviderRequest(
		provider.get(),
		history->peer,
		item->fullId().msg,
		text);
	ShowInlineLoading(item, text);
	provider->request(
		request,
		to,
		[=, original = text](Ui::TranslateProviderResult result) {
			if (const auto current = history->owner().message(item->fullId())) {
				if (result.error != Ui::TranslateProviderError::None
					|| !result.text) {
					ApplyInlineOverlay(
						current,
						FormatQuotedOverlay(
							original,
							TextWithEntities{
								u"Translation failed"_q }));
				} else {
					ApplyInlineOverlay(
						current,
						FormatQuotedOverlay(original, *result.text),
						to);
				}
			}
		});
}

void AiTranslateMessageInline(
		not_null<HistoryItem*> item,
		TextWithEntities text) {
	const auto history = item->history();
	const auto to = Ui::ChooseTranslateTo(history);
	const auto targetName = Ui::LanguageName(to);
	const auto system = QStringLiteral(
		"You are a professional translator. Translate the user's message into %1. "
		"Preserve tone, slang, humor, and meaning. "
		"If the text uses transliteration (Latin letters standing for another language), "
		"infer the intended language first, then translate naturally into %1. "
		"Keep names, usernames, URLs, and code unchanged. "
		"Output only the translation without explanations or quotes.")
		.arg(targetName);
	RunAiCompletionInline(item, text, system);
}

void AiCompressMessageInline(
		not_null<HistoryItem*> item,
		TextWithEntities text) {
	const auto system = QStringLiteral(
		"You compress long messages into concise summaries. "
		"Preserve key facts, names, numbers, and intent. "
		"Write in the same language as the input. "
		"Output only the compressed text without explanations or quotes.");
	RunAiCompletionInline(item, text, system);
}

void ShowTranslateBoxInternal(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId msgId,
		TextWithEntities text,
		bool hasCopyRestriction) {
	controller->show(Box(
		Ui::TranslateBox,
		peer,
		msgId,
		std::move(text),
		hasCopyRestriction));
}

void ShowAiTranslateBoxInternal(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId msgId,
		TextWithEntities text,
		bool hasCopyRestriction) {
	struct State {
		State(not_null<Main::Session*> session)
		: provider(Ui::CreateTranslateProvider(session)) {
		}

		std::unique_ptr<Ui::TranslateProvider> provider;
		rpl::variable<LanguageId> to;
	};
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto state = box->lifetime().make_state<State>(&peer->session());
		const auto history = peer->owner().history(peer);
		state->to = Ui::ChooseTranslateTo(history);
		const auto request = std::make_shared<Ui::TranslateProviderRequest>(
			Ui::PrepareTranslateProviderRequest(
				state->provider.get(),
				peer,
				msgId,
				text));
		Ui::TranslateBoxContent(box, {
			.text = request->text,
			.hasCopyRestriction = hasCopyRestriction,
			.textContext = Core::TextContext({ .session = &peer->session() }),
			.to = state->to.value(),
			.chooseTo = [=] {
				box->uiShow()->showBox(Ui::ChooseTranslateToBox(
					state->to.current(),
					crl::guard(box, [=](LanguageId id) { state->to = id; })));
			},
			.request = [=](
					LanguageId to,
					Fn<void(Ui::TranslateBoxContentResult)> done) {
				const auto personality = TeleForge::LoadPersonalityCore()
					.value_or(TeleForge::DefaultPersonalityCore());
				const auto targetName = Ui::LanguageName(to);
				const auto system = QStringLiteral(
					"You are a professional translator. Translate the user's message into %1. "
					"Preserve tone, slang, humor, and meaning. "
					"If the text uses transliteration (Latin letters standing for another language), "
					"infer the intended language first, then translate naturally into %1. "
					"Keep names, usernames, URLs, and code unchanged. "
					"Output only the translation without explanations or quotes.")
					.arg(targetName);
				auto options = TeleForge::LmStudioRequestOptions{};
				options.model = personality.chatModelId.trimmed();
				options.temperature = 0.2;
				options.maxTokens = 2048;
				TeleForge::LmStudioBridge::instance().requestCompletion(
					system,
					request->text.text,
					[=](const QString &reply) {
						done(Ui::TranslateBoxContentResult{
							.text = TextWithEntities{ .text = reply.trimmed() },
							.error = Ui::TranslateBoxContentError::None,
						});
					},
					[=](const QString &) {
						done(Ui::TranslateBoxContentResult{
							.error = Ui::TranslateBoxContentError::Unknown,
						});
					},
					options);
			},
		});
	}));
}

} // namespace

bool MessageTranslateEnabled(not_null<HistoryItem*> item) {
	if (!Core::App().settings().translateButtonEnabled()) {
		return false;
	}
	if (!PremiumTranslateAllowed(&item->history()->session())) {
		return false;
	}
	if (item->isService() || !item->isRegular()) {
		return false;
	}
	return true;
}

TextWithEntities MessageTranslatableText(not_null<HistoryItem*> item) {
	const auto view = item->mainView();
	const auto media = view ? view->media() : nullptr;
	const auto mediaHasTextForCopy = media && media->hasTextForCopy();
	if (mediaHasTextForCopy) {
		return HistoryView::TransribedText(item)
			.append('\n')
			.append(item->originalText());
	}
	return item->originalText();
}

bool AiTranslationAvailable() {
	if (!AyuSettings::getInstance().aiTranslationEnabled()) {
		return false;
	}
	const auto personality = TeleForge::LoadPersonalityCore()
		.value_or(TeleForge::DefaultPersonalityCore());
	if (!personality.chatModelPath.trimmed().isEmpty()
		&& QFileInfo::exists(personality.chatModelPath.trimmed())) {
		return true;
	}
	if (!personality.chatModelId.trimmed().isEmpty()) {
		return true;
	}
	if (!personality.lmStudioBaseUrl.trimmed().isEmpty()) {
		return true;
	}
	return TeleForge::LmStudioBridge::instance().nativeChatEnabled();
}

bool AiCompressionAvailable() {
	if (!AyuSettings::getInstance().aiCompressionEnabled()) {
		return false;
	}
	return AiTranslationAvailable();
}

void ForceTranslateChat(not_null<History*> history) {
	if (!Core::App().settings().translateChatEnabled()) {
		return;
	} else if (!PremiumTranslateAllowed(&history->session())) {
		return;
	}
	EnableChatTranslation(history);
	const auto to = Ui::ChooseTranslateTo(history);
	const auto from = ResolveOfferFrom(history, to);
	history->translateOfferFrom(from);
	history->translateTo(to);
	if (const auto migrated = history->migrateFrom()) {
		migrated->translateOfferFrom(from);
		migrated->translateTo(to);
	}
}

void ShowForceTranslateBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId msgId,
		TextWithEntities text,
		bool hasCopyRestriction) {
	if (const auto item = peer->owner().message(peer, msgId)) {
		ForceTranslateMessageInline(item, std::move(text));
		return;
	}
	ShowTranslateBoxInternal(
		controller,
		peer,
		msgId,
		std::move(text),
		hasCopyRestriction);
}

void ShowAiTranslateBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId msgId,
		TextWithEntities text,
		bool hasCopyRestriction) {
	if (const auto item = peer->owner().message(peer, msgId)) {
		AiTranslateMessageInline(item, std::move(text));
		return;
	}
	ShowAiTranslateBoxInternal(
		controller,
		peer,
		msgId,
		std::move(text),
		hasCopyRestriction);
}

void ShowAiCompressBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId msgId,
		TextWithEntities text) {
	if (const auto item = peer->owner().message(peer, msgId)) {
		AiCompressMessageInline(item, std::move(text));
		return;
	}
	Q_UNUSED(controller);
	Q_UNUSED(peer);
	Q_UNUSED(msgId);
	Q_UNUSED(text);
}

} // namespace Ayu::Translator

namespace AyuUi {

void AddForceTranslateChatAction(
		PeerData *peerData,
		not_null<Window::SessionController*> sessionController,
		const Window::PeerMenuCallback &addCallback) {
	if (!peerData
		|| !Core::App().settings().translateChatEnabled()
		|| !peerData->session().premium()) {
		return;
	}
	const auto history = peerData->owner().historyLoaded(peerData);
	if (!history || history->translatedTo()) {
		return;
	}
	addCallback(
		tr::ayu_ForceTranslateChat(tr::now),
		[=] {
			Ayu::Translator::ForceTranslateChat(history);
		},
		&st::menuIconTranslate);
}

void AddForceTranslateMessageActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<Window::SessionController*> controller,
		bool hasCopyRestriction) {
	if (!Ayu::Translator::MessageTranslateEnabled(item)) {
		return;
	}
	const auto translate = Ayu::Translator::MessageTranslatableText(item);
	if (translate.text.isEmpty()) {
		return;
	}
	const auto peer = item->history()->peer;
	const auto msgId = item->fullId().msg;
	const auto view = item->mainView();
	const auto media = view ? view->media() : nullptr;
	const auto mediaHasTextForCopy = media && media->hasTextForCopy();
	const auto boxMsgId = mediaHasTextForCopy ? MsgId() : msgId;

	if (Ui::SkipTranslate(translate)) {
		menu->addAction(tr::ayu_ForceTranslateMessage(tr::now), [=] {
			Ayu::Translator::ShowForceTranslateBox(
				controller,
				peer,
				boxMsgId,
				translate,
				hasCopyRestriction);
		}, &st::menuIconTranslate);
	}
	if (Ayu::Translator::AiTranslationAvailable()) {
		menu->addAction(tr::ayu_AiTranslateMessage(tr::now), [=] {
			Ayu::Translator::ShowAiTranslateBox(
				controller,
				peer,
				boxMsgId,
				translate,
				hasCopyRestriction);
		}, &st::menuIconTranslate);
	}
	if (Ayu::Translator::AiCompressionAvailable()) {
		menu->addAction(tr::ayu_AiCompressMessage(tr::now), [=] {
			Ayu::Translator::ShowAiCompressBox(
				controller,
				peer,
				boxMsgId,
				translate);
		}, &st::menuIconTranslate);
	}
}

void AddForceTranslateSelectedActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<Window::SessionController*> controller,
		const TextWithEntities &selectedText,
		bool hasCopyRestriction) {
	if (!Ayu::Translator::MessageTranslateEnabled(item)) {
		return;
	} else if (selectedText.text.isEmpty()) {
		return;
	}
	const auto peer = item->history()->peer;
	if (Ui::SkipTranslate(selectedText)) {
		menu->addAction(tr::ayu_ForceTranslateSelected(tr::now), [=] {
			Ayu::Translator::ShowForceTranslateBox(
				controller,
				peer,
				MsgId(),
				selectedText,
				hasCopyRestriction);
		}, &st::menuIconTranslate);
	}
	if (Ayu::Translator::AiTranslationAvailable()) {
		menu->addAction(tr::ayu_AiTranslateSelected(tr::now), [=] {
			Ayu::Translator::ShowAiTranslateBox(
				controller,
				peer,
				MsgId(),
				selectedText,
				hasCopyRestriction);
		}, &st::menuIconTranslate);
	}
}

} // namespace AyuUi
