/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file company_cmd.cpp Handling of companies. */

#include "stdafx.h"
#include "company_base.h"
#include "company_cmd.h"
#include "company_func.h"
#include "company_gui.h"
#include "town.h"
#include "news_func.h"
#include "command_func.h"
#include "command_serialisation.h"
#include "network/network.h"
#include "network/network_func.h"
#include "network/network_base.h"
#include "network/network_admin.h"
#include "ai/ai.hpp"
#include "ai/ai_instance.hpp"
#include "ai/ai_config.hpp"
#include "company_manager_face.h"
#include "window_func.h"
#include "strings_func.h"
#include "date_func.h"
#include "sound_func.h"
#include "rail.h"
#include "core/pool_func.hpp"
#include "settings_func.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "smallmap_gui.h"
#include "game/game.hpp"
#include "goal_base.h"
#include "story_base.h"
#include "zoning.h"
#include "tbtr_template_vehicle_func.h"
#include "core/backup_type.hpp"
#include "debug_desync.h"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "tilehighlight_func.h"
#include "plans_func.h"

#include "widgets/statusbar_widget.h"

#include "table/strings.h"

#include <vector>

#include "safeguards.h"

void ClearEnginesHiddenFlagOfCompany(CompanyID cid);
void UpdateObjectColours(const Company *c);

CompanyID _local_company;   ///< Company controlled by the human player at this client. Can also be #COMPANY_SPECTATOR.
CompanyID _current_company; ///< Company currently doing an action.
CompanyID _loaded_local_company; ///< Local company in loaded savegame
Colours _company_colours[MAX_COMPANIES];  ///< NOSAVE: can be determined from company structs.
CompanyManagerFace _company_manager_face; ///< for company manager face storage in openttd.cfg
uint _cur_company_tick_index;             ///< used to generate a name for one company that doesn't have a name yet per tick

CompanyMask _saved_PLYP_invalid_mask;
std::vector<uint8_t> _saved_PLYP_data;

CompanyPool _company_pool("Company"); ///< Pool of companies.
INSTANTIATE_POOL_METHODS(Company)

/**
 * Constructor.
 * @param name_1 Name of the company.
 * @param is_ai  A computer program is running for this company.
 */
Company::Company(StringID name_1, bool is_ai)
{
	this->name_1 = name_1;
	this->location_of_HQ = INVALID_TILE;
	this->is_ai = is_ai;
	this->terraform_limit = (uint32_t)_settings_game.construction.terraform_frame_burst << 16;
	this->clear_limit     = _settings_game.construction.clear_frame_burst << 16;
	this->tree_limit      = (uint32_t)(uint32_t)_settings_game.construction.tree_frame_burst << 16;
	this->purchase_land_limit = (uint32_t)_settings_game.construction.purchase_land_frame_burst << 16;
	this->build_object_limit = (uint32_t)_settings_game.construction.build_object_frame_burst << 16;

	std::fill(this->share_owners.begin(), this->share_owners.end(), INVALID_OWNER);
	InvalidateWindowData(WC_PERFORMANCE_DETAIL, 0, INVALID_COMPANY);
}

/** Destructor. */
Company::~Company()
{
	if (CleaningPool()) return;

	DeleteCompanyWindows(this->index);
	_saved_PLYP_invalid_mask.Set(this->index);
}

/**
 * Invalidating some stuff after removing item from the pool.
 * @param index index of deleted item
 */
void Company::PostDestructor(size_t index)
{
	InvalidateWindowData(WC_GRAPH_LEGEND, 0, (int)index);
	InvalidateWindowData(WC_PERFORMANCE_DETAIL, 0, (int)index);
	InvalidateWindowData(WC_COMPANY_LEAGUE, 0, 0);
	InvalidateWindowData(WC_LINKGRAPH_LEGEND, 0);
	/* If the currently shown error message has this company in it, then close it. */
	InvalidateWindowData(WC_ERRMSG, 0);
}

/**
 * Calculate the max allowed loan for this company.
 * @return the max loan amount.
 */
Money Company::GetMaxLoan() const
{
	if (this->max_loan == COMPANY_MAX_LOAN_DEFAULT) return _economy.max_loan;
	return this->max_loan;
}

/**
 * Sets the local company and updates the settings that are set on a
 * per-company basis to reflect the core's state in the GUI.
 * @param new_company the new company
 * @pre Company::IsValidID(new_company) || new_company == COMPANY_SPECTATOR || new_company == OWNER_NONE
 */
void SetLocalCompany(CompanyID new_company)
{
	/* company could also be COMPANY_SPECTATOR or OWNER_NONE */
	assert(Company::IsValidID(new_company) || new_company == COMPANY_SPECTATOR || new_company == OWNER_NONE);

	/* If actually changing to another company, several windows need closing */
	bool switching_company = _local_company != new_company;

	/* Delete the chat window, if you were team chatting. */
	if (switching_company) InvalidateWindowData(WC_SEND_NETWORK_MSG, DESTTYPE_TEAM, _local_company);

	assert(IsLocalCompany());

	_current_company = _local_company = new_company;

	if (switching_company) {
		InvalidateWindowClassesData(WC_COMPANY);
		/* Close any construction windows... */
		CloseConstructionWindows();
		ResetObjectToPlace();
	}

	if (switching_company && Company::IsValidID(new_company)) {
		for (Town *town : Town::Iterate()) {
			town->UpdateLabel();
		}
	}

	/* ... and redraw the whole screen. */
	MarkWholeScreenDirty();
	InvalidateWindowClassesData(WC_SIGN_LIST, -1);
	InvalidateWindowClassesData(WC_GOALS_LIST);
	ClearZoningCaches();
	InvalidatePlanCaches();

	extern void TraceRestrictClearRecentSlotsAndCounters();
	TraceRestrictClearRecentSlotsAndCounters();
}

/**
 * Get the colour for DrawString-subroutines which matches the colour of the company.
 * @param company Company to get the colour of.
 * @return Colour of \a company.
 */
TextColour GetDrawStringCompanyColour(CompanyID company)
{
	if (!Company::IsValidID(company)) return (TextColour)GetColourGradient(COLOUR_WHITE, SHADE_NORMAL) | TC_IS_PALETTE_COLOUR;
	return (TextColour)GetColourGradient(_company_colours[company], SHADE_NORMAL) | TC_IS_PALETTE_COLOUR;
}

/**
 * Draw the icon of a company.
 * @param c Company that needs its icon drawn.
 * @param x Horizontal coordinate of the icon.
 * @param y Vertical coordinate of the icon.
 */
void DrawCompanyIcon(CompanyID c, int x, int y)
{
	DrawSprite(SPR_COMPANY_ICON, COMPANY_SPRITE_COLOUR(c), x, y);
}

/**
 * Checks whether a company manager's face is a valid encoding.
 * Unused bits are not enforced to be 0.
 * @param cmf the fact to check
 * @return true if and only if the face is valid
 */
