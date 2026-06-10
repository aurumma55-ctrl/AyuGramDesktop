// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/bulk_chat_management/bulk_chat_management.h"

#include "apiwrap.h"
#include "ayu/ayu_settings.h"
#include "boxes/peer_list_box.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/rp_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

namespace AyuFeatures::BulkChatManagement {
namespace {

[[nodiscard]] bool IsBot(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return user && user->isBot();
}

[[nodiscard]] bool IsPersonal(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return user && !user->isBot();
}

[[nodiscard]] bool IsGroup(not_null<PeerData*> peer) {
	if (peer->isChat()) {
		return true;
	}
	if (const auto channel = peer->asChannel()) {
		return channel->isMegagroup();
	}
	return false;
}

[[nodiscard]] bool IsChannel(not_null<PeerData*> peer) {
	if (const auto channel = peer->asChannel()) {
		return channel->isBroadcast();
	}
	return false;
}

void ApplyBulkDelete(
		not_null<Window::SessionController*> controller,
		std::vector<not_null<PeerData*>> peers) {
	const auto &settings = AyuSettings::getInstance();
	const auto api = &controller->session().api();

	auto skipped = 0;
	auto processed = 0;
	for (const auto &peer : peers) {
		if (settings.isInBulkDeleteWhitelist(peer->id.value)) {
			++skipped;
			continue;
		}
		api->deleteConversation(peer, false);
		++processed;
	}
	controller->showToast(tr::ayu_BulkDeleteDone(
		tr::now,
		lt_processed,
		QString::number(processed),
		lt_skipped,
		QString::number(skipped)));
}

} // namespace

BulkChatController::BulkChatController(
	not_null<Main::Session*> session,
	Mode mode,
	const std::vector<int64> &initialSelected)
: ChatsListBoxController(session)
, _session(session)
, _mode(mode)
, _initialSelected(initialSelected)
, _applyInitialSelectionTimer([=] { applyInitialSelection(); }) {
}

Main::Session &BulkChatController::session() const {
	return *_session;
}

std::unique_ptr<BulkChatController::Row> BulkChatController::createRow(
		not_null<History*> history) {
	if (history->peer->isSelf()) {
		return nullptr;
	}
	if (!_initialSelected.empty()) {
		// Rows are appended after this call returns, so postpone
		// the initial selection until the current batch is built.
		_applyInitialSelectionTimer.callOnce(1);
	}
	return std::make_unique<Row>(history);
}

void BulkChatController::prepareViewHook() {
	delegate()->peerListSetTitle((_mode == Mode::Whitelist)
		? tr::ayu_BulkWhitelistTitle()
		: tr::ayu_BulkManagementTitle());
}

void BulkChatController::applyInitialSelection() {
	// ChatsListBoxController builds rows after prepareViewHook(),
	// so the initial selection is applied here, once rows exist.
	if (_initialSelected.empty()) {
		return;
	}
	const auto count = delegate()->peerListFullRowsCount();
	for (auto i = 0; i != count; ++i) {
		const auto row = delegate()->peerListRowAt(i);
		const auto id = int64(row->peer()->id.value);
		const auto it = ranges::find(_initialSelected, id);
		if (it != end(_initialSelected)) {
			if (!row->checked()) {
				delegate()->peerListSetRowChecked(row, true);
			}
			_initialSelected.erase(it);
		}
	}
	notifySelectedChanged();
}

void BulkChatController::rowClicked(not_null<PeerListRow*> row) {
	delegate()->peerListSetRowChecked(row, !row->checked());
	notifySelectedChanged();
}

void BulkChatController::selectAll() {
	const auto count = delegate()->peerListFullRowsCount();
	for (auto i = 0; i != count; ++i) {
		const auto row = delegate()->peerListRowAt(i);
		if (!row->checked()) {
			delegate()->peerListSetRowChecked(row, true);
		}
	}
	notifySelectedChanged();
}

void BulkChatController::selectFirst(int count) {
	const auto total = delegate()->peerListFullRowsCount();
	const auto take = std::min(count, total);
	for (auto i = 0; i != total; ++i) {
		const auto row = delegate()->peerListRowAt(i);
		const auto wantChecked = (i < take);
		if (row->checked() != wantChecked) {
			delegate()->peerListSetRowChecked(row, wantChecked);
		}
	}
	notifySelectedChanged();
}

void BulkChatController::selectByType(TypeFilter filter) {
	const auto count = delegate()->peerListFullRowsCount();
	for (auto i = 0; i != count; ++i) {
		const auto row = delegate()->peerListRowAt(i);
		const auto matches = matchesType(row->peer(), filter);
		if (matches && !row->checked()) {
			delegate()->peerListSetRowChecked(row, true);
		}
	}
	notifySelectedChanged();
}

void BulkChatController::clearSelection() {
	const auto count = delegate()->peerListFullRowsCount();
	for (auto i = 0; i != count; ++i) {
		const auto row = delegate()->peerListRowAt(i);
		if (row->checked()) {
			delegate()->peerListSetRowChecked(row, false);
		}
	}
	notifySelectedChanged();
}

bool BulkChatController::matchesType(
		not_null<PeerData*> peer,
		TypeFilter filter) const {
	switch (filter) {
	case TypeFilter::Channels: return IsChannel(peer);
	case TypeFilter::Groups: return IsGroup(peer);
	case TypeFilter::Personal: return IsPersonal(peer);
	case TypeFilter::Bots: return IsBot(peer);
	}
	return false;
}

int BulkChatController::selectedCount() const {
	return _selectedCount.current();
}

rpl::producer<int> BulkChatController::selectedCountValue() const {
	return _selectedCount.value();
}

std::vector<not_null<PeerData*>> BulkChatController::collectSelected() const {
	auto result = std::vector<not_null<PeerData*>>();
	const auto count = delegate()->peerListFullRowsCount();
	result.reserve(count);
	for (auto i = 0; i != count; ++i) {
		const auto row = delegate()->peerListRowAt(i);
		if (row->checked()) {
			result.push_back(row->peer());
		}
	}
	return result;
}

void BulkChatController::notifySelectedChanged() {
	auto selected = 0;
	const auto count = delegate()->peerListFullRowsCount();
	for (auto i = 0; i != count; ++i) {
		if (delegate()->peerListRowAt(i)->checked()) {
			++selected;
		}
	}
	_selectedCount = selected;
}

namespace {

[[nodiscard]] object_ptr<Ui::RpWidget> MakeToolbar(
		not_null<BulkChatController*> raw,
		Fn<void()> showSelectMenu) {
	auto result = object_ptr<Ui::VerticalLayout>((QWidget*)nullptr);
	const auto container = result.data();

	auto countText = raw->selectedCountValue(
	) | rpl::map([](int count) {
		return tr::ayu_BulkSelectedCount(
			tr::now,
			lt_number,
			QString::number(count));
	});
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			std::move(countText),
			st::boxLabel),
		st::boxRowPadding);

