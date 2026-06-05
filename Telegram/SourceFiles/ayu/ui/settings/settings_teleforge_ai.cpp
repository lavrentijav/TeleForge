// This is the source code of AyuGram for Desktop.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_teleforge_ai.h"

#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_openai_models.h"
#include "ayu/features/teleforge/teleforge_paths.h"
#include "ayu/features/teleforge/teleforge_rerank.h"
#include "ayu/features/teleforge/teleforge_storage.h"
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

#include "core/application.h"
#include "core/file_utilities.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "settings.h"

#include <QtCore/QDateTime>
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

void UpsertGlobalDefaults(
		TeleForge::Storage::PerChatSettingsRecord r,
		not_null<Window::SessionController*> controller) {
	r.peerId = TeleForge::Storage::kTeleForgeGlobalDefaultsPeerId;
	r.updatedAt = base::unixtime::now();
	TeleForge::Storage::upsertPerChatSettings(r);
	controller->showToast(u"Значения по умолчанию для чатов сохранены."_q);
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
	.title = u"Настройки ИИ"_q,
	.icon = &st::menuIconIpAddress,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	const auto controller = builder.controller();

	builder.addSkip();
	builder.addSubsectionTitle(rpl::single(u"Чат (LLM)"_q));

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
					rpl::single(u"Провайдер API…"_q),
					st::settingsButtonNoIcon));
			pickProvider->setClickedCallback([=] {
				const auto items = QStringList{
					u"OpenAI (https://api.openai.com)"_q,
					u"LM Studio (http://127.0.0.1:1234)"_q,
					u"Ollama (http://127.0.0.1:11434)"_q,
					u"Свой URL"_q,
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

			AddSettingsHint(c, rpl::single(u"Модель чата"_q));

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
					rpl::single(u"Загрузить список моделей (чат)"_q),
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

			AddSettingsHint(c, rpl::single(u"Локальный .gguf (пусто — только API)"_q));

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
					rpl::single(u"Выбрать файл модели чата (.gguf)…"_q),
					st::settingsButtonNoIcon));
			browseChatGguf->setClickedCallback([=] {
				FileDialog::GetOpenPath(
					Core::App().getFileDialogParent(),
					u"Модель чата (GGUF)"_q,
					u"GGUF (*.gguf);;All files (*.*)"_q,
					[=](const FileDialog::OpenResult &result) {
						if (!result.paths.isEmpty()) {
							chatGguf->setText(result.paths.front());
						}
					});
			});

			const auto pickChatFromModelsDir = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					rpl::single(u"Выбрать .gguf из models/…"_q),
					st::settingsButtonNoIcon));
			pickChatFromModelsDir->setClickedCallback([=] {
				const auto items = TeleForge::TeleForgeListGgufInModelsDirectory();
				if (items.isEmpty()) {
					controller->showToast(u"В каталоге models нет .gguf"_q);
					return;
				}
				ShowStringPickMenu(
					c,
					pickChatFromModelsDir->mapToGlobal(
						QPoint(0, pickChatFromModelsDir->height())),
					items,
					[=](const QString &p) { chatGguf->setText(p); });
			});

			AddSettingsHint(c, rpl::single(u"Сообщений в контексте"_q));

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

			builder.addSubsectionTitle(rpl::single(u"Эмбеддинги (память)"_q));

			AddSettingsHint(c, rpl::single(u"URL эмбеддингов"_q));

			const auto emb = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.embeddingEndpointUrl }),
				st::boxRowPadding);

			AddSettingsHint(c, rpl::single(u"Модель эмбеддингов"_q));

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
					rpl::single(u"Загрузить список моделей (эмбеддинги)"_q),
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

			builder.addSubsectionTitle(rpl::single(u"Реранкер"_q));

			AddSettingsHint(c, rpl::single(u"URL реранкера"_q));

			const auto rerankUrl = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.rerankEndpointUrl }),
				st::boxRowPadding);

			AddSettingsHint(c, rpl::single(u"Модель реранкера"_q));

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
					rpl::single(u"Загрузить список моделей (реранкер)"_q),
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

			AddSettingsHint(c, rpl::single(u"Локальный GGUF реранкера"_q));

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
					rpl::single(u"Выбрать файл реранкера (.gguf)…"_q),
					st::settingsButtonNoIcon));
			browseRerankGguf->setClickedCallback([=] {
				FileDialog::GetOpenPath(
					Core::App().getFileDialogParent(),
					u"Модель реранкера (GGUF)"_q,
					u"GGUF (*.gguf);;All files (*.*)"_q,
					[=](const FileDialog::OpenResult &result) {
						if (!result.paths.isEmpty()) {
							rerankPath->setText(result.paths.front());
						}
					});
			});

			const auto pickRerankFromModelsDir = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					rpl::single(u"Выбрать реранкер .gguf из models/…"_q),
					st::settingsButtonNoIcon));
			pickRerankFromModelsDir->setClickedCallback([=] {
				const auto items = TeleForge::TeleForgeListGgufInModelsDirectory();
				if (items.isEmpty()) {
					controller->showToast(u"В каталоге models нет .gguf"_q);
					return;
				}
				ShowStringPickMenu(
					c,
					pickRerankFromModelsDir->mapToGlobal(
						QPoint(0, pickRerankFromModelsDir->height())),
					items,
					[=](const QString &p) { rerankPath->setText(p); });
			});

			const auto save = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					rpl::single(u"Сохранить подключение"_q),
					st::settingsButtonNoIcon));
			save->setClickedCallback([=] {
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
				p.updatedAt = QDateTime::currentDateTimeUtc();
				TeleForge::PersistPersonalityCore(p);
				controller->showToast(u"Подключение сохранено."_q);
			});

		}, [](const SearchContext &) {});
	});

	ayu.addSectionDivider();
	builder.addSubsectionTitle(rpl::single(u"Перевод через ИИ"_q));
	BuildAiTranslation(builder, ayu);

	ayu.addSectionDivider();
	builder.addSubsectionTitle(rpl::single(u"Системный промпт"_q));
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
					rpl::single(u"Сохранить системный промпт"_q),
					st::settingsButtonNoIcon));
			save->setClickedCallback([=] {
				auto p = TeleForge::LoadPersonalityCore().value_or(
					TeleForge::DefaultPersonalityCore());
				p.systemPrompt = prompt->getLastText();
				p.updatedAt = QDateTime::currentDateTimeUtc();
				TeleForge::PersistPersonalityCore(p);
				controller->showToast(u"Системный промпт сохранён."_q);
			});
		}, [](const SearchContext &) {});
	});

	ayu.addSectionDivider();
	builder.addSubsectionTitle(rpl::single(u"Стиль ответов (0–10)"_q));

	ayu.addSlider({
		.id = u"teleforge/weight_aggression"_q,
		.title = rpl::single(u"Нажим / жёсткость"_q),
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
		.title = rpl::single(u"Краткость"_q),
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
		.title = rpl::single(u"Эмодзи"_q),
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
		.title = rpl::single(u"Резкость / мат"_q),
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
		.title = rpl::single(u"Креативность"_q),
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
					rpl::single(u"Подстроить стиль по моим сообщениям (текущий чат)"_q),
					st::settingsButtonNoIcon));
			autoTune->setClickedCallback([=] {
				auto texts = std::vector<QString>();
				const auto history = controller->activeChatCurrent().owningHistory();
				if (!history) {
					controller->showToast(u"Откройте чат для анализа."_q);
					return;
				}
				for (const auto &block : history->blocks) {
					for (const auto &view : block->messages) {
						const auto item = view->data();
						if (!item->out() || !item->isRegular()) {
							continue;
						}
						const auto t = item->originalText().text.trimmed();
						if (t.size() >= 8) {
							texts.push_back(t);
						}
						if (texts.size() >= 200) {
							break;
						}
					}
					if (texts.size() >= 200) {
						break;
					}
				}
				if (texts.size() < 5) {
					controller->showToast(
						u"Нужно хотя бы 5 исходящих сообщений в чате."_q);
					return;
				}
				auto tuned = TeleForge::BuildPersonalityCoreFromMessages(texts);
				auto p = TeleForge::LoadPersonalityCore().value_or(
					TeleForge::DefaultPersonalityCore());
				p.weights = tuned.weights;
				if (!tuned.systemPrompt.isEmpty()) {
					p.systemPrompt = tuned.systemPrompt;
				}
				p.updatedAt = QDateTime::currentDateTimeUtc();
				TeleForge::PersistPersonalityCore(p);
				controller->showToast(u"Стиль ответов обновлён по вашим сообщениям."_q);
			});
		}, [](const SearchContext &) {});
	});

	ayu.addSectionDivider();
	builder.addSubsectionTitle(
		rpl::single(
			u"По умолчанию для чатов (если у диалога нет своих настроек в БД)"_q));

	ayu.addToggle({
		.id = u"teleforge/defaultAiAnswer"_q,
		.title = rpl::single(u"Разрешить ответы ИИ (Ctrl+Shift+M)"_q),
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
		.title = rpl::single(u"Читать память в промпт"_q),
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
		.title = rpl::single(u"Записывать память из сообщений"_q),
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
		.title = rpl::single(u"Доступ в интернет (заготовка)"_q),
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
		.title = rpl::single(u"Доступ к календарю (заготовка)"_q),
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
		.title = rpl::single(u"Агент ПК (заготовка)"_q),
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
				u"Глобальная память, top-K"_q,
				QString::number(r.globalMemoryTopK));
			const auto fCk = addNumRow(
				u"Память чата, top-K"_q,
				QString::number(r.chatMemoryTopK));
			const auto fUk = addNumRow(
				u"Память пользователя, top-K"_q,
				QString::number(r.userMemoryTopK));
			const auto fAk = addNumRow(
				u"Семантическое приложение, top-K"_q,
				QString::number(r.memoryAppendixTopK));
			const auto fRx = addNumRow(
				u"Число сводок (X)"_q,
				QString::number(r.recentSummaryLimit));
			const auto fSy = addNumRow(
				u"Число стабильных фактов (Y)"_q,
				QString::number(r.stableFactsLimit));
			const auto fDec = addNumRow(
				u"Линейное затухание в день"_q,
				QString::number(r.memoryDecayPerDay, 'g', 4));
			const auto fMax = addNumRow(
				u"Max chars на блок памяти в промпте"_q,
				QString::number(r.maxMemoryBlockChars));

			AddSettingsHint(c, rpl::single(u"Белый список папок (JSON)"_q));
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
					rpl::single(
						u"Сохранить числовые параметры и whitelist"_q),
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
	return rpl::single(u"Настройки ИИ"_q);
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
