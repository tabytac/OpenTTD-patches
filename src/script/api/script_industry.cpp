/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file script_industry.cpp Implementation of ScriptIndustry. */

#include "../../stdafx.h"
#include "script_industry.hpp"
#include "script_cargo.hpp"
#include "script_company.hpp"
#include "script_error.hpp"
#include "script_map.hpp"
#include "../../company_base.h"
#include "../../industry.h"
#include "../../industry_cmd.h"
#include "../../string_func.h"
#include "../../strings_func.h"
#include "../../station_base.h"
#include "../../newgrf_industries.h"
#include "table/strings.h"
#include <numeric>

#include "../../safeguards.h"

/* static */ SQInteger ScriptIndustry::GetIndustryCount()
{
	return ::Industry::GetNumItems();
}

/* static */ bool ScriptIndustry::IsValidIndustry(IndustryID industry_id)
{
	return ::Industry::IsValidID(industry_id);
}

/* static */ IndustryID ScriptIndustry::GetIndustryID(TileIndex tile)
{
	if (!::IsValidTile(tile) || !::IsTileType(tile, MP_INDUSTRY)) return INVALID_INDUSTRY;
	return ::GetIndustryIndex(tile);
}

/* static */ std::optional<std::string> ScriptIndustry::GetName(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return std::nullopt;

	::SetDParam(0, industry_id);
	return GetString(STR_INDUSTRY_NAME);
}

/* static */ ScriptDate::Date ScriptIndustry::GetConstructionDate(IndustryID industry_id)
{
	const Industry *i = Industry::GetIfValid(industry_id);
	if (i == nullptr) return ScriptDate::DATE_INVALID;
	return (ScriptDate::Date)i->construction_date.base();
}

/* static */ bool ScriptIndustry::SetText(IndustryID industry_id, Text *text)
{
	ScriptObjectRef counter(text);

	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidIndustry(industry_id));

	return ScriptObject::Command<CMD_INDUSTRY_SET_TEXT>::Do(industry_id, text != nullptr ? text->GetEncodedText() : std::string{});
}

/* static */ ScriptIndustry::CargoAcceptState ScriptIndustry::IsCargoAccepted(IndustryID industry_id, CargoType cargo_type)
{
	if (!IsValidIndustry(industry_id)) return CAS_NOT_ACCEPTED;
	if (!ScriptCargo::IsValidCargo(cargo_type)) return CAS_NOT_ACCEPTED;

	/* Not const because IndustryTemporarilyRefusesCargo tests a callback which needs a non-const object. */
	Industry *i = ::Industry::Get(industry_id);

	if (!i->IsCargoAccepted(cargo_type)) return CAS_NOT_ACCEPTED;
	if (IndustryTemporarilyRefusesCargo(i, cargo_type)) return CAS_TEMP_REFUSED;

	return CAS_ACCEPTED;
}

/* static */ SQInteger ScriptIndustry::GetStockpiledCargo(IndustryID industry_id, CargoType cargo_type)
{
	if (!IsValidIndustry(industry_id)) return -1;
	if (!ScriptCargo::IsValidCargo(cargo_type)) return -1;

	const Industry *i = ::Industry::Get(industry_id);

	int j = i->GetCargoAcceptedIndex(cargo_type);
	if (j < 0) return -1;

	return i->accepted[j].waiting;
}

/* static */ SQInteger ScriptIndustry::GetLastMonthProduction(IndustryID industry_id, CargoType cargo_type)
{
	if (!IsValidIndustry(industry_id)) return -1;
	if (!ScriptCargo::IsValidCargo(cargo_type)) return -1;

	const Industry *i = ::Industry::Get(industry_id);

	int j = i->GetCargoProducedIndex(cargo_type);
	if (j < 0) return -1;

	return i->produced[j].history[LAST_MONTH].production;
}

