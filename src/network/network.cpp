/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file network.cpp Base functions for networking support. */

#include "../stdafx.h"

#include "../core/string_builder.hpp"
#include "../strings_func.h"
#include "../command_func.h"
#include "../date_func.h"
#include "network_admin.h"
#include "network_client.h"
#include "network_query.h"
#include "network_server.h"
#include "network_content.h"
#include "network_udp.h"
#include "network_gamelist.h"
#include "network_base.h"
#include "network_coordinator.h"
#include "core/udp.h"
#include "core/host.h"
#include "network_gui.h"
#include "../console_func.h"
#include "../3rdparty/md5/md5.h"
#include "../core/random_func.hpp"
#include "../window_func.h"
#include "../company_func.h"
#include "../company_base.h"
#include "../landscape_type.h"
#include "../rev.h"
#include "../core/pool_func.hpp"
#include "../gfx_func.h"
#include "../error.h"
#include "../core/checksum_func.hpp"
#include "../string_func.h"
#include "../string_func_extra.h"
#include "../core/serialisation.hpp"
#include "../3rdparty/monocypher/monocypher.h"
#include "../settings_internal.h"
#include "../misc_cmd.h"
#ifdef DEBUG_DUMP_COMMANDS
#	include "../fileio_func.h"
#	include "../3rdparty/nlohmann/json.hpp"
#	include <charconv>
#endif
#include <tuple>

#ifdef DEBUG_DUMP_COMMANDS
/** Helper variable to make the dedicated server go fast until the (first) join.
 * Used to load the desync debug logs, i.e. for reproducing a desync.
 * There's basically no need to ever enable this, unless you really know what
 * you are doing, i.e. debugging a desync.
 * See docs/desync.md for details. */
bool _ddc_fastforward = true;
#endif /* DEBUG_DUMP_COMMANDS */

#include "../safeguards.h"

/** Make sure both pools have the same size. */
static_assert(NetworkClientInfoPool::MAX_SIZE == NetworkClientSocketPool::MAX_SIZE);

/** The pool with client information. */
NetworkClientInfoPool _networkclientinfo_pool("NetworkClientInfo");
INSTANTIATE_POOL_METHODS(NetworkClientInfo)

bool _networking;                                       ///< are we in networking mode?
bool _network_server;                                   ///< network-server is active
bool _network_available;                                ///< is network mode available?
bool _network_dedicated;                                ///< are we a dedicated server?
bool _is_network_server;                                ///< Does this client wants to be a network-server?
bool _network_settings_access;                          ///< Can this client change server settings?
NetworkCompanyState *_network_company_states = nullptr; ///< Statistics about some companies.
std::string _network_company_server_id;                 ///< Server ID string used for company passwords
std::array<uint8_t, 16> _network_company_password_storage_token; ///< Non-secret token for storage of company passwords in savegames
std::array<uint8_t, 32> _network_company_password_storage_key;   ///< Key for storage of company passwords in savegames
ClientID _network_own_client_id;                        ///< Our client identifier.
ClientID _redirect_console_to_client;                   ///< If not invalid, redirect the console output to a client.
uint8_t _network_reconnect;                             ///< Reconnect timeout
StringList _network_bind_list;                          ///< The addresses to bind on.
StringList _network_host_list;                          ///< The servers we know.
StringList _network_ban_list;                           ///< The banned clients.
uint32_t _frame_counter_server;                         ///< The frame_counter of the server, if in network-mode
uint32_t _frame_counter_max;                            ///< To where we may go with our clients
uint32_t _frame_counter;                                ///< The current frame.
uint32_t _last_sync_frame;                              ///< Used in the server to store the last time a sync packet was sent to clients.
NetworkAddressList _broadcast_list;                     ///< List of broadcast addresses.
uint32_t _sync_seed_1;                                  ///< Seed to compare during sync checks.
uint64_t _sync_state_checksum;                          ///< State checksum to compare during sync checks.
uint32_t _sync_frame;                                   ///< The frame to perform the sync check.
EconTime::Date   _last_sync_date;                       ///< The game date of the last successfully received sync frame
EconTime::DateFract _last_sync_date_fract;              ///< "
uint8_t  _last_sync_tick_skip_counter;                  ///< "
uint32_t _last_sync_frame_counter;                      ///< "
bool _network_first_time;                               ///< Whether we have finished joining or not.
CompanyMask _network_company_passworded;                ///< Bitmask of the password status of all companies.

ring_buffer<NetworkSyncRecord> _network_sync_records;
ring_buffer<uint> _network_sync_record_counts;
bool _record_sync_records = false;

/** The amount of clients connected */
uint8_t _network_clients_connected = 0;

extern std::string GenerateUid(std::string_view subject);

/**
 * Return whether there is any client connected or trying to connect at all.
 * @return whether we have any client activity
 */
bool HasClients()
{
	return !NetworkClientSocket::Iterate().empty();
}

/**
 * Basically a client is leaving us right now.
 */
NetworkClientInfo::~NetworkClientInfo()
{
	/* Delete the chat window, if you were chatting with this client. */
	InvalidateWindowData(WC_SEND_NETWORK_MSG, DESTTYPE_CLIENT, this->client_id);
}

/**
 * Return the CI given it's client-identifier
 * @param client_id the ClientID to search for
 * @return return a pointer to the corresponding NetworkClientInfo struct or nullptr when not found
 */
/* static */ NetworkClientInfo *NetworkClientInfo::GetByClientID(ClientID client_id)
{
	for (NetworkClientInfo *ci : NetworkClientInfo::Iterate()) {
		if (ci->client_id == client_id) return ci;
	}

	return nullptr;
}

/**
 * Return the client state given it's client-identifier
 * @param client_id the ClientID to search for
 * @return return a pointer to the corresponding NetworkClientSocket struct or nullptr when not found
 */
/* static */ ServerNetworkGameSocketHandler *ServerNetworkGameSocketHandler::GetByClientID(ClientID client_id)
{
	for (NetworkClientSocket *cs : NetworkClientSocket::Iterate()) {
		if (cs->client_id == client_id) return cs;
	}

	return nullptr;
}


/**
 * Simple helper to find the location of the given authorized key in the authorized keys.
 * @param authorized_keys The keys to look through.
 * @param authorized_key The key to look for.
 * @return The iterator to the location of the authorized key, or \c authorized_keys.end().
 */
static auto FindKey(auto *authorized_keys, std::string_view authorized_key)
{
	return std::ranges::find_if(*authorized_keys, [authorized_key](auto &value) { return StrEqualsIgnoreCase(value, authorized_key); });
}

/**
 * Check whether the given key is contains in these authorized keys.
 * @param key The key to look for.
 * @return \c true when the key has been found, otherwise \c false.
 */
bool NetworkAuthorizedKeys::Contains(std::string_view key) const
{
	return FindKey(this, key) != this->end();
}

/**
 * Add the given key to the authorized keys, when it is not already contained.
 * @param key The key to add.
 * @return \c true when the key was added, \c false when the key already existed or the key was empty.
 */
bool NetworkAuthorizedKeys::Add(std::string_view key)
{
	if (key.empty()) return false;

	auto iter = FindKey(this, key);
	if (iter != this->end()) return false;

	this->emplace_back(key);
	return true;
}

/**
 * Remove the given key from the authorized keys, when it is exists.
 * @param key The key to remove.
 * @return \c true when the key was removed, \c false when the key did not exist.
 */