static bool IsValidCompanyManagerFace(CompanyManagerFace cmf)
{
	if (!AreCompanyManagerFaceBitsValid(cmf, CMFV_GEN_ETHN, GE_WM)) return false;

	GenderEthnicity ge   = (GenderEthnicity)GetCompanyManagerFaceBits(cmf, CMFV_GEN_ETHN, GE_WM);
	bool has_moustache   = !HasBit(ge, GENDER_FEMALE) && GetCompanyManagerFaceBits(cmf, CMFV_HAS_MOUSTACHE,   ge) != 0;
	bool has_tie_earring = !HasBit(ge, GENDER_FEMALE) || GetCompanyManagerFaceBits(cmf, CMFV_HAS_TIE_EARRING, ge) != 0;
	bool has_glasses     = GetCompanyManagerFaceBits(cmf, CMFV_HAS_GLASSES, ge) != 0;

	if (!AreCompanyManagerFaceBitsValid(cmf, CMFV_EYE_COLOUR, ge)) return false;
	for (CompanyManagerFaceVariable cmfv = CMFV_CHEEKS; cmfv < CMFV_END; cmfv++) {
		switch (cmfv) {
			case CMFV_MOUSTACHE:   if (!has_moustache)   continue; break;
			case CMFV_LIPS:
			case CMFV_NOSE:        if (has_moustache)    continue; break;
			case CMFV_TIE_EARRING: if (!has_tie_earring) continue; break;
			case CMFV_GLASSES:     if (!has_glasses)     continue; break;
			default: break;
		}
		if (!AreCompanyManagerFaceBitsValid(cmf, cmfv, ge)) return false;
	}

	return true;
}

/**
 * Refresh all windows owned by a company.
 * @param company Company that changed, and needs its windows refreshed.
 */
void InvalidateCompanyWindows(const Company *company)
{
	CompanyID cid = company->index;

	if (cid == _local_company) SetWindowWidgetDirty(WC_STATUS_BAR, 0, WID_S_RIGHT);
	SetWindowDirty(WC_FINANCES, cid);
}

/**
 * Get the amount of money that a company has available, or INT64_MAX
 * if there is no such valid company.
 *
 * @param company Company to check
 * @return The available money of the company or INT64_MAX
 */
Money GetAvailableMoney(CompanyID company)
{
	if (_settings_game.difficulty.infinite_money) return INT64_MAX;
	if (!Company::IsValidID(company)) return INT64_MAX;
	return Company::Get(company)->money;
}

/**
 * This functions returns the money which can be used to execute a command.
 * This is either the money of the current company, or INT64_MAX if infinite money
 * is enabled or there is no such a company "at the moment" like the server itself.
 *
 * @return The available money of the current company or INT64_MAX
 */
Money GetAvailableMoneyForCommand()
{
	return GetAvailableMoney(_current_company);
}

/**
 * Verify whether the company can pay the bill.
 * @param[in,out] cost Money to pay, is changed to an error if the company does not have enough money.
 * @return Function returns \c true if the company has enough money or infinite money is enabled,
 * else it returns \c false.
 */
bool CheckCompanyHasMoney(CommandCost &cost)
{
	if (cost.GetCost() <= 0) return true;
	if (_settings_game.difficulty.infinite_money) return true;

	const Company *c = Company::GetIfValid(_current_company);
	if (c != nullptr && cost.GetCost() > c->money) {
		SetDParam(0, cost.GetCost());
		cost.MakeError(STR_ERROR_NOT_ENOUGH_CASH_REQUIRES_CURRENCY);
		return false;
	}
	return true;
}

/**
 * Deduct costs of a command from the money of a company.
 * @param c Company to pay the bill.
 * @param cost Money to pay.
 */
static void SubtractMoneyFromAnyCompany(Company *c, const CommandCost &cost)
{
	if (cost.GetCost() == 0) return;
	assert(cost.GetExpensesType() != INVALID_EXPENSES);

	c->money -= cost.GetCost();
	c->yearly_expenses[0][cost.GetExpensesType()] += cost.GetCost();

	if (HasBit(1 << EXPENSES_TRAIN_REVENUE    |
	           1 << EXPENSES_ROADVEH_REVENUE  |
	           1 << EXPENSES_AIRCRAFT_REVENUE |
	           1 << EXPENSES_SHIP_REVENUE     |
	           1 << EXPENSES_SHARING_INC, cost.GetExpensesType())) {
		c->cur_economy.income -= cost.GetCost();
	} else if (HasBit(1 << EXPENSES_TRAIN_RUN    |
	                  1 << EXPENSES_ROADVEH_RUN  |
	                  1 << EXPENSES_AIRCRAFT_RUN |
	                  1 << EXPENSES_SHIP_RUN     |
	                  1 << EXPENSES_PROPERTY     |
	                  1 << EXPENSES_LOAN_INTEREST |
	                  1 << EXPENSES_SHARING_COST, cost.GetExpensesType())) {
		c->cur_economy.expenses -= cost.GetCost();
	}

	InvalidateCompanyWindows(c);
}

/**
 * Subtract money from the #_current_company, if the company is valid.
 * @param cost Money to pay.
 */
void SubtractMoneyFromCompany(const CommandCost &cost)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c != nullptr) SubtractMoneyFromAnyCompany(c, cost);
}

/**
 * Subtract money from a company, including the money fraction.
 * @param company Company paying the bill.
 * @param cst     Cost of a command.
 */
void SubtractMoneyFromCompanyFract(CompanyID company, const CommandCost &cst)
{
	Company *c = Company::Get(company);
	uint8_t m = c->money_fraction;
	Money cost = cst.GetCost();

	c->money_fraction = m - (uint8_t)cost;
	cost >>= 8;
	if (c->money_fraction > m) cost++;
	if (cost != 0) SubtractMoneyFromAnyCompany(c, CommandCost(cst.GetExpensesType(), cost));
}

static constexpr void UpdateLandscapingLimit(uint32_t &limit, uint64_t per_64k_frames, uint64_t burst)
{
	limit = static_cast<uint32_t>(std::min<uint64_t>(limit + per_64k_frames, burst << 16));
}

/** Update the landscaping limits per company. */
void UpdateLandscapingLimits()
{
	for (Company *c : Company::Iterate()) {
		UpdateLandscapingLimit(c->terraform_limit,     _settings_game.construction.terraform_per_64k_frames,     _settings_game.construction.terraform_frame_burst);
		UpdateLandscapingLimit(c->clear_limit,         _settings_game.construction.clear_per_64k_frames,         _settings_game.construction.clear_frame_burst);
		UpdateLandscapingLimit(c->tree_limit,          _settings_game.construction.tree_per_64k_frames,          _settings_game.construction.tree_frame_burst);
		UpdateLandscapingLimit(c->purchase_land_limit, _settings_game.construction.purchase_land_per_64k_frames, _settings_game.construction.purchase_land_frame_burst);
		UpdateLandscapingLimit(c->build_object_limit,  _settings_game.construction.build_object_per_64k_frames,  _settings_game.construction.build_object_frame_burst);
	}
}

/**
 * Set the right DParams for STR_ERROR_OWNED_BY.
 * @param owner the owner to get the name of.
 * @param tile  optional tile to get the right town.
 * @pre if tile == 0, then owner can't be OWNER_TOWN.
 */
void SetDParamsForOwnedBy(Owner owner, TileIndex tile)
{
	SetDParam(OWNED_BY_OWNER_IN_PARAMETERS_OFFSET, owner);

	if (owner != OWNER_TOWN) {
		if (!Company::IsValidID(owner)) {
			SetDParam(0, STR_COMPANY_SOMEONE);
		} else {
			SetDParam(0, STR_COMPANY_NAME);
			SetDParam(1, owner);
		}
	} else {
		assert(tile != 0);
		const Town *t = ClosestTownFromTile(tile, UINT_MAX);

		SetDParam(0, STR_TOWN_NAME);
		SetDParam(1, t->index);
	}
}