/* static */ SQInteger ScriptIndustry::GetLastMonthTransported(IndustryID industry_id, CargoType cargo_type)
{
	if (!IsValidIndustry(industry_id)) return -1;
	if (!ScriptCargo::IsValidCargo(cargo_type)) return -1;

	const Industry *i = ::Industry::Get(industry_id);

	int j = i->GetCargoProducedIndex(cargo_type);
	if (j < 0) return -1;

	return i->produced[j].history[LAST_MONTH].transported;
}

/* static */ SQInteger ScriptIndustry::GetLastMonthTransportedPercentage(IndustryID industry_id, CargoType cargo_type)
{
	if (!IsValidIndustry(industry_id)) return -1;
	if (!ScriptCargo::IsValidCargo(cargo_type)) return -1;

	const Industry *i = ::Industry::Get(industry_id);

	int j = i->GetCargoProducedIndex(cargo_type);
	if (j < 0) return -1;

	return ::ToPercent8(i->produced[j].history[LAST_MONTH].PctTransported());
}

/* static */ TileIndex ScriptIndustry::GetLocation(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return INVALID_TILE;

	return ::Industry::Get(industry_id)->location.tile;
}

/* static */ SQInteger ScriptIndustry::GetAmountOfStationsAround(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return -1;

	const Industry *ind = ::Industry::Get(industry_id);
	return ind->stations_near.size();
}

/* static */ SQInteger ScriptIndustry::GetDistanceManhattanToTile(IndustryID industry_id, TileIndex tile)
{
	if (!IsValidIndustry(industry_id)) return -1;

	return ScriptMap::DistanceManhattan(tile, GetLocation(industry_id));
}

/* static */ SQInteger ScriptIndustry::GetDistanceSquareToTile(IndustryID industry_id, TileIndex tile)
{
	if (!IsValidIndustry(industry_id)) return -1;

	return ScriptMap::DistanceSquare(tile, GetLocation(industry_id));
}

/* static */ bool ScriptIndustry::IsBuiltOnWater(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return false;

	return ::GetIndustrySpec(::Industry::Get(industry_id)->type)->behaviour.Test(IndustryBehaviour::BuiltOnWater);
}

/* static */ bool ScriptIndustry::HasHeliport(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return false;

	return ::GetIndustrySpec(::Industry::Get(industry_id)->type)->behaviour.Test(IndustryBehaviour::AIAirShipRoutes);
}

/* static */ TileIndex ScriptIndustry::GetHeliportLocation(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return INVALID_TILE;
	if (!HasHeliport(industry_id)) return INVALID_TILE;

	const Industry *ind = ::Industry::Get(industry_id);
	for (TileIndex tile_cur : ind->location) {
		if (IsTileType(tile_cur, MP_STATION) && IsOilRig(tile_cur)) {
			return tile_cur;
		}
	}

	return INVALID_TILE;
}

/* static */ bool ScriptIndustry::HasDock(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return false;

	return ::GetIndustrySpec(::Industry::Get(industry_id)->type)->behaviour.Test(IndustryBehaviour::AIAirShipRoutes);
}

/* static */ TileIndex ScriptIndustry::GetDockLocation(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return INVALID_TILE;
	if (!HasDock(industry_id)) return INVALID_TILE;

	const Industry *ind = ::Industry::Get(industry_id);
	for (TileIndex tile_cur : ind->location) {
		if (IsTileType(tile_cur, MP_STATION) && IsOilRig(tile_cur)) {
			return tile_cur;
		}
	}

	return INVALID_TILE;
}

/* static */ IndustryType ScriptIndustry::GetIndustryType(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return IT_INVALID;

	return ::Industry::Get(industry_id)->type;
}

/* static */ SQInteger ScriptIndustry::GetLastProductionYear(IndustryID industry_id)
{
	const Industry *i = Industry::GetIfValid(industry_id);
	if (i == nullptr) return 0;
	return i->last_prod_year.base();
}