bool NetworkAuthorizedKeys::Remove(std::string_view key)
{
	auto iter = FindKey(this, key);
	if (iter == this->end()) return false;

	this->erase(iter);
	return true;
}


uint8_t NetworkSpectatorCount()
{
	uint8_t count = 0;

	for (const NetworkClientInfo *ci : NetworkClientInfo::Iterate()) {
		if (ci->client_playas == COMPANY_SPECTATOR) count++;
	}

	/* Don't count a dedicated server as spectator */
	if (_network_dedicated) count--;

	return count;
}

uint NetworkClientCount() {
	return (uint)NetworkClientInfo::GetNumItems();
}

/**
 * Change the company password of a given company.
 * @param company_id ID of the company the password should be changed for.
 * @param password The unhashed password we like to set ('*' or '' resets the password)
 * @return The password.
 */
std::string NetworkChangeCompanyPassword(CompanyID company_id, std::string password)
{
	if (password.compare("*") == 0) password = "";

	if (_network_server) {
		NetworkServerSetCompanyPassword(company_id, password, false);
	} else {
		NetworkClientSetCompanyPassword(password);
	}

	return password;
}

/**
 * Hash the given password using server ID and game seed.
 * @param password Password to hash.
 * @param password_server_id Server ID.
 * @param password_game_seed Game seed.
 * @return The hashed password.
 */
std::string GenerateCompanyPasswordHash(const std::string &password, const std::string &password_server_id, uint32_t password_game_seed)
{
	if (password.empty()) return password;

	size_t password_length = password.size();
	size_t password_server_id_length = password_server_id.size();

	std::string salted_password_string;
	/* Add the password with the server's ID and game seed as the salt. */
	for (uint i = 0; i < NETWORK_SERVER_ID_LENGTH - 1; i++) {
		char password_char = (i < password_length ? password[i] : 0);
		char server_id_char = (i < password_server_id_length ? password_server_id[i] : 0);
		char seed_char = password_game_seed >> (i % 32);
		salted_password_string += (char)(password_char ^ server_id_char ^ seed_char); // Cast needed, otherwise interpreted as integer to format
	}

	Md5 checksum;
	MD5Hash digest;

	/* Generate the MD5 hash */
	checksum.Append(salted_password_string.data(), salted_password_string.size());
	checksum.Finish(digest);

	return FormatArrayAsHex(digest, false);
}

/**
 * Hash the given password using server ID and game seed.
 * @param password Password to hash.
 * @param password_server_id Server ID.
 * @param password_game_seed Game seed.
 * @return The hashed password.
 */
std::vector<uint8_t> GenerateGeneralPasswordHash(const std::string &password, const std::string &password_server_id, uint64_t password_game_seed)
{
	if (password.empty()) return {};

	std::vector<uint8_t> data;
	data.reserve(password_server_id.size() + password.size() + 10);
	BufferSerialisationRef buffer(data);

	buffer.Send_uint64(password_game_seed);
	buffer.Send_string(password_server_id);
	buffer.Send_string(password);

	std::vector<uint8_t> output;
	output.resize(64);
	crypto_blake2b(output.data(), output.size(), data.data(), data.size());

	return output;
}

/**
 * Check if the company we want to join requires a password.
 * @param company_id id of the company we want to check the 'passworded' flag for.
 * @return true if the company requires a password.
 */
bool NetworkCompanyIsPassworded(CompanyID company_id)
{
	return _networking && company_id < MAX_COMPANIES && _network_company_passworded.Test(company_id);
}

/* This puts a text-message to the console, or in the future, the chat-box,
 *  (to keep it all a bit more general)
 * If 'self_send' is true, this is the client who is sending the message */
void NetworkTextMessage(NetworkAction action, TextColour colour, bool self_send, const std::string &name, const std::string &str, NetworkTextMessageData data, const char *data_str)
{
	std::string_view name_view = name;
	std::string replacement_name;

	StringID strid;
	switch (action) {
		case NETWORK_ACTION_SERVER_MESSAGE:
			/* Ignore invalid messages */
			strid = STR_NETWORK_SERVER_MESSAGE;
			colour = CC_DEFAULT;
			break;
		case NETWORK_ACTION_COMPANY_SPECTATOR:
			colour = CC_DEFAULT;
			strid = STR_NETWORK_MESSAGE_CLIENT_COMPANY_SPECTATE;
			break;
		case NETWORK_ACTION_COMPANY_JOIN:
			colour = CC_DEFAULT;
			strid = STR_NETWORK_MESSAGE_CLIENT_COMPANY_JOIN;
			break;
		case NETWORK_ACTION_COMPANY_NEW:
			colour = CC_DEFAULT;
			strid = STR_NETWORK_MESSAGE_CLIENT_COMPANY_NEW;
			break;
		case NETWORK_ACTION_JOIN:
			/* Show the Client ID for the server but not for the client. */
			strid = _network_server ? STR_NETWORK_MESSAGE_CLIENT_JOINED_ID :  STR_NETWORK_MESSAGE_CLIENT_JOINED;
			break;
		case NETWORK_ACTION_LEAVE:          strid = STR_NETWORK_MESSAGE_CLIENT_LEFT; break;
		case NETWORK_ACTION_NAME_CHANGE:    strid = STR_NETWORK_MESSAGE_NAME_CHANGE; break;

		case NETWORK_ACTION_GIVE_MONEY: {
			replacement_name = GetString(STR_NETWORK_MESSAGE_MONEY_GIVE_SRC_DESCRIPTION, name, data.auxdata >> 16);
			name_view = replacement_name;

			extern uint8_t GetCurrentGrfLangID();
			uint8_t lang_id = GetCurrentGrfLangID();
			bool use_specific_string = lang_id <= 2 || lang_id == 0x15 || lang_id == 0x3A || lang_id == 0x3D; // English, German, Korean, Czech
			if (use_specific_string && self_send) {
				strid = STR_NETWORK_MESSAGE_GAVE_MONEY_AWAY;
			} else if (use_specific_string && (CompanyID) (data.auxdata & 0xFFFF) == _local_company) {
				strid = STR_NETWORK_MESSAGE_GIVE_MONEY_RECEIVE;
			} else {
				strid = STR_NETWORK_MESSAGE_GIVE_MONEY;
			}
			break;
		}

		case NETWORK_ACTION_CHAT_COMPANY:   strid = self_send ? STR_NETWORK_CHAT_TO_COMPANY : STR_NETWORK_CHAT_COMPANY; break;
		case NETWORK_ACTION_CHAT_CLIENT:    strid = self_send ? STR_NETWORK_CHAT_TO_CLIENT  : STR_NETWORK_CHAT_CLIENT;  break;
		case NETWORK_ACTION_KICKED:         strid = STR_NETWORK_MESSAGE_KICKED; break;
		case NETWORK_ACTION_EXTERNAL_CHAT:  strid = STR_NETWORK_CHAT_EXTERNAL; break;
		default:                            strid = STR_NETWORK_CHAT_ALL; break;
	}

	format_buffer message;

	/* All of these strings start with "***". These characters are interpreted as both left-to-right and
	 * right-to-left characters depending on the context. As the next text might be an user's name, the
	 * user name's characters will influence the direction of the "***" instead of the language setting
	 * of the game. Manually set the direction of the "***" by inserting a text-direction marker. */
	StringBuilder(message).Utf8Encode(_current_text_dir == TD_LTR ? CHAR_TD_LRM : CHAR_TD_RLM);
	AppendStringInPlace(message, strid, name_view, str, data.data, data_str);

	Debug(desync, 1, "msg: {}; {}", debug_date_dumper().HexDate(), message);
	IConsolePrint(colour, message.to_string());
	NetworkAddChatMessage(colour, _settings_client.gui.network_chat_timeout, message);
}

