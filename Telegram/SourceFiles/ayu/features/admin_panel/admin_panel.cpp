// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/admin_panel/admin_panel.h"

#include "api/api_chat_participants.h"
#include "apiwrap.h"
#include "ayu/data/ayu_database.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_participant_status.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

namespace AyuFeatures::AdminPanel {
namespace {

constexpr auto kAutobanWarnCount = 3;
constexpr auto kAutobanDurationDays = 15;

[[nodiscard]] bool IsAdmin(not_null<PeerData*> peer) {
	if (const auto channel = peer->asChannel()) {
		return channel->amCreator()
			|| (channel->adminRights() != ChatAdminRights());
	}
	if (const auto chat = peer->asChat()) {
		return chat->amCreator()
			|| chat->hasAdminRights();
	}
	return false;
}

[[nodiscard]] not_null<UserData*> ExtractUser(
		not_null<HistoryItem*> item) {
	return item->from()->asUser()
		? not_null(item->from()->asUser())
		: item->history()->session().user();
}

[[nodiscard]] QString UserName(not_null<UserData*> user) {
	if (!user->username().isEmpty()) {
		return u"@"_q + user->username();
	}
	return user->name();
}

[[nodiscard]] TimeId ComputeUntilDate(int value, int unitIndex) {
	if (value <= 0) {
		return 0;
	}
	auto multiplier = 1;
	switch (unitIndex) {
	case 0: multiplier = 1; break;
	case 1: multiplier = 60; break;
	case 2: multiplier = 3600; break;
	case 3: multiplier = 86400; break;
	case 4: multiplier = 604800; break;
	case 5: multiplier = 2592000; break;
	case 6: multiplier = 31536000; break;
	}
	return base::unixtime::now() + value * multiplier;
}

void SendNotification(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		const QString &text) {
	auto &api = peer->session().api();
	auto flags = MTPmessages_SendMessage::Flags(0);
	api.request(MTPmessages_SendMessage(
		MTP_flags(flags),
		peer->input(),
		MTPInputReplyTo(),
		MTP_string(text),
		MTP_long(base::RandomValue<uint64>()),
		MTPReplyMarkup(),
		MTP_vector<MTPMessageEntity>(),
		MTPint(),
		MTPint(),
		MTPInputPeer(),
		MTPInputQuickReplyShortcut(),
		MTPlong(),
		MTPlong(),
		MTPSuggestedPost()
	)).done([=](const MTPUpdates &result) {
		peer->session().api().applyUpdates(result);
	}).send();
}

void PerformBan(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		not_null<UserData*> user,
		TimeId untilDate) {
	if (const auto channel = peer->asChannel()) {
		auto flags = ChatRestriction::ViewMessages
			| ChatRestriction::SendPhotos
			| ChatRestriction::SendVideos
			| ChatRestriction::SendVideoMessages
			| ChatRestriction::SendMusic
			| ChatRestriction::SendVoiceMessages
			| ChatRestriction::SendFiles
			| ChatRestriction::SendOther
			| ChatRestriction::SendStickers
			| ChatRestriction::SendGifs
			| ChatRestriction::SendInline
			| ChatRestriction::SendPolls
			| ChatRestriction::SendGames
			| ChatRestriction::EmbedLinks
			| ChatRestriction::AddParticipants
			| ChatRestriction::PinMessages
			| ChatRestriction::ChangeInfo;
		auto rights = ChatRestrictionsInfo(flags, untilDate);
		peer->session().api().chatParticipants().kick(
			channel,
			user,
			rights);
	} else if (const auto chat = peer->asChat()) {
		peer->session().api().chatParticipants().kick(chat, user);
	}
}

void PerformMute(
		not_null<PeerData*> peer,
		not_null<UserData*> user,
		TimeId untilDate) {
	if (const auto channel = peer->asChannel()) {
		auto flags = ChatRestriction::SendPhotos
			| ChatRestriction::SendVideos
			| ChatRestriction::SendVideoMessages
			| ChatRestriction::SendMusic
			| ChatRestriction::SendVoiceMessages
			| ChatRestriction::SendFiles
			| ChatRestriction::SendOther
			| ChatRestriction::SendStickers
			| ChatRestriction::SendGifs
			| ChatRestriction::SendInline
			| ChatRestriction::SendPolls
			| ChatRestriction::SendGames
			| ChatRestriction::EmbedLinks;
		auto rights = RestrictionsToMTP(ChatRestrictionsInfo(flags, untilDate));
		peer->session().api().request(MTPchannels_EditBanned(
			channel->inputChannel(),
			user->input(),
			rights
		)).done([=](const MTPUpdates &result) {
			peer->session().api().applyUpdates(result);
		}).send();
	}
}

void PerformKick(
		not_null<PeerData*> peer,
		not_null<UserData*> user) {
	if (const auto channel = peer->asChannel()) {
		peer->session().api().chatParticipants().kick(
			channel,
			user,
			ChatRestrictionsInfo());
	} else if (const auto chat = peer->asChat()) {
		peer->session().api().chatParticipants().kick(chat, user);
	}
}

void PerformUnban(
		not_null<PeerData*> peer,
		not_null<UserData*> user) {
	if (const auto channel = peer->asChannel()) {
		peer->session().api().chatParticipants().unblock(channel, user);
	}
}

void PerformUnmute(
		not_null<PeerData*> peer,
		not_null<UserData*> user) {
	if (const auto channel = peer->asChannel()) {
		peer->session().api().request(MTPchannels_EditBanned(
			channel->inputChannel(),
			user->input(),
			MTP_chatBannedRights(MTP_flags(0), MTP_int(0))
		)).done([=](const MTPUpdates &result) {
			peer->session().api().applyUpdates(result);
		}).send();
	}
}

void ShowActionBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		not_null<UserData*> user,
		ActionType type) {
	const auto myUser = peer->session().user();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto titles = std::array{
			tr::ayu_AdminKick(tr::now),
			tr::ayu_AdminBan(tr::now),
			tr::ayu_AdminMute(tr::now),
			tr::ayu_AdminWarn(tr::now),
		};
		box->setTitle(rpl::single(titles[int(type)]));

		struct State {
			int unitIndex = 2;
		};
		const auto state = box->lifetime().make_state<State>();

		Ui::InputField *durationInput = nullptr;

		if (type != ActionType::Kick) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					tr::ayu_AdminDurationTitle(),
					st::boxLabel),
				st::boxRowPadding);

			durationInput = box->addRow(object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				tr::ayu_AdminDurationPlaceholder()));

			const auto units = std::array{
				tr::ayu_AdminDurationSeconds(tr::now),
				tr::ayu_AdminDurationMinutes(tr::now),
				tr::ayu_AdminDurationHours(tr::now),
				tr::ayu_AdminDurationDays(tr::now),
				tr::ayu_AdminDurationWeeks(tr::now),
				tr::ayu_AdminDurationMonths(tr::now),
				tr::ayu_AdminDurationYears(tr::now),
			};
			const auto unitLayout = box->addRow(
				object_ptr<Ui::VerticalLayout>(box));
			for (auto i = 0; i < int(units.size()); ++i) {
				const auto btn = unitLayout->add(
					object_ptr<Ui::RoundButton>(
						unitLayout,
						rpl::single(units[i]),
						st::defaultActiveButton));
				btn->setClickedCallback([=] {
					state->unitIndex = i;
				});
			}
		}

		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				tr::ayu_AdminReasonTitle(),
				st::boxLabel),
			st::boxRowPadding);
		const auto reasonField = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::ayu_AdminReasonPlaceholder()));

		const auto notifyCheck = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				tr::ayu_AdminNotify(tr::now),
				true,
				st::defaultBoxCheckbox),
			st::boxRowPadding);

		box->addButton(tr::ayu_AdminConfirmAction(), [=] {
			auto reason = reasonField->getLastText();
			auto untilDate = TimeId(0);
			if (type != ActionType::Kick && durationInput) {
				auto val = durationInput->getLastText().toInt();
				untilDate = ComputeUntilDate(val, state->unitIndex);
			}

			auto adminName = UserName(myUser);
			auto userName = UserName(user);

			switch (type) {
			case ActionType::Kick:
				PerformKick(peer, user);
				if (notifyCheck->checked()) {
					SendNotification(controller, peer,
						tr::ayu_AdminNotifyKick(
							tr::now,
							lt_user,
							userName,
							lt_admin,
							adminName,
							lt_reason,
							reason));
				}
				break;
			case ActionType::Ban:
				PerformBan(controller, peer, user, untilDate);
				if (notifyCheck->checked()) {
					SendNotification(controller, peer,
						tr::ayu_AdminNotifyBan(
							tr::now,
							lt_user,
							userName,
							lt_admin,
							adminName,
							lt_reason,
							reason));
				}
				break;
			case ActionType::Mute:
				PerformMute(peer, user, untilDate);
				if (notifyCheck->checked()) {
					SendNotification(controller, peer,
						tr::ayu_AdminNotifyMute(
							tr::now,
							lt_user,
							userName,
							lt_admin,
							adminName,
							lt_reason,
							reason));
				}
				break;
			case ActionType::Warn: {
				auto warn = AyuWarnEntry{
					.fakeId = 0,
					.chatId = static_cast<ID>(peer->id.value),
					.userId = static_cast<ID>(peerToUser(user->id).bare),
					.adminId = static_cast<ID>(peerToUser(myUser->id).bare),
					.reason = reason.toStdString(),
					.createdDate = base::unixtime::now(),
					.expiresDate = untilDate,
				};
				AyuDatabase::addWarn(warn);
				auto warnCount = AyuDatabase::getActiveWarnCount(
					static_cast<ID>(peer->id.value),
					static_cast<ID>(peerToUser(user->id).bare));
				if (warnCount >= kAutobanWarnCount) {
					auto autoUntil = base::unixtime::now()
						+ kAutobanDurationDays * 86400;
					PerformBan(controller, peer, user, autoUntil);
					AyuDatabase::removeAllWarns(
						static_cast<ID>(peer->id.value),
						static_cast<ID>(peerToUser(user->id).bare));
					if (notifyCheck->checked()) {
						SendNotification(controller, peer,
							tr::ayu_AdminAutobanWarn(
								tr::now,
								lt_user,
								userName,
								lt_admin,
								adminName));
					}
				} else if (notifyCheck->checked()) {
					SendNotification(controller, peer,
						tr::ayu_AdminNotifyWarn(
							tr::now,
							lt_user,
							userName,
							lt_admin,
							adminName,
							lt_reason,
							reason));
				}
				break;
			}
			}

			box->closeBox();
			controller->showToast(tr::ayu_AdminDone(tr::now));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowPanelBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_AdminPanel());

		const auto myUser = peer->session().user();
		const auto channel = peer->asChannel();
		if (!channel) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					tr::ayu_AdminNoEntries(),
					st::boxLabel),
				st::boxRowPadding);
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
			return;
		}

		struct State {
			bool notify = false;
		};
		const auto state = box->lifetime().make_state<State>();

		const auto notifyCheck = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				tr::ayu_AdminNotify(tr::now),
				false,
				st::defaultBoxCheckbox),
			st::boxRowPadding);
		notifyCheck->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			state->notify = checked;
		}, notifyCheck->lifetime());

		const auto content = box->addRow(
			object_ptr<Ui::VerticalLayout>(box));

		const auto addSection = [&](
				const QString &title,
				auto &&entries,
				auto &&removeCallback) {
			content->add(
				object_ptr<Ui::FlatLabel>(
					content,
					rpl::single(title),
					st::boxLabel),
				st::boxRowPadding);
			if (entries.empty()) {
				content->add(
					object_ptr<Ui::FlatLabel>(
						content,
						tr::ayu_AdminNoEntries(),
						st::boxLabel),
					st::boxRowPadding);
			}
			for (auto &entry : entries) {
				const auto row = content->add(
					object_ptr<Ui::RoundButton>(
						content,
						rpl::single(entry.name),
						st::defaultActiveButton),
					st::boxRowPadding);
				row->setClickedCallback(entry.removeAction);
			}
		};

		auto warns = AyuDatabase::getWarns(static_cast<ID>(peer->id.value));
		struct EntryInfo {
			QString name;
			Fn<void()> removeAction;
		};
		auto warnEntries = std::vector<EntryInfo>();
		for (const auto &w : warns) {
			auto userId = UserId(w.userId);
			const auto warnUser = peer->owner().userLoaded(userId);
			if (!warnUser) {
				continue;
			}
			auto displayName = warnUser->name();
			auto fakeId = w.fakeId;
			warnEntries.push_back({
				.name = displayName + u" — "_q + tr::ayu_AdminRemoveWarn(tr::now),
				.removeAction = [=] {
					AyuDatabase::removeWarn(fakeId);
					if (state->notify) {
						SendNotification(controller, peer,
							tr::ayu_AdminNotifyUnwarn(
								tr::now,
								lt_user,
								UserName(warnUser),
								lt_admin,
								UserName(myUser)));
					}
					box->closeBox();
					ShowPanelBox(controller, peer);
				},
			});
		}
		addSection(tr::ayu_AdminPanelWarns(tr::now),
			warnEntries,
			nullptr);

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace

