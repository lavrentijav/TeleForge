// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "data/data_msg_id.h"
#include "window/window_peer_menu.h"

class History;
class HistoryItem;
class PeerData;

struct TextWithEntities;

namespace HistoryView {
class Element;
} // namespace HistoryView

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Ayu::Translator {

[[nodiscard]] bool MessageTranslateEnabled(not_null<HistoryItem*> item);

[[nodiscard]] TextWithEntities MessageTranslatableText(not_null<HistoryItem*> item);

[[nodiscard]] bool AiTranslationAvailable();

[[nodiscard]] bool AiCompressionAvailable();

void ForceTranslateChat(not_null<History*> history);

void ShowForceTranslateBox(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId msgId,
	TextWithEntities text,
	bool hasCopyRestriction);

void ShowAiTranslateBox(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId msgId,
	TextWithEntities text,
	bool hasCopyRestriction);

void ShowAiCompressBox(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId msgId,
	TextWithEntities text);

} // namespace Ayu::Translator

namespace AyuUi {

void AddForceTranslateChatAction(
	PeerData *peerData,
	not_null<Window::SessionController*> sessionController,
	const Window::PeerMenuCallback &addCallback);

void AddForceTranslateMessageActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	not_null<Window::SessionController*> controller,
	bool hasCopyRestriction);

void AddForceTranslateSelectedActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	not_null<Window::SessionController*> controller,
	const TextWithEntities &selectedText,
	bool hasCopyRestriction);

} // namespace AyuUi