/* Calculate the frame-lag of a client */
uint NetworkCalculateLag(const NetworkClientSocket *cs)
{
	int lag = cs->last_frame_server - cs->last_frame;
	/* This client has missed their ACK packet after 1 DAY_TICKS..
	 *  so we increase their lag for every frame that passes!
	 * The packet can be out by a max of _net_frame_freq */
	if (cs->last_frame_server + DAY_TICKS + _settings_client.network.frame_freq < _frame_counter) {
		lag += _frame_counter - (cs->last_frame_server + DAY_TICKS + _settings_client.network.frame_freq);
	}
	return lag;
}


/* There was a non-recoverable error, drop back to the main menu with a nice
 *  error */
void ShowNetworkError(StringID error_string)
{
	_switch_mode = SM_MENU;
	ShowErrorMessage(error_string, INVALID_STRING_ID, WL_CRITICAL);
}

/**
 * Retrieve the string id of an internal error number
 * @param err NetworkErrorCode
 * @return the StringID
 */
StringID GetNetworkErrorMsg(NetworkErrorCode err)
{
	/* List of possible network errors, used by
	 * PACKET_SERVER_ERROR and PACKET_CLIENT_ERROR */
	static const StringID network_error_strings[] = {
		STR_NETWORK_ERROR_CLIENT_GENERAL,
		STR_NETWORK_ERROR_CLIENT_DESYNC,
		STR_NETWORK_ERROR_CLIENT_SAVEGAME,
		STR_NETWORK_ERROR_CLIENT_CONNECTION_LOST,
		STR_NETWORK_ERROR_CLIENT_PROTOCOL_ERROR,
		STR_NETWORK_ERROR_CLIENT_NEWGRF_MISMATCH,
		STR_NETWORK_ERROR_CLIENT_NOT_AUTHORIZED,
		STR_NETWORK_ERROR_CLIENT_NOT_EXPECTED,
		STR_NETWORK_ERROR_CLIENT_WRONG_REVISION,
		STR_NETWORK_ERROR_CLIENT_NAME_IN_USE,
		STR_NETWORK_ERROR_CLIENT_WRONG_PASSWORD,
		STR_NETWORK_ERROR_CLIENT_COMPANY_MISMATCH,
		STR_NETWORK_ERROR_CLIENT_KICKED,
		STR_NETWORK_ERROR_CLIENT_CHEATER,
		STR_NETWORK_ERROR_CLIENT_SERVER_FULL,
		STR_NETWORK_ERROR_CLIENT_TOO_MANY_COMMANDS,
		STR_NETWORK_ERROR_CLIENT_TIMEOUT_PASSWORD,
		STR_NETWORK_ERROR_CLIENT_TIMEOUT_COMPUTER,
		STR_NETWORK_ERROR_CLIENT_TIMEOUT_MAP,
		STR_NETWORK_ERROR_CLIENT_TIMEOUT_JOIN,
		STR_NETWORK_ERROR_CLIENT_INVALID_CLIENT_NAME,
		STR_NETWORK_ERROR_CLIENT_NOT_ON_ALLOW_LIST,
		STR_NETWORK_ERROR_CLIENT_NO_AUTHENTICATION_METHOD_AVAILABLE,
	};
	static_assert(lengthof(network_error_strings) == NETWORK_ERROR_END);

	if (err >= (ptrdiff_t)lengthof(network_error_strings)) err = NETWORK_ERROR_GENERAL;

	return network_error_strings[err];
}

/**
 * Handle the pause mode change so we send the right messages to the chat.
 * @param prev_mode The previous pause mode.
 * @param changed_mode The pause mode that got changed.
 */
void NetworkHandlePauseChange(PauseMode prev_mode, PauseMode changed_mode)
{
	if (!_networking) return;

	switch (changed_mode) {
		case PM_PAUSED_NORMAL:
		case PM_PAUSED_JOIN:
		case PM_PAUSED_GAME_SCRIPT:
		case PM_PAUSED_ACTIVE_CLIENTS:
		case PM_PAUSED_LINK_GRAPH: {
			bool changed = ((_pause_mode == PM_UNPAUSED) != (prev_mode == PM_UNPAUSED));
			bool paused = (_pause_mode != PM_UNPAUSED);
			if (!paused && !changed) return;

			StringID str;
			if (!changed) {
				int i = -1;

				if ((_pause_mode & PM_PAUSED_NORMAL) != PM_UNPAUSED)         SetDParam(++i, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_MANUAL);
				if ((_pause_mode & PM_PAUSED_JOIN) != PM_UNPAUSED)           SetDParam(++i, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_CONNECTING_CLIENTS);
				if ((_pause_mode & PM_PAUSED_GAME_SCRIPT) != PM_UNPAUSED)    SetDParam(++i, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_GAME_SCRIPT);
				if ((_pause_mode & PM_PAUSED_ACTIVE_CLIENTS) != PM_UNPAUSED) SetDParam(++i, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_NOT_ENOUGH_PLAYERS);
				if ((_pause_mode & PM_PAUSED_LINK_GRAPH) != PM_UNPAUSED)     SetDParam(++i, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_LINK_GRAPH);
				str = STR_NETWORK_SERVER_MESSAGE_GAME_STILL_PAUSED_1 + i;
			} else {
				switch (changed_mode) {
					case PM_PAUSED_NORMAL:         SetDParam(0, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_MANUAL); break;
					case PM_PAUSED_JOIN:           SetDParam(0, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_CONNECTING_CLIENTS); break;
					case PM_PAUSED_GAME_SCRIPT:    SetDParam(0, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_GAME_SCRIPT); break;
					case PM_PAUSED_ACTIVE_CLIENTS: SetDParam(0, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_NOT_ENOUGH_PLAYERS); break;
					case PM_PAUSED_LINK_GRAPH:     SetDParam(0, STR_NETWORK_SERVER_MESSAGE_GAME_REASON_LINK_GRAPH); break;
					default: NOT_REACHED();
				}
				str = paused ? STR_NETWORK_SERVER_MESSAGE_GAME_PAUSED : STR_NETWORK_SERVER_MESSAGE_GAME_UNPAUSED;
			}

			NetworkTextMessage(NETWORK_ACTION_SERVER_MESSAGE, CC_DEFAULT, false, "", GetString(str));
			break;
		}

		default:
			return;
	}
}


/**
 * Helper function for the pause checkers. If pause is true and the
 * current pause mode isn't set the game will be paused, if it it false
 * and the pause mode is set the game will be unpaused. In the other
 * cases nothing happens to the pause state.
 * @param pause whether we'd like to pause
 * @param pm the mode which we would like to pause with
 */
static void CheckPauseHelper(bool pause, PauseMode pm)
{
	if (pause == ((_pause_mode & pm) != PM_UNPAUSED)) return;

	Command<CMD_PAUSE>::Post(pm, pause);
}

/**
 * Counts the number of active clients connected.
 * It has to be in STATUS_ACTIVE and not a spectator
 * @return number of active clients
 */
