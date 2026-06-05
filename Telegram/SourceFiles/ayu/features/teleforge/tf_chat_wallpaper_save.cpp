// Copyright @Radolyn, 2026

#include "ayu/features/teleforge/tf_chat_wallpaper_save.h"

#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_peer.h"
#include "data/data_wall_paper.h"
#include "main/main_session.h"
#include "rpl/rpl.h"
#include "ui/chat/chat_theme.h"
#include "window/window_session_controller.h"

#include <QGuiApplication>
#include <QImage>

namespace TeleForge::ChatWallpaper {
namespace {

[[nodiscard]] bool HasCustomPeerWallpaper(not_null<PeerData*> peer) {
	const auto paper = peer->wallPaper();
	return paper && !paper->isNull() && !Data::IsDefaultWallPaper(*paper);
}

void SaveImageToFile(not_null<Main::Session*> session, const QImage &image) {
	if (image.isNull()) {
		return;
	}
	const auto filter = u"PNG Image (*.png);;JPEG Image (*.jpg);;"_q
		+ FileDialog::AllFilesFilter();
	FileDialog::GetWritePath(
		Core::App().getFileDialogParent(),
		u"Сохранить обои чата"_q,
		filter,
		filedialogDefaultName(u"chat_wallpaper"_q, u".png"_q),
		crl::guard(session, [=](QString &&path) {
			if (path.isEmpty()) {
				return;
			}
			image.save(path);
		}));
}

void SaveFromDocument(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		Data::FileOrigin origin) {
	document->save(origin, QString());
	const auto media = document->createMediaView();
	session->downloaderTaskFinished(
	) | rpl::take(1) | rpl::on_next([=] {
		if (!media->loaded(true)) {
			return;
		}
		const auto bytes = media->bytes();
		if (bytes.isEmpty()) {
			return;
		}
		auto image = QImage::fromData(bytes);
		if (image.isNull()) {
			return;
		}
		SaveImageToFile(session, image);
	}, session->lifetime());
}

} // namespace

bool canSaveForPeer(not_null<PeerData*> peer) {
	return HasCustomPeerWallpaper(peer);
}

void trySaveOnEmptyClick(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	if (!HasCustomPeerWallpaper(peer)) {
		return;
	}
	const auto paper = peer->wallPaper();
	const auto session = &peer->session();
	if (const auto document = paper->document()) {
		SaveFromDocument(session, document, paper->fileOrigin());
		return;
	}
	const auto &theme = controller->currentChatTheme();
	const auto &background = theme->background();
	if (!background.prepared.isNull()) {
		SaveImageToFile(session, background.prepared);
	} else if (!background.gradientForFill.isNull()) {
		SaveImageToFile(session, background.gradientForFill);
	}
}

} // namespace TeleForge::ChatWallpaper