/**
 * Check whether the current owner owns something.
 * If that isn't the case an appropriate error will be given.
 * @param owner the owner of the thing to check.
 * @param tile  optional tile to get the right town.
 * @pre if tile == 0 then the owner can't be OWNER_TOWN.
 * @return A succeeded command iff it's owned by the current company, else a failed command.
 */
CommandCost CheckOwnership(Owner owner, TileIndex tile)
{
	assert(owner < OWNER_END);
	assert(owner != OWNER_TOWN || tile != 0);

	if (owner == _current_company) return CommandCost();

	SetDParamsForOwnedBy(owner, tile);
	return CommandCost(STR_ERROR_OWNED_BY);
}

/**
 * Check whether the current owner owns the stuff on
 * the given tile.  If that isn't the case an
 * appropriate error will be given.
 * @param tile the tile to check.
 * @return A succeeded command iff it's owned by the current company, else a failed command.
 */
CommandCost CheckTileOwnership(TileIndex tile)
{
	Owner owner = GetTileOwner(tile);

	assert(owner < OWNER_END);

	if (owner == _current_company) return CommandCost();

	/* no need to get the name of the owner unless we're the local company (saves some time) */
	if (IsLocalCompany()) SetDParamsForOwnedBy(owner, tile);
	return CommandCost(STR_ERROR_OWNED_BY);
}

/**
 * Generate the name of a company from the last build coordinate.
 * @param c Company to give a name.
 */
static void GenerateCompanyName(Company *c)
{
	if (c->name_1 != STR_SV_UNNAMED) return;
	if (c->last_build_coordinate == 0) return;

	Town *t = ClosestTownFromTile(c->last_build_coordinate, UINT_MAX);

	StringID str;
	uint32_t strp;
	std::string name;
	if (t->name.empty() && IsInsideMM(t->townnametype, SPECSTR_TOWNNAME_START, SPECSTR_TOWNNAME_END)) {
		str = t->townnametype - SPECSTR_TOWNNAME_START + SPECSTR_COMPANY_NAME_START;
		strp = t->townnameparts;

verify_name:;
		/* No companies must have this name already */
		for (const Company *cc : Company::Iterate()) {
			if (cc->name_1 == str && cc->name_2 == strp) goto bad_town_name;
		}

		SetDParam(0, strp);
		name = GetString(str);
		if (Utf8StringLength(name) >= MAX_LENGTH_COMPANY_NAME_CHARS) goto bad_town_name;

set_name:;
		c->name_1 = str;
		c->name_2 = strp;

		MarkWholeScreenDirty();
		AI::BroadcastNewEvent(new ScriptEventCompanyRenamed(c->index, name));
		Game::NewEvent(new ScriptEventCompanyRenamed(c->index, name));

		if (c->is_ai) {
			auto cni = std::make_unique<CompanyNewsInformation>(c);
			SetDParam(0, STR_NEWS_COMPANY_LAUNCH_TITLE);
			SetDParam(1, STR_NEWS_COMPANY_LAUNCH_DESCRIPTION);
			SetDParamStr(2, cni->company_name);
			SetDParam(3, t->index);
			AddNewsItem(STR_MESSAGE_NEWS_FORMAT, NewsType::CompanyInfo, NewsStyle::Company, {}, NewsReferenceType::Tile, c->last_build_coordinate.base(), NewsReferenceType::None, UINT32_MAX, std::move(cni));
		}
		return;
	}
bad_town_name:;

	if (c->president_name_1 == SPECSTR_PRESIDENT_NAME) {
		str = SPECSTR_ANDCO_NAME;
		strp = c->president_name_2;
		goto set_name;
	} else {
		str = SPECSTR_ANDCO_NAME;
		strp = Random();
		goto verify_name;
	}
}

/** Sorting weights for the company colours. */
static const uint8_t _colour_sort[COLOUR_END] = {2, 2, 3, 2, 3, 2, 3, 2, 3, 2, 2, 2, 3, 1, 1, 1};
/** Similar colours, so we can try to prevent same coloured companies. */
static const Colours _similar_colour[COLOUR_END][2] = {
	{ COLOUR_BLUE,       COLOUR_LIGHT_BLUE }, // COLOUR_DARK_BLUE
	{ COLOUR_GREEN,      COLOUR_DARK_GREEN }, // COLOUR_PALE_GREEN
	{ INVALID_COLOUR,    INVALID_COLOUR    }, // COLOUR_PINK
	{ COLOUR_ORANGE,     INVALID_COLOUR    }, // COLOUR_YELLOW
	{ INVALID_COLOUR,    INVALID_COLOUR    }, // COLOUR_RED
	{ COLOUR_DARK_BLUE,  COLOUR_BLUE       }, // COLOUR_LIGHT_BLUE
	{ COLOUR_PALE_GREEN, COLOUR_DARK_GREEN }, // COLOUR_GREEN
	{ COLOUR_PALE_GREEN, COLOUR_GREEN      }, // COLOUR_DARK_GREEN
	{ COLOUR_DARK_BLUE,  COLOUR_LIGHT_BLUE }, // COLOUR_BLUE
	{ COLOUR_BROWN,      COLOUR_ORANGE     }, // COLOUR_CREAM
	{ COLOUR_PURPLE,     INVALID_COLOUR    }, // COLOUR_MAUVE
	{ COLOUR_MAUVE,      INVALID_COLOUR    }, // COLOUR_PURPLE
	{ COLOUR_YELLOW,     COLOUR_CREAM      }, // COLOUR_ORANGE
	{ COLOUR_CREAM,      INVALID_COLOUR    }, // COLOUR_BROWN
	{ COLOUR_WHITE,      INVALID_COLOUR    }, // COLOUR_GREY
	{ COLOUR_GREY,       INVALID_COLOUR    }, // COLOUR_WHITE
};

/**
 * Generate a company colour.
 * @return Generated company colour.
 */
static Colours GenerateCompanyColour()
{
	Colours colours[COLOUR_END];

	/* Initialize array */
	for (uint i = 0; i < COLOUR_END; i++) colours[i] = static_cast<Colours>(i);

	/* And randomize it */
	for (uint i = 0; i < 100; i++) {
		uint r = Random();
		Swap(colours[GB(r, 0, 4)], colours[GB(r, 4, 4)]);
	}

	/* Bubble sort it according to the values in table 1 */
	for (uint i = 0; i < COLOUR_END; i++) {
		for (uint j = 1; j < COLOUR_END; j++) {
			if (_colour_sort[colours[j - 1]] < _colour_sort[colours[j]]) {
				Swap(colours[j - 1], colours[j]);
			}
		}
	}

	/* Move the colours that look similar to each company's colour to the side */
	for (const Company *c : Company::Iterate()) {
		Colours pcolour = c->colour;

		for (uint i = 0; i < COLOUR_END; i++) {
			if (colours[i] == pcolour) {
				colours[i] = INVALID_COLOUR;
				break;
			}
		}

		for (uint j = 0; j < 2; j++) {
			Colours similar = _similar_colour[pcolour][j];
			if (similar == INVALID_COLOUR) break;

			for (uint i = 1; i < COLOUR_END; i++) {
				if (colours[i - 1] == similar) Swap(colours[i - 1], colours[i]);
			}
		}
	}

	/* Return the first available colour */
	for (uint i = 0; i < COLOUR_END; i++) {
		if (colours[i] != INVALID_COLOUR) return colours[i];
	}

	NOT_REACHED();
}