static uint NetworkCountActiveClients()
{
	uint count = 0;

	for (const NetworkClientSocket *cs : NetworkClientSocket::Iterate()) {
		if (cs->status != NetworkClientSocket::STATUS_ACTIVE) continue;
		if (!Company::IsValidID(cs->GetInfo()->client_playas)) continue;
		count++;
	}

	return count;
}

/**
 * Check if the minimum number of active clients has been reached and pause or unpause the game as appropriate
 */
static void CheckMinActiveClients()
{
	if ((_pause_mode & PM_PAUSED_ERROR) != PM_UNPAUSED ||
			!_network_dedicated ||
			(_settings_client.network.min_active_clients == 0 && (_pause_mode & PM_PAUSED_ACTIVE_CLIENTS) == PM_UNPAUSED)) {
		return;
	}
	CheckPauseHelper(NetworkCountActiveClients() < _settings_client.network.min_active_clients, PM_PAUSED_ACTIVE_CLIENTS);
}

/**
 * Checks whether there is a joining client
 * @return true iff one client is joining (but not authorizing)
 */
static bool NetworkHasJoiningClient()
{
	for (const NetworkClientSocket *cs : NetworkClientSocket::Iterate()) {
		if (cs->status >= NetworkClientSocket::STATUS_AUTHORIZED && cs->status < NetworkClientSocket::STATUS_ACTIVE) return true;
	}

	return false;
}

/**
 * Check whether we should pause on join
 */
static void CheckPauseOnJoin()
{
	if ((_pause_mode & PM_PAUSED_ERROR) != PM_UNPAUSED ||
			(!_settings_client.network.pause_on_join && (_pause_mode & PM_PAUSED_JOIN) == PM_UNPAUSED)) {
		return;
	}
	CheckPauseHelper(NetworkHasJoiningClient(), PM_PAUSED_JOIN);
}

/**
 * Parse the company part ("#company" postfix) of a connecting string.
 * @param connection_string The string with the connection data.
 * @param company_id        The company ID to set, if available.
 * @return A std::string_view into the connection string without the company part.
 */
std::string_view ParseCompanyFromConnectionString(const std::string &connection_string, CompanyID *company_id)
{
	std::string_view ip = connection_string;
	if (company_id == nullptr) return ip;

	size_t offset = ip.find_last_of('#');
	if (offset != std::string::npos) {
		std::string_view company_string = ip.substr(offset + 1);
		ip = ip.substr(0, offset);

		uint8_t company_value;
		bool success = IntFromChars(company_string.data(), company_string.data() + company_string.size(), company_value);
		if (success) {
			if (company_value != COMPANY_NEW_COMPANY && company_value != COMPANY_SPECTATOR) {
				if (company_value > MAX_COMPANIES || company_value == 0) {
					*company_id = COMPANY_SPECTATOR;
				} else {
					/* "#1" means the first company, which has index 0. */
					*company_id = (CompanyID)(company_value - 1);
				}
			} else {
				*company_id = (CompanyID)company_value;
			}
		}
	}

	return ip;
}

/**
 * Converts a string to ip/port/company
 *  Format: IP:port#company
 *
 * Returns the IP part as a string view into the passed string. This view is
 * valid as long the passed connection string is valid. If there is no port
 * present in the connection string, the port reference will not be touched.
 * When there is no company ID present in the connection string or company_id
 * is nullptr, then company ID will not be touched.
 *
 * @param connection_string The string with the connection data.
 * @param port              The port reference to set.
 * @param company_id        The company ID to set, if available.
 * @return A std::string_view into the connection string with the (IP) address part.
 */
std::string_view ParseFullConnectionString(const std::string &connection_string, uint16_t &port, CompanyID *company_id)
{
	std::string_view ip = ParseCompanyFromConnectionString(connection_string, company_id);

	size_t port_offset = ip.find_last_of(':');
	size_t ipv6_close = ip.find_last_of(']');
	if (port_offset != std::string::npos && (ipv6_close == std::string::npos || ipv6_close < port_offset)) {
		std::string_view port_string = ip.substr(port_offset + 1);
		ip = ip.substr(0, port_offset);
		IntFromChars(port_string.data(), port_string.data() + port_string.size(), port);
	}
	return ip;
}

/**
 * Normalize a connection string. That is, ensure there is a port in the string.
 * @param connection_string The connection string to normalize.
 * @param default_port The port to use if none is given.
 * @return The normalized connection string.
 */
std::string NormalizeConnectionString(const std::string &connection_string, uint16_t default_port)
{
	uint16_t port = default_port;
	std::string_view ip = ParseFullConnectionString(connection_string, port);
	return std::string(ip) + ":" + std::to_string(port);
}

/**
 * Convert a string containing either "hostname" or "hostname:ip" to a
 * NetworkAddress.
 *
 * @param connection_string The string to parse.
 * @param default_port The default port to set port to if not in connection_string.
 * @return A valid NetworkAddress of the parsed information.
 */
NetworkAddress ParseConnectionString(const std::string &connection_string, uint16_t default_port)
{
	uint16_t port = default_port;
	std::string_view ip = ParseFullConnectionString(connection_string, port);
	return NetworkAddress(ip, port);
}

/**
 * Handle the accepting of a connection to the server.
 * @param s The socket of the new connection.
 * @param address The address of the peer.
 */
/* static */ void ServerNetworkGameSocketHandler::AcceptConnection(SOCKET s, const NetworkAddress &address)
{
	/* Register the login */
	_network_clients_connected++;

	ServerNetworkGameSocketHandler *cs = new ServerNetworkGameSocketHandler(s);
	cs->client_address = address; // Save the IP of the client

	InvalidateWindowData(WC_CLIENT_LIST, 0);
}

/**
 * Resets the pools used for network clients, and the admin pool if needed.
 * @param close_admins Whether the admin pool has to be cleared as well.
 */
static void InitializeNetworkPools(bool close_admins = true)
{
	PoolTypes to_clean{PoolType::NetworkClient};
	if (close_admins) to_clean.Set(PoolType::NetworkAdmin);
	PoolBase::Clean(to_clean);
}

/**
 * Close current connections.
 * @param close_admins Whether the admin connections have to be closed as well.
 */
void NetworkClose(bool close_admins)
{
	if (_network_server) {
		if (close_admins) {
			for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::Iterate()) {
				as->CloseConnection(true);
			}
		}

		for (NetworkClientSocket *cs : NetworkClientSocket::Iterate()) {
			cs->CloseConnection(NETWORK_RECV_STATUS_CLIENT_QUIT);
		}
		ServerNetworkGameSocketHandler::CloseListeners();
		ServerNetworkAdminSocketHandler::CloseListeners();

		_network_coordinator_client.CloseConnection();
	} else {
		if (MyClient::my_client != nullptr) {
			MyClient::SendQuit();
			MyClient::my_client->CloseConnection(NETWORK_RECV_STATUS_CLIENT_QUIT);
		}

		_network_coordinator_client.CloseAllConnections();
	}
	NetworkGameSocketHandler::ProcessDeferredDeletions();

	TCPConnecter::KillAll();

	_networking = false;
	_network_server = false;

	NetworkFreeLocalCommandQueue();

	delete[] _network_company_states;
	_network_company_states = nullptr;
	_network_company_server_id.clear();
	_network_company_passworded = {};

	InitializeNetworkPools(close_admins);

	_network_sync_records.clear();
	_network_sync_records.shrink_to_fit();
	_network_sync_record_counts.clear();
	_network_sync_record_counts.shrink_to_fit();
}

