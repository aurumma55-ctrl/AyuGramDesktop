// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace AyuFeatures::AdminPanel {

enum class ActionType {
	Kick,
	Ban,
	Mute,
	Warn,
};

void AddAdminAction(
	not_null<Ui::PopupMenu*> menu,
	HistoryItem *item,
	not_null<Window::SessionController*> controller);

void ShowAdminPanel(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

} // namespace AyuFeatures::AdminPanel