	const auto selectButton = container->add(
		object_ptr<Ui::RoundButton>(
			container,
			tr::ayu_BulkSelectMenu(),
			st::defaultActiveButton),
		st::boxRowPadding);
	selectButton->setClickedCallback(std::move(showSelectMenu));

	return result;
}

void OpenSelectFirstNBox(
		not_null<Window::SessionController*> controller,
		not_null<BulkChatController*> raw) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_BulkSelectFirstNTitle());
		const auto height = st::boxPadding.bottom()
			+ st::defaultInputField.heightMin
			+ st::boxPadding.bottom();
		const auto wrap = box->addRow(
			object_ptr<Ui::FixedHeightWidget>(box, height));
		const auto input = Ui::CreateChild<Ui::NumberInput>(
			wrap,
			st::defaultInputField,
			tr::ayu_BulkSelectFirstNPlaceholder(),
			QString(),
			9999);
		wrap->widthValue(
		) | rpl::on_next([=](int width) {
			input->resize(width, input->height());
			input->moveToLeft(0, st::boxPadding.bottom());
		}, input->lifetime());
		box->setFocusCallback([=] { input->setFocusFast(); });
		const auto submit = [=] {
			const auto value = input->getLastText().toInt();
			if (value <= 0) {
				input->showError();
				return;
			}
			raw->selectFirst(value);
			box->closeBox();
		};
		QObject::connect(input, &Ui::NumberInput::submitted, submit);
		box->addButton(tr::lng_settings_save(), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace

void Show(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	auto chatController = std::make_unique<BulkChatController>(
		session,
		BulkChatController::Mode::Manage);
	const auto raw = chatController.get();

	const auto weakWindow = base::make_weak(controller);

	auto initBox = [=](not_null<PeerListBox*> box) {
		box->setCloseByOutsideClick(false);

		const auto showSelectMenu = [=] {
			const auto window = weakWindow.get();
			if (!window) {
				return;
			}
			const auto menu = Ui::CreateChild<Ui::PopupMenu>(
				box.get(),
				st::popupMenuWithIcons);
			menu->addAction(tr::ayu_BulkSelectAll(tr::now), [=] {
				raw->selectAll();
			});
			menu->addAction(tr::ayu_BulkSelectFirstN(tr::now), [=] {
				if (const auto strong = weakWindow.get()) {
					OpenSelectFirstNBox(strong, raw);
				}
			});
			menu->addSeparator();
			menu->addAction(tr::ayu_BulkSelectChannels(tr::now), [=] {
				raw->selectByType(TypeFilter::Channels);
			});
			menu->addAction(tr::ayu_BulkSelectGroups(tr::now), [=] {
				raw->selectByType(TypeFilter::Groups);
			});
			menu->addAction(tr::ayu_BulkSelectPersonal(tr::now), [=] {
				raw->selectByType(TypeFilter::Personal);
			});
			menu->addAction(tr::ayu_BulkSelectBots(tr::now), [=] {
				raw->selectByType(TypeFilter::Bots);
			});
			menu->addSeparator();
			menu->addAction(tr::ayu_BulkClearSelection(tr::now), [=] {
				raw->clearSelection();
			});
			menu->popup(QCursor::pos());
		};

		raw->delegate()->peerListSetAboveWidget(
			MakeToolbar(raw, showSelectMenu));

		box->addButton(tr::ayu_BulkApplyDelete(), [=] {
			const auto window = weakWindow.get();
			if (!window) {
				return;
			}
			const auto peers = raw->collectSelected();
			if (peers.empty()) {
				box->closeBox();
				return;
			}
			const auto countString = QString::number(int(peers.size()));
			window->show(Ui::MakeConfirmBox({
				.text = tr::ayu_BulkConfirmDelete(
					tr::now,
					lt_number,
					countString),
				.confirmed = [=](Fn<void()> close) {
					if (const auto strong = weakWindow.get()) {
						ApplyBulkDelete(strong, peers);
					}
					close();
					box->closeBox();
				},
				.confirmText = tr::ayu_BulkApplyDelete(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};

	controller->show(Box<PeerListBox>(
		std::move(chatController),
		std::move(initBox)));
}

void ShowWhitelistEditor(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	auto &settings = AyuSettings::getInstance();
	auto chatController = std::make_unique<BulkChatController>(
		session,
		BulkChatController::Mode::Whitelist,
		settings.bulkDeleteWhitelist());
	const auto raw = chatController.get();

	auto initBox = [=, &settings](not_null<PeerListBox*> box) {
		box->setCloseByOutsideClick(false);

		auto countText = raw->selectedCountValue(
		) | rpl::map([](int count) {
			return tr::ayu_BulkWhitelistSelectedCount(
				tr::now,
				lt_number,
				QString::number(count));
		});
		auto label = object_ptr<Ui::FlatLabel>(
			(QWidget*)nullptr,
			std::move(countText),
			st::boxLabel);
		auto wrap = object_ptr<Ui::PaddingWrap<Ui::FlatLabel>>(
			(QWidget*)nullptr,
			std::move(label),
			st::boxRowPadding);
		raw->delegate()->peerListSetAboveWidget(std::move(wrap));

		box->addButton(tr::lng_settings_save(), [=, &settings] {
			const auto peers = raw->collectSelected();
			// Keep whitelisted ids that never got a row in this list,
			// otherwise saving would silently drop them.
			auto ids = raw->unappliedInitialSelection();
			ids.reserve(ids.size() + peers.size());
			for (const auto &peer : peers) {
				ids.push_back(peer->id.value);
			}
			settings.setBulkDeleteWhitelist(std::move(ids));
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};

	controller->show(Box<PeerListBox>(
		std::move(chatController),
		std::move(initBox)));
}

} // namespace AyuFeatures::BulkChatManagement