/* Initializes the network (cleans sockets and stuff) */
static void NetworkInitialize(bool close_admins = true)
{
	InitializeNetworkPools(close_admins);

	_sync_frame = 0;
	_network_first_time = true;

	_network_reconnect = 0;

	_last_sync_date = EconTime::Date{0};
	_last_sync_date_fract = 0;
	_last_sync_tick_skip_counter = 0;
	_last_sync_frame_counter = 0;
}

/** Non blocking connection to query servers for their game info. */
class TCPQueryConnecter : public TCPServerConnecter {
private:
	std::string connection_string;

public:
	TCPQueryConnecter(const std::string &connection_string) : TCPServerConnecter(connection_string, NETWORK_DEFAULT_PORT), connection_string(connection_string) {}

	void OnFailure() override
	{
		NetworkGameList *item = NetworkGameListAddItem(connection_string);
		item->status = NGLS_OFFLINE;
		item->refreshing = false;

		UpdateNetworkGameWindow();
	}

	void OnConnect(SOCKET s) override
	{
		QueryNetworkGameSocketHandler::QueryServer(s, this->connection_string);
	}
};

/**
 * Query a server to fetch the game-info.
 * @param connection_string the address to query.
 */
void NetworkQueryServer(const std::string &connection_string)
{
	if (!_network_available) return;

	/* Mark the entry as refreshing, so the GUI can show the refresh is pending. */
	NetworkGameList *item = NetworkGameListAddItem(connection_string);
	item->refreshing = true;

	TCPConnecter::Create<TCPQueryConnecter>(connection_string);
}

/**
 * Validates an address entered as a string and adds the server to
 * the list. If you use this function, the games will be marked
 * as manually added.
 * @param connection_string The IP:port of the server to add.
 * @param manually Whether the enter should be marked as manual added.
 * @param never_expire Whether the entry can expire (removed when no longer found in the public listing).
 * @return The entry on the game list.
 */
NetworkGameList *NetworkAddServer(const std::string &connection_string, bool manually, bool never_expire)
{
	if (connection_string.empty()) return nullptr;

	/* Ensure the item already exists in the list */
	NetworkGameList *item = NetworkGameListAddItem(connection_string);
	if (item->info.server_name.empty()) {
		ClearGRFConfigList(item->info.grfconfig);
		item->info.server_name = connection_string;

		UpdateNetworkGameWindow();

		NetworkQueryServer(connection_string);
	}

	if (manually) item->manually = true;
	if (never_expire) item->version = INT32_MAX;

	return item;
}

/**
 * Get the addresses to bind to.
 * @param addresses the list to write to.
 * @param port the port to bind to.
 */
void GetBindAddresses(NetworkAddressList *addresses, uint16_t port)
{
	for (const auto &iter : _network_bind_list) {
		addresses->emplace_back(iter.c_str(), port);
	}

	/* No address, so bind to everything. */
	if (addresses->empty()) {
		addresses->emplace_back("", port);
	}
}

/* Generates the list of manually added hosts from NetworkGameList and
 * dumps them into the array _network_host_list. This array is needed
 * by the function that generates the config file. */
void NetworkRebuildHostList()
{
	_network_host_list.clear();

	for (NetworkGameList *item = _network_game_list; item != nullptr; item = item->next) {
		if (item->manually) _network_host_list.emplace_back(item->connection_string);
	}
}

/** Non blocking connection create to actually connect to servers */
class TCPClientConnecter : public TCPServerConnecter {
private:
	std::string connection_string;

public:
	TCPClientConnecter(const std::string &connection_string) : TCPServerConnecter(connection_string, NETWORK_DEFAULT_PORT), connection_string(connection_string) {}

	void OnFailure() override
	{
		ShowNetworkError(STR_NETWORK_ERROR_NOCONNECTION);
	}

	void OnConnect(SOCKET s) override
	{
		_networking = true;
		_network_own_client_id = ClientID{};
		new ClientNetworkGameSocketHandler(s, this->connection_string);
		IConsoleCmdExec("exec scripts/on_client.scr 0");
		NetworkClient_Connected();
	}
};

/**
 * Join a client to the server at with the given connection string.
 * The default for the passwords is \c nullptr. When the server or company needs a
 * password and none is given, the user is asked to enter the password in the GUI.
 * This function will return false whenever some information required to join is not
 * correct such as the company number or the client's name, or when there is not
 * networking avalabile at all. If the function returns false the connection with
 * the existing server is not disconnected.
 * It will return true when it starts the actual join process, i.e. when it
 * actually shows the join status window.
 *
 * @param connection_string     The IP address, port and company number to join as.
 * @param default_company       The company number to join as when none is given.
 * @param join_server_password  The password for the server.
 * @param join_company_password The password for the company.
 * @return Whether the join has started.
 */
bool NetworkClientConnectGame(const std::string &connection_string, CompanyID default_company, const std::string &join_server_password, const std::string &join_company_password)
{
	CompanyID join_as = default_company;
	std::string resolved_connection_string = ServerAddress::Parse(connection_string, NETWORK_DEFAULT_PORT, &join_as).connection_string;

	if (!_network_available) return false;
	if (!NetworkValidateOurClientName()) return false;

	_network_join.connection_string = resolved_connection_string;
	_network_join.company = join_as;
	_network_join.server_password = join_server_password;
	_network_join.company_password = join_company_password;

	if (_game_mode == GM_MENU) {
		/* From the menu we can immediately continue with the actual join. */
		NetworkClientJoinGame();
	} else {
		/* When already playing a game, first go back to the main menu. This
		 * disconnects the user from the current game, meaning we can safely
		 * load in the new. After all, there is little point in continuing to
		 * play on a server if we are connecting to another one.
		 */
		_switch_mode = SM_JOIN_GAME;
	}
	return true;
}

/**
 * Actually perform the joining to the server. Use #NetworkClientConnectGame
 * when you want to connect to a specific server/company. This function
 * assumes _network_join is already fully set up.
 */
void NetworkClientJoinGame()
{
	NetworkDisconnect();
	NetworkInitialize();

	_settings_client.network.last_joined = _network_join.connection_string;
	_network_join_status = NETWORK_JOIN_STATUS_CONNECTING;
	ShowJoinStatusWindow();

	TCPConnecter::Create<TCPClientConnecter>(_network_join.connection_string);
}

static void NetworkInitGameInfo()
{
	FillStaticNetworkServerGameInfo();
	/* The server is a client too */
	_network_game_info.clients_on = _network_dedicated ? 0 : 1;

	/* There should be always space for the server. */
	assert(NetworkClientInfo::CanAllocateItem());
	NetworkClientInfo *ci = new NetworkClientInfo(CLIENT_ID_SERVER);
	ci->client_playas = _network_dedicated ? COMPANY_SPECTATOR : GetDefaultLocalCompany();

	ci->client_name = _settings_client.network.client_name;
}

/**
 * Trim the given server name in place, i.e. remove leading and trailing spaces.
 * After the trim check whether the server name is not empty.
 * When the server name is empty a GUI error message is shown telling the
 * user to set the servername and this function returns false.
 *
 * @param server_name The server name to validate. It will be trimmed of leading
 *                    and trailing spaces.
 * @return True iff the server name is valid.
 */