/* static */ ScriptDate::Date ScriptIndustry::GetCargoLastAcceptedDate(IndustryID industry_id, CargoType cargo_type)
{
	const Industry *i = Industry::GetIfValid(industry_id);
	if (i == nullptr) return ScriptDate::DATE_INVALID;

	if (cargo_type == INVALID_CARGO) {
		EconTime::Date date{0};
		for (const auto &a : i->Accepted()) {
			date = std::max(date, a.last_accepted);
		}
		return (ScriptDate::Date)date.base();
	} else {
		int index = i->GetCargoAcceptedIndex(cargo_type);
		if (index < 0) return ScriptDate::DATE_INVALID;
		return (ScriptDate::Date)i->accepted[index].last_accepted.base();
	}
}

/* static */ SQInteger ScriptIndustry::GetControlFlags(IndustryID industry_id)
{
	const Industry *i = Industry::GetIfValid(industry_id);
	if (i == nullptr) return 0;
	return i->ctlflags.base();
}

/* static */ bool ScriptIndustry::SetControlFlags(IndustryID industry_id, SQInteger control_flags)
{
	EnforceDeityMode(false);
	if (!IsValidIndustry(industry_id)) return false;

	return ScriptObject::Command<CMD_INDUSTRY_SET_FLAGS>::Do(industry_id, ::IndustryControlFlags(control_flags));
}

/* static */ ScriptCompany::CompanyID ScriptIndustry::GetExclusiveSupplier(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return ScriptCompany::COMPANY_INVALID;

	auto company_id = ::Industry::Get(industry_id)->exclusive_supplier;
	if (!::Company::IsValidID(company_id)) return ScriptCompany::COMPANY_INVALID;

	return ScriptCompany::ToScriptCompanyID(company_id);
}

/* static */ bool ScriptIndustry::SetExclusiveSupplier(IndustryID industry_id, ScriptCompany::CompanyID company_id)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidIndustry(industry_id));

	auto company = ScriptCompany::ResolveCompanyID(company_id);
	::Owner owner = (company == ScriptCompany::COMPANY_INVALID ? ::INVALID_OWNER : (::Owner)company);
	return ScriptObject::Command<CMD_INDUSTRY_SET_EXCLUSIVITY>::Do(industry_id, owner, false);
}

/* static */ ScriptCompany::CompanyID ScriptIndustry::GetExclusiveConsumer(IndustryID industry_id)
{
	if (!IsValidIndustry(industry_id)) return ScriptCompany::COMPANY_INVALID;

	auto company_id = ::Industry::Get(industry_id)->exclusive_consumer;
	if (!::Company::IsValidID(company_id)) return ScriptCompany::COMPANY_INVALID;

	return ScriptCompany::ToScriptCompanyID(company_id);
}

/* static */ bool ScriptIndustry::SetExclusiveConsumer(IndustryID industry_id, ScriptCompany::CompanyID company_id)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidIndustry(industry_id));

	auto company = ScriptCompany::ResolveCompanyID(company_id);
	::Owner owner = (company == ScriptCompany::COMPANY_INVALID ? ::INVALID_OWNER : (::Owner)company);
	return ScriptObject::Command<CMD_INDUSTRY_SET_EXCLUSIVITY>::Do(industry_id, owner, true);
}

/* static */ SQInteger ScriptIndustry::GetProductionLevel(IndustryID industry_id)
{
	const Industry *i = Industry::GetIfValid(industry_id);
	if (i == nullptr) return 0;
	return i->prod_level;
}

/* static */ bool ScriptIndustry::SetProductionLevel(IndustryID industry_id, SQInteger prod_level, bool show_news, Text *custom_news)
{
	ScriptObjectRef counter(custom_news);

	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidIndustry(industry_id));
	EnforcePrecondition(false, prod_level >= PRODLEVEL_MINIMUM && prod_level <= PRODLEVEL_MAXIMUM);

	return ScriptObject::Command<CMD_INDUSTRY_SET_PRODUCTION>::Do(industry_id, prod_level, show_news, custom_news != nullptr ? custom_news->GetEncodedText() : std::string{});
}
