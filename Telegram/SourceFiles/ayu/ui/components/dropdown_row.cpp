// Copyright @Radolyn, 2026

#include "ayu/ui/components/dropdown_row.h"

#include "settings/settings_common.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <rpl/map.h>
#include <rpl/variable.h>

#include <algorithm>

namespace AyuUi {

not_null<Ui::SettingsButton*> AddDropdownRow(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> title,
		std::vector<QString> options,
		int current,
		Fn<void(int)> onPick,
		const style::SettingsButton *stOverride) {
	const auto index = container->lifetime().make_state<rpl::variable<int>>(
		std::clamp(current, 0, std::max(0, int(options.size()) - 1)));
	auto label = index->value() | rpl::map([options](int value) {
		return (value >= 0 && value < int(options.size()))
			? options[value]
			: QString();
	});
	const auto button = Settings::AddButtonWithLabel(
		container,
		std::move(title),
		std::move(label),
		stOverride ? *stOverride : st::settingsButtonNoIcon);
	button->setClickedCallback([=] {
		if (options.empty()) {
			return;
		}
		const auto menu = Ui::CreateChild<Ui::PopupMenu>(
			button.get(),
			st::defaultPopupMenu);
		for (auto i = 0; i != int(options.size()); ++i) {
			const auto choose = i;
			menu->addAction(options[i], [=] {
				*index = choose;
				if (onPick) {
					onPick(choose);
				}
			});
		}
		menu->deleteOnHide(true);
		menu->popup(button->mapToGlobal(
			QPoint(button->width() / 2, button->height())));
	});
	return button;
}

} // namespace AyuUi