bool NetworkValidateServerName(std::string &server_name)
{
	StrTrimInPlace(server_name);
	if (!server_name.empty()) return true;

	ShowErrorMessage(STR_NETWORK_ERROR_BAD_SERVER_NAME, INVALID_STRING_ID, WL_ERROR);
	return false;
}

/**
 * Check whether the client and server name are set, for a dedicated server and if not set them to some default
 * value and tell the user to change this as soon as possible.
 * If the saved name is the default value, then the user is told to override  this value too.
 * This is only meant dedicated servers, as for the other servers the GUI ensures a name has been entered.
 */
static void CheckClientAndServerName()
{
	static const std::string fallback_client_name = "Unnamed Client";
	StrTrimInPlace(_settings_client.network.client_name);
	if (_settings_client.network.client_name.empty() || _settings_client.network.client_name.compare(fallback_client_name) == 0) {
		Debug(net, 1, "No \"client_name\" has been set, using \"{}\" instead. Please set this now using the \"name <new name>\" command", fallback_client_name);
		_settings_client.network.client_name = fallback_client_name;
	}

	static const std::string fallback_server_name = "Unnamed Server";
	StrTrimInPlace(_settings_client.network.server_name);
	if (_settings_client.network.server_name.empty() || _settings_client.network.server_name.compare(fallback_server_name) == 0) {
		Debug(net, 1, "No \"server_name\" has been set, using \"{}\" instead. Please set this now using the \"server_name <new name>\" command", fallback_server_name);
		_settings_client.network.server_name = fallback_server_name;
	}
}

bool NetworkServerStart()
{
	if (!_network_available) return false;

	/* Call the pre-scripts */
	IConsoleCmdExec("exec scripts/pre_server.scr 0");
	if (_network_dedicated) IConsoleCmdExec("exec scripts/pre_dedicated.scr 0");

	/* Check for the client and server names to be set, but only after the scripts had a chance to set them.*/
	if (_network_dedicated) CheckClientAndServerName();

	NetworkDisconnect(false);
	NetworkInitialize(false);
	NetworkUDPInitialize();
	Debug(net, 5, "Starting listeners for clients");
	if (!ServerNetworkGameSocketHandler::Listen(_settings_client.network.server_port)) return false;

	/* Only listen for admins when the authentication is configured. */
	if (_settings_client.network.AdminAuthenticationConfigured()) {
		Debug(net, 5, "Starting listeners for admins");
		if (!ServerNetworkAdminSocketHandler::Listen(_settings_client.network.server_admin_port)) return false;
	}

	/* Try to start UDP-server */
	Debug(net, 5, "Starting listeners for incoming server queries");
	NetworkUDPServerListen();

	_network_company_states = new NetworkCompanyState[MAX_COMPANIES];
	_network_company_server_id = NetworkGenerateRandomKeyString(16);
	_network_server = true;
	_networking = true;
	_frame_counter = 0;
	_frame_counter_server = 0;
	_frame_counter_max = 0;
	_last_sync_frame = 0;
	_network_own_client_id = CLIENT_ID_SERVER;

	_network_sync_records.clear();
	_network_sync_record_counts.clear();
	_record_sync_records = false;

	_network_clients_connected = 0;
	_network_company_passworded = {};

	NetworkInitGameInfo();

	if (_settings_client.network.server_game_type != SERVER_GAME_TYPE_LOCAL) {
		_network_coordinator_client.Register();
	}

	/* execute server initialization script */
	IConsoleCmdExec("exec scripts/on_server.scr 0");
	/* if the server is dedicated ... add some other script */
	if (_network_dedicated) IConsoleCmdExec("exec scripts/on_dedicated.scr 0");

	return true;
}

/**
 * Perform tasks when the server is started. This consists of things
 * like putting the server's client in a valid company and resetting the restart time.
 */
void NetworkOnGameStart()
{
	if (!_network_server) return;

	/* Update the static game info to set the values from the new game. */
	NetworkServerUpdateGameInfo();

	ChangeNetworkRestartTime(true);

	if (!_network_dedicated) {
		Company *c = Company::GetIfValid(_local_company);
		NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(CLIENT_ID_SERVER);
		if (c != nullptr && ci != nullptr) {
			/*
			 * If the company has not been named yet, the company was just started.
			 * Otherwise it would have gotten a name already, so announce it as a new company.
			 */
			if (c->name_1 == STR_SV_UNNAMED && c->name.empty()) NetworkServerNewCompany(c, ci);
		}

		ShowClientList();
	} else {
		/* welcome possibly still connected admins - this can only happen on a dedicated server. */
		ServerNetworkAdminSocketHandler::WelcomeAll();
	}
}

/* The server is rebooting...
 * The only difference with NetworkDisconnect, is the packets that is sent */
void NetworkReboot()
{
	if (_network_server) {
		for (NetworkClientSocket *cs : NetworkClientSocket::Iterate()) {
			cs->SendNewGame();
			cs->SendPackets();
		}

		for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
			as->SendNewGame();
			as->SendPackets();
		}
	}

	/* For non-dedicated servers we have to kick the admins as we are not
	 * certain that we will end up in a new network game. */
	NetworkClose(!_network_dedicated);
}

/**
 * We want to disconnect from the host/clients.
 * @param close_admins Whether the admin sockets need to be closed as well.
 */
void NetworkDisconnect(bool close_admins)
{
	if (_network_server) {
		for (NetworkClientSocket *cs : NetworkClientSocket::Iterate()) {
			cs->SendShutdown();
			cs->SendPackets();
		}

		if (close_admins) {
			for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
				as->SendShutdown();
				as->SendPackets();
			}
		}
	}

	CloseWindowById(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);

	NetworkClose(close_admins);

	/* Reinitialize the UDP stack, i.e. close all existing connections. */
	NetworkUDPInitialize();
}

/**
 * The setting server_game_type was updated; possibly we need to take some
 * action.
 */
void NetworkUpdateServerGameType()
{
	if (!_networking) return;

	switch (_settings_client.network.server_game_type) {
		case SERVER_GAME_TYPE_LOCAL:
			_network_coordinator_client.CloseConnection();
			break;

		case SERVER_GAME_TYPE_INVITE_ONLY:
		case SERVER_GAME_TYPE_PUBLIC:
			_network_coordinator_client.Register();
			break;

		default:
			NOT_REACHED();
	}
}

/**
 * Receives something from the network.
 * @return true if everything went fine, false when the connection got closed.
 */
static bool NetworkReceive()
{
	bool result;
	if (_network_server) {
		ServerNetworkAdminSocketHandler::Receive();
		result = ServerNetworkGameSocketHandler::Receive();
	} else {
		result = ClientNetworkGameSocketHandler::Receive();
	}
	NetworkGameSocketHandler::ProcessDeferredDeletions();
	return result;
}

/* This sends all buffered commands (if possible) */
static void NetworkSend()
{
	if (_network_server) {
		ServerNetworkAdminSocketHandler::Send();
		ServerNetworkGameSocketHandler::Send();
	} else {
		ClientNetworkGameSocketHandler::Send();
	}
	NetworkGameSocketHandler::ProcessDeferredDeletions();
}

/**
 * We have to do some (simple) background stuff that runs normally,
 * even when we are not in multiplayer. For example stuff needed
 * for finding servers or downloading content.
 */
