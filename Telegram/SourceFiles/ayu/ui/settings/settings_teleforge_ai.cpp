// This is the source code of AyuGram for Desktop.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_teleforge_ai.h"

#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_model_downloader.h"
#include "ayu/features/teleforge/teleforge_openai_models.h"
#include "ayu/features/teleforge/teleforge_paths.h"
#include "ayu/features/teleforge/teleforge_rerank.h"
#include "ayu/features/teleforge/teleforge_storage.h"
#include "ayu/features/translator/ayu_translate_actions.h"
#include "ayu/ayu_settings.h"
#include "ayu/ui/settings/ayu_builder.h"
#include "ayu/ui/settings/settings_ayu_utils.h"
#include "lang_auto.h"
#include "ayu/ui/settings/settings_main.h"
#include "base/basic_types.h"
#include "base/unixtime.h"
#include "base/variant.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include "apiwrap.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_search_controller.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QPoint>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include <cmath>

namespace Settings {

using namespace Builder;
using namespace AyBuilder;

namespace {

[[nodiscard]] int ParseIntClamped(
		const QString &text,
		int lo,
		int hi,
		int fallback) {
	auto ok = false;
	const auto v = text.trimmed().toInt(&ok);
	if (!ok) {
		return fallback;
	}
	return std::clamp(v, lo, hi);
}

[[nodiscard]] double ParseDouble(
		const QString &text,
		double fallback) {
	auto ok = false;
	const auto v = text.trimmed().toDouble(&ok);
	return ok ? v : fallback;
}

void ApplyTunedStyle(
		std::shared_ptr<std::vector<QString>> texts,
		not_null<Window::SessionController*> controller) {
	if (texts->size() < 5) {
		controller->showToast(tr::ayu_AiTuneNotEnoughMessages(tr::now));
		return;
	}
	auto tuned = TeleForge::BuildPersonalityCoreFromMessages(*texts);
	auto p = TeleForge::LoadPersonalityCore().value_or(
		TeleForge::DefaultPersonalityCore());
	p.weights = tuned.weights;
	if (!tuned.systemPrompt.isEmpty()) {
		p.systemPrompt = tuned.systemPrompt;
	}
	p.updatedAt = QDateTime::currentDateTimeUtc();
	TeleForge::PersistPersonalityCore(p);
	controller->showToast(tr::ayu_AiTuneDone(tr::now));
}

// Pulls the account owner's own (outgoing) messages for the given peer from
// the server, page by page, so style tuning does not depend on how far the
// chat view happens to be scrolled. Mirrors the paging used by the
// "delete my messages" feature.
void AutoTuneStyleFromServer(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto session = &peer->session();
	auto texts = std::make_shared<std::vector<QString>>();
	const auto requestNext = std::make_shared<Fn<void(MsgId)>>();

	*requestNext = [=](MsgId from) {
		using Flag = MTPmessages_Search::Flag;
		session->api().request(MTPmessages_Search(
			MTP_flags(Flag::f_from_id),
			peer->input(),
			MTP_string(),
			MTP_inputPeerSelf(),
			MTPInputPeer(),
			MTPVector<MTPReaction>(),
			MTP_int(0), // top_msg_id
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(from.bare), // offset_id
			MTP_int(0), // add_offset
			MTP_int(100), // limit
			MTP_int(0), // max_id
			MTP_int(0), // min_id
			MTP_long(0)) // hash
		).done([=](const Api::HistoryRequestResult &result) {
			auto parsed = Api::ParseHistoryResult(
				peer,
				from,
				Data::LoadDirection::Before,
				result);
			auto minId = MsgId();
			for (const auto &id : parsed.messageIds) {
				if (!minId || id < minId) {
					minId = id;
				}
				const auto item = session->data().message(peer->id, id);
				if (!item || !item->out() || !item->isRegular()) {
					continue;
				}
				const auto t = item->originalText().text.trimmed();
				if (t.size() >= 8) {
					texts->push_back(t);
				}
			}
			if (parsed.messageIds.size() == 100
				&& minId
				&& texts->size() < 300) {
				(*requestNext)(minId - MsgId(1));
			} else {
				ApplyTunedStyle(texts, controller);
			}
		}).fail([=](const MTP::Error &error) {
			ApplyTunedStyle(texts, controller);
		}).send();
	};

	controller->showToast(tr::ayu_AiTuneAnalyzing(tr::now));
	(*requestNext)(MsgId(0));
}

void UpsertGlobalDefaults(
		TeleForge::Storage::PerChatSettingsRecord r,
		not_null<Window::SessionController*> controller) {
	r.peerId = TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId;
	r.updatedAt = base::unixtime::now();
	TeleForge::Storage::upsertPerChatSettings(r);
	controller->showToast(tr::ayu_AiDefaultsSaved(tr::now));
}

void ShowStringPickMenu(
		QWidget *parent,
		QPoint globalPos,
		const QStringList &items,
		Fn<void(const QString &)> onPick) {
	if (!parent || items.isEmpty()) {
		return;
	}
	const auto menu = Ui::CreateChild<Ui::PopupMenu>(parent, st::defaultPopupMenu);
	for (const auto &s : items) {
		menu->addAction(s, [=] {
			onPick(s);
			menu->hideMenu();
		});
	}
	menu->deleteOnHide(true);
	menu->popup(globalPos);
}

void BuildAiTranslation(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	auto *settings = &AyuSettings::getInstance();

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto core = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			const auto localPath = core.chatModelPath.trimmed();
			const auto modelId = core.chatModelId.trimmed();
			const auto endpoint = core.lmStudioBaseUrl.trimmed().isEmpty()
				? u"http://127.0.0.1:1234"_q
				: core.lmStudioBaseUrl.trimmed();
			auto model = QString();
			if (!localPath.isEmpty() && QFileInfo::exists(localPath)) {
				model = QFileInfo(localPath).fileName();
			} else if (!modelId.isEmpty()) {
				model = modelId;
			}
			const auto configured = !model.isEmpty()
				|| !core.lmStudioBaseUrl.trimmed().isEmpty();
			auto status = QString();
			if (!configured) {
				status = tr::ayu_AiTranslationModelMissing(tr::now);
			} else {
				const auto name = model.isEmpty()
					? tr::ayu_AiLocalModel(tr::now)
					: model;
				status = Ayu::Translator::AiTranslationAvailable()
					? tr::ayu_AiTranslationStatus(
						tr::now,
						lt_model,
						name,
						lt_endpoint,
						endpoint)
					: tr::ayu_AiTranslationStatusInactive(
						tr::now,
						lt_model,
						name,
						lt_endpoint,
						endpoint);
			}
			AddSettingsHint(c, rpl::single(status));
		}, [](const SearchContext &) {});
	});

	ayu.addToggle({
		.id = u"ayu/aiTranslationEnabled"_q,
		.title = tr::ayu_AiTranslationEnabled(),
		.getter = [=] {
			return settings->aiTranslationEnabled();
		},
		.setter = [=](bool enabled) {
			settings->setAiTranslationEnabled(enabled);
		},
	});
	ayu.addToggle({
		.id = u"ayu/aiCompressionEnabled"_q,
		.title = tr::ayu_AiCompressionEnabled(),
		.getter = [=] {
			return settings->aiCompressionEnabled();
		},
		.setter = [=](bool enabled) {
			settings->setAiCompressionEnabled(enabled);
		},
	});
	builder.addSkip();
	builder.addDividerText(tr::ayu_AiTranslationEnabledDescription());
}

