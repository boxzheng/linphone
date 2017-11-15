/***************************************************************************
 *            chat.c
 *
 *  Sun Jun  5 19:34:18 2005
 *  Copyright  2005  Simon Morlat
 *  Email simon dot morlat at linphone dot org
 ****************************************************************************/

/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlwriter.h>

#include <linphone/utils/utils.h>

#include "linphone/core.h"
#include "private.h"
#include "linphone/lpconfig.h"
#include "belle-sip/belle-sip.h"
#include "ortp/b64.h"
#include "linphone/wrapper_utils.h"

#include "c-wrapper/c-wrapper.h"
#include "chat/chat-room/basic-chat-room.h"
#include "chat/chat-room/client-group-chat-room.h"
#include "chat/chat-room/real-time-text-chat-room-p.h"
#include "chat/chat-room/real-time-text-chat-room.h"
#include "content/content-type.h"
#include "core/core-p.h"

using namespace std;

void linphone_core_disable_chat(LinphoneCore *lc, LinphoneReason deny_reason) {
	lc->chat_deny_code = deny_reason;
}

void linphone_core_enable_chat(LinphoneCore *lc) {
	lc->chat_deny_code = LinphoneReasonNone;
}

bool_t linphone_core_chat_enabled(const LinphoneCore *lc) {
	return lc->chat_deny_code != LinphoneReasonNone;
}

const bctbx_list_t *linphone_core_get_chat_rooms (LinphoneCore *lc) {
	if (lc->chat_rooms)
		bctbx_list_free_with_data(lc->chat_rooms, (bctbx_list_free_func)linphone_chat_room_unref);
	lc->chat_rooms = L_GET_RESOLVED_C_LIST_FROM_CPP_LIST(lc->cppCore->getChatRooms());
	return lc->chat_rooms;
}

static LinphoneChatRoom *linphone_chat_room_new (LinphoneCore *core, const LinphoneAddress *addr) {
	return L_GET_C_BACK_PTR(core->cppCore->getOrCreateBasicChatRoom(
		*L_GET_CPP_PTR_FROM_C_OBJECT(addr),
		linphone_core_realtime_text_enabled(core)
	));
}

LinphoneChatRoom *_linphone_core_create_chat_room_from_call(LinphoneCall *call){
	LinphoneChatRoom *cr = linphone_chat_room_new(linphone_call_get_core(call),
		linphone_address_clone(linphone_call_get_remote_address(call)));
	linphone_chat_room_set_call(cr, call);
	return cr;
}

LinphoneChatRoom *linphone_core_get_chat_room (LinphoneCore *lc, const LinphoneAddress *addr) {
	return L_GET_C_BACK_PTR(lc->cppCore->getOrCreateBasicChatRoom(*L_GET_CPP_PTR_FROM_C_OBJECT(addr)));
}

LinphoneChatRoom *linphone_core_create_client_group_chat_room (LinphoneCore *lc, const char *subject) {
	return L_GET_C_BACK_PTR(lc->cppCore->createClientGroupChatRoom(L_C_TO_STRING(subject)));
}

LinphoneChatRoom *_linphone_core_join_client_group_chat_room (LinphoneCore *lc, const LinphonePrivate::Address &addr) {
	LinphoneChatRoom *cr = _linphone_client_group_chat_room_new(lc, addr.asString().c_str(), nullptr);
	L_GET_CPP_PTR_FROM_C_OBJECT(cr)->join();
	L_GET_PRIVATE(lc->cppCore)->insertChatRoom(L_GET_CPP_PTR_FROM_C_OBJECT(cr));
	L_GET_PRIVATE(lc->cppCore)->insertChatRoomWithDb(L_GET_CPP_PTR_FROM_C_OBJECT(cr));
	return cr;
}

LinphoneChatRoom *_linphone_core_create_server_group_chat_room (LinphoneCore *lc, LinphonePrivate::SalCallOp *op) {
	LinphoneChatRoom *cr = _linphone_server_group_chat_room_new(lc, op);
	L_GET_PRIVATE(lc->cppCore)->insertChatRoom(L_GET_CPP_PTR_FROM_C_OBJECT(cr));
	L_GET_PRIVATE(lc->cppCore)->insertChatRoomWithDb(L_GET_CPP_PTR_FROM_C_OBJECT(cr));
	return cr;
}

void linphone_core_delete_chat_room (LinphoneCore *, LinphoneChatRoom *cr) {
	LinphonePrivate::Core::deleteChatRoom(L_GET_CPP_PTR_FROM_C_OBJECT(cr));
}

LinphoneChatRoom *linphone_core_get_chat_room_from_uri(LinphoneCore *lc, const char *to) {
	return L_GET_C_BACK_PTR(lc->cppCore->getOrCreateBasicChatRoomFromUri(L_C_TO_STRING(to)));
}

int linphone_core_message_received(LinphoneCore *lc, LinphonePrivate::SalOp *op, const SalMessage *sal_msg) {
	LinphoneReason reason = LinphoneReasonNotAcceptable;
	const char *peerAddress = linphone_core_conference_server_enabled(lc) ? op->get_to() : op->get_from();

	// TODO: Use local address.
	list<shared_ptr<LinphonePrivate::ChatRoom>> chatRooms = lc->cppCore->findChatRooms(
		LinphonePrivate::SimpleAddress(peerAddress)
	);

	if (!chatRooms.empty())
		reason = L_GET_PRIVATE(chatRooms.front())->messageReceived(op, sal_msg);
	else {
		LinphoneAddress *addr = linphone_address_new(sal_msg->from);
		linphone_address_clean(addr);
		LinphoneChatRoom *cr = linphone_core_get_chat_room(lc, addr);
		if (cr)
			reason = L_GET_PRIVATE_FROM_C_OBJECT(cr)->messageReceived(op, sal_msg);
		linphone_address_unref(addr);
	}
	return reason;
}

void linphone_core_real_time_text_received(LinphoneCore *lc, LinphoneChatRoom *cr, uint32_t character, LinphoneCall *call) {
	if (linphone_core_realtime_text_enabled(lc)) {
		shared_ptr<LinphonePrivate::RealTimeTextChatRoom> rttcr =
			static_pointer_cast<LinphonePrivate::RealTimeTextChatRoom>(L_GET_CPP_PTR_FROM_C_OBJECT(cr));
		L_GET_PRIVATE(rttcr)->realtimeTextReceived(character, call);
		//L_GET_PRIVATE(static_pointer_cast<LinphonePrivate::RealTimeTextChatRoom>(L_GET_CPP_PTR_FROM_C_OBJECT(cr)))->realtimeTextReceived(character, call);
	}
}