void NetworkBackgroundLoop()
{
	_network_content_client.SendReceive();
	_network_coordinator_client.SendReceive();
	TCPConnecter::CheckCallbacks();
	NetworkHTTPSocketHandler::HTTPReceive();
	QueryNetworkGameSocketHandler::SendReceive();
	NetworkGameSocketHandler::ProcessDeferredDeletions();

	NetworkBackgroundUDPLoop();
}

void RecordSyncEventData(NetworkSyncRecordEvents event)
{
	_network_sync_records.push_back({ event, _random.state[0], _state_checksum.state });
}

const char *GetSyncRecordEventName(NetworkSyncRecordEvents event)
{
	static const char *names[] = {
		"BEGIN",
		"CMD",
		"AUX_TILE",
		"TILE",
		"TOWN",
		"TREE",
		"STATION",
		"INDUSTRY",
		"PRE_DATES",
		"PRE_COMPANY_STATE",
		"VEH_PERIODIC",
		"VEH_LOAD_UNLOAD",
		"VEH_EFFECT",
		"VEH_TRAIN",
		"VEH_ROAD",
		"VEH_AIR",
		"VEH_SHIP",
		"VEH_OTHER",
		"VEH_SELL",
		"VEH_TBTR",
		"VEH_AUTOREPLACE",
		"VEH_REPAIR",
		"FRAME_DONE"
	};
	static_assert(lengthof(names) == NSRE_LAST);
	if (event < NSRE_LAST) return names[event];
	return "???";
}

/* The main loop called from ttd.c
 *  Here we also have to do StateGameLoop if needed! */
void NetworkGameLoop()
{
	if (!_networking) return;

	if (!NetworkReceive()) return;

	if (_network_server) {
		/* Log the sync state to check for in-syncedness of replays. */
		if (EconTime::CurDateFract() == 0 && TickSkipCounter() == 0) {
			/* We don't want to log multiple times if paused. */
			static EconTime::Date last_log;
			if (last_log != EconTime::CurDate()) {
				Debug(desync, 2, "sync: {}; {:08x}; {:08x}", debug_date_dumper().HexDate(), _random.state[0], _random.state[1]);
				last_log = EconTime::CurDate();
			}
		}

#ifdef DEBUG_DUMP_COMMANDS
		/* Loading of the debug commands from -ddesync>=1 */
		static auto f = FioFOpenFile("commands.log", "rb", SAVE_DIR);
		static EconTime::Date next_date = {};
		static uint next_date_fract;
		static uint next_tick_skip_counter;
		static std::unique_ptr<CommandPacket> cp;
		static bool check_sync_state = false;
		static uint32_t sync_state[2];
		if (!f.has_value() && next_date == 0) {
			Debug(desync, 0, "Cannot open commands.log");
			next_date = EconTime::Date{1};
		}

		while (f.has_value() && !feof(*f)) {
			if (EconTime::CurDate() == next_date && EconTime::CurDateFract() == next_date_fract && TickSkipCounter() == next_tick_skip_counter) {
				if (cp != nullptr) {
					extern void NetworkSendCommandImplementation(Commands cmd, TileIndex tile, const CommandPayloadBase &payload, StringID error_msg, CommandCallback callback, CallbackParameter callback_param, CompanyID company);
					NetworkSendCommandImplementation(cp->command_container.cmd, cp->command_container.tile, *cp->command_container.payload, (StringID)0, CommandCallback::None, 0, cp->company);
					Debug(net, 0, "injecting: {}; {:02x}; {:06x}; {:08x} ({})",
							debug_date_dumper().HexDate(), (int)_current_company, cp->command_container.tile, cp->command_container.cmd, GetCommandName(cp->command_container.cmd));
					cp.reset();
				}
				if (check_sync_state) {
					if (sync_state[0] == _random.state[0] && sync_state[1] == _random.state[1]) {
						Debug(net, 0, "sync check: {}; match", debug_date_dumper().HexDate());
					} else {
						Debug(net, 0, "sync check: {}; mismatch: expected {{{:08x}, {:08x}}}, got {{{:08x}, {:08x}}}",
									debug_date_dumper().HexDate(), sync_state[0], sync_state[1], _random.state[0], _random.state[1]);
						NOT_REACHED();
					}
					check_sync_state = false;
				}
			}

			/* Skip all entries in the command-log till we caught up with the current game again. */
			if (std::make_tuple(EconTime::CurDate(), EconTime::CurDateFract(), TickSkipCounter()) > std::make_tuple(next_date, next_date_fract, next_tick_skip_counter)) {
				Debug(net, 0, "Skipping to next command at {}", debug_date_dumper().HexDate(next_date, next_date_fract, next_tick_skip_counter));
				cp.reset();
				check_sync_state = false;
			}

			if (cp != nullptr || check_sync_state) break;

			static char buff[65536];
			if (fgets(buff, lengthof(buff), *f) == nullptr) break;

			char *p = buff;
			/* Ignore the "[date time] " part of the message */
			if (*p == '[') {
				p = strchr(p, ']');
				if (p == nullptr) break;
				p += 2;
			}

			if (strncmp(p, "cmd: ", 5) == 0
#ifdef DEBUG_FAILED_DUMP_COMMANDS
				|| strncmp(p, "cmdf: ", 6) == 0
#endif
				) {
				p += 5;
				if (*p == ' ') p++;
				uint cmd;
				int company;
				uint tile;
				int offset;
				int ret = sscanf(p, "date{%x; %x; %x}; company: %x; tile: %x (%*u x %*u); cmd: %x; %n\"",
						&next_date.edit_base(), &next_date_fract, &next_tick_skip_counter, &company, &tile, &cmd, &offset);
				assert(ret == 6);
				if (!IsValidCommand(static_cast<Commands>(cmd))) {
					Debug(desync, 0, "Trying to parse: {}, invalid command: {}", p, cmd);
					NOT_REACHED();
				}

				cp.reset(new CommandPacket());
				cp->company = (CompanyID)company;

				const char *payload_start = p + offset;
				while (*payload_start != 0 && *payload_start != '<') payload_start++;
				if (*payload_start != '<') {
					Debug(desync, 0, "Trying to parse: {}", p);
					NOT_REACHED();
				}
				payload_start++;

				const char *payload_end = payload_start;
				while (*payload_end != 0 && *payload_end != '>') payload_end++;
				if (*payload_end != '>' || ((payload_end - payload_start) & 1) != 0) {
					Debug(desync, 0, "Trying to parse: {}", p);
					NOT_REACHED();
				}

				std::vector<uint8_t> cmd_buffer;
				/* Prepend the fields expected by DynBaseCommandContainer::Deserialise */
				BufferSerialisationRef write_buffer(cmd_buffer);
				write_buffer.Send_uint16(static_cast<uint16_t>(cmd));
				write_buffer.Send_uint16(0);
				write_buffer.Send_uint32(tile);

				size_t payload_size_pos = write_buffer.GetSendOffset();
				write_buffer.Send_uint16(0);
				for (const char *data = payload_start; data < payload_end; data += 2) {
					uint8_t e = 0;
					std::from_chars(data, data + 2, e, 16);
					write_buffer.Send_uint8(e);
				}
				write_buffer.SendAtOffset_uint16(payload_size_pos, (uint16_t)(write_buffer.GetSendOffset() - payload_size_pos - 2));

				DeserialisationBuffer read_buffer(cmd_buffer.data(), cmd_buffer.size());
				const char *error = cp->command_container.Deserialise(read_buffer);
				if (error != nullptr) {
					Debug(desync, 0, "Trying to parse: {} --> {}", p, error);
					NOT_REACHED();
				}
			} else if (strncmp(p, "join: ", 6) == 0) {
				/* Manually insert a pause when joining; this way the client can join at the exact right time. */
				int ret = sscanf(p + 6, "date{%x; %x; %x}", &next_date.edit_base(), &next_date_fract, &next_tick_skip_counter);
				assert(ret == 3);
				Debug(net, 0, "injecting pause for join at {}; please join when paused", debug_date_dumper().HexDate(next_date, next_date_fract, next_tick_skip_counter));
				cp.reset(new CommandPacket());
				cp->command_container.tile = {};
				cp->company = COMPANY_SPECTATOR;
				cp->command_container.cmd = CMD_PAUSE;
				cp->command_container.payload = CmdPayload<CMD_PAUSE>::Make(PM_PAUSED_NORMAL, true).Clone();
				_ddc_fastforward = false;
			} else if (strncmp(p, "sync: ", 6) == 0) {
				int ret = sscanf(p + 6, "date{%x; %x; %x}; %x; %x", &next_date.edit_base(), &next_date_fract, &next_tick_skip_counter, &sync_state[0], &sync_state[1]);
				assert(ret == 5);
				check_sync_state = true;
			} else if (strncmp(p, "msg: ", 5) == 0 || strncmp(p, "client: ", 8) == 0 ||
						strncmp(p, "load: ", 6) == 0 || strncmp(p, "save: ", 6) == 0 ||
						strncmp(p, "new_company: ", 13) == 0 || strncmp(p, "new_company_ai: ", 16) == 0 ||
						strncmp(p, "buy_company: ", 13) == 0 || strncmp(p, "delete_company: ", 16) == 0 ||
						strncmp(p, "merge_companies: ", 17) == 0) {
				/* A message that is not very important to the log playback, but part of the log. */
#ifndef DEBUG_FAILED_DUMP_COMMANDS
			} else if (strncmp(p, "cmdf: ", 6) == 0) {
				Debug(desync, 0, "Skipping replay of failed command: {}", p + 6);
#endif
			} else {
				/* Can't parse a line; what's wrong here? */
				Debug(desync, 0, "Trying to parse: {}", p);
				NOT_REACHED();
			}
		}
		if (f.has_value() && feof(*f)) {
			Debug(desync, 0, "End of commands.log");
			f.reset();
		}
#endif /* DEBUG_DUMP_COMMANDS */
		if (_frame_counter >= _frame_counter_max) {
			/* Only check for active clients just before we're going to send out
			 * the commands so we don't send multiple pause/unpause commands when
			 * the frame_freq is more than 1 tick. Same with distributing commands. */
			CheckPauseOnJoin();
			CheckMinActiveClients();
			NetworkDistributeCommands();
		}

		bool send_frame = false;

		/* We first increase the _frame_counter */
		_frame_counter++;
		/* Update max-frame-counter */
		if (_frame_counter > _frame_counter_max) {
			_frame_counter_max = _frame_counter + _settings_client.network.frame_freq;
			send_frame = true;
		}

		const size_t total_sync_records = _network_sync_records.size();
		_network_sync_records.push_back({ _frame_counter, _random.state[0], _state_checksum.state });
		_record_sync_records = true;

		NetworkExecuteLocalCommandQueue();

		/* Then we make the frame */
		StateGameLoop();

		_sync_seed_1 = _random.state[0];
		_sync_state_checksum = _state_checksum.state;

		_network_sync_records.push_back({ NSRE_FRAME_DONE, _random.state[0], _state_checksum.state });
		_network_sync_record_counts.push_back((uint)(_network_sync_records.size() - total_sync_records));
		_record_sync_records = false;
		if (_network_sync_record_counts.size() >= 256) {
			/* Remove records from start of queue */
			_network_sync_records.erase(_network_sync_records.begin(), _network_sync_records.begin() + _network_sync_record_counts[0]);
			_network_sync_record_counts.pop_front();
		}

		NetworkServer_Tick(send_frame);
	} else {
		/* Client */

		/* Make sure we are at the frame were the server is (quick-frames) */
		if (_frame_counter_server > _frame_counter) {
			/* Run a number of frames; when things go bad, get out. */
			while (_frame_counter_server > _frame_counter) {
				if (!ClientNetworkGameSocketHandler::GameLoop()) return;
			}
		} else {
			/* Else, keep on going till _frame_counter_max */
			if (_frame_counter_max > _frame_counter) {
				/* Run one frame; if things went bad, get out. */
				if (!ClientNetworkGameSocketHandler::GameLoop()) return;
			}
		}
	}

	NetworkSend();
}