void AddAdminAction(
		not_null<Ui::PopupMenu*> menu,
		HistoryItem *item,
		not_null<Window::SessionController*> controller) {
	if (!item) {
		return;
	}
	const auto peer = item->history()->peer;
	if (!IsAdmin(peer)) {
		return;
	}
	const auto user = item->from()->asUser();
	if (!user || user->isSelf()) {
		return;
	}

	menu->addAction(tr::ayu_AdminPanel(tr::now), [=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(tr::ayu_AdminPanel());
			const auto content = box->addRow(
				object_ptr<Ui::VerticalLayout>(box));
			const auto addBtn = [&](
					const QString &text,
					ActionType type) {
				content->add(
					object_ptr<Ui::RoundButton>(
						content,
						rpl::single(text),
						st::defaultActiveButton),
					st::boxRowPadding
				)->setClickedCallback([=] {
					box->closeBox();
					ShowActionBox(controller, peer, user, type);
				});
			};
			addBtn(tr::ayu_AdminKick(tr::now), ActionType::Kick);
			addBtn(tr::ayu_AdminBan(tr::now), ActionType::Ban);
			addBtn(tr::ayu_AdminMute(tr::now), ActionType::Mute);
			addBtn(tr::ayu_AdminWarn(tr::now), ActionType::Warn);
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		}));
	}, &st::menuIconAdmin);
}

void ShowAdminPanel(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	if (!IsAdmin(peer)) {
		return;
	}
	ShowPanelBox(controller, peer);
}

} // namespace AyuFeatures::AdminPanel