/**
 * Generate a random president name of a company.
 * @param c Company that needs a new president name.
 */
static void GeneratePresidentName(Company *c)
{
	for (;;) {
restart:;
		c->president_name_2 = Random();
		c->president_name_1 = SPECSTR_PRESIDENT_NAME;

		/* Reserve space for extra unicode character. We need to do this to be able
		 * to detect too long president name. */
		SetDParam(0, c->index);
		std::string name = GetString(STR_PRESIDENT_NAME);
		if (Utf8StringLength(name) >= MAX_LENGTH_PRESIDENT_NAME_CHARS) continue;

		for (const Company *cc : Company::Iterate()) {
			if (c != cc) {
				SetDParam(0, cc->index);
				std::string other_name = GetString(STR_PRESIDENT_NAME);
				if (name == other_name) goto restart;
			}
		}
		return;
	}
}

/**
 * Reset the livery schemes to the company's primary colour.
 * This is used on loading games without livery information and on new company start up.
 * @param c Company to reset.
 */
void ResetCompanyLivery(Company *c)
{
	for (LiveryScheme scheme = LS_BEGIN; scheme < LS_END; scheme++) {
		c->livery[scheme].in_use  = 0;
		c->livery[scheme].colour1 = c->colour;
		c->livery[scheme].colour2 = c->colour;
	}

	for (Group *g : Group::Iterate()) {
		if (g->owner == c->index) {
			g->livery.in_use  = 0;
			g->livery.colour1 = c->colour;
			g->livery.colour2 = c->colour;
		}
	}
}

/**
 * Create a new company and sets all company variables default values
 *
 * @param flags oepration flags
 * @param company CompanyID to use for the new company
 * @return the company struct
 */
Company *DoStartupNewCompany(DoStartupNewCompanyFlag flags, CompanyID company)
{
	if (!Company::CanAllocateItem()) return nullptr;

	const bool is_ai = (flags & DSNC_AI);

	/* we have to generate colour before this company is valid */
	Colours colour = GenerateCompanyColour();

	Company *c;
	if (company == INVALID_COMPANY) {
		c = new Company(STR_SV_UNNAMED, is_ai);
	} else {
		if (Company::IsValidID(company)) return nullptr;
		c = new (company) Company(STR_SV_UNNAMED, is_ai);
	}

	c->colour = colour;

	ResetCompanyLivery(c);
	_company_colours[c->index] = c->colour;

	/* Scale the initial loan based on the inflation rounded down to the loan interval. The maximum loan has already been inflation adjusted. */
	c->money = c->current_loan = std::min<int64_t>((INITIAL_LOAN * _economy.inflation_prices >> 16) / LOAN_INTERVAL * LOAN_INTERVAL, _economy.max_loan);

	std::fill(c->share_owners.begin(), c->share_owners.end(), INVALID_OWNER);

	c->avail_railtypes = GetCompanyRailTypes(c->index);
	c->avail_roadtypes = GetCompanyRoadTypes(c->index);
	c->inaugurated_year = CalTime::CurYear();
	c->display_inaugurated_period = EconTime::Detail::WallClockYearToDisplay(EconTime::CurYear());

	/* If starting a player company in singleplayer and a favorite company manager face is selected, choose it. Otherwise, use a random face.
	 * In a network game, we'll choose the favorite face later in CmdCompanyCtrl to sync it to all clients. */
	if (_company_manager_face != 0 && !is_ai && !_networking) {
		c->face = _company_manager_face;
	} else {
		RandomCompanyManagerFaceBits(c->face, (GenderEthnicity)Random(), false, _random);
	}

	SetDefaultCompanySettings(c->index);
	ClearEnginesHiddenFlagOfCompany(c->index);

	GeneratePresidentName(c);

	SetWindowDirty(WC_GRAPH_LEGEND, 0);
	InvalidateWindowData(WC_CLIENT_LIST, 0);
	InvalidateWindowData(WC_LINKGRAPH_LEGEND, 0);
	BuildOwnerLegend();
	InvalidateWindowData(WC_SMALLMAP, 0, 1);

	if (is_ai && (!_networking || _network_server)) AI::StartNew(c->index);

	AI::BroadcastNewEvent(new ScriptEventCompanyNew(c->index), c->index);
	Game::NewEvent(new ScriptEventCompanyNew(c->index));

	if (!is_ai && !(flags & DSNC_DURING_LOAD)) UpdateAllTownVirtCoords();

	return c;
}

/** Start a new competitor company if possible. */
TimeoutTimer<TimerGameTick> _new_competitor_timeout({ TimerGameTick::Priority::COMPETITOR_TIMEOUT, 0 }, []() {
	if (_game_mode == GM_MENU || !AI::CanStartNew()) return;
	if (_networking && Company::GetNumItems() >= _settings_client.network.max_companies) return;

	/* count number of competitors */
	uint8_t n = 0;
	for (const Company *c : Company::Iterate()) {
		if (c->is_ai) n++;
	}

	if (n >= _settings_game.difficulty.max_no_competitors) return;

	/* Send a command to all clients to start up a new AI.
	 * Works fine for Multiplayer and Singleplayer */
	Command<CMD_COMPANY_CTRL>::Post(CCA_NEW_AI, INVALID_COMPANY, CRR_NONE, INVALID_CLIENT_ID, {});
});

/** Start of a new game. */
void StartupCompanies()
{
	/* Ensure the timeout is aborted, so it doesn't fire based on information of the last game. */
	_new_competitor_timeout.Abort();
}

static void ClearSavedPLYP()
{
	_saved_PLYP_invalid_mask = {};
	_saved_PLYP_data.clear();
}

/** Initialize the pool of companies. */
void InitializeCompanies()
{
	_cur_company_tick_index = 0;
	ClearSavedPLYP();
}

void UninitializeCompanies()
{
	ClearSavedPLYP();
}

/**
 * Can company \a cbig buy company \a csmall without exceeding vehicle limits?
 * @param cbig   Company buying \a csmall.
 * @param csmall Company getting bought.
 * @return Return \c true if it is allowed.
 */
bool CheckTakeoverVehicleLimit(CompanyID cbig, CompanyID csmall)
{
	const Company *c1 = Company::Get(cbig);
	const Company *c2 = Company::Get(csmall);

	/* Do the combined vehicle counts stay within the limits? */
	return c1->group_all[VEH_TRAIN].num_vehicle + c2->group_all[VEH_TRAIN].num_vehicle <= _settings_game.vehicle.max_trains &&
		c1->group_all[VEH_ROAD].num_vehicle     + c2->group_all[VEH_ROAD].num_vehicle     <= _settings_game.vehicle.max_roadveh &&
		c1->group_all[VEH_SHIP].num_vehicle     + c2->group_all[VEH_SHIP].num_vehicle     <= _settings_game.vehicle.max_ships &&
		c1->group_all[VEH_AIRCRAFT].num_vehicle + c2->group_all[VEH_AIRCRAFT].num_vehicle <= _settings_game.vehicle.max_aircraft;
}

