// Copyright @Radolyn, 2026
#include "tfc/ui/settings/settings_teleforge_sync.h"

#include "tfc/features/sync/teleforge_sync.h"
#include "tfc/ui/teleforge_upload_bar.h"
#include "tfc/features/sync/teleforge_sync_mysql.h"
#include "tfc/features/sync/teleforge_sync_pg.h"
#include "tfc/features/teleforge/teleforge_core.h"
#include "tfc/features/teleforge/teleforge_storage.h"
#include "tfc/ui/settings/tf_builder.h"
#include "tfc/ui/settings/settings_tf_utils.h"
#include "tfc/ui/settings/settings_main.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <rpl/combine.h>
#include <rpl/map.h>
#include <rpl/rpl.h>
#include <rpl/variable.h>

#include <algorithm>

#include <QtGui/QPainter>

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

namespace Settings {

using namespace Builder;
using namespace AyBuilder;

namespace {

struct ParsedConn {
	QString host;
	QString port;
	QString db;
	QString user;
	QString password;
};

[[nodiscard]] ParsedConn ParseConnString(const QString &s) {
	auto r = ParsedConn();
	const auto tokens = s.split(
		QRegularExpression(u"[\\s;]+"_q),
		Qt::SkipEmptyParts);
	for (const auto &t : tokens) {
		const auto eq = t.indexOf('=');
		if (eq <= 0) {
			continue;
		}
		const auto k = t.left(eq).trimmed().toLower();
		const auto v = t.mid(eq + 1).trimmed();
		if (k == u"host"_q || k == u"server"_q) {
			r.host = v;
		} else if (k == u"port"_q) {
			r.port = v;
		} else if (k == u"dbname"_q || k == u"db"_q || k == u"database"_q) {
			r.db = v;
		} else if (k == u"user"_q || k == u"uid"_q) {
			r.user = v;
		} else if (k == u"password"_q || k == u"pwd"_q) {
			r.password = v;
		}
	}
	return r;
}

[[nodiscard]] QString ComposeConnString(const ParsedConn &p) {
	auto parts = QStringList();
	if (!p.host.isEmpty()) {
		parts << u"host="_q + p.host;
	}
	if (!p.port.isEmpty()) {
		parts << u"port="_q + p.port;
	}
	if (!p.db.isEmpty()) {
		parts << u"dbname="_q + p.db;
	}
	if (!p.user.isEmpty()) {
		parts << u"user="_q + p.user;
	}
	if (!p.password.isEmpty()) {
		parts << u"password="_q + p.password;
	}
	return parts.join(' ');
}

void ShowPickMenu(
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

const auto kMeta = BuildHelper({
	.id = TeleForgeSync::Id(),
	.parentId = AyuMain::Id(),
	.title = &tr::ayu_CategoryTeleForgeSync,
	.icon = &st::menuIconDownload,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	const auto controller = builder.controller();

	builder.addSkip();
	builder.addSubsectionTitle(tr::ayu_SyncDevicesSection());

	ayu.addToggle({
		.id = u"teleforge/memorySync"_q,
		.title = tr::ayu_SyncViaTelegram(),
		.getter = [=] {
			return TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore()).memorySyncEnabled;
		},
		.setter = [=](bool v) {
			auto p = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			p.memorySyncEnabled = v;
			p.updatedAt = QDateTime::currentDateTimeUtc();
			TeleForge::PersistPersonalityCore(p);
			if (v) {
				TeleForge::Sync::ensureSyncChat(
					&controller->session(),
					[=](bool ok, QString message) {
						controller->showToast(message);
					});
			}
		},
	});

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto syncNow = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					tr::ayu_SyncNow(),
					st::settingsButtonNoIcon));
			syncNow->setClickedCallback([=] {
				TeleForge::Sync::syncNow(&controller->session(), [=](QString msg) {
					controller->showToast(msg);
				});
			});
		}, [](const SearchContext &) {});
	});

	builder.addSkip();
	builder.addSubsectionTitle(tr::ayu_SyncCloudSection());

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto core = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			const auto parsed = ParseConnString(core.pgConnString);

			// Reactive provider state — holds the cloudBackend value
			// (0 = Telegram, 1 = PostgreSQL, 2 = MySQL).
			const auto provider = c->lifetime().make_state<rpl::variable<int>>(
				core.cloudBackend);
			const auto backendName = [](int b) {
				return (b == 1)
					? u"PostgreSQL"_q
					: (b == 2)
					? u"MySQL / MariaDB"_q
					: tr::ayu_SyncBackendTelegram(tr::now);
			};

			// --- Provider selector (Telegram / MySQL / PostgreSQL) ---
			const auto providerBtn = c->add(object_ptr<Ui::SettingsButton>(
				c,
				provider->value() | rpl::map([=](int b) {
					return tr::ayu_SyncProvider(
						tr::now,
						lt_backend,
						backendName(b));
				}),
				st::settingsButtonNoIcon));
			providerBtn->setClickedCallback([=] {
				ShowPickMenu(
					providerBtn,
					providerBtn->mapToGlobal(
						QPoint(0, providerBtn->height())),
					{
						tr::ayu_SyncBackendTelegram(tr::now),
						u"MySQL / MariaDB"_q,
						u"PostgreSQL"_q,
					},
					[=](const QString &choice) {
						const auto value = choice.startsWith(u"PostgreSQL"_q)
							? 1
							: choice.startsWith(u"MySQL"_q)
							? 2
							: 0;
						*provider = value;
						auto p = TeleForge::LoadPersonalityCore().value_or(
							TeleForge::DefaultPersonalityCore());
						p.cloudBackend = value;
						p.updatedAt = QDateTime::currentDateTimeUtc();
						TeleForge::PersistPersonalityCore(p);
					});
			});

			// --- PostgreSQL version (shown only for PostgreSQL) ---
			const auto versionWrap = c->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					c,
					object_ptr<Ui::VerticalLayout>(c)));
			const auto versionInner = versionWrap->entity();
			const auto pgVersion = c->lifetime()
				.make_state<rpl::variable<QString>>(
					core.pgVersion.isEmpty() ? u"18"_q : core.pgVersion);
			const auto versionBtn = versionInner->add(
				object_ptr<Ui::SettingsButton>(
					versionInner,
					pgVersion->value() | rpl::map([](const QString &v) {
						return tr::ayu_SyncPgVersion(
							tr::now,
							lt_version,
							v);
					}),
					st::settingsButtonNoIcon));
			versionBtn->setClickedCallback([=] {
				ShowPickMenu(
					versionBtn,
					versionBtn->mapToGlobal(QPoint(0, versionBtn->height())),
					{ u"18"_q, u"17"_q, u"16"_q, u"15"_q, u"14"_q, u"13"_q },
					[=](const QString &choice) {
						*pgVersion = choice;
						auto p = TeleForge::LoadPersonalityCore().value_or(
							TeleForge::DefaultPersonalityCore());
						p.pgVersion = choice;
						p.updatedAt = QDateTime::currentDateTimeUtc();
						TeleForge::PersistPersonalityCore(p);
					});
			});
			versionWrap->toggleOn(provider->value() | rpl::map([](int b) {
				return b == 1;
			}));
			versionWrap->finishAnimating();

			// --- Connection fields (shown for any DB backend) ---
			const auto connWrap = c->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					c,
					object_ptr<Ui::VerticalLayout>(c)));
			const auto inner = connWrap->entity();

			// host : port on a single row (two inputs split by a colon).
			const auto row = inner->add(
				object_ptr<Ui::RpWidget>(inner),
				st::boxRowPadding);
			const auto host = Ui::CreateChild<Ui::InputField>(
				row,
				st::defaultInputField,
				Ui::InputField::Mode::SingleLine,
				rpl::single(tr::ayu_SyncHostPlaceholder(tr::now)),
				TextWithTags{ parsed.host });
			const auto colon = Ui::CreateChild<Ui::FlatLabel>(
				row,
				rpl::single(u":"_q),
				st::defaultFlatLabel);
			const auto port = Ui::CreateChild<Ui::InputField>(
				row,
				st::defaultInputField,
				Ui::InputField::Mode::SingleLine,
				rpl::single(tr::ayu_SyncPortPlaceholder(tr::now)),
				TextWithTags{ parsed.port });
			rpl::combine(
				row->widthValue(),
				host->heightValue()
			) | rpl::on_next([=](int w, int h) {
				constexpr auto kColonSlot = 12;
				constexpr auto kGap = 8;
				const auto portW = std::clamp(w / 4, 56, 110);
				const auto hostW = std::max(
					0,
					w - portW - kColonSlot - 2 * kGap);
				host->setGeometry(0, 0, hostW, h);
				colon->moveToLeft(
					hostW + kGap + (kColonSlot - colon->width()) / 2,
					(h - colon->height()) / 2);
				port->setGeometry(
					hostW + kGap + kColonSlot + kGap,
					0,
					portW,
					h);
				row->resize(w, h);
			}, row->lifetime());

			const auto db = inner->add(
				object_ptr<Ui::InputField>(
					inner,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(tr::ayu_SyncDatabasePlaceholder(tr::now)),
					TextWithTags{ parsed.db }),
				st::boxRowPadding);
			const auto user = inner->add(
				object_ptr<Ui::InputField>(
					inner,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(tr::ayu_SyncUserPlaceholder(tr::now)),
					TextWithTags{ parsed.user }),
				st::boxRowPadding);
			const auto password = inner->add(
				object_ptr<Ui::InputField>(
					inner,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(tr::ayu_SyncPasswordPlaceholder(tr::now)),
					TextWithTags{ parsed.password }),
				st::boxRowPadding);

			const auto collect = [=] {
				return ParsedConn{
					.host = host->getLastText().trimmed(),
					.port = port->getLastText().trimmed(),
					.db = db->getLastText().trimmed(),
					.user = user->getLastText().trimmed(),
					.password = password->getLastText().trimmed(),
				};
			};

			const auto save = inner->add(object_ptr<Ui::SettingsButton>(
				inner,
				tr::ayu_SyncSaveConnection(),
				st::settingsButtonNoIcon));
			save->setClickedCallback([=] {
				auto p = TeleForge::LoadPersonalityCore().value_or(
					TeleForge::DefaultPersonalityCore());
				p.pgConnString = ComposeConnString(collect());
				p.updatedAt = QDateTime::currentDateTimeUtc();
				TeleForge::PersistPersonalityCore(p);
				controller->showToast(tr::ayu_SyncConnectionSaved(tr::now));
			});

			const auto test = inner->add(object_ptr<Ui::SettingsButton>(
				inner,
				tr::ayu_SyncTestConnection(),
				st::settingsButtonNoIcon));
			test->setClickedCallback([=] {
				const auto text = ComposeConnString(collect());
				const auto cb = [=](bool ok, QString error) {
					controller->showToast(ok
						? tr::ayu_SyncConnectionOk(tr::now)
						: (error.isEmpty()
							? tr::ayu_SyncConnectionFailed(tr::now)
							: error));
				};
				if (provider->current() == 2) {
					TeleForge::Sync::MySqlTestConnection(text, cb);
				} else {
					TeleForge::Sync::PgTestConnection(text, cb);
				}
			});

			const auto exportAll = inner->add(
				object_ptr<Ui::SettingsButton>(
					inner,
					tr::ayu_SyncExportAll(),
					st::settingsButtonNoIcon));
			exportAll->setClickedCallback([=] {
				TeleForge::UI::ShowUploadProgress(0, 1);
				TeleForge::Sync::uploadNow(
					&controller->session(),
					[=](QString msg) {
						controller->showToast(msg);
						TeleForge::UI::HideUploadProgress();
					},
					[=](int done, int total) {
						TeleForge::UI::ShowUploadProgress(done, total);
					});
			});

			// --- SSH tunnel (optional; applies to whichever DB backend is
			// active above, so the DB itself never needs a public port). ---
			ayu.base().addSkip();
			AddSettingsHint(inner, tr::ayu_SyncSshHint());

			const auto sshEnabled = inner->lifetime()
				.make_state<rpl::variable<bool>>(core.sshTunnelEnabled);
			AddToggle(
				inner,
				tr::ayu_SyncSshToggle(),
				[=] { return sshEnabled->current(); },
				[=](bool v) {
					*sshEnabled = v;
					auto p = TeleForge::LoadPersonalityCore().value_or(
						TeleForge::DefaultPersonalityCore());
					p.sshTunnelEnabled = v;
					p.updatedAt = QDateTime::currentDateTimeUtc();
					TeleForge::PersistPersonalityCore(p);
				});

			const auto sshWrap = inner->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					inner,
					object_ptr<Ui::VerticalLayout>(inner)));
			const auto sshInner = sshWrap->entity();

			AddSettingsHint(sshInner, tr::ayu_SyncSshTarget());
			const auto sshTarget = sshInner->add(
				object_ptr<Ui::InputField>(
					sshInner,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.sshTunnelTarget }),
				st::boxRowPadding);

			AddSettingsHint(sshInner, tr::ayu_SyncSshIdentity());
			const auto sshIdentity = sshInner->add(
				object_ptr<Ui::InputField>(
					sshInner,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(QString()),
					TextWithTags{ core.sshTunnelIdentityFile }),
				st::boxRowPadding);
			const auto browseIdentity = sshInner->add(
				object_ptr<Ui::SettingsButton>(
					sshInner,
					tr::ayu_SyncBrowseKey(),
					st::settingsButtonNoIcon));
			browseIdentity->setClickedCallback([=] {
				FileDialog::GetOpenPath(
					Core::App().getFileDialogParent(),
					tr::ayu_SyncSshKeyDialog(tr::now),
					tr::ayu_FileFilterAllFiles(tr::now),
					[=](const FileDialog::OpenResult &result) {
						if (!result.paths.isEmpty()) {
							sshIdentity->setText(result.paths.front());
						}
					});
			});

			const auto persistSsh = [=] {
				auto p = TeleForge::LoadPersonalityCore().value_or(
					TeleForge::DefaultPersonalityCore());
				p.sshTunnelTarget = sshTarget->getLastText().trimmed();
				p.sshTunnelIdentityFile = sshIdentity->getLastText().trimmed();
				p.updatedAt = QDateTime::currentDateTimeUtc();
				TeleForge::PersistPersonalityCore(p);
			};
			for (const auto field : { sshTarget, sshIdentity }) {
				field->focusedChanges(
				) | rpl::on_next([=](bool focused) {
					if (!focused) {
						persistSsh();
					}
				}, field->lifetime());
			}

			sshWrap->toggleOn(sshEnabled->value());
			sshWrap->finishAnimating();

			ayu.base().addSkip();
			ayu.base().addDividerText(tr::ayu_SyncSshHintLong());

			connWrap->toggleOn(provider->value() | rpl::map([](int b) {
				return b != 0;
			}));
			connWrap->finishAnimating();
		}, [](const SearchContext &) {});
	});
});

} // namespace

rpl::producer<QString> TeleForgeSync::title() {
	return tr::ayu_CategoryTeleForgeSync();
}

TeleForgeSync::TeleForgeSync(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void TeleForgeSync::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type TeleForgeSyncId() {
	return TeleForgeSync::Id();
}

} // namespace Settings
