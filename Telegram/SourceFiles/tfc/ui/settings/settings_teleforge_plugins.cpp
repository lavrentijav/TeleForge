// Copyright @Radolyn, 2026
#include "tfc/ui/settings/settings_teleforge_plugins.h"

#include "tfc/features/plugins/plugin_catalog.h"
#include "tfc/features/plugins/plugin_catalog_store.h"
#include "tfc/features/plugins/plugin_channel_install.h"
#include "tfc/features/plugins/plugin_dev_mode.h"
#include "tfc/features/plugins/plugin_developers_store.h"
#include "tfc/features/plugins/plugin_manager.h"
#include "tfc/features/plugins/plugin_registry.h"
#include "tfc/features/plugins/plugin_runner.h"
#include "tfc/features/subscription/teleforge_subscription.h"
#include "tfc/ui/settings/tf_builder.h"
#include "tfc/ui/settings/settings_tf_utils.h"
#include "tfc/ui/settings/settings_main.h"
#include "base/timer.h"
#include "base/variant.h"
#include "core/file_utilities.h"
#include "settings.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/layers/box_content.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QDesktopServices>

namespace Settings {

using namespace Builder;
using namespace AyBuilder;

namespace {

[[nodiscard]] not_null<Ui::Checkbox*> AddWrappedSettingsCheckbox(
		not_null<Ui::VerticalLayout*> parent,
		const QString &text,
		const bool checked) {
	const auto row = parent->add(object_ptr<Ui::Checkbox>(
		parent,
		rpl::single(text),
		checked,
		st::settingsCheckbox));
	row->setAllowTextLines();
	row->setTextBreakEverywhere(true);
	return row;
}

void AddHint(not_null<Ui::VerticalLayout*> parent, const QString &text) {
	if (text.isEmpty()) {
		return;
	}
	AddSettingsHint(parent, rpl::single(text));
}

} // namespace

namespace {

void ShowSubscribePrompt(
		not_null<Window::SessionController*> controller) {
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_PluginsSubscribeTitle());
		const auto layout = box->verticalLayout();
		for (const auto &ch : TeleForge::Plugins::Catalog::kSubscribeChannels) {
			const auto username = QString::fromLatin1(ch.username);
			const auto label = tr::ayu_PluginsChannelLabel(
				tr::now,
				lt_title,
				QString::fromLatin1(ch.title),
				lt_username,
				username);
			const auto button = layout->add(
				object_ptr<Ui::SettingsButton>(
					layout,
					rpl::single(label),
					st::settingsButtonNoIcon));
			button->setClickedCallback([=] {
				controller->showPeerByLink(Window::PeerByLinkInfo{
					.usernameOrId = username,
				});
			});
		}
	}));
}

void ShowChannelOffersBox(
		not_null<Window::SessionController*> controller,
		const QVector<TeleForge::Plugins::ChannelPluginOffer> &offers) {
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_PluginsChannelOffersTitle());
		const auto layout = box->verticalLayout();
		for (const auto &offer : offers) {
			const auto label = tr::ayu_PluginsChannelOfferLabel(
				tr::now,
				lt_file,
				offer.fileName,
				lt_channel,
				offer.channelUsername);
			const auto button = layout->add(
				object_ptr<Ui::SettingsButton>(
					layout,
					rpl::single(label),
					st::settingsButtonNoIcon));
			const auto captured = offer;
			button->setClickedCallback([=] {
				box->closeBox();
				TeleForge::Plugins::installChannelOffer(
					&controller->session(),
					captured,
					[=](bool ok, QString message) {
						controller->showToast(message);
					});
			});
		}
		if (offers.isEmpty()) {
			AddSettingsHint(layout, tr::ayu_PluginsNoChannelOffers());
		}
	}));
}