/**
 * Handle the bankruptcy take over of a company.
 * Companies going bankrupt will ask the other companies in order of their
 * performance rating, so better performing companies get the 'do you want to
 * merge with Y' question earlier. The question will then stay till either the
 * company has gone bankrupt or got merged with a company.
 *
 * @param c the company that is going bankrupt.
 */
static void HandleBankruptcyTakeover(Company *c)
{
	/* Amount of time out for each company to take over a company;
	 * Timeout is a quarter (3 months of 30 days) divided over the
	 * number of companies. The minimum number of days in a quarter
	 * is 90: 31 in January, 28 in February and 31 in March.
	 * Note that the company going bankrupt can't buy itself. */
	static const int TAKE_OVER_TIMEOUT = 3 * 30 * DAY_TICKS / (MAX_COMPANIES - 1);

	assert(c->bankrupt_asked.Any());


	/* We're currently asking some company to buy 'us' */
	if (c->bankrupt_timeout != 0) {
		if (!Company::IsValidID(c->bankrupt_last_asked)) {
			c->bankrupt_timeout = 0;
			return;
		}
		if (_network_server && Company::IsValidHumanID(c->bankrupt_last_asked) && !NetworkCompanyHasClients(c->bankrupt_last_asked)) {
			/* This company can no longer accept the offer as there are no clients connected, decline the offer on the company's behalf */
			Backup<CompanyID> cur_company(_current_company, c->bankrupt_last_asked, FILE_LINE);
			Command<CMD_DECLINE_BUY_COMPANY>::Post(c->index);
			cur_company.Restore();
		}
		c->bankrupt_timeout -= MAX_COMPANIES;
		if (c->bankrupt_timeout > 0) return;
		c->bankrupt_timeout = 0;

		return;
	}

	/* Did we ask everyone for bankruptcy? If so, bail out. */
	if (c->bankrupt_asked.All()) return;

	Company *best = nullptr;
	int32_t best_performance = -1;

	/* Ask the company with the highest performance history first */
	for (Company *c2 : Company::Iterate()) {
		if ((c2->bankrupt_asked.None() || (c2->bankrupt_flags & CBRF_SALE_ONLY)) && // Don't ask companies going bankrupt themselves
				!c->bankrupt_asked.Test(c2->index) &&
				best_performance < c2->old_economy[1].performance_history &&
				CheckTakeoverVehicleLimit(c2->index, c->index)) {
			best_performance = c2->old_economy[1].performance_history;
			best = c2;
		}
	}

	/* Asked all companies? */
	if (best_performance == -1) {
		if (c->bankrupt_flags & CBRF_SALE_ONLY) {
			c->bankrupt_asked = {};
			CloseWindowById(WC_BUY_COMPANY, c->index);
		} else {
			c->bankrupt_asked.Set();
		}
		c->bankrupt_flags = CBRF_NONE;
		return;
	}

	c->bankrupt_asked.Set(best->index);
	c->bankrupt_last_asked = best->index;

	c->bankrupt_timeout = TAKE_OVER_TIMEOUT;

	AI::NewEvent(best->index, new ScriptEventCompanyAskMerger(c->index, c->bankrupt_value));
	if (IsInteractiveCompany(best->index)) {
		ShowBuyCompanyDialog(c->index, false);
	} else if ((!_networking || (_network_server && !NetworkCompanyHasClients(best->index))) && !best->is_ai) {
		/* This company can never accept the offer as there are no clients connected, decline the offer on the company's behalf */
		Backup<CompanyID> cur_company(_current_company, best->index, FILE_LINE);
		Command<CMD_DECLINE_BUY_COMPANY>::Post(c->index);
		cur_company.Restore();
	}
}

/** Called every tick for updating some company info. */
void OnTick_Companies(bool main_tick)
{
	if (_game_mode == GM_EDITOR) return;

	if (main_tick) {
		Company *c = Company::GetIfValid(_cur_company_tick_index);
		if (c != nullptr) {
			if (c->bankrupt_asked.Any()) HandleBankruptcyTakeover(c);
		}
		_cur_company_tick_index = (_cur_company_tick_index + 1) % MAX_COMPANIES;
	}
	for (Company *c : Company::Iterate()) {
		if (c->name_1 != 0) GenerateCompanyName(c);
		if (c->bankrupt_asked.Any() && c->bankrupt_timeout == 0) HandleBankruptcyTakeover(c);
	}

	if (_new_competitor_timeout.HasFired() && _game_mode != GM_MENU && AI::CanStartNew()) {
		int32_t timeout = _settings_game.difficulty.competitors_interval * 60 * TICKS_PER_SECOND;
		/* If the interval is zero, start as many competitors as needed then check every ~10 minutes if a company went bankrupt and needs replacing. */
		if (timeout == 0) {
			/* count number of competitors */
			uint8_t n = 0;
			for (const Company *cc : Company::Iterate()) {
				if (cc->is_ai) n++;
			}

			for (auto i = 0; i < _settings_game.difficulty.max_no_competitors; i++) {
				if (_networking && Company::GetNumItems() >= _settings_client.network.max_companies) break;
				if (n++ >= _settings_game.difficulty.max_no_competitors) break;
				Command<CMD_COMPANY_CTRL>::Post(CCA_NEW_AI, INVALID_COMPANY, CRR_NONE, INVALID_CLIENT_ID, {});
			}
			timeout = 10 * 60 * TICKS_PER_SECOND;
		}
		/* Randomize a bit when the AI is actually going to start; ranges from 87.5% .. 112.5% of indicated value. */
		timeout += ScriptObject::GetRandomizer(OWNER_NONE).Next(timeout / 4) - timeout / 8;

		_new_competitor_timeout.Reset({ TimerGameTick::Priority::COMPETITOR_TIMEOUT, static_cast<uint>(std::max(1, timeout)) });
	}
}

/**
 * A year has passed, update the economic data of all companies, and perhaps show the
 * financial overview window of the local company.
 */
void CompaniesYearlyLoop()
{
	/* Copy statistics */
	for (Company *c : Company::Iterate()) {
		/* Move expenses to previous years. */
		std::rotate(std::rbegin(c->yearly_expenses), std::rbegin(c->yearly_expenses) + 1, std::rend(c->yearly_expenses));
		c->yearly_expenses[0].fill(0);
		c->age_years++;
		InvalidateWindowData(WC_FINANCES, c->index);
	}

	if (_settings_client.gui.show_finances && _local_company != COMPANY_SPECTATOR) {
		ShowCompanyFinances(_local_company);
		Company *c = Company::Get(_local_company);
		if (c->num_valid_stat_ent > 5 && c->old_economy[0].performance_history < c->old_economy[4].performance_history) {
			if (_settings_client.sound.new_year) SndPlayFx(SND_01_BAD_YEAR);
		} else {
			if (_settings_client.sound.new_year) SndPlayFx(SND_00_GOOD_YEAR);
		}
	}
}

/**
 * Fill the CompanyNewsInformation struct with the required data.
 * @param c the current company.
 * @param other the other company (use \c nullptr if not relevant).
 */
CompanyNewsInformation::CompanyNewsInformation(const Company *c, const Company *other)
{
	SetDParam(0, c->index);
	this->company_name = GetString(STR_COMPANY_NAME);

	if (other != nullptr) {
		SetDParam(0, other->index);
		this->other_company_name = GetString(STR_COMPANY_NAME);
		c = other;
	}

	SetDParam(0, c->index);
	this->president_name = GetString(STR_PRESIDENT_NAME_MANAGER);

	this->colour = c->colour;
	this->face = c->face;

}