const auto kMeta = BuildHelper({
	.id = TeleForgeAi::Id(),
	.parentId = AyuMain::Id(),
	.title = &tr::ayu_CategoryTeleForgeAi,
	.icon = &st::menuIconIpAddress,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	const auto controller = builder.controller();

	builder.addSkip();
	builder.addSubsectionTitle(tr::ayu_AiChatSection());

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto core = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());

			TeleForge::TeleForgeEnsureModelsDirectoryExists();

			const auto lm = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(u"https://api.openai.com"_q),
					TextWithTags{
						core.lmStudioBaseUrl.isEmpty()
							? u"http://127.0.0.1:1234"_q
							: core.lmStudioBaseUrl,
					}),
				st::boxRowPadding);

			const auto pickProvider = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiProviderButton(),
					st::settingsButtonNoIcon));
			pickProvider->setClickedCallback([=] {
				const auto items = QStringList{
					u"OpenAI (https://api.openai.com)"_q,
					u"LM Studio (http://127.0.0.1:1234)"_q,
					u"Ollama (http://127.0.0.1:11434)"_q,
					tr::ayu_AiOwnUrl(tr::now),
				};
				ShowStringPickMenu(
					c,
					pickProvider->mapToGlobal(
						QPoint(0, pickProvider->height())),
					items,
					[=](const QString &choice) {
						if (choice.startsWith(u"OpenAI"_q)) {
							lm->setText(u"https://api.openai.com"_q);
						} else if (choice.startsWith(u"LM Studio"_q)) {
							lm->setText(u"http://127.0.0.1:1234"_q);
						} else if (choice.startsWith(u"Ollama"_q)) {
							lm->setText(u"http://127.0.0.1:11434"_q);
						}
					});
			});

			const auto apiKey = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(u"sk-…"_q),
					TextWithTags{ core.openAiApiKey }),
				st::boxRowPadding);

			AddSettingsHint(c, tr::ayu_AiChatModel());

			const auto chatModelIdField = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.chatModelId }),
				st::boxRowPadding);

			const auto loadChatModels = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiLoadChatModels(),
					st::settingsButtonNoIcon));
			loadChatModels->setClickedCallback([=] {
				const auto base = TeleForge::OpenAiOriginBaseForModelsList(
					lm->getLastText());
				TeleForge::FetchOpenAiModelIdsAsync(
					base,
					[=](QStringList ids, QString error) {
						if (!error.isEmpty()) {
							controller->showToast(error);
							return;
						}
						ShowStringPickMenu(
							c,
							loadChatModels->mapToGlobal(
								QPoint(0, loadChatModels->height())),
							ids,
							[=](const QString &id) { chatModelIdField->setText(id); });
					},
					apiKey->getLastText());
			});

			AddSettingsHint(c, tr::ayu_AiLocalGguf());

			const auto chatGguf = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.chatModelPath }),
				st::boxRowPadding);

			const auto browseChatGguf = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiBrowseChatModel(),
					st::settingsButtonNoIcon));
			browseChatGguf->setClickedCallback([=] {
				FileDialog::GetOpenPath(
					Core::App().getFileDialogParent(),
					tr::ayu_AiChatModelGgufDialog(tr::now),
					tr::ayu_GgufFilter(tr::now),
					[=](const FileDialog::OpenResult &result) {
						if (!result.paths.isEmpty()) {
							chatGguf->setText(result.paths.front());
						}
					});
			});

			const auto pickChatFromModelsDir = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiPickChatFromModels(),
					st::settingsButtonNoIcon));
			pickChatFromModelsDir->setClickedCallback([=] {
				const auto items = TeleForge::TeleForgeListGgufInModelsDirectory();
				if (items.isEmpty()) {
					controller->showToast(tr::ayu_AiNoGgufInModels(tr::now));
					return;
				}
				ShowStringPickMenu(
					c,
					pickChatFromModelsDir->mapToGlobal(
						QPoint(0, pickChatFromModelsDir->height())),
					items,
					[=](const QString &p) { chatGguf->setText(p); });
			});

			AddSettingsHint(c, tr::ayu_AiContextMessages());

			const auto ctxMsgs = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{
						QString::number(
							core.chatContextMessages > 0 ? core.chatContextMessages : 40),
					}),
				st::boxRowPadding);

			builder.addSubsectionTitle(tr::ayu_AiEmbeddingsSection());

			AddSettingsHint(c, tr::ayu_AiEmbeddingUrl());

			const auto emb = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.embeddingEndpointUrl }),
				st::boxRowPadding);

			AddSettingsHint(c, tr::ayu_AiEmbeddingModel());

			const auto embModel = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.embeddingModelId }),
				st::boxRowPadding);

			const auto loadEmbModels = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiLoadEmbeddingModels(),
					st::settingsButtonNoIcon));
			loadEmbModels->setClickedCallback([=] {
				const auto base = TeleForge::OpenAiOriginBaseForModelsList(
					emb->getLastText());
				TeleForge::FetchOpenAiModelIdsAsync(
					base,
					[=](QStringList ids, QString error) {
						if (!error.isEmpty()) {
							controller->showToast(error);
							return;
						}
						ShowStringPickMenu(
							c,
							loadEmbModels->mapToGlobal(
								QPoint(0, loadEmbModels->height())),
							ids,
							[=](const QString &id) { embModel->setText(id); });
					},
					apiKey->getLastText());
			});

			builder.addSubsectionTitle(tr::ayu_AiRerankSection());

			AddSettingsHint(c, tr::ayu_AiRerankUrl());

			const auto rerankUrl = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.rerankEndpointUrl }),
				st::boxRowPadding);

			AddSettingsHint(c, tr::ayu_AiRerankModel());

			const auto rerankModel = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.rerankModelId }),
				st::boxRowPadding);

			const auto loadRerankModels = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiLoadRerankModels(),
					st::settingsButtonNoIcon));
			loadRerankModels->setClickedCallback([=] {
				const auto base = TeleForge::OpenAiOriginBaseForModelsList(
					rerankUrl->getLastText());
				TeleForge::FetchOpenAiModelIdsAsync(
					base,
					[=](QStringList ids, QString error) {
						if (!error.isEmpty()) {
							controller->showToast(error);
							return;
						}
						ShowStringPickMenu(
							c,
							loadRerankModels->mapToGlobal(
								QPoint(0, loadRerankModels->height())),
							ids,
							[=](const QString &id) { rerankModel->setText(id); });
					},
					apiKey->getLastText());
			});

			AddSettingsHint(c, tr::ayu_AiRerankGguf());

			const auto defaultGguf = TeleForge::DefaultTeleForgeRerankGgufPath();
			const auto rerankPath = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{
						core.rerankModelPath.isEmpty()
							? defaultGguf
							: core.rerankModelPath,
					}),
				st::boxRowPadding);

			const auto browseRerankGguf = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiBrowseRerank(),
					st::settingsButtonNoIcon));
			browseRerankGguf->setClickedCallback([=] {
				FileDialog::GetOpenPath(
					Core::App().getFileDialogParent(),
					tr::ayu_AiRerankGgufDialog(tr::now),
					tr::ayu_GgufFilter(tr::now),
					[=](const FileDialog::OpenResult &result) {
						if (!result.paths.isEmpty()) {
							rerankPath->setText(result.paths.front());
						}
					});
			});

			const auto pickRerankFromModelsDir = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiPickRerankFromModels(),
					st::settingsButtonNoIcon));
			pickRerankFromModelsDir->setClickedCallback([=] {
				const auto items = TeleForge::TeleForgeListGgufInModelsDirectory();
				if (items.isEmpty()) {
					controller->showToast(tr::ayu_AiNoGgufInModels(tr::now));
					return;
				}
				ShowStringPickMenu(
					c,
					pickRerankFromModelsDir->mapToGlobal(
						QPoint(0, pickRerankFromModelsDir->height())),
					items,
					[=](const QString &p) { rerankPath->setText(p); });
			});

			AddSettingsHint(c, tr::ayu_AiEmbeddingGguf());

			const auto embGguf = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{
						core.embeddingModelPath.isEmpty()
							? TeleForge::DefaultTeleForgeEmbeddingGgufPath()
							: core.embeddingModelPath,
					}),
				st::boxRowPadding);

			const auto browseEmbGguf = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiBrowseEmbedding(),
					st::settingsButtonNoIcon));
			browseEmbGguf->setClickedCallback([=] {
				FileDialog::GetOpenPath(
					Core::App().getFileDialogParent(),
					tr::ayu_AiEmbeddingGgufDialog(tr::now),
					tr::ayu_GgufFilter(tr::now),
					[=](const FileDialog::OpenResult &result) {
						if (!result.paths.isEmpty()) {
							embGguf->setText(result.paths.front());
						}
					});
			});

			const auto downloadEmbGguf = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiDownloadModel(),
					st::settingsButtonNoIcon));
			downloadEmbGguf->setClickedCallback([=] {
				auto titles = QStringList();
				for (const auto &entry : TeleForge::ModelCatalog()) {
					titles.push_back(TeleForge::ModelDownloaded(entry)
						? tr::ayu_AiModelAlreadyDownloaded(
							tr::now,
							lt_title,
							entry.title)
						: entry.title);
				}
				ShowStringPickMenu(
					c,
					downloadEmbGguf->mapToGlobal(
						QPoint(0, downloadEmbGguf->height())),
					titles,
					[=](const QString &picked) {
						for (const auto &entry : TeleForge::ModelCatalog()) {
							if (!picked.startsWith(entry.title)) {
								continue;
							}
							if (TeleForge::ModelDownloaded(entry)) {
								embGguf->setText(
									TeleForge::ModelLocalPath(entry));
								return;
							}
							TeleForge::ShowModelDownloadBox(entry);
							return;
						}
					});
			});

			const auto save = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiSaveConnection(),
					st::settingsButtonNoIcon));
			const auto persistConnection = [=] {
				auto p = TeleForge::LoadPersonalityCore().value_or(
					TeleForge::DefaultPersonalityCore());
				p.lmStudioBaseUrl = lm->getLastText().trimmed();
				p.openAiApiKey = apiKey->getLastText().trimmed();
				p.chatModelId = chatModelIdField->getLastText().trimmed();
				p.chatModelPath = chatGguf->getLastText().trimmed();
				p.chatContextMessages = ParseIntClamped(
					ctxMsgs->getLastText(),
					4,
					128,
					40);
				const auto embParsed = TeleForge::ParseOpenAiEndpointUrl(
					emb->getLastText(),
					embModel->getLastText());
				const auto rerankParsed = TeleForge::ParseOpenAiEndpointUrl(
					rerankUrl->getLastText(),
					rerankModel->getLastText());
				p.embeddingEndpointUrl = embParsed.url;
				p.embeddingModelId = embParsed.modelId;
				p.rerankEndpointUrl = rerankParsed.url;
				p.rerankModelId = rerankParsed.modelId;
				p.rerankModelPath = rerankPath->getLastText().trimmed();
				p.embeddingModelPath = embGguf->getLastText().trimmed();
				p.updatedAt = QDateTime::currentDateTimeUtc();
				TeleForge::PersistPersonalityCore(p);
			};
			for (const auto field : {
					lm, apiKey, chatModelIdField, chatGguf, ctxMsgs,
					emb, embModel, rerankUrl, rerankModel, rerankPath,
					embGguf }) {
				field->focusedChanges(
				) | rpl::on_next([=](bool focused) {
					if (!focused) {
						persistConnection();
					}
				}, field->lifetime());
			}
			save->setClickedCallback([=] {
				persistConnection();
				controller->showToast(tr::ayu_AiConnectionSaved(tr::now));
			});

		}, [](const SearchContext &) {});
	});

	ayu.addSectionDivider();
	builder.addSubsectionTitle(tr::ayu_AiDeliverySection());
	ayu.addToggle({
		.id = u"teleforge/autoSend"_q,
		.title = tr::ayu_AiAutoSend(),
		.getter = [] {
			return TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore()).autoSendEnabled;
		},
		.setter = [](bool v) {
			auto p = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			p.autoSendEnabled = v;
			p.updatedAt = QDateTime::currentDateTimeUtc();
			TeleForge::PersistPersonalityCore(p);
		},
	});
	ayu.addToggle({
		.id = u"teleforge/vision"_q,
		.title = tr::ayu_AiVision(),
		.getter = [] {
			return TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore()).visionEnabled;
		},
		.setter = [](bool v) {
			auto p = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			p.visionEnabled = v;
			p.updatedAt = QDateTime::currentDateTimeUtc();
			TeleForge::PersistPersonalityCore(p);
		},
	});
	builder.addSkip();

	ayu.addSectionDivider();
	builder.addSubsectionTitle(tr::ayu_AiTranslationSection());
	BuildAiTranslation(builder, ayu);

	ayu.addSectionDivider();
	builder.addSubsectionTitle(tr::ayu_AiSystemPromptSection());
	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto core = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			const auto prompt = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::MultiLine,
					rpl::single(QString()),
					TextWithTags{ core.systemPrompt }),
				st::boxRowPadding);
			prompt->setMaxHeight(st::defaultInputField.heightMin * 8);
			const auto save = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiSaveSystemPrompt(),
					st::settingsButtonNoIcon));
			const auto persistPrompt = [=] {
				auto p = TeleForge::LoadPersonalityCore().value_or(
					TeleForge::DefaultPersonalityCore());
				p.systemPrompt = prompt->getLastText();
				p.updatedAt = QDateTime::currentDateTimeUtc();
				TeleForge::PersistPersonalityCore(p);
			};
			prompt->focusedChanges(
			) | rpl::on_next([=](bool focused) {
				if (!focused) {
					persistPrompt();
				}
			}, prompt->lifetime());
			save->setClickedCallback([=] {
				persistPrompt();
				controller->showToast(tr::ayu_AiSystemPromptSaved(tr::now));
			});
		}, [](const SearchContext &) {});
	});

	ayu.addSectionDivider();
	builder.addSubsectionTitle(tr::ayu_AiStyleSection());

	ayu.addSlider({
		.id = u"teleforge/weight_aggression"_q,
		.title = tr::ayu_AiStyleAggression(),
		.steps = 11,
		.current = int(std::round(
			TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore()).weights.aggression)),
		.onFinalChanged = [=](int v) {
			auto p = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			p.weights.aggression = static_cast<double>(v);
			p.updatedAt = QDateTime::currentDateTimeUtc();
			TeleForge::PersistPersonalityCore(p);
		},
		.formatLabel = [](int x) { return QString::number(x); },
	});
	ayu.addSlider({
		.id = u"teleforge/weight_brevity"_q,
		.title = tr::ayu_AiStyleBrevity(),
		.steps = 11,
		.current = int(std::round(
			TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore()).weights.brevity)),
		.onFinalChanged = [=](int v) {
			auto p = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			p.weights.brevity = static_cast<double>(v);
			p.updatedAt = QDateTime::currentDateTimeUtc();
			TeleForge::PersistPersonalityCore(p);
		},
		.formatLabel = [](int x) { return QString::number(x); },
	});
	ayu.addSlider({
		.id = u"teleforge/weight_emojis"_q,
		.title = tr::ayu_AiStyleEmojis(),
		.steps = 11,
		.current = int(std::round(
			TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore()).weights.emojis)),
		.onFinalChanged = [=](int v) {
			auto p = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			p.weights.emojis = static_cast<double>(v);
			p.updatedAt = QDateTime::currentDateTimeUtc();
			TeleForge::PersistPersonalityCore(p);
		},
		.formatLabel = [](int x) { return QString::number(x); },
	});
	ayu.addSlider({
		.id = u"teleforge/weight_toxicity"_q,
		.title = tr::ayu_AiStyleToxicity(),
		.steps = 11,
		.current = int(std::round(
			TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore()).weights.toxicity)),
		.onFinalChanged = [=](int v) {
			auto p = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			p.weights.toxicity = static_cast<double>(v);
			p.updatedAt = QDateTime::currentDateTimeUtc();
			TeleForge::PersistPersonalityCore(p);
		},
		.formatLabel = [](int x) { return QString::number(x); },
	});
	ayu.addSlider({
		.id = u"teleforge/weight_creativity"_q,
		.title = tr::ayu_AiStyleCreativity(),
		.steps = 11,
		.current = int(std::round(
			TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore()).weights.creativity)),
		.onFinalChanged = [=](int v) {
			auto p = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			p.weights.creativity = static_cast<double>(v);
			p.updatedAt = QDateTime::currentDateTimeUtc();
			TeleForge::PersistPersonalityCore(p);
		},
		.formatLabel = [](int x) { return QString::number(x); },
	});

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto autoTune = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiTuneStyle(),
					st::settingsButtonNoIcon));
			autoTune->setClickedCallback([=] {
				const auto history =
						controller->activeChatCurrent().owningHistory();
					if (!history) {
						controller->showToast(tr::ayu_AiOpenChatToAnalyze(tr::now));
						return;
					}
					AutoTuneStyleFromServer(controller, history->peer);
			});
		}, [](const SearchContext &) {});
	});

	ayu.addSectionDivider();
	builder.addSubsectionTitle(tr::ayu_AiDefaultsSection());

	ayu.addToggle({
		.id = u"teleforge/defaultAiAnswer"_q,
		.title = tr::ayu_AiAllowAnswers(),
		.getter = [=] {
			return TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId).aiAnswer;
		},
		.setter = [=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId);
			p.aiAnswer = v;
			UpsertGlobalDefaults(p, controller);
		},
	});
	ayu.addToggle({
		.id = u"teleforge/defaultMemRead"_q,
		.title = tr::ayu_AiMemoryRead(),
		.getter = [=] {
			return TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId).memoryReadEnabled;
		},
		.setter = [=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId);
			p.memoryReadEnabled = v;
			UpsertGlobalDefaults(p, controller);
		},
	});
	ayu.addToggle({
		.id = u"teleforge/defaultMemWrite"_q,
		.title = tr::ayu_AiMemoryWrite(),
		.getter = [=] {
			return TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId).memoryWriteEnabled;
		},
		.setter = [=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId);
			p.memoryWriteEnabled = v;
			UpsertGlobalDefaults(p, controller);
		},
	});
	ayu.addToggle({
		.id = u"teleforge/defaultWeb"_q,
		.title = tr::ayu_AiWebAccess(),
		.getter = [=] {
			return TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId).webAccess;
		},
		.setter = [=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId);
			p.webAccess = v;
			UpsertGlobalDefaults(p, controller);
		},
	});
	ayu.addToggle({
		.id = u"teleforge/defaultCal"_q,
		.title = tr::ayu_AiCalendarAccess(),
		.getter = [=] {
			return TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId).calendarAccess;
		},
		.setter = [=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId);
			p.calendarAccess = v;
			UpsertGlobalDefaults(p, controller);
		},
	});
	ayu.addToggle({
		.id = u"teleforge/defaultPc"_q,
		.title = tr::ayu_AiPcAgent(),
		.getter = [=] {
			return TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId).pcAgent;
		},
		.setter = [=](bool v) {
			auto p = TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId);
			p.pcAgent = v;
			UpsertGlobalDefaults(p, controller);
		},
	});

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto r = TeleForge::Storage::effectivePerChatSettings(
				TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId);

			auto addNumRow = [&](const QString &label, const QString &value) {
				AddSettingsHint(c, rpl::single(label));
				return c->add(
					object_ptr<Ui::InputField>(
						c,
						st::defaultInputField,
						Ui::InputField::Mode::SingleLine,
						rpl::single(QString()),
						TextWithTags{ value }),
					st::boxRowPadding);
			};

			const auto fGk = addNumRow(
				tr::ayu_AiGlobalMemoryTopK(tr::now),
				QString::number(r.globalMemoryTopK));
			const auto fCk = addNumRow(
				tr::ayu_AiChatMemoryTopK(tr::now),
				QString::number(r.chatMemoryTopK));
			const auto fUk = addNumRow(
				tr::ayu_AiUserMemoryTopK(tr::now),
				QString::number(r.userMemoryTopK));
			const auto fAk = addNumRow(
				tr::ayu_AiMemoryAppendixTopK(tr::now),
				QString::number(r.memoryAppendixTopK));
			const auto fRx = addNumRow(
				tr::ayu_AiRecentSummariesLimit(tr::now),
				QString::number(r.recentSummaryLimit));
			const auto fSy = addNumRow(
				tr::ayu_AiStableFactsLimit(tr::now),
				QString::number(r.stableFactsLimit));
			const auto fDec = addNumRow(
				tr::ayu_AiMemoryDecay(tr::now),
				QString::number(r.memoryDecayPerDay, 'g', 4));
			const auto fMax = addNumRow(
				tr::ayu_AiMaxMemoryBlockChars(tr::now),
				QString::number(r.maxMemoryBlockChars));

			AddSettingsHint(c, tr::ayu_AiDirectoryWhitelist());
			const auto fWl = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::MultiLine,
					rpl::single(QString()),
					TextWithTags{ QString::fromStdString(r.directoryWhitelistJson) }),
				st::boxRowPadding);
			fWl->setMaxHeight(st::defaultInputField.heightMin * 6);

			const auto saveNums = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_AiSaveNumeric(),
					st::settingsButtonNoIcon));
			saveNums->setClickedCallback([=] {
				auto next = TeleForge::Storage::effectivePerChatSettings(
					TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId);
				next.globalMemoryTopK = ParseIntClamped(
					fGk->getLastText(),
					1,
					64,
					next.globalMemoryTopK);
				next.chatMemoryTopK = ParseIntClamped(
					fCk->getLastText(),
					1,
					64,
					next.chatMemoryTopK);
				next.userMemoryTopK = ParseIntClamped(
					fUk->getLastText(),
					1,
					32,
					next.userMemoryTopK);
				next.memoryAppendixTopK = ParseIntClamped(
					fAk->getLastText(),
					0,
					64,
					next.memoryAppendixTopK);
				next.recentSummaryLimit = ParseIntClamped(
					fRx->getLastText(),
					0,
					64,
					next.recentSummaryLimit);
				next.stableFactsLimit = ParseIntClamped(
					fSy->getLastText(),
					0,
					64,
					next.stableFactsLimit);
				next.memoryDecayPerDay = std::max(
					0.,
					ParseDouble(fDec->getLastText(), next.memoryDecayPerDay));
				next.maxMemoryBlockChars = ParseIntClamped(
					fMax->getLastText(),
					256,
					500000,
					next.maxMemoryBlockChars);
				next.directoryWhitelistJson = fWl->getLastText().trimmed().toStdString();
				UpsertGlobalDefaults(next, controller);
			});
		}, [](const SearchContext &) {});
	});

	builder.addSkip();
});

} // namespace

rpl::producer<QString> TeleForgeAi::title() {
	return tr::ayu_CategoryTeleForgeAi();
}

TeleForgeAi::TeleForgeAi(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void TeleForgeAi::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type TeleForgeAiId() {
	return TeleForgeAi::Id();
}

} // namespace Settings