void ShowCatalogInstallBox(
		not_null<Window::SessionController*> controller,
		const QVector<TeleForge::Plugins::CatalogEntry> &entries) {
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_PluginsCatalogTitle());
		const auto layout = box->verticalLayout();
		for (const auto &entry : entries) {
			const auto label = tr::ayu_PluginsCatalogEntryLabel(
				tr::now,
				lt_file,
				entry.fileName,
				lt_title,
				entry.title,
				lt_developer,
				entry.devId.isEmpty() ? entry.sourceLabel : entry.devId);
			const auto button = layout->add(
				object_ptr<Ui::SettingsButton>(
					layout,
					rpl::single(label),
					st::settingsButtonNoIcon));
			const auto captured = entry;
			button->setClickedCallback([=] {
				box->closeBox();
				TeleForge::Plugins::installCatalogEntry(
					captured,
					[=](bool ok, QString message) {
						controller->showToast(message);
					});
			});
		}
		if (entries.isEmpty()) {
			AddSettingsHint(layout, tr::ayu_PluginsCatalogEmpty());
		}
	}));
}

const auto kMeta = BuildHelper({
	.id = TeleForgePlugins::Id(),
	.parentId = AyuMain::Id(),
	.title = &tr::ayu_CategoryTeleForgePlugins,
	.icon = &st::menuIconBot,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	const auto controller = builder.controller();
	const auto pluginsHost = std::make_shared<QPointer<Ui::VerticalLayout>>();
	const auto developersHost = std::make_shared<QPointer<Ui::VerticalLayout>>();
	const auto catalogHost = std::make_shared<QPointer<Ui::VerticalLayout>>();

	// Refresh subscription status and suggest subscribing only when the user
	// is not already subscribed (avoids nagging active subscribers).
	TeleForge::Subscription::refresh(&controller->session());
	if (!TeleForge::Subscription::active()) {
		ShowSubscribePrompt(controller);
	}

	builder.addSkip();
	builder.addSubsectionTitle(tr::ayu_PluginsSubscriptionsSection());

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			for (const auto &ch : TeleForge::Plugins::Catalog::kSubscribeChannels) {
				const auto username = QString::fromLatin1(ch.username);
				const auto subscribe = wctx.container->add(
					object_ptr<Ui::SettingsButton>(
						wctx.container,
						tr::ayu_PluginsSubscribeTo(
							tr::now,
							lt_channel,
							username),
						st::settingsButtonNoIcon));
				subscribe->setClickedCallback([=] {
					controller->showPeerByLink(Window::PeerByLinkInfo{
						.usernameOrId = username,
					});
				});
			}
			const auto remind = wctx.container->add(
				object_ptr<Ui::SettingsButton>(
					wctx.container,
					tr::ayu_PluginsRemindSubscription(),
					st::settingsButtonNoIcon));
			remind->setClickedCallback([=] {
				ShowSubscribePrompt(controller);
			});
		}, [](const SearchContext &) {});
	});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle(tr::ayu_PluginsDevelopersSection());

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			AddHint(wctx.container, tr::ayu_PluginsDevelopersHint(tr::now));
		}, [](const SearchContext &) {});
	});

	const auto refreshDevelopers = std::make_shared<Fn<void()>>();
	*refreshDevelopers = [=] {
		if (!*developersHost) {
			return;
		}
		(*developersHost)->clear();
		const auto devs = TeleForge::Plugins::cachedTrustedDevelopers();
		if (devs.isEmpty()) {
			AddHint(*developersHost, tr::ayu_PluginsDevelopersEmpty(tr::now));
			return;
		}
		for (const auto &dev : devs) {
			const auto block = (*developersHost)->add(
				object_ptr<Ui::VerticalLayout>((*developersHost).data()));
			AddHint(
				block,
				tr::ayu_PluginsDeveloperLabel(
					tr::now,
					lt_title,
					dev.title,
					lt_channel,
					dev.channelUsername));
			const auto open = block->add(
				object_ptr<Ui::SettingsButton>(
					block,
					tr::ayu_PluginsOpenDevChannel(),
					st::settingsButtonNoIcon));
			open->setClickedCallback([=] {
				controller->showPeerByLink(Window::PeerByLinkInfo{
					.usernameOrId = dev.channelUsername,
				});
			});
			const auto scan = block->add(
				object_ptr<Ui::SettingsButton>(
					block,
					tr::ayu_PluginsScanChannel(),
					st::settingsButtonNoIcon));
			scan->setClickedCallback([=, id = dev.devId] {
				controller->showToast(tr::ayu_PluginsScanningChannel(
					tr::now,
					lt_channel,
					dev.channelUsername));
				TeleForge::Plugins::scanDeveloperChannel(
					&controller->session(),
					id,
					[=](QVector<TeleForge::Plugins::ChannelPluginOffer> offers, QString error) {
						if (!error.isEmpty() && offers.isEmpty()) {
							controller->showToast(error);
							return;
						}
						ShowChannelOffersBox(controller, offers);
					});
			});
		}
	};

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto host = wctx.container->add(
				object_ptr<Ui::VerticalLayout>(wctx.container));
			*developersHost = host;
			const auto update = wctx.container->add(
				object_ptr<Ui::SettingsButton>(
					wctx.container,
					tr::ayu_PluginsRefreshDevelopers(),
					st::settingsButtonNoIcon));
			update->setClickedCallback([=] {
				TeleForge::Plugins::fetchTrustedDevelopers(
					[=](QVector<TeleForge::Plugins::TrustedDeveloper> devs, QString error) {
						if (!error.isEmpty()) {
							controller->showToast(error);
						}
						(*refreshDevelopers)();
					});
			});
			(*refreshDevelopers)();
			TeleForge::Plugins::fetchTrustedDevelopers(
				[=](QVector<TeleForge::Plugins::TrustedDeveloper>, QString error) {
					if (!error.isEmpty()) {
						controller->showToast(error);
					}
					(*refreshDevelopers)();
				});
		}, [](const SearchContext &) {});
	});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle(tr::ayu_PluginsCatalogSection());

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			AddHint(wctx.container, tr::ayu_PluginsCatalogHint(tr::now));
		}, [](const SearchContext &) {});
	});

	const auto refreshCatalog = std::make_shared<Fn<void()>>();
	*refreshCatalog = [=] {
		if (!*catalogHost) {
			return;
		}
		(*catalogHost)->clear();
		const auto entries = TeleForge::Plugins::cachedCatalog();
		if (entries.isEmpty()) {
			AddHint(*catalogHost, tr::ayu_PluginsCatalogStale(tr::now));
			return;
		}
		for (const auto &entry : entries) {
			const auto block = (*catalogHost)->add(
				object_ptr<Ui::VerticalLayout>((*catalogHost).data()));
			AddHint(
				block,
				tr::ayu_PluginsCatalogRow(
					tr::now,
					lt_file,
					entry.fileName,
					lt_title,
					entry.title));
			const auto install = block->add(
				object_ptr<Ui::SettingsButton>(
					block,
					tr::ayu_PluginsDownloadInstall(),
					st::settingsButtonNoIcon));
			const auto captured = entry;
			install->setClickedCallback([=] {
				TeleForge::Plugins::installCatalogEntry(
					captured,
					[=](bool ok, QString message) {
						controller->showToast(message);
					});
			});
		}
	};

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto host = wctx.container->add(
				object_ptr<Ui::VerticalLayout>(wctx.container));
			*catalogHost = host;
			const auto update = wctx.container->add(
				object_ptr<Ui::SettingsButton>(
					wctx.container,
					tr::ayu_PluginsRefreshCatalog(),
					st::settingsButtonNoIcon));
			update->setClickedCallback([=] {
				controller->showToast(tr::ayu_PluginsLoadingIndexes(tr::now));
				TeleForge::Plugins::fetchTrustedDevelopers([=](auto, QString) {
					TeleForge::Plugins::fetchCatalog(
						[=](QVector<TeleForge::Plugins::CatalogEntry> entries, QString error) {
							if (!error.isEmpty()) {
								controller->showToast(error);
							}
							(*refreshCatalog)();
							if (!entries.isEmpty()) {
								ShowCatalogInstallBox(controller, entries);
							}
						});
				});
			});
			(*refreshCatalog)();
			// Auto-fetch on open.
			TeleForge::Plugins::fetchTrustedDevelopers([=](auto, QString) {
				TeleForge::Plugins::fetchCatalog(
					[=](QVector<TeleForge::Plugins::CatalogEntry> entries, QString error) {
						if (!error.isEmpty()) {
							controller->showToast(error);
						}
						(*refreshCatalog)();
					});
			});
		}, [](const SearchContext &) {});
	});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle(tr::ayu_PluginsDevModeSection());

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			struct ArmingState {
				bool ackRisk = false;
				bool ackNoSupport = false;
				bool ackTesting = false;
				bool cooldownReady = false;
				int secondsLeft = 0;
				Ui::FlatLabel *status = nullptr;
				Ui::Checkbox *finalSwitch = nullptr;
				std::unique_ptr<base::Timer> timer;
			};
			const auto state = std::make_shared<ArmingState>();
			const auto c = wctx.container;

			const auto updateUi = [=] {
				if (!state->status || !state->finalSwitch) {
					return;
				}
				if (TeleForge::Plugins::DevMode::allowUnverifiedInstalls()) {
					state->status->setText(tr::ayu_PluginsDevModeOn(tr::now));
					state->finalSwitch->setChecked(true);
					state->finalSwitch->setEnabled(true);
					return;
				}
				if (!TeleForge::Subscription::active()) {
					state->cooldownReady = false;
					state->secondsLeft = 0;
					if (state->timer) {
						state->timer->cancel();
					}
					state->status->setText(
						tr::ayu_PluginsDevModeSubscriptionRequired(tr::now));
					state->finalSwitch->setChecked(false);
					state->finalSwitch->setEnabled(false);
					return;
				}
				state->finalSwitch->setChecked(false);
				const auto allAck = state->ackRisk
					&& state->ackNoSupport
					&& state->ackTesting;
				if (!allAck) {
					state->cooldownReady = false;
					state->secondsLeft = 0;
					if (state->timer) {
						state->timer->cancel();
					}
					state->status->setText(tr::ayu_PluginsDevModeAckAll(
						tr::now,
						lt_seconds,
						QString::number(
							TeleForge::Plugins::DevMode::kCooldownSeconds)));
					state->finalSwitch->setEnabled(false);
					return;
				}
				if (!state->cooldownReady) {
					state->finalSwitch->setEnabled(false);
					state->status->setText(tr::ayu_PluginsDevModeWait(
						tr::now,
						lt_seconds,
						QString::number(state->secondsLeft)));
					return;
				}
				state->status->setText(tr::ayu_PluginsDevModeReady(tr::now));
				state->finalSwitch->setEnabled(true);
			};

			const auto tryStartCooldown = [=] {
				if (TeleForge::Plugins::DevMode::allowUnverifiedInstalls()) {
					return;
				}
				const auto allAck = state->ackRisk
					&& state->ackNoSupport
					&& state->ackTesting;
				if (!allAck) {
					updateUi();
					return;
				}
				if (state->cooldownReady || (state->timer && state->timer->isActive())) {
					updateUi();
					return;
				}
				state->secondsLeft = TeleForge::Plugins::DevMode::kCooldownSeconds;
				state->timer = std::make_unique<base::Timer>([=] {
					if (--state->secondsLeft <= 0) {
						state->cooldownReady = true;
						if (state->timer) {
							state->timer->cancel();
						}
					}
					updateUi();
				});
				state->timer->callEach(1000);
				updateUi();
			};

			Ui::AddDividerText(
				c,
				tr::ayu_PluginsDevModeHint(),
				st::defaultBoxDividerLabelPadding);

			state->status = Ui::AddDividerText(
				c,
				rpl::single(QString()),
				st::defaultBoxDividerLabelPadding);

			const auto onAckChanged = [=](bool checked, bool &slot) {
				slot = checked;
				if (!checked) {
					state->cooldownReady = false;
					state->secondsLeft = 0;
					if (state->timer) {
						state->timer->cancel();
					}
				}
				tryStartCooldown();
			};

			if (TeleForge::Plugins::DevMode::allowUnverifiedInstalls()) {
				Ui::AddSkip(c, st::settingsCheckboxesSkip);
				state->finalSwitch = AddWrappedSettingsCheckbox(
					c,
					tr::ayu_PluginsAllowUnverified(tr::now),
					true);
				state->finalSwitch->checkedChanges(
				) | rpl::on_next([=](bool checked) {
					if (!checked) {
						TeleForge::Plugins::DevMode::setAllowUnverifiedInstalls(false);
						state->cooldownReady = false;
						state->ackRisk = false;
						state->ackNoSupport = false;
						state->ackTesting = false;
						if (state->finalSwitch) {
							state->finalSwitch->setChecked(false);
						}
						updateUi();
						controller->showToast(
							tr::ayu_PluginsDevModeDisabled(tr::now));
					}
				}, state->finalSwitch->lifetime());
			} else {
				Ui::AddSkip(c, st::settingsCheckboxesSkip);

				const auto ack1 = AddWrappedSettingsCheckbox(
					c,
					tr::ayu_PluginsAckRisk(tr::now),
					false);
				ack1->checkedChanges(
				) | rpl::on_next([=](bool checked) {
					onAckChanged(checked, state->ackRisk);
				}, ack1->lifetime());

				Ui::AddSkip(c, st::settingsCheckboxesSkip);

				const auto ack2 = AddWrappedSettingsCheckbox(
					c,
					tr::ayu_PluginsAckNoSupport(tr::now),
					false);
				ack2->checkedChanges(
				) | rpl::on_next([=](bool checked) {
					onAckChanged(checked, state->ackNoSupport);
				}, ack2->lifetime());

				Ui::AddSkip(c, st::settingsCheckboxesSkip);

				const auto ack3 = AddWrappedSettingsCheckbox(
					c,
					tr::ayu_PluginsAckTesting(tr::now),
					false);
				ack3->checkedChanges(
				) | rpl::on_next([=](bool checked) {
					onAckChanged(checked, state->ackTesting);
				}, ack3->lifetime());

				Ui::AddSkip(c, st::settingsCheckboxesSkip);

				state->finalSwitch = AddWrappedSettingsCheckbox(
					c,
					tr::ayu_PluginsAllowUnverified(tr::now),
					false);
				state->finalSwitch->setEnabled(false);
				state->finalSwitch->checkedChanges(
				) | rpl::on_next([=](bool checked) {
					if (!checked || !state->cooldownReady) {
						state->finalSwitch->setChecked(false);
						return;
					}
					TeleForge::Plugins::DevMode::setAllowUnverifiedInstalls(true);
					updateUi();
					controller->showToast(
						tr::ayu_PluginsDevModeEnabled(tr::now));
				}, state->finalSwitch->lifetime());

					const auto gateAck = [=](not_null<Ui::Checkbox*> box) {
						TeleForge::Subscription::activeValue(
						) | rpl::on_next([=](bool isActive) {
							box->setEnabled(isActive);
							if (!isActive && box->checked()) {
								box->setChecked(false);
							}
						}, box->lifetime());
					};
					gateAck(ack1);
					gateAck(ack2);
					gateAck(ack3);
					TeleForge::Subscription::activeValue(
					) | rpl::on_next([=](bool) {
						updateUi();
					}, c->lifetime());
			}
			updateUi();
		}, [](const SearchContext &) {});
	});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle(tr::ayu_PluginsButtonsSection());

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto json =
				TeleForge::Plugins::PluginRunner::menuItemsJson();
			const auto items = QJsonDocument::fromJson(json.toUtf8()).array();
			if (items.isEmpty()) {
				AddHint(c, tr::ayu_PluginsNoButtons(tr::now));
				return;
			}
			for (const auto &value : items) {
				const auto object = value.toObject();
				const auto id = object.value(u"id"_q).toString();
				const auto title = object.value(u"title"_q).toString();
				if (id.isEmpty()) {
					continue;
				}
				const auto button = c->add(object_ptr<Ui::SettingsButton>(
					c,
					rpl::single(title.isEmpty() ? id : title),
					st::settingsButtonNoIcon));
				button->setClickedCallback([=] {
					TeleForge::Plugins::PluginRunner::invokeMenuItem(id);
				});
			}
		}, [](const SearchContext &) {});
	});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle(tr::ayu_PluginsInstalledSection());

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto host = wctx.container->add(
				object_ptr<Ui::VerticalLayout>(wctx.container));
			*pluginsHost = host;
			const auto refresh = std::make_shared<Fn<void()>>();
			*refresh = [=] {
				if (!*pluginsHost) {
					return;
				}
				(*pluginsHost)->clear();
				const auto plugins = TeleForge::Plugins::listWithRuntimeState();
				if (plugins.empty()) {
					AddHint(*pluginsHost, tr::ayu_PluginsNoInstalled(tr::now));
					return;
				}
				for (const auto &plugin : plugins) {
					const auto block = (*pluginsHost)->add(
						object_ptr<Ui::VerticalLayout>((*pluginsHost).data()));
					const auto status = plugin.loaded
						? (plugin.enabled
							? tr::ayu_PluginsStateRunning(tr::now)
							: tr::ayu_PluginsStateStopped(tr::now))
						: (plugin.enabled
							? tr::ayu_PluginsStateEnabledNotLoaded(tr::now)
							: tr::ayu_PluginsStateDisabled(tr::now));
					AddHint(
						block,
						tr::ayu_PluginsRow(
							tr::now,
							lt_file,
							plugin.fileName,
							lt_status,
							status));
					const auto toggle = block->add(
						object_ptr<Ui::SettingsButton>(
							block,
							rpl::single(plugin.enabled
								? tr::ayu_PluginsDisable(tr::now)
								: tr::ayu_PluginsEnable(tr::now)),
							st::settingsButtonNoIcon));
					toggle->setClickedCallback([=, name = plugin.fileName] {
						const auto now = TeleForge::Plugins::isEnabled(name);
						TeleForge::Plugins::setEnabled(name, !now);
						if (now) {
							TeleForge::Plugins::stopPlugin(name);
						}
						(*refresh)();
						controller->showToast(tr::ayu_PluginsSaved(tr::now));
					});
					const auto start = block->add(
						object_ptr<Ui::SettingsButton>(
							block,
							tr::ayu_PluginsStartReload(),
							st::settingsButtonNoIcon));
					start->setClickedCallback([=, name = plugin.fileName] {
						TeleForge::Plugins::setEnabled(name, true);
						TeleForge::Plugins::reloadAll();
						(*refresh)();
						controller->showToast(tr::ayu_PluginsReloaded(tr::now));
					});
					const auto stop = block->add(
						object_ptr<Ui::SettingsButton>(
							block,
							tr::ayu_PluginsStop(),
							st::settingsButtonNoIcon));
					stop->setClickedCallback([=, name = plugin.fileName] {
						TeleForge::Plugins::stopPlugin(name);
						(*refresh)();
						controller->showToast(tr::ayu_PluginsStopped(tr::now));
					});
					const auto remove = block->add(
						object_ptr<Ui::SettingsButton>(
							block,
							tr::ayu_PluginsRemoveFile(),
							st::settingsButtonNoIcon));
					remove->setClickedCallback([=, name = plugin.fileName] {
						TeleForge::Plugins::stopPlugin(name);
						if (TeleForge::Plugins::removeInstalledFile(name)) {
							TeleForge::Plugins::reloadAll();
							(*refresh)();
							controller->showToast(tr::ayu_PluginsRemoved(tr::now));
						} else {
							controller->showToast(tr::ayu_PluginsRemoveFailed(tr::now));
						}
					});
				}
			};
			(*refresh)();
		}, [](const SearchContext &) {});
	});

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto reload = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_PluginsReloadAll(),
					st::settingsButtonNoIcon));
			reload->setClickedCallback([=] {
				TeleForge::Plugins::reloadAll();
				controller->showToast(tr::ayu_PluginsAllReloaded(tr::now));
			});
			const auto folder = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_PluginsOpenFolder(),
					st::settingsButtonNoIcon));
			folder->setClickedCallback([=] {
				const auto path = QDir(cWorkingDir() + u"plugins"_q).absolutePath();
				QDir().mkpath(path);
				const auto marker = path + u"/.teleforge"_q;
				QFile markerFile(marker);
				if (markerFile.open(QIODevice::WriteOnly)) {
					markerFile.close();
				}
				File::ShowInFolder(marker);
			});
			const auto site = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_PluginsSiteAndFormat(),
					st::settingsButtonNoIcon));
			site->setClickedCallback([=] {
				QDesktopServices::openUrl(QUrl(
					u"https://lavrentijav.github.io/TeleForge/"_q));
			});
		}, [](const SearchContext &) {});
	});
});

} // namespace

rpl::producer<QString> TeleForgePlugins::title() {
	return tr::ayu_CategoryTeleForgePlugins();
}

TeleForgePlugins::TeleForgePlugins(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void TeleForgePlugins::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type TeleForgePluginsId() {
	return TeleForgePlugins::Id();
}

} // namespace Settings