/**
 * Called whenever company related information changes in order to notify admins.
 * @param company The company data changed of.
 */
void CompanyAdminUpdate(const Company *company)
{
	if (_network_server) NetworkAdminCompanyUpdate(company);
}

/**
 * Called whenever a company is removed in order to notify admins.
 * @param company_id The company that was removed.
 * @param reason     The reason the company was removed.
 */
void CompanyAdminRemove(CompanyID company_id, CompanyRemoveReason reason)
{
	if (_network_server) NetworkAdminCompanyRemove(company_id, (AdminCompanyRemoveReason)reason);
}

/**
 * Control the companies: add, delete, etc.
 * @param flags operation to perform
 * @param cca action to perform
 * @param company_id company to perform the action on
 * @param reason company remove reason (with CCA_DELETE)
 * @param client_id ClientID
 * @param to_merge_id CompanyID to merge (with CCA_MERGE)
 * @return the cost of this operation or an error
 */
CommandCost CmdCompanyCtrl(DoCommandFlag flags, CompanyCtrlAction cca, CompanyID company_id, CompanyRemoveReason reason, ClientID client_id, CompanyID to_merge_id)
{
	InvalidateWindowData(WC_COMPANY_LEAGUE, 0, 0);

	switch (cca) {
		case CCA_NEW: { // Create a new company
			/* This command is only executed in a multiplayer game */
			if (!_networking) return CMD_ERROR;

			/* Has the network client a correct ClientID? */
			if (!(flags & DC_EXEC)) return CommandCost();

			NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(client_id);

			/* Delete multiplayer progress bar */
			CloseWindowById(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);

			Company *c = DoStartupNewCompany(DSNC_NONE);

			/* A new company could not be created, revert to being a spectator */
			if (c == nullptr) {
				/* We check for "ci != nullptr" as a client could have left by
				 * the time we execute this command. */
				if (_network_server && ci != nullptr) {
					ci->client_playas = COMPANY_SPECTATOR;
					NetworkUpdateClientInfo(ci->client_id);
				}
				break;
			}

			/* Send new companies, before potentially setting the password. Otherwise,
			 * the password update could be sent when the company is not yet known. */
			NetworkAdminCompanyNew(c);
			NetworkServerNewCompany(c, ci);

			/* This is the client (or non-dedicated server) who wants a new company */
			if (client_id == _network_own_client_id) {
				assert(_local_company == COMPANY_SPECTATOR);
				SetLocalCompany(c->index);
				if (!_settings_client.network.default_company_pass.empty()) {
					NetworkChangeCompanyPassword(_local_company, _settings_client.network.default_company_pass);
				}

				/* In network games, we need to try setting the company manager face here to sync it to all clients.
				 * If a favorite company manager face is selected, choose it. Otherwise, use a random face. */
				if (_company_manager_face != 0) {
					NetworkSendCommand<CMD_SET_COMPANY_MANAGER_FACE>({}, CmdPayload<CMD_SET_COMPANY_MANAGER_FACE>::Make(_company_manager_face), (StringID)0, CommandCallback::None, 0, _local_company);
				}

				/* Now that we have a new company, broadcast our company settings to
				 * all clients so everything is in sync */
				SyncCompanySettings();

				MarkWholeScreenDirty();
			}

			Debug(desync, 1, "new_company: {}, company_id: {}", debug_date_dumper().HexDate(), c->index);
			break;
		}

		case CCA_NEW_AI: { // Make a new AI company
			if (company_id != INVALID_COMPANY && company_id >= MAX_COMPANIES) return CMD_ERROR;

			/* For network games, company deletion is delayed. */
			if (!_networking && company_id != INVALID_COMPANY && Company::IsValidID(company_id)) return CMD_ERROR;

			if (!(flags & DC_EXEC)) return CommandCost();

			/* For network game, just assume deletion happened. */
			assert(company_id == INVALID_COMPANY || !Company::IsValidID(company_id));

			Company *c = DoStartupNewCompany(DSNC_AI, company_id);
			if (c != nullptr) {
				NetworkAdminCompanyNew(c);
				NetworkServerNewCompany(c, nullptr);
				Debug(desync, 1, "new_company_ai: {}, company_id: {}", debug_date_dumper().HexDate(), c->index);
			}
			break;
		}

		case CCA_DELETE: { // Delete a company
			if (reason >= CRR_END) return CMD_ERROR;

			/* We can't delete the last existing company in singleplayer mode. */
			if (!_networking && Company::GetNumItems() == 1) return CMD_ERROR;

			Company *c = Company::GetIfValid(company_id);
			if (c == nullptr) return CMD_ERROR;

			if (!(flags & DC_EXEC)) return CommandCost();

			Debug(desync, 1, "delete_company: {}, company_id: {}, reason: {}", debug_date_dumper().HexDate(), company_id, reason);

			auto cni = std::make_unique<CompanyNewsInformation>(c);

			/* Show the bankrupt news */
			SetDParam(0, STR_NEWS_COMPANY_BANKRUPT_TITLE);
			SetDParam(1, STR_NEWS_COMPANY_BANKRUPT_DESCRIPTION);
			SetDParamStr(2, cni->company_name);
			AddCompanyNewsItem(STR_MESSAGE_NEWS_FORMAT, std::move(cni));

			/* Remove the company */
			ChangeOwnershipOfCompanyItems(c->index, INVALID_OWNER);
			if (c->is_ai) AI::Stop(c->index);

			CompanyID c_index = c->index;
			delete c;
			AI::BroadcastNewEvent(new ScriptEventCompanyBankrupt(c_index));
			Game::NewEvent(new ScriptEventCompanyBankrupt(c_index));
			CompanyAdminRemove(c_index, (CompanyRemoveReason)reason);

			if (StoryPage::GetNumItems() == 0 || Goal::GetNumItems() == 0) InvalidateWindowData(WC_MAIN_TOOLBAR, 0);

			InvalidateWindowData(WC_CLIENT_LIST, 0);
			InvalidateWindowClassesData(WC_DEPARTURES_BOARD, 0);

			CheckCaches(true, nullptr, CHECK_CACHE_ALL | CHECK_CACHE_EMIT_LOG);
			break;
		}

		case CCA_SALE: {
			Company *c = Company::GetIfValid(company_id);
			if (c == nullptr) return CMD_ERROR;

			if (!(flags & DC_EXEC)) return CommandCost();

			c->bankrupt_flags |= CBRF_SALE;
			if (c->bankrupt_asked.None()) c->bankrupt_flags |= CBRF_SALE_ONLY;
			c->bankrupt_value = CalculateCompanyValue(c, false);
			c->bankrupt_asked = CompanyMask{}.Set(c->index); // Don't ask the owner
			c->bankrupt_timeout = 0;
			CloseWindowById(WC_BUY_COMPANY, c->index);
			break;
		}

		case CCA_MERGE: {
			Company *c = Company::GetIfValid(company_id);
			if (c == nullptr) return CMD_ERROR;

			if (to_merge_id == company_id) return CMD_ERROR;

			Company *to_merge = Company::GetIfValid(to_merge_id);
			if (to_merge == nullptr) return CMD_ERROR;

			if (!(flags & DC_EXEC)) return CommandCost();

			SubtractMoneyFromAnyCompany(c, CommandCost(EXPENSES_OTHER, to_merge->current_loan - to_merge->money));

			Debug(desync, 1, "merge_companies: {}, company_id: {}, merged_company_id: {}", debug_date_dumper().HexDate(), company_id, to_merge_id);

			auto cni = std::make_unique<CompanyNewsInformation>(to_merge, c);

			SetDParam(0, STR_NEWS_COMPANY_MERGER_TITLE);
			SetDParam(1, STR_NEWS_MERGER_TAKEOVER_TITLE);
			SetDParamStr(2, cni->company_name);
			SetDParamStr(3, cni->other_company_name);
			AddCompanyNewsItem(STR_MESSAGE_NEWS_FORMAT, std::move(cni));
			AI::BroadcastNewEvent(new ScriptEventCompanyMerger(to_merge_id, company_id));
			Game::NewEvent(new ScriptEventCompanyMerger(to_merge_id, company_id));

			ChangeOwnershipOfCompanyItems(to_merge_id, company_id);

			PostAcquireCompany(to_merge);
			break;
		}

		default: return CMD_ERROR;
	}

	InvalidateWindowClassesData(WC_GAME_OPTIONS);
	InvalidateWindowClassesData(WC_SCRIPT_SETTINGS);
	InvalidateWindowClassesData(WC_SCRIPT_LIST);

	return CommandCost();
}

