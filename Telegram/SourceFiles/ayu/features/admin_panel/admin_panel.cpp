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
#include "base/debug_log.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
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
#include "ui/controls/userpic_button.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/text/format_values.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace AyuFeatures::AdminPanel {
namespace {

constexpr auto kAutobanWarnCount = 3;
constexpr auto kAutobanDurationDays = 15;

struct ParticipantInfo {
	UserData *user = nullptr;
	bool isBanned = false;
	bool isMuted = false;
	TimeId banUntil = 0;
	TimeId muteUntil = 0;
	int activeWarns = 0;
	TimeId warnExpire = 0;
};

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

[[nodiscard]] QString FormatDuration(TimeId untilDate) {
	if (!untilDate) {
		return tr::lng_rights_chat_banned_forever(tr::now);
	}
	const auto seconds = untilDate - base::unixtime::now();
	if (seconds <= 0) {
		return tr::lng_rights_chat_banned_forever(tr::now);
	}
	return Ui::FormatMuteFor(float64(seconds));
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
	}).fail([=](const MTP::Error &error) {
		LOG(("AdminPanel: notification send failed: %1").arg(error.type()));
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
			const auto unitGroup = std::make_shared<Ui::RadiobuttonGroup>(
				state->unitIndex);
			for (auto i = 0; i < int(units.size()); ++i) {
				Ui::AddSkip(unitLayout, st::defaultVerticalListSkip);
				unitLayout->add(
					object_ptr<Ui::Radiobutton>(
						unitLayout,
						unitGroup,
						i,
						units[i],
						st::defaultBoxCheckbox),
					st::boxRowPadding);
			}
			unitGroup->setChangedCallback([=](int value) {
				state->unitIndex = value;
			});
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
							lt_duration,
							FormatDuration(untilDate),
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
							lt_duration,
							FormatDuration(untilDate),
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

void LoadRestrictedList(
		not_null<ChannelData*> channel,
		const MTPChannelParticipantsFilter &filter,
		bool markBanned,
		Fn<void(std::vector<ParticipantInfo>)> callback) {
	channel->session().api().request(MTPchannels_GetParticipants(
		channel->inputChannel(),
		filter,
		MTP_int(0),
		MTP_int(200),
		MTP_long(0)
	)).done([=](const MTPchannels_ChannelParticipants &result) {
		result.match([&](const MTPDchannels_channelParticipants &data) {
			channel->owner().processUsers(data.vusers());

			std::vector<ParticipantInfo> participants;
			for (const auto &p : data.vparticipants().v) {
				p.match([&](const MTPDchannelParticipantBanned &banned) {
					auto userId = peerToUser(peerFromMTP(banned.vpeer()));
					auto user = channel->owner().userLoaded(userId);
					if (!user) return;

					auto info = ParticipantInfo{.user = user};

					auto restrictions = ChatRestrictionsInfo(banned.vbanned_rights());
					auto flags = restrictions.flags;
					auto until = restrictions.until;

					if (markBanned || (flags & ChatRestriction::ViewMessages)) {
						info.isBanned = true;
						info.banUntil = until;
					} else {
						info.isMuted = true;
						info.muteUntil = until;
					}

					participants.push_back(info);
				}, [](const auto &) {});
			}

			callback(std::move(participants));
		}, [&](const MTPDchannels_channelParticipantsNotModified &) {
			LOG(("API Error: "
				"channels.channelParticipantsNotModified received!"));
			callback({});
		});
	}).fail([=](const MTP::Error &error) {
		LOG(("AdminPanel: failed to load participants: %1"
		).arg(error.type()));
		callback({});
	}).send();
}

void LoadRestrictedParticipants(
		not_null<ChannelData*> channel,
		Fn<void(std::vector<ParticipantInfo>)> callback) {
	// Fully banned (removed) users live in the "kicked" list, while
	// restricted (muted) ones live in the "banned" list, so both
	// lists are requested and merged.
	LoadRestrictedList(
		channel,
		MTP_channelParticipantsKicked(MTP_string()),
		true,
		[=](std::vector<ParticipantInfo> kicked) {
			LoadRestrictedList(
				channel,
				MTP_channelParticipantsBanned(MTP_string()),
				false,
				[=, kicked = std::move(kicked)](
						std::vector<ParticipantInfo> restricted) {
					auto result = std::move(kicked);
					for (auto &info : restricted) {
						const auto already = ranges::find(
							result,
							info.user,
							&ParticipantInfo::user);
						if (already != end(result)) {
							already->isMuted = info.isMuted;
							already->muteUntil = info.muteUntil;
						} else {
							result.push_back(info);
						}
					}
					callback(std::move(result));
				});
		});
}

std::vector<ParticipantInfo> LoadWarnedParticipants(
		not_null<PeerData*> peer) {
	auto warns = AyuDatabase::getWarns(static_cast<ID>(peer->id.value));
	auto now = base::unixtime::now();

	std::map<ID, std::vector<AyuWarnEntry>> warnsByUser;
	for (const auto &w : warns) {
		if (w.expiresDate == 0 || w.expiresDate > now) {
			warnsByUser[w.userId].push_back(w);
		}
	}

	std::vector<ParticipantInfo> participants;
	for (const auto &[userId, userWarns] : warnsByUser) {
		auto user = peer->owner().userLoaded(UserId(userId));
		if (!user) continue;

		auto info = ParticipantInfo{.user = user};
		info.activeWarns = userWarns.size();

		for (const auto &w : userWarns) {
			if (w.expiresDate > info.warnExpire) {
				info.warnExpire = w.expiresDate;
			}
		}

		participants.push_back(info);
	}

	return participants;
}

void LoadAllParticipants(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		Fn<void(std::vector<ParticipantInfo>)> callback) {
	auto channel = peer->asChannel();
	if (!channel) {
		// Bans/mutes are unavailable in basic groups,
		// but local warns still work there.
		callback(LoadWarnedParticipants(peer));
		return;
	}

	LoadRestrictedParticipants(channel, [=](std::vector<ParticipantInfo> restricted) {
		auto warned = LoadWarnedParticipants(peer);

		std::map<uint64, ParticipantInfo> merged;
		for (auto &r : restricted) {
			merged[r.user->id.value] = r;
		}
		for (auto &w : warned) {
			auto it = merged.find(w.user->id.value);
			if (it != merged.end()) {
				it->second.activeWarns = w.activeWarns;
				it->second.warnExpire = w.warnExpire;
			} else {
				merged[w.user->id.value] = w;
			}
		}

		std::vector<ParticipantInfo> result;
		for (auto &[_, info] : merged) {
			result.push_back(info);
		}

		callback(std::move(result));
	});
}

void AddParticipantRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		const ParticipantInfo &info,
		Fn<void()> refreshCallback) {
	const auto wrap = container->add(
		object_ptr<Ui::RpWidget>(container),
		QMargins(0, 0, 0, 0));

	const auto height = st::defaultPeerListItem.height;
	wrap->resize(wrap->width(), height);

	// Аватарка слева
	const auto userpic = Ui::CreateChild<Ui::UserpicButton>(
		wrap,
		info.user,
		st::defaultUserpicButton);
	userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
	userpic->setGeometry(
		st::defaultPeerListItem.photoPosition.x(),
		st::defaultPeerListItem.photoPosition.y(),
		st::defaultUserpicButton.size.width(),
		st::defaultUserpicButton.size.height());

	// Контейнер для текста (имя, bio, статус)
	const auto textLeft = st::defaultPeerListItem.namePosition.x();
	const auto textTop = st::defaultPeerListItem.namePosition.y();

	// Имя пользователя (крупный шрифт)
	const auto nameLabel = Ui::CreateChild<Ui::FlatLabel>(
		wrap,
		rpl::single(info.user->name()),
		st::defaultFlatLabel);
	nameLabel->moveToLeft(textLeft, textTop);

	// Bio под именем (если есть и короткое)
	auto bioText = info.user->about();
	const auto hasBio = !bioText.isEmpty() && bioText.length() < 50;
	Ui::FlatLabel *bioLabel = nullptr;
	if (hasBio) {
		bioLabel = Ui::CreateChild<Ui::FlatLabel>(
			wrap,
			rpl::single(bioText),
			st::defaultFlatLabel);
		bioLabel->moveToLeft(textLeft, textTop + st::semiboldFont->height);
	}

	// Статус с таймерами под bio
	auto now = base::unixtime::now();
	auto statusText = QString();

	if (info.isBanned && info.banUntil > now) {
		statusText = tr::ayu_AdminBanExpires(tr::now) + " "
			+ Ui::FormatTTL(info.banUntil - now);
	}
	if (info.isMuted && info.muteUntil > now) {
		if (!statusText.isEmpty()) statusText += " | ";
		statusText += tr::ayu_AdminMuteExpires(tr::now) + " "
			+ Ui::FormatTTL(info.muteUntil - now);
	}
	if (info.activeWarns > 0) {
		if (!statusText.isEmpty()) statusText += " | ";
		statusText += QString::number(info.activeWarns) + " "
			+ tr::ayu_AdminPillWarnShort(tr::now);
		if (info.warnExpire > now) {
			statusText += " (" + Ui::FormatTTL(info.warnExpire - now) + ")";
		}
	}

	Ui::FlatLabel *statusLabel = nullptr;
	if (!statusText.isEmpty()) {
		const auto statusTop = hasBio
			? textTop + st::semiboldFont->height + st::normalFont->height
			: textTop + st::semiboldFont->height;
		statusLabel = Ui::CreateChild<Ui::FlatLabel>(
			wrap,
			rpl::single(statusText),
			st::defaultFlatLabel);
		statusLabel->moveToLeft(textLeft, statusTop);
	}

	// Кнопки действий горизонтально справа.
	auto buttons = std::vector<Ui::RoundButton*>();
	const auto addButton = [&](const QString &text, Fn<void()> handler) {
		const auto btn = Ui::CreateChild<Ui::RoundButton>(
			wrap,
			rpl::single(text),
			st::defaultLightButton);
		btn->setClickedCallback(std::move(handler));
		buttons.push_back(btn);
	};

	if (info.activeWarns > 0) {
		addButton(tr::ayu_AdminRemoveWarn(tr::now), [=] {
			AyuDatabase::removeAllWarns(
				static_cast<ID>(peer->id.value),
				static_cast<ID>(peerToUser(info.user->id).bare));
			controller->showToast(tr::ayu_AdminDone(tr::now));
			refreshCallback();
		});
	}

	if (info.isMuted) {
		addButton(tr::ayu_AdminRemoveMute(tr::now), [=] {
			PerformUnmute(peer, info.user);
			controller->showToast(tr::ayu_AdminDone(tr::now));
			refreshCallback();
		});
	}

	if (info.isBanned) {
		addButton(tr::ayu_AdminRemoveBan(tr::now), [=] {
			PerformUnban(peer, info.user);
			controller->showToast(tr::ayu_AdminDone(tr::now));
			refreshCallback();
		});
	}

	// Вся раскладка зависит от ширины, поэтому считается здесь,
	// а не в момент создания (ширина тогда ещё нулевая).
	wrap->widthValue(
	) | rpl::on_next([=](int newWidth) {
		if (newWidth <= 0) {
			return;
		}
		wrap->resize(newWidth, height);
		userpic->moveToLeft(
			st::defaultPeerListItem.photoPosition.x(),
			st::defaultPeerListItem.photoPosition.y());

		const auto buttonY = (height - st::defaultLightButton.height) / 2;
		const auto buttonSpacing = st::boxRowPadding.left();
		auto buttonX = newWidth - st::boxRowPadding.right();
		for (const auto btn : buttons) {
			buttonX -= btn->width();
			btn->moveToLeft(buttonX, buttonY);
			buttonX -= buttonSpacing;
		}

		const auto textWidth = std::max(
			buttonX - textLeft,
			st::defaultPeerListItem.namePosition.x());
		nameLabel->resizeToWidth(textWidth);
		if (bioLabel) {
			bioLabel->resizeToWidth(textWidth);
		}
		if (statusLabel) {
			statusLabel->resizeToWidth(textWidth);
		}
	}, wrap->lifetime());

	// Разделитель после строки.
	Ui::AddDivider(container);
}

void ShowPanelBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	if (!peer->asChannel()) {
		// Basic groups have no ban/mute API, but warns are local.
		controller->showToast(tr::ayu_AdminOnlyChannels(tr::now));
	}

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_AdminPanel());

		struct State {
			std::vector<ParticipantInfo> allParticipants;
			int currentTab = 0;
		};
		auto state = box->lifetime().make_state<State>();

		auto slider = box->addRow(
			object_ptr<Ui::SettingsSlider>(box, st::settingsSlider),
			st::boxRowPadding);
		slider->addSection(tr::ayu_AdminTabBanned(tr::now));
		slider->addSection(tr::ayu_AdminTabMuted(tr::now));
		slider->addSection(tr::ayu_AdminTabWarned(tr::now));

		auto mainContent = box->addRow(object_ptr<Ui::VerticalLayout>(box));

		auto bannedWrap = mainContent->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				mainContent,
				object_ptr<Ui::VerticalLayout>(mainContent)));
		auto mutedWrap = mainContent->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				mainContent,
				object_ptr<Ui::VerticalLayout>(mainContent)));
		auto warnedWrap = mainContent->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				mainContent,
				object_ptr<Ui::VerticalLayout>(mainContent)));

		bannedWrap->toggle(true, anim::type::instant);
		mutedWrap->toggle(false, anim::type::instant);
		warnedWrap->toggle(false, anim::type::instant);

		auto wraps = std::array{bannedWrap, mutedWrap, warnedWrap};

		const auto refresh = std::make_shared<Fn<void()>>();
		const auto weakBox = base::make_weak(box.get());
		*refresh = [=] {
			LoadAllParticipants(controller, peer, [=](std::vector<ParticipantInfo> participants) {
				if (!weakBox) {
					// The box was closed before the request finished.
					return;
				}
				state->allParticipants = std::move(participants);

				bannedWrap->entity()->clear();
				mutedWrap->entity()->clear();
				warnedWrap->entity()->clear();

				for (const auto &p : state->allParticipants) {
					if (p.isBanned) {
						AddParticipantRow(bannedWrap->entity(), controller, peer, p, *refresh);
					}
				}
				if (bannedWrap->entity()->count() == 0) {
					bannedWrap->entity()->add(
						object_ptr<Ui::FlatLabel>(
							bannedWrap->entity(),
							tr::ayu_AdminListEmpty(),
							st::boxLabel),
						st::boxRowPadding);
				}

				for (const auto &p : state->allParticipants) {
					if (p.isMuted) {
						AddParticipantRow(mutedWrap->entity(), controller, peer, p, *refresh);
					}
				}
				if (mutedWrap->entity()->count() == 0) {
					mutedWrap->entity()->add(
						object_ptr<Ui::FlatLabel>(
							mutedWrap->entity(),
							tr::ayu_AdminListEmpty(),
							st::boxLabel),
						st::boxRowPadding);
				}

				for (const auto &p : state->allParticipants) {
					if (p.activeWarns > 0) {
						AddParticipantRow(warnedWrap->entity(), controller, peer, p, *refresh);
					}
				}
				if (warnedWrap->entity()->count() == 0) {
					warnedWrap->entity()->add(
						object_ptr<Ui::FlatLabel>(
							warnedWrap->entity(),
							tr::ayu_AdminListEmpty(),
							st::boxLabel),
						st::boxRowPadding);
				}
			});
		};

		slider->sectionActivated(
		) | rpl::on_next([=](int index) {
			state->currentTab = index;
			for (auto i = 0; i < wraps.size(); ++i) {
				wraps[i]->toggle(i == index, anim::type::normal);
			}
		}, slider->lifetime());

		(*refresh)();

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
				Ui::AddSkip(content, st::defaultVerticalListSkip);
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
			if (peer->isChannel()) {
				// Per-user mute is unavailable in basic groups.
				addBtn(tr::ayu_AdminMute(tr::now), ActionType::Mute);
			}
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