static void NetworkGenerateServerId()
{
	_settings_client.network.network_id = GenerateUid("OpenTTD Server ID");
}

std::string NetworkGenerateRandomKeyString(uint bytes)
{
	TempBufferST<uint8_t> key(bytes);
	RandomBytesWithFallback({key.get(), bytes});

	return FormatArrayAsHex({key.get(), bytes}, false);
}

/** This tries to launch the network for a given OS */
void NetworkStartUp()
{
	Debug(net, 3, "Starting network");

	/* Network is available */
	_network_available = NetworkCoreInitialize();
	_network_dedicated = false;

	/* Generate an server id when there is none yet */
	if (_settings_client.network.network_id.empty()) NetworkGenerateServerId();

	if (_settings_client.network.company_password_storage_token.empty() || _settings_client.network.company_password_storage_secret.empty()) {
		SetSettingValue(GetSettingFromName("network.company_password_storage_token")->AsStringSetting(), NetworkGenerateRandomKeyString(16));
		SetSettingValue(GetSettingFromName("network.company_password_storage_secret")->AsStringSetting(), NetworkGenerateRandomKeyString(32));
	}

	_network_game_info = {};

	NetworkInitialize();
	NetworkUDPInitialize();
	Debug(net, 3, "Network online, multiplayer available");
	NetworkFindBroadcastIPs(&_broadcast_list);
	NetworkHTTPInitialize();
}

/** This shuts the network down */
void NetworkShutDown()
{
	NetworkDisconnect();
	NetworkHTTPUninitialize();
	NetworkUDPClose();

	Debug(net, 3, "Shutting down network");

	_network_available = false;

	NetworkCoreShutdown();
}

void NetworkGameKeys::Initialise()
{
	assert(!this->inited);

	this->inited = true;

	static_assert(std::tuple_size<decltype(NetworkGameKeys::x25519_priv_key)>::value == 32);
	static_assert(std::tuple_size<decltype(NetworkGameKeys::x25519_pub_key)>::value == 32);
	RandomBytesWithFallback(this->x25519_priv_key);
	crypto_x25519_public_key(this->x25519_pub_key.data(), this->x25519_priv_key.data());
}

NetworkSharedSecrets::~NetworkSharedSecrets()
{
	static_assert(sizeof(*this) == 64);
	crypto_wipe(this, sizeof(*this));
}

#ifdef __EMSCRIPTEN__
extern "C" {

void CDECL em_openttd_add_server(const char *connection_string)
{
	NetworkAddServer(connection_string, false, true);
}

}
#endif