static bool ExecuteAllowListCtrlAction(CompanyAllowListCtrlAction action, Company *c, const std::string &public_key)
{
	switch (action) {
		case CALCA_ADD:
			return c->allow_list.Add(public_key);

		case CALCA_REMOVE:
			return c->allow_list.Remove(public_key);

		default:
			NOT_REACHED();
	}
}

/**
 * Add or remove the given public key to the allow list of this company.
 * @param flags Operation to perform.
 * @param action The action to perform.
 * @param public_key The public key of the client to add or remove.
 * @return The cost of this operation or an error.
 */
CommandCost CmdCompanyAllowListCtrl(DoCommandFlag flags, CompanyAllowListCtrlAction action, const std::string &public_key)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;

	/* The public key length includes the '\0'. */
	if (public_key.size() != NETWORK_PUBLIC_KEY_LENGTH - 1) return CMD_ERROR;

	switch (action) {
		case CALCA_ADD:
		case CALCA_REMOVE:
			break;

		default:
			return CMD_ERROR;
	}

	if (flags & DC_EXEC) {
		if (ExecuteAllowListCtrlAction(action, c, public_key)) {
			InvalidateWindowData(WC_CLIENT_LIST, 0);
			SetWindowDirty(WC_COMPANY, _current_company);
		}
	}

	return CommandCost();
}

/**
 * Change the company manager's face.
 * @param flags operation to perform
 * @param cmf face bitmasked
 * @return the cost of this operation or an error
 */
CommandCost CmdSetCompanyManagerFace(DoCommandFlag flags, CompanyManagerFace cmf)
{
	if (!IsValidCompanyManagerFace(cmf)) return CMD_ERROR;

	if (flags & DC_EXEC) {
		Company::Get(_current_company)->face = cmf;
		MarkWholeScreenDirty();
	}
	return CommandCost();
}

/**
 * Update liveries for a company. This is called when the LS_DEFAULT scheme is changed, to update schemes with colours
 * set to default.
 * @param c Company to update.
 */
void UpdateCompanyLiveries(Company *c)
{
	for (int i = 1; i < LS_END; i++) {
		if (!HasBit(c->livery[i].in_use, 0)) c->livery[i].colour1 = c->livery[LS_DEFAULT].colour1;
		if (!HasBit(c->livery[i].in_use, 1)) c->livery[i].colour2 = c->livery[LS_DEFAULT].colour2;
	}
	UpdateCompanyGroupLiveries(c);
}

/**
 * Change the company's company-colour
 * @param flags operation to perform
 * @param scheme scheme to set
 * @param primary set first/second colour
 * @param colour new colour for vehicles, property, etc.
 * @return the cost of this operation or an error
 */
CommandCost CmdSetCompanyColour(DoCommandFlag flags, LiveryScheme scheme, bool primary, Colours colour)
{
	if (scheme >= LS_END || (colour >= COLOUR_END && colour != INVALID_COLOUR)) return CMD_ERROR;

	/* Default scheme can't be reset to invalid. */
	if (scheme == LS_DEFAULT && colour == INVALID_COLOUR) return CMD_ERROR;

	Company *c = Company::Get(_current_company);

	/* Ensure no two companies have the same primary colour */
	if (scheme == LS_DEFAULT && primary) {
		for (const Company *cc : Company::Iterate()) {
			if (cc != c && cc->colour == colour) return CMD_ERROR;
		}
	}

	if (flags & DC_EXEC) {
		if (primary) {
			if (scheme != LS_DEFAULT) AssignBit(c->livery[scheme].in_use, 0, colour != INVALID_COLOUR);
			if (colour == INVALID_COLOUR) colour = c->livery[LS_DEFAULT].colour1;
			c->livery[scheme].colour1 = colour;

			/* If setting the first colour of the default scheme, adjust the
			 * original and cached company colours too. */
			if (scheme == LS_DEFAULT) {
				UpdateCompanyLiveries(c);
				_company_colours[_current_company] = colour;
				c->colour = colour;
				CompanyAdminUpdate(c);
			}
		} else {
			if (scheme != LS_DEFAULT) AssignBit(c->livery[scheme].in_use, 1, colour != INVALID_COLOUR);
			if (colour == INVALID_COLOUR) colour = c->livery[LS_DEFAULT].colour2;
			c->livery[scheme].colour2 = colour;

			if (scheme == LS_DEFAULT) {
				UpdateCompanyLiveries(c);
			}
		}

		if (c->livery[scheme].in_use != 0) {
			/* If enabling a scheme, set the default scheme to be in use too */
			c->livery[LS_DEFAULT].in_use = 1;
		} else {
			/* Else loop through all schemes to see if any are left enabled.
			 * If not, disable the default scheme too. */
			c->livery[LS_DEFAULT].in_use = 0;
			for (scheme = LS_DEFAULT; scheme < LS_END; scheme++) {
				if (c->livery[scheme].in_use != 0) {
					c->livery[LS_DEFAULT].in_use = 1;
					break;
				}
			}
		}

		ResetVehicleColourMap();
		InvalidateTemplateReplacementImages();
		MarkWholeScreenDirty();

		/* All graph related to companies use the company colour. */
		InvalidateWindowData(WC_INCOME_GRAPH, 0);
		InvalidateWindowData(WC_OPERATING_PROFIT, 0);
		InvalidateWindowData(WC_DELIVERED_CARGO, 0);
		InvalidateWindowData(WC_PERFORMANCE_HISTORY, 0);
		InvalidateWindowData(WC_COMPANY_VALUE, 0);
		InvalidateWindowData(WC_LINKGRAPH_LEGEND, 0);
		/* The smallmap owner view also stores the company colours. */
		BuildOwnerLegend();
		InvalidateWindowData(WC_SMALLMAP, 0, 1);

		extern void MarkAllViewportMapLandscapesDirty();
		MarkAllViewportMapLandscapesDirty();

		/* Company colour data is indirectly cached. */
		for (Vehicle *v : Vehicle::Iterate()) {
			if (v->owner == _current_company) {
				v->InvalidateNewGRFCache();
				v->InvalidateImageCache();
			}
		}

		UpdateObjectColours(c);
	}
	return CommandCost();
}

