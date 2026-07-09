// Copyright @Radolyn, 2026

#pragma once

#include "base/basic_types.h"

#include <rpl/producer.h>

#include <QtCore/QString>

#include <vector>

namespace style {
struct SettingsButton;
} // namespace style

namespace Ui {
class VerticalLayout;
class SettingsButton;
} // namespace Ui

namespace AyuUi {

// A settings-style row showing the current value on the right that opens a
// classic dropdown (Ui::PopupMenu) with the given options when clicked.
not_null<Ui::SettingsButton*> AddDropdownRow(
	not_null<Ui::VerticalLayout*> container,
	rpl::producer<QString> title,
	std::vector<QString> options,
	int current,
	Fn<void(int)> onPick,
	const style::SettingsButton *stOverride = nullptr);

} // namespace AyuUi
