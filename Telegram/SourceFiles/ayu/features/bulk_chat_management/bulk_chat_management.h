// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/timer.h"
#include "boxes/peer_list_controllers.h"

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace AyuFeatures::BulkChatManagement {

enum class TypeFilter {
	Channels,
	Groups,
	Personal,
	Bots,
};

class BulkChatController final : public ChatsListBoxController {
public:
	enum class Mode {
		Manage,
		Whitelist,
	};

	BulkChatController(
		not_null<Main::Session*> session,
		Mode mode,
		const std::vector<int64> &initialSelected = {});

	[[nodiscard]] Main::Session &session() const override;

	void rowClicked(not_null<PeerListRow*> row) override;

	void selectAll();
	void selectFirst(int count);
	void selectByType(TypeFilter filter);
	void clearSelection();

	[[nodiscard]] int selectedCount() const;
	[[nodiscard]] rpl::producer<int> selectedCountValue() const;
	[[nodiscard]] std::vector<not_null<PeerData*>> collectSelected() const;

	// Initially selected ids that never got a matching row (e.g. the chat
	// is not in the rows list). They must be preserved on save.
	[[nodiscard]] const std::vector<int64> &unappliedInitialSelection() const {
		return _initialSelected;
	}

protected:
	std::unique_ptr<Row> createRow(not_null<History*> history) override;
	void prepareViewHook() override;

private:
	void applyInitialSelection();
	void notifySelectedChanged();
	[[nodiscard]] bool matchesType(
		not_null<PeerData*> peer,
		TypeFilter filter) const;

	const not_null<Main::Session*> _session;
	const Mode _mode;
	std::vector<int64> _initialSelected;
	rpl::variable<int> _selectedCount = 0;
	base::Timer _applyInitialSelectionTimer;

};

void Show(not_null<Window::SessionController*> controller);
void ShowWhitelistEditor(not_null<Window::SessionController*> controller);

} // namespace AyuFeatures::BulkChatManagement