/**
 * Is the given name in use as name of a company?
 * @param name Name to search.
 * @return \c true if the name us unique (that is, not in use), else \c false.
 */
static bool IsUniqueCompanyName(std::string_view name)
{
	for (const Company *c : Company::Iterate()) {
		if (!c->name.empty() && c->name == name) return false;
	}

	return true;
}

/**
 * Change the name of the company.
 * @param flags operation to perform
 * @param text the new name or an empty string when resetting to the default
 * @return the cost of this operation or an error
 */
CommandCost CmdRenameCompany(DoCommandFlag flags, const std::string &text)
{
	bool reset = text.empty();

	if (!reset) {
		if (Utf8StringLength(text) >= MAX_LENGTH_COMPANY_NAME_CHARS) return CMD_ERROR;
		if (!IsUniqueCompanyName(text)) return CommandCost(STR_ERROR_NAME_MUST_BE_UNIQUE);
	}

	if (flags & DC_EXEC) {
		Company *c = Company::Get(_current_company);
		if (reset) {
			c->name.clear();
		} else {
			c->name = text;
		}
		MarkWholeScreenDirty();
		CompanyAdminUpdate(c);

		SetDParam(0, c->index);
		std::string new_name = GetString(STR_COMPANY_NAME);
		AI::BroadcastNewEvent(new ScriptEventCompanyRenamed(c->index, new_name));
		Game::NewEvent(new ScriptEventCompanyRenamed(c->index, new_name));
	}

	return CommandCost();
}

/**
 * Is the given name in use as president name of a company?
 * @param name Name to search.
 * @return \c true if the name us unique (that is, not in use), else \c false.
 */
static bool IsUniquePresidentName(std::string_view name)
{
	for (const Company *c : Company::Iterate()) {
		if (!c->president_name.empty() && c->president_name == name) return false;
	}

	return true;
}

/**
 * Change the name of the president.
 * @param flags operation to perform
 * @param text the new name or an empty string when resetting to the default
 * @return the cost of this operation or an error
 */
CommandCost CmdRenamePresident(DoCommandFlag flags, const std::string &text)
{
	bool reset = text.empty();

	if (!reset) {
		if (Utf8StringLength(text) >= MAX_LENGTH_PRESIDENT_NAME_CHARS) return CMD_ERROR;
		if (!IsUniquePresidentName(text)) return CommandCost(STR_ERROR_NAME_MUST_BE_UNIQUE);
	}

	if (flags & DC_EXEC) {
		Company *c = Company::Get(_current_company);

		if (reset) {
			c->president_name.clear();
		} else {
			c->president_name = text;

			if (c->name_1 == STR_SV_UNNAMED && c->name.empty()) {
				Command<CMD_RENAME_COMPANY>::Do(DC_EXEC, text + " Transport");
			}
		}

		InvalidateWindowClassesData(WC_COMPANY, 1);
		MarkWholeScreenDirty();
		CompanyAdminUpdate(c);

		SetDParam(0, c->index);
		std::string new_name = GetString(STR_PRESIDENT_NAME);
		AI::BroadcastNewEvent(new ScriptEventPresidentRenamed(c->index, new_name));
		Game::NewEvent(new ScriptEventPresidentRenamed(c->index, new_name));
	}

	return CommandCost();
}

/**
 * Get the service interval for the given company and vehicle type.
 * @param c The company, or nullptr for client-default settings.
 * @param type The vehicle type to get the interval for.
 * @return The service interval.
 */
int CompanyServiceInterval(const Company *c, VehicleType type)
{
	const VehicleDefaultSettings *vds = (c == nullptr) ? &_settings_client.company.vehicle : &c->settings.vehicle;
	switch (type) {
		default: NOT_REACHED();
		case VEH_TRAIN:    return vds->servint_trains;
		case VEH_ROAD:     return vds->servint_roadveh;
		case VEH_AIRCRAFT: return vds->servint_aircraft;
		case VEH_SHIP:     return vds->servint_ships;
	}
}

/**
 * Get the default local company after loading a new game
 */
CompanyID GetDefaultLocalCompany()
{
	if (_loaded_local_company < MAX_COMPANIES && Company::IsValidID(_loaded_local_company)) {
		return _loaded_local_company;
	}
	for (CompanyID i = COMPANY_FIRST; i < MAX_COMPANIES; i++) {
		if (Company::IsValidID(i)) return i;
	}
	return COMPANY_FIRST;
}

/**
 * Get total sum of all owned road bits.
 * @return Combined total road road bits.
 */
uint32_t CompanyInfrastructure::GetRoadTotal() const
{
	uint32_t total = 0;
	for (RoadType rt = ROADTYPE_BEGIN; rt != ROADTYPE_END; rt++) {
		if (RoadTypeIsRoad(rt)) total += this->road[rt];
	}
	return total;
}

/**
 * Get total sum of all owned tram bits.
 * @return Combined total of tram road bits.
 */
uint32_t CompanyInfrastructure::GetTramTotal() const
{
	uint32_t total = 0;
	for (RoadType rt = ROADTYPE_BEGIN; rt != ROADTYPE_END; rt++) {
		if (RoadTypeIsTram(rt)) total += this->road[rt];
	}
	return total;
}

void CompanyInfrastructure::Dump(format_target &buffer) const
{
	uint rail_total = 0;
	for (RailType rt = RAILTYPE_BEGIN; rt != RAILTYPE_END; rt++) {
		if (rail[rt]) buffer.format("Rail: {}: {}\n", GetStringPtr(GetRailTypeInfo(rt)->strings.name), rail[rt]);
		rail_total += rail[rt];
	}
	buffer.format("Total Rail: {}\n", rail_total);
	buffer.format("Signal: {}\n", signal);
	for (RoadType rt = ROADTYPE_BEGIN; rt != ROADTYPE_END; rt++) {
		if (road[rt]) buffer.format("{}: {}: {}\n", RoadTypeIsTram(rt) ? "Tram" : "Road", GetStringPtr(GetRoadTypeInfo(rt)->strings.name), road[rt]);
	}
	buffer.format("Total Road: {}\n", this->GetRoadTotal());
	buffer.format("Total Tram: {}\n", this->GetTramTotal());
	buffer.format("Water: {}\n", water);
	buffer.format("Station: {}\n", station);
	buffer.format("Airport: {}\n", airport);
}

void CmdCompanyCtrlData::FormatDebugSummary(format_target &output) const
{
	auto cca_name = [&]() -> const char * {
		switch (this->cca) {
			case CCA_NEW: return "new";
			case CCA_NEW_AI: return "new_ai";
			case CCA_DELETE: return "delete";
			case CCA_SALE: return "sale";
			case CCA_MERGE: return "merge";
			default: return "???";
		}
	};

	output.format("cca: {} ({}), cid: {}, client: {}", this->cca, cca_name(), this->company_id, this->client_id);
	if (this->cca == CCA_DELETE) output.format(", reason: {}", this->reason);
	if (this->cca == CCA_MERGE) output.format(", to_merge: {}", this->to_merge_id);
}
