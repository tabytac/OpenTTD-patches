/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file vehicle.cpp Base implementations of all vehicles. */

#include "stdafx.h"
#include "error.h"
#include "roadveh.h"
#include "ship.h"
#include "spritecache.h"
#include "timetable.h"
#include "viewport_func.h"
#include "news_func.h"
#include "command_func.h"
#include "company_func.h"
#include "train.h"
#include "aircraft.h"
#include "newgrf_debug.h"
#include "newgrf_sound.h"
#include "newgrf_station.h"
#include "newgrf_roadstop.h"
#include "group_gui.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "date_func.h"
#include "vehicle_func.h"
#include "autoreplace_cmd.h"
#include "autoreplace_func.h"
#include "autoreplace_gui.h"
#include "station_base.h"
#include "ai/ai.hpp"
#include "depot_func.h"
#include "network/network.h"
#include "core/pool_func.hpp"
#include "economy_base.h"
#include "articulated_vehicles.h"
#include "roadstop_base.h"
#include "core/random_func.hpp"
#include "core/backup_type.hpp"
#include "core/container_func.hpp"
#include "infrastructure_func.h"
#include "order_backup.h"
#include "sound_func.h"
#include "effectvehicle_func.h"
#include "effectvehicle_base.h"
#include "vehiclelist.h"
#include "bridge_map.h"
#include "tunnel_map.h"
#include "depot_map.h"
#include "gamelog.h"
#include "tracerestrict.h"
#include "linkgraph/linkgraph.h"
#include "linkgraph/refresh.h"
#include "framerate_type.h"
#include "blitter/factory.hpp"
#include "tbtr_template_vehicle_func.h"
#include "tbtr_template_vehicle_cmd.h"
#include "string_func.h"
#include "scope_info.h"
#include "debug_settings.h"
#include "network/network_sync.h"
#include "pathfinder/water_regions.h"
#include "event_logs.h"
#include "misc_cmd.h"
#include "train_cmd.h"
#include "vehicle_cmd.h"
#include "3rdparty/cpp-btree/btree_set.h"
#include "3rdparty/cpp-btree/btree_map.h"
#include "3rdparty/robin_hood/robin_hood.h"

#include "table/strings.h"

#include <algorithm>

#include "safeguards.h"

/* Number of bits in the hash to use from each vehicle coord */
static const uint GEN_HASHX_BITS = 6;
static const uint GEN_HASHY_BITS = 6;

/* Size of each hash bucket */
static const uint GEN_HASHX_BUCKET_BITS = 7;
static const uint GEN_HASHY_BUCKET_BITS = 6;

/* Compute hash for vehicle coord */
#define GEN_HASHX(x)    GB((x), GEN_HASHX_BUCKET_BITS + ZOOM_BASE_SHIFT, GEN_HASHX_BITS)
#define GEN_HASHY(y)   (GB((y), GEN_HASHY_BUCKET_BITS + ZOOM_BASE_SHIFT, GEN_HASHY_BITS) << GEN_HASHX_BITS)
#define GEN_HASH(x, y) (GEN_HASHY(y) + GEN_HASHX(x))

/* Maximum size until hash repeats */
//static const int GEN_HASHX_SIZE = 1 << (GEN_HASHX_BUCKET_BITS + GEN_HASHX_BITS + ZOOM_BASE_SHIFT);
//static const int GEN_HASHY_SIZE = 1 << (GEN_HASHY_BUCKET_BITS + GEN_HASHY_BITS + ZOOM_BASE_SHIFT);

/* Increments to reach next bucket in hash table */
//static const int GEN_HASHX_INC = 1;
//static const int GEN_HASHY_INC = 1 << GEN_HASHX_BITS;

/* Mask to wrap-around buckets */
//static const uint GEN_HASHX_MASK =  (1 << GEN_HASHX_BITS) - 1;
//static const uint GEN_HASHY_MASK = ((1 << GEN_HASHY_BITS) - 1) << GEN_HASHX_BITS;

uint _returned_refit_capacity;        ///< Stores the capacity after a refit operation.
uint16_t _returned_mail_refit_capacity; ///< Stores the mail capacity after a refit operation (Aircraft only).
CargoArray _returned_vehicle_capacities; ///< Stores the cargo capacities after a vehicle build operation


/** The pool with all our precious vehicles. */
VehiclePool _vehicle_pool("Vehicle");
INSTANTIATE_POOL_METHODS(Vehicle)

static btree::btree_set<VehicleID> _vehicles_to_pay_repair;
static btree::btree_set<VehicleID> _vehicles_to_sell;

btree::btree_multimap<VehicleID, PendingSpeedRestrictionChange> _pending_speed_restriction_change_map;

/**
 * Determine shared bounds of all sprites.
 * @param[out] bounds Shared bounds.
 */
Rect16 VehicleSpriteSeq::GetBounds() const
{
	Rect16 bounds;
	bounds.left = bounds.top = bounds.right = bounds.bottom = 0;
	for (uint i = 0; i < this->count; ++i) {
		const Sprite *spr = GetSprite(this->seq[i].sprite, SpriteType::Normal, 0);
		if (i == 0) {
			bounds.left = spr->x_offs;
			bounds.top  = spr->y_offs;
			bounds.right  = spr->width  + spr->x_offs - 1;
			bounds.bottom = spr->height + spr->y_offs - 1;
		} else {
			if (spr->x_offs < bounds.left) bounds.left = spr->x_offs;
			if (spr->y_offs < bounds.top)  bounds.top  = spr->y_offs;
			int right  = spr->width  + spr->x_offs - 1;
			int bottom = spr->height + spr->y_offs - 1;
			if (right  > bounds.right)  bounds.right  = right;
			if (bottom > bounds.bottom) bounds.bottom = bottom;
		}
	}
	return bounds;
}

/**
 * Draw the sprite sequence.
 * @param x X position
 * @param y Y position
 * @param default_pal Vehicle palette
 * @param force_pal Whether to ignore individual palettes, and draw everything with \a default_pal.
 */
void VehicleSpriteSeq::Draw(int x, int y, PaletteID default_pal, bool force_pal) const
{
	for (uint i = 0; i < this->count; ++i) {
		PaletteID pal = force_pal || !this->seq[i].pal ? default_pal : this->seq[i].pal;
		DrawSprite(this->seq[i].sprite, pal, x, y);
	}
}

/**
 * Function to tell if a vehicle needs to be autorenewed
 * @param *c The vehicle owner
 * @param use_renew_setting Should the company renew setting be considered?
 * @return true if the vehicle is old enough for replacement
 */
bool Vehicle::NeedsAutorenewing(const Company *c, bool use_renew_setting) const
{
	/* We can always generate the Company pointer when we have the vehicle.
	 * However this takes time and since the Company pointer is often present
	 * when this function is called then it's faster to pass the pointer as an
	 * argument rather than finding it again. */
	dbg_assert(c == Company::Get(this->owner));

	if (use_renew_setting && !c->settings.engine_renew) return false;
	if (this->age - this->max_age < (c->settings.engine_renew_months * 30)) return false;

	/* Only engines need renewing */
	if (this->type == VEH_TRAIN && !Train::From(this)->IsEngine()) return false;

	return true;
}

/**
 * Service a vehicle and all subsequent vehicles in the consist
 *
 * @param *v The vehicle or vehicle chain being serviced
 */
void VehicleServiceInDepot(Vehicle *v)
{
	dbg_assert(v != nullptr);
	const Engine *e = Engine::Get(v->engine_type);
	if (v->type == VEH_TRAIN) {
		if (v->Next() != nullptr) VehicleServiceInDepot(v->Next());
		if (!(Train::From(v)->IsEngine()) && !(Train::From(v)->IsRearDualheaded())) return;
		ClrBit(Train::From(v)->flags, VRF_NEED_REPAIR);
		ClrBit(Train::From(v)->flags, VRF_HAS_HIT_RV);
		ClrBit(Train::From(v)->flags, VRF_CONSIST_BREAKDOWN);
		Train::From(v)->critical_breakdown_count = 0;
		const RailVehicleInfo *rvi = &e->u.rail;
		v->vcache.cached_max_speed = rvi->max_speed;
		if (Train::From(v)->IsFrontEngine()) {
			Train::From(v)->ConsistChanged(CCF_REFIT);
			CLRBITS(Train::From(v)->flags, (1 << VRF_BREAKDOWN_BRAKING) | VRF_IS_BROKEN );
		}
	} else if (v->type == VEH_ROAD) {
		RoadVehicle::From(v)->critical_breakdown_count = 0;
	} else if (v->type == VEH_SHIP) {
		Ship::From(v)->critical_breakdown_count = 0;
	}
	v->vehstatus &= ~VS_AIRCRAFT_BROKEN;
	ClrBit(v->vehicle_flags, VF_REPLACEMENT_PENDING);
	SetWindowDirty(WC_VEHICLE_DETAILS, v->index); // ensure that last service date and reliability are updated

	do {
		v->date_of_last_service = EconTime::CurDate();
		v->date_of_last_service_newgrf = CalTime::CurDate();
		if (_settings_game.vehicle.pay_for_repair && v->breakdowns_since_last_service) {
			_vehicles_to_pay_repair.insert(v->index);
		} else {
			v->breakdowns_since_last_service = 0;
		}
		v->reliability = v->GetEngine()->reliability;
		/* Prevent vehicles from breaking down directly after exiting the depot. */
		v->breakdown_chance = 0;
		v->breakdown_ctr = 0;
		v = v->Next();
	} while (v != nullptr && v->HasEngineType());
}

/**
 * Check if the vehicle needs to go to a depot in near future (if a opportunity presents itself) for service or replacement.
 *
 * @see NeedsAutomaticServicing()
 * @return true if the vehicle should go to a depot if a opportunity presents itself.
 */
bool Vehicle::NeedsServicing() const
{
	/* Stopped or crashed vehicles will not move, as such making unmovable
	 * vehicles to go for service is lame. */
	if (this->vehstatus & (VS_STOPPED | VS_CRASHED)) return false;

	bool service_not_due;
	/* Service intervals can be measured in different units, which we handle individually. */
	if (this->ServiceIntervalIsPercent()) {
		/* Service interval is in percents. */
		service_not_due = (this->reliability >= this->GetEngine()->reliability * (100 - this->GetServiceInterval()) / 100);
	} else if (EconTime::UsingWallclockUnits()) {
		/* Service interval is in minutes. */
		service_not_due = (this->date_of_last_service + (this->GetServiceInterval() * EconTime::DAYS_IN_ECONOMY_WALLCLOCK_MONTH) >= EconTime::CurDate());
	} else {
		/* Service interval is in days. */
		service_not_due = (this->date_of_last_service + this->GetServiceInterval() >= EconTime::CurDate());
	}

	/* Are we ready for the next service cycle? */
	bool needs_service = true;
	const Company *c = Company::Get(this->owner);
	if (service_not_due
			&& !(this->type == VEH_TRAIN && HasBit(Train::From(this)->flags, VRF_CONSIST_BREAKDOWN) && Train::From(this)->ConsistNeedsRepair())
			&& !(this->type == VEH_ROAD && RoadVehicle::From(this)->critical_breakdown_count > 0)
			&& !(this->type == VEH_SHIP && Ship::From(this)->critical_breakdown_count > 0)) {
		needs_service = false;
	}

	if (!needs_service && !HasBit(this->vehicle_flags, VF_REPLACEMENT_PENDING)) {
		return false;
	}

	/* If we're servicing anyway, because we have not disabled servicing when
	 * there are no breakdowns or we are playing with breakdowns, bail out. */
	if (needs_service && (!_settings_game.order.no_servicing_if_no_breakdowns ||
			_settings_game.difficulty.vehicle_breakdowns != 0)) {
		return true;
	}

	/* Is vehicle old and renewing is enabled */
	if (needs_service && this->NeedsAutorenewing(c, true)) {
		return true;
	}

	if (this->type == VEH_TRAIN) {
		const TemplateVehicle *tv = GetTemplateVehicleByGroupIDRecursive(this->group_id);
		if (tv != nullptr) {
			return ShouldServiceTrainForTemplateReplacement(Train::From(this), tv);
		}
	}

	/* Test whether there is some pending autoreplace.
	 * Note: We do this after the service-interval test.
	 * There are a lot more reasons for autoreplace to fail than we can test here reasonably. */
	bool pending_replace = false;
	Money needed_money = c->settings.engine_renew_money;
	if (needed_money > GetAvailableMoney(c->index)) return false;

	for (const Vehicle *v = this; v != nullptr; v = (v->type == VEH_TRAIN) ? Train::From(v)->GetNextUnit() : nullptr) {
		bool replace_when_old = false;
		EngineID new_engine = EngineReplacementForCompany(c, v->engine_type, v->group_id, &replace_when_old);

		/* Check engine availability */
		if (new_engine == INVALID_ENGINE || !Engine::Get(new_engine)->company_avail.Test(v->owner)) continue;
		/* Is the vehicle old if we are not always replacing? */
		if (replace_when_old && !v->NeedsAutorenewing(c, false)) continue;

		/* Check refittability */
		CargoTypes available_cargo_types, union_mask;
		GetArticulatedRefitMasks(new_engine, true, &union_mask, &available_cargo_types);

		/* Is this a multi-cargo ship? */
		if (union_mask != 0 && v->type == VEH_SHIP && v->Next() != nullptr) {
			CargoTypes cargoes = 0;
			for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
				if (u->cargo_type != INVALID_CARGO && u->GetEngine()->CanCarryCargo()) {
					SetBit(cargoes, u->cargo_type);
				}
			}
			if (!HasAtMostOneBit(cargoes)) {
				/* Ship has more than one cargo, special handling */
				if (!AutoreplaceMultiPartShipWouldSucceed(new_engine, v, cargoes)) continue;
				union_mask = 0;
			}
		}

		/* Is there anything to refit? */
		if (union_mask != 0) {
			CargoType cargo_type;
			CargoTypes cargo_mask = GetCargoTypesOfArticulatedVehicle(v, &cargo_type);
			if (!HasAtMostOneBit(cargo_mask)) {
				CargoTypes new_engine_default_cargoes = GetCargoTypesOfArticulatedParts(new_engine);
				if ((cargo_mask & new_engine_default_cargoes) != cargo_mask) {
					/* We cannot refit to mixed cargoes in an automated way */
					continue;
				}
				/* engine_type is already a mixed cargo type which matches the incoming vehicle by default, no refit required */
			} else {
				/* Did the old vehicle carry anything? */
				if (cargo_type != INVALID_CARGO) {
					/* We can't refit the vehicle to carry the cargo we want */
					if (!HasBit(available_cargo_types, cargo_type)) continue;
				}
			}
		}

		/* Check money.
		 * We want 2*(the price of the new vehicle) without looking at the value of the vehicle we are going to sell. */
		pending_replace = true;
		needed_money += 2 * Engine::Get(new_engine)->GetCost();
		if (needed_money > GetAvailableMoney(c->index)) return false;
	}

	return pending_replace;
}

/**
 * Checks if the current order should be interrupted for a service-in-depot order.
 * @see NeedsServicing()
 * @return true if the current order should be interrupted.
 */
bool Vehicle::NeedsAutomaticServicing() const
{
	if (this->HasDepotOrder()) return false;
	if (this->current_order.IsType(OT_LOADING)) return false;
	if (this->current_order.IsType(OT_LOADING_ADVANCE)) return false;
	if (this->current_order.IsType(OT_GOTO_DEPOT) && (this->current_order.GetDepotOrderType() & ODTFB_SERVICE) == 0) return false;
	return NeedsServicing();
}

uint Vehicle::Crash(bool)
{
	assert((this->vehstatus & VS_CRASHED) == 0);
	assert(this->Previous() == nullptr); // IsPrimaryVehicle fails for free-wagon-chains

	uint pass = 0;
	/* Stop the vehicle. */
	if (this->IsPrimaryVehicle()) this->vehstatus |= VS_STOPPED;
	/* crash all wagons, and count passengers */
	for (Vehicle *v = this; v != nullptr; v = v->Next()) {
		/* We do not transfer reserver cargo back, so TotalCount() instead of StoredCount() */
		if (IsCargoInClass(v->cargo_type, CC_PASSENGERS)) pass += v->cargo.TotalCount();
		v->vehstatus |= VS_CRASHED;
		v->MarkAllViewportsDirty();
		v->InvalidateImageCache();
	}

	this->ClearSeparation();
	if (HasBit(this->vehicle_flags, VF_TIMETABLE_SEPARATION)) ClrBit(this->vehicle_flags, VF_TIMETABLE_STARTED);

	/* Dirty some windows */
	InvalidateWindowClassesData(GetWindowClassForVehicleType(this->type), 0);
	SetWindowWidgetDirty(WC_VEHICLE_VIEW, this->index, WID_VV_START_STOP);
	SetWindowDirty(WC_VEHICLE_DETAILS, this->index);
	SetWindowDirty(WC_VEHICLE_DEPOT, this->tile.base());
	InvalidateWindowClassesData(WC_DEPARTURES_BOARD, 0);

	delete this->cargo_payment;
	assert(this->cargo_payment == nullptr); // cleared by ~CargoPayment

	return RandomRange(pass + 1); // Randomise deceased passengers.
}

/**
 * Update cache of whether the vehicle should be drawn (i.e. if it isn't hidden, or it is in a tunnel but being shown transparently)
 * @return whether to show vehicle
 */
void Vehicle::UpdateIsDrawn()
{
	bool drawn = !(HasBit(this->subtype, GVSF_VIRTUAL)) && (!(this->vehstatus & VS_HIDDEN) ||
			(IsTransparencySet(TO_TUNNELS) &&
				((this->type == VEH_TRAIN && Train::From(this)->track == TRACK_BIT_WORMHOLE) ||
				(this->type == VEH_ROAD && RoadVehicle::From(this)->state == RVSB_WORMHOLE))));

	AssignBit(this->vcache.cached_veh_flags, VCF_IS_DRAWN, drawn);
}

void UpdateAllVehiclesIsDrawn()
{
	for (Vehicle *v : Vehicle::Iterate()) { v->UpdateIsDrawn(); }
}

/**
 * Displays a "NewGrf Bug" error message for a engine, and pauses the game if not networking.
 * @param engine The engine that caused the problem
 * @param part1  Part 1 of the error message, taking the grfname as parameter 1
 * @param part2  Part 2 of the error message, taking the engine as parameter 2
 * @param bug_type Flag to check and set in grfconfig
 * @param critical Shall the "OpenTTD might crash"-message be shown when the player tries to unpause?
 */
void ShowNewGrfVehicleError(EngineID engine, StringID part1, StringID part2, GRFBug bug_type, bool critical)
{
	const Engine *e = Engine::Get(engine);
	GRFConfig *grfconfig = GetGRFConfig(e->GetGRFID());

	/* Missing GRF. Nothing useful can be done in this situation. */
	if (grfconfig == nullptr) return;

	if (!grfconfig->grf_bugs.Test(bug_type)) {
		grfconfig->grf_bugs.Set(bug_type);
		SetDParamStr(0, grfconfig->GetName());
		SetDParam(1, engine);
		ShowErrorMessage(part1, part2, WL_CRITICAL);
		if (!_networking) Command<CMD_PAUSE>::Do(DC_EXEC, critical ? PM_PAUSED_ERROR : PM_PAUSED_NORMAL, true);
	}

	std::string log_msg;
	auto log = [&](StringID str) {
		std::string msg = GetString(str);
		const char *start = strip_leading_colours(msg);
		Debug(grf, 0, "{}", start);
		log_msg += start;
	};

	SetDParamStr(0, grfconfig->GetName());
	log(part1);

	log_msg += ", ";

	SetDParam(1, engine);
	log(part2);

	AppendSpecialEventsLogEntry(std::move(log_msg));
}

/**
 * Logs a bug in GRF and shows a warning message if this
 * is for the first time this happened.
 * @param u first vehicle of chain
 */
void VehicleLengthChanged(const Vehicle *u)
{
	/* show a warning once for each engine in whole game and once for each GRF after each game load */
	const Engine *engine = u->GetEngine();
	if (engine->grf_prop.grffile == nullptr) {
		// This can be reached if an engine is unexpectedly no longer attached to a GRF at all
		if (GamelogGRFBugReverse(0, engine->grf_prop.local_id)) {
			ShowNewGrfVehicleError(u->engine_type, STR_NEWGRF_BROKEN, STR_NEWGRF_BROKEN_VEHICLE_LENGTH, GRFBug::VehLength, true);
		}
		return;
	}
	uint32_t grfid = engine->grf_prop.grfid;
	GRFConfig *grfconfig = GetGRFConfig(grfid);
	if (GamelogGRFBugReverse(grfid, engine->grf_prop.local_id) || !grfconfig->grf_bugs.Test(GRFBug::VehLength)) {
		ShowNewGrfVehicleError(u->engine_type, STR_NEWGRF_BROKEN, STR_NEWGRF_BROKEN_VEHICLE_LENGTH, GRFBug::VehLength, true);
	}
}

/**
 * Vehicle constructor.
 * @param type Type of the new vehicle.
 */
Vehicle::Vehicle(VehicleType type)
{
	this->type               = type;
	this->coord.left         = INVALID_COORD;
	this->group_id           = DEFAULT_GROUP;
	this->fill_percent_te_id = INVALID_TE_ID;
	this->first              = this;
	this->colourmap          = PAL_NONE;
	this->cargo_age_counter  = 1;
	this->last_station_visited = INVALID_STATION;
	this->last_loading_station = INVALID_STATION;
	this->last_loading_tick = StateTicks{0};
	this->cur_image_valid_dir  = INVALID_DIR;
	this->vcache.cached_veh_flags = 0;
}

using VehicleTypeTileHash = robin_hood::unordered_map<TileIndex, VehicleID>;
static std::array<VehicleTypeTileHash, 4> _vehicle_tile_hashes;

static Vehicle *VehicleFromTileHash(int xl, int yl, int xu, int yu, VehicleType type, void *data, VehicleFromPosProc *proc, bool find_first)
{
	VehicleTypeTileHash &vhash = _vehicle_tile_hashes[type];

	for (int y = yl; ; y++) {
		for (int x = xl; ; x++) {
			auto iter = vhash.find(TileXY(x, y));
			if (iter != vhash.end()) {
				Vehicle *v = Vehicle::Get(iter->second);
				do {
					Vehicle *a = proc(v, data);
					if (find_first && a != nullptr) return a;

					v = v->hash_tile_next;
				} while (v != nullptr);
			}
			if (x == xu) break;
		}
		if (y == yu) break;
	}

	return nullptr;
}


/**
 * Helper function for FindVehicleOnPos/HasVehicleOnPos.
 * @note Do not call this function directly!
 * @param x    The X location on the map
 * @param y    The Y location on the map
 * @param data Arbitrary data passed to proc
 * @param proc The proc that determines whether a vehicle will be "found".
 * @param find_first Whether to return on the first found or iterate over
 *                   all vehicles
 * @return the best matching or first vehicle (depending on find_first).
 */
Vehicle *VehicleFromPosXY(int x, int y, VehicleType type, void *data, VehicleFromPosProc *proc, bool find_first)
{
	const int COLL_DIST = 6;

	/* Hash area to scan is from xl,yl to xu,yu */
	int xl = (x - COLL_DIST) / TILE_SIZE;
	int xu = (x + COLL_DIST) / TILE_SIZE;
	int yl = (y - COLL_DIST) / TILE_SIZE;
	int yu = (y + COLL_DIST) / TILE_SIZE;

	return VehicleFromTileHash(xl, yl, xu, yu, type, data, proc, find_first);
}

/**
 * Helper function for FindVehicleOnPos/HasVehicleOnPos.
 * @note Do not call this function directly!
 * @param tile The location on the map
 * @param type The vehicle type
 * @param data Arbitrary data passed to \a proc.
 * @param proc The proc that determines whether a vehicle will be "found".
 * @param find_first Whether to return on the first found or iterate over
 *                   all vehicles
 * @return the best matching or first vehicle (depending on find_first).
 */
Vehicle *VehicleFromPos(TileIndex tile, VehicleType type, void *data, VehicleFromPosProc *proc, bool find_first)
{
	VehicleTypeTileHash &vhash = _vehicle_tile_hashes[type];

	auto iter = vhash.find(tile);
	if (iter != vhash.end()) {
		Vehicle *v = Vehicle::Get(iter->second);
		do {
			Vehicle *a = proc(v, data);
			if (find_first && a != nullptr) return a;

			v = v->hash_tile_next;
		} while (v != nullptr);
	}

	return nullptr;
}

/**
 * Returns the first vehicle on a specific location, this should be iterated using Vehicle::HashTileNext.
 * @note Use #GetFirstVehicleOnPos when you have the intention that all vehicles should be iterated over using Vehicle::HashTileNext. The iteration order is non-deterministic.
 * @param tile The location on the map
 * @param type The vehicle type
 * @return First vehicle or nullptr.
 */
Vehicle *GetFirstVehicleOnPos(TileIndex tile, VehicleType type)
{
	VehicleTypeTileHash &vhash = _vehicle_tile_hashes[type];

	auto iter = vhash.find(tile);
	if (iter != vhash.end()) {
		return Vehicle::Get(iter->second);
	} else {
		return nullptr;
	}
}

/**
 * Callback that returns 'real' vehicles lower or at height \c *(int*)data .
 * @param v Vehicle to examine.
 * @param data unused.
 * @return \a v if conditions are met, else \c nullptr.
 */
static Vehicle *EnsureNoVehicleProc(Vehicle *v, void *data)
{
	return v;
}

/**
 * Callback that returns 'real' train-collidable road vehicles lower or at height \c *(int*)data .
 * @param v Vehicle to examine.
 * @param data unused
 * @return \a v if conditions are met, else \c nullptr.
 */
static Vehicle *EnsureNoTrainCollidableRoadVehicleProc(Vehicle *v, void *data)
{
	if (HasBit(_roadtypes_non_train_colliding, RoadVehicle::From(v)->roadtype)) return nullptr;

	return v;
}

/**
 * Callback that returns 'real' vehicles lower or at height \c *(int*)data .
 * @param v Vehicle to examine.
 * @param data Pointer to height data.
 * @return \a v if conditions are met, else \c nullptr.
 */
static Vehicle *EnsureNoAircraftProcZ(Vehicle *v, void *data)
{
	int z = static_cast<int>(reinterpret_cast<intptr_t>(data));

	if (v->subtype == AIR_SHADOW) return nullptr;
	if (v->z_pos > z) return nullptr;

	return v;
}

/**
 * Ensure there is no vehicle at the ground at the given position.
 * @param tile Position to examine.
 * @return Succeeded command (ground is free) or failed command (a vehicle is found).
 */
CommandCost EnsureNoVehicleOnGround(TileIndex tile)
{
	if (IsAirportTile(tile)) {
		int z = GetTileMaxPixelZ(tile);
		if (VehicleFromPos(tile, VEH_AIRCRAFT, reinterpret_cast<void *>(static_cast<intptr_t>(z)), &EnsureNoAircraftProcZ, true) != nullptr) {
			return CommandCost(STR_ERROR_AIRCRAFT_IN_THE_WAY);
		}
		return CommandCost();
	}

	if (IsTileType(tile, MP_RAILWAY) || IsLevelCrossingTile(tile) || HasStationTileRail(tile) || IsRailTunnelBridgeTile(tile)) {
		if (VehicleFromPos(tile, VEH_TRAIN, nullptr, &EnsureNoVehicleProc, true) != nullptr) {
			return CommandCost(STR_ERROR_TRAIN_IN_THE_WAY);
		}
	}
	if (IsTileType(tile, MP_ROAD) || IsAnyRoadStopTile(tile) || (IsTileType(tile, MP_TUNNELBRIDGE) && GetTunnelBridgeTransportType(tile) == TRANSPORT_ROAD)) {
		if (VehicleFromPos(tile, VEH_ROAD, nullptr, &EnsureNoVehicleProc, true) != nullptr) {
			return CommandCost(STR_ERROR_ROAD_VEHICLE_IN_THE_WAY);
		}
	}
	if (HasTileWaterClass(tile) || (IsBridgeTile(tile) && GetTunnelBridgeTransportType(tile) == TRANSPORT_WATER)) {
		if (VehicleFromPos(tile, VEH_SHIP, nullptr, &EnsureNoVehicleProc, true) != nullptr) {
			return CommandCost(STR_ERROR_SHIP_IN_THE_WAY);
		}
	}

	return CommandCost();
}

bool IsTrainCollidableRoadVehicleOnGround(TileIndex tile)
{
	return VehicleFromPos(tile, VEH_ROAD, nullptr, &EnsureNoTrainCollidableRoadVehicleProc, true) != nullptr;
}

struct GetVehicleTunnelBridgeProcData {
	const Vehicle *v;
	TileIndex t;
	TunnelBridgeIsFreeMode mode;
};

/** Procedure called for every vehicle found in tunnel/bridge in the hash map */
static Vehicle *GetVehicleTunnelBridgeProc(Vehicle *v, void *data)
{
	const GetVehicleTunnelBridgeProcData *info = (GetVehicleTunnelBridgeProcData*) data;
	if (v == info->v) return nullptr;

	if (v->type == VEH_TRAIN && info->mode != TBIFM_ALL && IsBridge(info->t)) {
		TrackBits vehicle_track = Train::From(v)->track;
		if (!(vehicle_track & TRACK_BIT_WORMHOLE)) {
			if (info->mode == TBIFM_ACROSS_ONLY && !(GetAcrossBridgePossibleTrackBits(info->t) & vehicle_track)) return nullptr;
			if (info->mode == TBIFM_PRIMARY_ONLY && !(GetPrimaryTunnelBridgeTrackBits(info->t) & vehicle_track)) return nullptr;
		}
	}

	return v;
}

/**
 * Finds vehicle in tunnel / bridge
 * @param tile first end
 * @param endtile second end
 * @param ignore Ignore this vehicle when searching
 * @param mode Whether to only find vehicles which are passing across the bridge/tunnel or on connecting bridge head track pieces, or only on primary track type pieces
 * @return Succeeded command (if tunnel/bridge is free) or failed command (if a vehicle is using the tunnel/bridge).
 */
CommandCost TunnelBridgeIsFree(TileIndex tile, TileIndex endtile, const Vehicle *ignore, TunnelBridgeIsFreeMode mode)
{
	/* Value v is not safe in MP games, however, it is used to generate a local
	 * error message only (which may be different for different machines).
	 * Such a message does not affect MP synchronisation.
	 */
	GetVehicleTunnelBridgeProcData data;
	data.v = ignore;
	data.t = tile;
	data.mode = mode;
	VehicleType type = static_cast<VehicleType>(GetTunnelBridgeTransportType(tile));
	Vehicle *v = VehicleFromPos(tile, type, &data, &GetVehicleTunnelBridgeProc, true);
	if (v == nullptr) {
		data.t = endtile;
		v = VehicleFromPos(endtile, type, &data, &GetVehicleTunnelBridgeProc, true);
	}

	if (v != nullptr) return CommandCost(STR_ERROR_TRAIN_IN_THE_WAY + v->type);
	return CommandCost();
}

struct FindTrainClosestToTunnelBridgeEndInfo {
	Train *best;     ///< The currently "best" vehicle we have found.
	int32_t best_pos;
	DiagDirection direction;

	FindTrainClosestToTunnelBridgeEndInfo(DiagDirection direction) : best(nullptr), best_pos(INT32_MIN), direction(direction) {}
};

/** Callback for Has/FindVehicleOnPos to find a train in a signalled tunnel/bridge */
static Vehicle *FindClosestTrainToTunnelBridgeEndEnum(Vehicle *v, void *data)
{
	FindTrainClosestToTunnelBridgeEndInfo *info = (FindTrainClosestToTunnelBridgeEndInfo *)data;

	/* Only look for train heads and tails. */
	if (v->Previous() != nullptr && v->Next() != nullptr) return nullptr;

	if ((v->vehstatus & VS_CRASHED)) return nullptr;

	Train *t = Train::From(v);

	if (!IsDiagonalDirection(t->direction)) {
		/* Check for vehicles on non-across track pieces of custom bridge head */
		if ((GetAcrossTunnelBridgeTrackBits(t->tile) & t->track & TRACK_BIT_ALL) == TRACK_BIT_NONE) return nullptr;
	}

	int32_t pos;
	switch (info->direction) {
		default: NOT_REACHED();
		case DIAGDIR_NE: pos = -v->x_pos; break; // X: lower is better
		case DIAGDIR_SE: pos =  v->y_pos; break; // Y: higher is better
		case DIAGDIR_SW: pos =  v->x_pos; break; // X: higher is better
		case DIAGDIR_NW: pos = -v->y_pos; break; // Y: lower is better
	}

	/* ALWAYS return the lowest ID (anti-desync!) if the coordinate is the same */
	if (pos > info->best_pos || (pos == info->best_pos && t->First()->index < info->best->index)) {
		info->best = t->First();
		info->best_pos = pos;
	}

	return t;
}

Train *GetTrainClosestToTunnelBridgeEnd(TileIndex tile, TileIndex other_tile)
{
	FindTrainClosestToTunnelBridgeEndInfo info(ReverseDiagDir(GetTunnelBridgeDirection(tile)));
	FindVehicleOnPos(tile, VEH_TRAIN, &info, FindClosestTrainToTunnelBridgeEndEnum);
	FindVehicleOnPos(other_tile, VEH_TRAIN, &info, FindClosestTrainToTunnelBridgeEndEnum);
	return info.best;
}


struct GetAvailableFreeTilesInSignalledTunnelBridgeChecker {
	DiagDirection direction;
	int pos;
	int lowest_seen;
};

static Vehicle *GetAvailableFreeTilesInSignalledTunnelBridgeEnum(Vehicle *v, void *data)
{
	/* Don't look at wagons between front and back of train. */
	if ((v->Previous() != nullptr && v->Next() != nullptr)) return nullptr;

	if (!IsDiagonalDirection(v->direction)) {
		/* Check for vehicles on non-across track pieces of custom bridge head */
		if ((GetAcrossTunnelBridgeTrackBits(v->tile) & Train::From(v)->track & TRACK_BIT_ALL) == TRACK_BIT_NONE) return nullptr;
	}

	GetAvailableFreeTilesInSignalledTunnelBridgeChecker *checker = (GetAvailableFreeTilesInSignalledTunnelBridgeChecker*) data;
	int v_pos;

	switch (checker->direction) {
		default: NOT_REACHED();
		case DIAGDIR_NE: v_pos = -v->x_pos + TILE_UNIT_MASK; break;
		case DIAGDIR_SE: v_pos =  v->y_pos; break;
		case DIAGDIR_SW: v_pos =  v->x_pos; break;
		case DIAGDIR_NW: v_pos = -v->y_pos + TILE_UNIT_MASK; break;
	}
	if (v_pos > checker->pos && v_pos < checker->lowest_seen) {
		checker->lowest_seen = v_pos;
	}

	return nullptr;
}

int GetAvailableFreeTilesInSignalledTunnelBridgeWithStartOffset(TileIndex entrance, TileIndex exit, int offset)
{
	if (offset < 0) offset = 0;
	TileIndex tile = entrance;
	if (offset > 0) tile += offset * TileOffsByDiagDir(GetTunnelBridgeDirection(entrance));
	int free_tiles = GetAvailableFreeTilesInSignalledTunnelBridge(entrance, exit, tile);
	if (free_tiles != INT_MAX && offset > 0) free_tiles += offset;
	return free_tiles;
}

int GetAvailableFreeTilesInSignalledTunnelBridge(TileIndex entrance, TileIndex exit, TileIndex tile)
{
	GetAvailableFreeTilesInSignalledTunnelBridgeChecker checker;
	checker.direction = GetTunnelBridgeDirection(entrance);
	checker.lowest_seen = INT_MAX;
	switch (checker.direction) {
		default: NOT_REACHED();
		case DIAGDIR_NE: checker.pos = -(int)(TileX(tile) * TILE_SIZE); break;
		case DIAGDIR_SE: checker.pos =       (TileY(tile) * TILE_SIZE); break;
		case DIAGDIR_SW: checker.pos =       (TileX(tile) * TILE_SIZE); break;
		case DIAGDIR_NW: checker.pos = -(int)(TileY(tile) * TILE_SIZE); break;
	}

	FindVehicleOnPos(entrance, VEH_TRAIN, &checker, &GetAvailableFreeTilesInSignalledTunnelBridgeEnum);
	FindVehicleOnPos(exit, VEH_TRAIN, &checker, &GetAvailableFreeTilesInSignalledTunnelBridgeEnum);

	if (checker.lowest_seen == INT_MAX) {
		/* Remainder of bridge/tunnel is clear */
		return INT_MAX;
	}

	return (checker.lowest_seen - checker.pos) / TILE_SIZE;
}

static Vehicle *EnsureNoTrainOnTrackProc(Vehicle *v, void *data)
{
	TrackBits rail_bits = *(TrackBits *)data;

	Train *t = Train::From(v);
	if (rail_bits & TRACK_BIT_WORMHOLE) {
		if (t->track & TRACK_BIT_WORMHOLE) return v;
		rail_bits &= ~TRACK_BIT_WORMHOLE;
	} else if (t->track & TRACK_BIT_WORMHOLE) {
		return nullptr;
	}
	if ((t->track != rail_bits) && !TracksOverlap(t->track | rail_bits)) return nullptr;

	return v;
}

/**
 * Tests if a vehicle interacts with the specified track bits.
 * All track bits interact except parallel #TRACK_BIT_HORZ or #TRACK_BIT_VERT.
 *
 * @param tile The tile.
 * @param track_bits The track bits.
 * @return \c true if no train that interacts, is found. \c false if a train is found.
 */
CommandCost EnsureNoTrainOnTrackBits(TileIndex tile, TrackBits track_bits)
{
	/* Value v is not safe in MP games, however, it is used to generate a local
	 * error message only (which may be different for different machines).
	 * Such a message does not affect MP synchronisation.
	 */
	Vehicle *v = VehicleFromPos(tile, VEH_TRAIN, &track_bits, &EnsureNoTrainOnTrackProc, true);
	if (v != nullptr) return CommandCost(STR_ERROR_TRAIN_IN_THE_WAY + v->type);
	return CommandCost();
}

void UpdateVehicleTileHash(Vehicle *v, bool remove)
{
	TileIndex old_hash_tile = v->hash_tile_current;
	TileIndex new_hash_tile;

	if (remove || HasBit(v->subtype, GVSF_VIRTUAL) || (v->tile == 0 && _settings_game.construction.freeform_edges)) {
		new_hash_tile = INVALID_TILE;
	} else {
		new_hash_tile = v->tile;
	}

	if (old_hash_tile == new_hash_tile) return;

	VehicleTypeTileHash &vhash = _vehicle_tile_hashes[v->type];

	/* Remove from the old position in the hash table */
	if (old_hash_tile != INVALID_TILE) {
		if (v->hash_tile_next != nullptr) v->hash_tile_next->hash_tile_prev = v->hash_tile_prev;
		if (v->hash_tile_prev != nullptr) {
			v->hash_tile_prev->hash_tile_next = v->hash_tile_next;
		} else {
			/* This was the first vehicle in the chain */
			if (v->hash_tile_next != nullptr) {
				vhash[old_hash_tile] = v->hash_tile_next->index;
			} else {
				vhash.erase(old_hash_tile);
			}
		}
	}

	/* Insert vehicle at beginning of the new position in the hash table */
	if (new_hash_tile != INVALID_TILE) {
		auto res = vhash.insert({ new_hash_tile, v->index });
		if (res.second) {
			/* Insert took place */
			v->hash_tile_next = nullptr;
			v->hash_tile_prev = nullptr;
		} else {
			/* Key already existed */
			Vehicle *next = Vehicle::Get(res.first->second);
			next->hash_tile_prev = v;
			v->hash_tile_next = next;
			v->hash_tile_prev = nullptr;
			res.first->second = v->index;
		}
	}

	/* Remember current hash tile */
	v->hash_tile_current = new_hash_tile;
}

bool ValidateVehicleTileHash(const Vehicle *v)
{
	if ((v->type == VEH_TRAIN && Train::From(v)->IsVirtual())
			|| (v->type == VEH_SHIP && HasBit(v->subtype, GVSF_VIRTUAL))
			|| (v->type == VEH_AIRCRAFT && v->tile == 0 && _settings_game.construction.freeform_edges)
			|| v->type >= VEH_COMPANY_END) {
		return v->hash_tile_current == INVALID_TILE;
	}

	if (v->hash_tile_current != v->tile) return false;

	auto iter = _vehicle_tile_hashes[v->type].find(v->hash_tile_current);
	if (iter == _vehicle_tile_hashes[v->type].end()) return false;

	for (const Vehicle *u = Vehicle::GetIfValid(iter->second); u != nullptr; u = u->hash_tile_next) {
		if (u == v) return true;
	}

	return false;
}

static Vehicle *_vehicle_viewport_hash[1 << (GEN_HASHX_BITS + GEN_HASHY_BITS)];

static void UpdateVehicleViewportHash(Vehicle *v, int x, int y)
{
	Vehicle **old_hash, **new_hash;
	int old_x = v->coord.left;
	int old_y = v->coord.top;

	new_hash = (x == INVALID_COORD) ? nullptr : &_vehicle_viewport_hash[GEN_HASH(x, y)];
	old_hash = (old_x == INVALID_COORD) ? nullptr : &_vehicle_viewport_hash[GEN_HASH(old_x, old_y)];

	if (old_hash == new_hash) return;

	/* remove from hash table? */
	if (old_hash != nullptr) {
		if (v->hash_viewport_next != nullptr) v->hash_viewport_next->hash_viewport_prev = v->hash_viewport_prev;
		*v->hash_viewport_prev = v->hash_viewport_next;
	}

	/* insert into hash table? */
	if (new_hash != nullptr) {
		v->hash_viewport_next = *new_hash;
		if (v->hash_viewport_next != nullptr) v->hash_viewport_next->hash_viewport_prev = &v->hash_viewport_next;
		v->hash_viewport_prev = new_hash;
		*new_hash = v;
	}
}

struct ViewportHashDeferredItem {
	Vehicle *v;
	int new_hash;
	int old_hash;
};
static std::vector<ViewportHashDeferredItem> _viewport_hash_deferred;

static void UpdateVehicleViewportHashDeferred(Vehicle *v, int x, int y)
{
	int old_x = v->coord.left;
	int old_y = v->coord.top;

	int new_hash = (x == INVALID_COORD) ? INVALID_COORD : GEN_HASH(x, y);
	int old_hash = (old_x == INVALID_COORD) ? INVALID_COORD : GEN_HASH(old_x, old_y);

	if (new_hash != old_hash) {
		_viewport_hash_deferred.push_back({ v, new_hash, old_hash });
	}
}

static void ProcessDeferredUpdateVehicleViewportHashes()
{
	for (const ViewportHashDeferredItem &item : _viewport_hash_deferred) {
		Vehicle *v = item.v;

		/* remove from hash table? */
		if (item.old_hash != INVALID_COORD) {
			if (v->hash_viewport_next != nullptr) v->hash_viewport_next->hash_viewport_prev = v->hash_viewport_prev;
			*v->hash_viewport_prev = v->hash_viewport_next;
		}

		/* insert into hash table? */
		if (item.new_hash != INVALID_COORD) {
			Vehicle **new_hash = &_vehicle_viewport_hash[item.new_hash];
			v->hash_viewport_next = *new_hash;
			if (v->hash_viewport_next != nullptr) v->hash_viewport_next->hash_viewport_prev = &v->hash_viewport_next;
			v->hash_viewport_prev = new_hash;
			*new_hash = v;
		}
	}
	_viewport_hash_deferred.clear();
}

void ResetVehicleHash()
{
	for (Vehicle *v : Vehicle::Iterate()) {
		v->hash_tile_next = nullptr;
		v->hash_tile_prev = nullptr;
		v->hash_tile_current = INVALID_TILE;
	}
	memset(_vehicle_viewport_hash, 0, sizeof(_vehicle_viewport_hash));
	for (VehicleTypeTileHash &vhash : _vehicle_tile_hashes) {
		vhash.clear();
	}
}

void ResetVehicleColourMap()
{
	for (Vehicle *v : Vehicle::Iterate()) { v->colourmap = PAL_NONE; }
}

/**
 * List of vehicles that should check for autoreplace this tick.
 * Mapping of vehicle -> leave depot immediately after autoreplace.
 */
static btree::btree_map<VehicleID, bool> _vehicles_to_autoreplace;

/**
 * List of vehicles that are issued for template replacement this tick.
 */
static btree::btree_set<VehicleID> _vehicles_to_templatereplace;

void InitializeVehicles()
{
	_vehicles_to_autoreplace.clear();
	ResetVehicleHash();
	ResetDisasterVehicleTargeting();
}

uint CountVehiclesInChain(const Vehicle *v)
{
	uint count = 0;
	do count++; while ((v = v->Next()) != nullptr);
	return count;
}

/**
 * Check if a vehicle is counted in num_engines in each company struct
 * @return true if the vehicle is counted in num_engines
 */
bool Vehicle::IsEngineCountable() const
{
	if (HasBit(this->subtype, GVSF_VIRTUAL)) return false;
	switch (this->type) {
		case VEH_AIRCRAFT: return Aircraft::From(this)->IsNormalAircraft(); // don't count plane shadows and helicopter rotors
		case VEH_TRAIN:
			return !this->IsArticulatedPart() && // tenders and other articulated parts
					!Train::From(this)->IsRearDualheaded(); // rear parts of multiheaded engines
		case VEH_ROAD: return RoadVehicle::From(this)->IsFrontEngine();
		case VEH_SHIP: return Ship::From(this)->IsPrimaryVehicle();
		default: return false; // Only count company buildable vehicles
	}
}

/**
 * Check whether Vehicle::engine_type has any meaning.
 * @return true if the vehicle has a usable engine type.
 */
bool Vehicle::HasEngineType() const
{
	switch (this->type) {
		case VEH_AIRCRAFT: return Aircraft::From(this)->IsNormalAircraft();
		case VEH_TRAIN:
		case VEH_ROAD:
		case VEH_SHIP: return true;
		default: return false;
	}
}

/**
 * Retrieves the engine of the vehicle.
 * @return Engine of the vehicle.
 * @pre HasEngineType() == true
 */
const Engine *Vehicle::GetEngine() const
{
	return Engine::Get(this->engine_type);
}

/**
 * Retrieve the NewGRF the vehicle is tied to.
 * This is the GRF providing the Action 3 for the engine type.
 * @return NewGRF associated to the vehicle.
 */
const GRFFile *Vehicle::GetGRF() const
{
	return this->GetEngine()->GetGRF();
}

/**
 * Retrieve the GRF ID of the NewGRF the vehicle is tied to.
 * This is the GRF providing the Action 3 for the engine type.
 * @return GRF ID of the associated NewGRF.
 */
uint32_t Vehicle::GetGRFID() const
{
	return this->GetEngine()->GetGRFID();
}

/**
 * Handle the pathfinding result, especially the lost status.
 * If the vehicle is now lost and wasn't previously fire an
 * event to the AIs and a news message to the user. If the
 * vehicle is not lost anymore remove the news message.
 * @param path_found Whether the vehicle has a path to its destination.
 */
void Vehicle::HandlePathfindingResult(bool path_found)
{
	if (path_found) {
		/* Route found, is the vehicle marked with "lost" flag? */
		if (!HasBit(this->vehicle_flags, VF_PATHFINDER_LOST)) return;

		/* Clear the flag as the PF's problem was solved. */
		ClrBit(this->vehicle_flags, VF_PATHFINDER_LOST);
		if (this->type == VEH_SHIP) {
			Ship::From(this)->lost_count = 0;
		}

		SetWindowWidgetDirty(WC_VEHICLE_VIEW, this->index, WID_VV_START_STOP);
		DirtyVehicleListWindowForVehicle(this);

		/* Delete the news item. */
		DeleteVehicleNews(this->index, AdviceType::VehicleLost);
		return;
	}

	if (!HasBit(this->vehicle_flags, VF_PATHFINDER_LOST)) {
		SetWindowWidgetDirty(WC_VEHICLE_VIEW, this->index, WID_VV_START_STOP);
		DirtyVehicleListWindowForVehicle(this);
	}

	/* Unbunching data is no longer valid. */
	this->ResetDepotUnbunching();

	if (this->type == VEH_SHIP) {
		SetBit(this->vehicle_flags, VF_PATHFINDER_LOST);
		if (Ship::From(this)->lost_count == 255) return;
		Ship::From(this)->lost_count++;
		if (Ship::From(this)->lost_count != 16) return;
	} else {
		/* Were we already lost? */
		if (HasBit(this->vehicle_flags, VF_PATHFINDER_LOST)) return;

		/* It is first time the problem occurred, set the "lost" flag. */
		SetBit(this->vehicle_flags, VF_PATHFINDER_LOST);
	}

	/* Notify user about the event. */
	AI::NewEvent(this->owner, new ScriptEventVehicleLost(this->index));
	if (_settings_client.gui.lost_vehicle_warn && this->owner == _local_company) {
		SetDParam(0, this->index);
		AddVehicleAdviceNewsItem(AdviceType::VehicleLost, STR_NEWS_VEHICLE_IS_LOST, this->index);
	}
}

/** Destroy all stuff that (still) needs the virtual functions to work properly */
void Vehicle::PreDestructor()
{
	if (CleaningPool()) return;

	SCOPE_INFO_FMT([this], "Vehicle::PreDestructor: {}", VehicleInfoDumper(this));

	if (Station::IsValidID(this->last_station_visited)) {
		Station *st = Station::Get(this->last_station_visited);
		st->loading_vehicles.erase(std::remove(st->loading_vehicles.begin(), st->loading_vehicles.end(), this), st->loading_vehicles.end());

		HideFillingPercent(&this->fill_percent_te_id);
		this->CancelReservation(INVALID_STATION, st);
		delete this->cargo_payment;
		dbg_assert(this->cargo_payment == nullptr); // cleared by ~CargoPayment
	}

	if (this->IsEngineCountable()) {
		GroupStatistics::CountEngine(this, -1);
		if (this->IsPrimaryVehicle()) GroupStatistics::CountVehicle(this, -1);
		GroupStatistics::UpdateAutoreplace(this->owner);

		if (this->owner == _local_company) InvalidateAutoreplaceWindow(this->engine_type, this->group_id);
		DeleteGroupHighlightOfVehicle(this);
		if (this->type == VEH_TRAIN) {
			extern void DeleteTraceRestrictSlotHighlightOfVehicle(const Vehicle *v);

			DeleteTraceRestrictSlotHighlightOfVehicle(this);
		}
	}

	Company::Get(this->owner)->freeunits[this->type].ReleaseID(this->unitnumber);

	if (this->type == VEH_AIRCRAFT && this->IsPrimaryVehicle()) {
		Aircraft *a = Aircraft::From(this);
		Station *st = GetTargetAirportIfValid(a);
		if (st != nullptr) {
			const auto &layout = st->airport.GetFTA()->layout;
			CLRBITS(st->airport.flags, layout[a->previous_pos].block | layout[a->pos].block);
		}
	}


	if (this->type == VEH_ROAD && this->IsPrimaryVehicle()) {
		RoadVehicle *v = RoadVehicle::From(this);
		if ((!(v->vehstatus & VS_CRASHED) && IsInsideMM(v->state, RVSB_IN_DT_ROAD_STOP, RVSB_IN_DT_ROAD_STOP_END)) || IsInsideMM(v->state, RVSB_IN_ROAD_STOP, RVSB_IN_ROAD_STOP_END)) {
			/* Leave the roadstop (bay or drive-through), when you have not already left it. */
			RoadStop::GetByTile(v->tile, GetRoadStopType(v->tile))->Leave(v);
		}

		ReleaseDisasterVehicleTargetingVehicle(this->index);
	}

	if (HasBit(this->vehicle_flags, VF_HAVE_SLOT)) {
		TraceRestrictRemoveVehicleFromAllSlots(this->index);
		ClrBit(this->vehicle_flags, VF_HAVE_SLOT);
	}
	if (this->type == VEH_TRAIN && HasBit(Train::From(this)->flags, VRF_PENDING_SPEED_RESTRICTION)) {
		_pending_speed_restriction_change_map.erase(this->index);
		ClrBit(Train::From(this)->flags, VRF_PENDING_SPEED_RESTRICTION);
	}

	if (this->Previous() == nullptr) {
		InvalidateWindowData(WC_VEHICLE_DEPOT, this->tile.base());
	}

	if (this->IsPrimaryVehicle()) {
		CloseWindowById(WC_VEHICLE_VIEW, this->index);
		CloseWindowById(WC_VEHICLE_ORDERS, this->index);
		CloseWindowById(WC_VEHICLE_REFIT, this->index);
		CloseWindowById(WC_VEHICLE_DETAILS, this->index);
		CloseWindowById(WC_VEHICLE_TIMETABLE, this->index);
		CloseWindowById(WC_SCHDISPATCH_SLOTS, this->index);
		CloseWindowById(WC_VEHICLE_CARGO_TYPE_LOAD_ORDERS, this->index);
		CloseWindowById(WC_VEHICLE_CARGO_TYPE_UNLOAD_ORDERS, this->index);
		SetWindowDirty(WC_COMPANY, this->owner);
		OrderBackup::ClearVehicle(this);
	}
	InvalidateWindowClassesData(GetWindowClassForVehicleType(this->type), 0);
	InvalidateWindowClassesData(WC_DEPARTURES_BOARD, 0);

	this->cargo.Truncate();
	DeleteVehicleOrders(this);
	DeleteDepotHighlightOfVehicle(this);

	StopGlobalFollowVehicle(this);

	/* sometimes, eg. for disaster vehicles, when company bankrupts, when removing crashed/flooded vehicles,
	 * it may happen that vehicle chain is deleted when visible */
	if (this->IsDrawn()) this->MarkAllViewportsDirty();
}

Vehicle::~Vehicle()
{
	if (CleaningPool()) {
		this->cargo.OnCleanPool();
		return;
	}

	if (this->type != VEH_EFFECT) InvalidateVehicleTickCaches();

	if (this->type == VEH_DISASTER) RemoveFromOtherVehicleTickCache(this);

	if (this->breakdowns_since_last_service) _vehicles_to_pay_repair.erase(this->index);

	if (this->type >= VEH_COMPANY_END) {
		/* sometimes, eg. for disaster vehicles, when company bankrupts, when removing crashed/flooded vehicles,
		 * it may happen that vehicle chain is deleted when visible.
		 * Do not redo this for vehicle types where it is done in PreDestructor(). */
		if (this->IsDrawn()) this->MarkAllViewportsDirty();
	}

	Vehicle *v = this->Next();
	this->SetNext(nullptr);

	delete v;

	if (this->type < VEH_COMPANY_END) UpdateVehicleTileHash(this, true);
	UpdateVehicleViewportHash(this, INVALID_COORD, 0);
	if (this->type != VEH_EFFECT) {
		DeleteVehicleNews(this->index);
		DeleteNewGRFInspectWindow(GetGrfSpecFeature(this->type), this->index);
	}
}

/**
 * Vehicle pool is about to be cleaned
 */
void Vehicle::PreCleanPool()
{
	_pending_speed_restriction_change_map.clear();
}

/**
 * Adds a vehicle to the list of vehicles that visited a depot this tick
 * @param *v vehicle to add
 */
void VehicleEnteredDepotThisTick(Vehicle *v)
{
	/* Template Replacement Setup stuff */
	if (GetTemplateIDByGroupIDRecursive(v->group_id) != INVALID_TEMPLATE) {
		/* Vehicle should stop in the depot if it was in 'stopping' state */
		_vehicles_to_templatereplace.insert(v->index);
	}

	/* Vehicle should stop in the depot if it was in 'stopping' state */
	_vehicles_to_autoreplace[v->index] = !(v->vehstatus & VS_STOPPED);

	/* We ALWAYS set the stopped state. Even when the vehicle does not plan on
	 * stopping in the depot, so we stop it to ensure that it will not reserve
	 * the path out of the depot before we might autoreplace it to a different
	 * engine. The new engine would not own the reserved path we store that we
	 * stopped the vehicle, so autoreplace can start it again */
	v->vehstatus |= VS_STOPPED;
}

template <typename T>
void CallVehicleOnNewDay(Vehicle *v)
{
	T::From(v)->T::OnNewDay();

	/* Vehicle::OnPeriodic is decoupled from Vehicle::OnNewDay at day lengths >= 8 */
	if (DayLengthFactor() < 8) T::From(v)->T::OnPeriodic();
}

/**
 * Increases the day counter for all vehicles and calls 1-day and 32-day handlers.
 * Each tick, it processes vehicles with "index % DAY_TICKS == _date_fract",
 * so each day, all vehicles are processes in DAY_TICKS steps.
 */
static void RunVehicleDayProc()
{
	if (_game_mode != GM_NORMAL) return;

	/* Run the day_proc for every DAY_TICKS vehicle starting at _date_fract. */
	Vehicle *v = nullptr;
	SCOPE_INFO_FMT([&v], "RunVehicleDayProc: {}", VehicleInfoDumper(v));
	for (size_t i = EconTime::CurDateFract(); i < Vehicle::GetPoolSize(); i += DAY_TICKS) {
		v = Vehicle::Get(i);
		if (v == nullptr) continue;

		/* Call the 32-day callback if needed */
		if ((v->day_counter & 0x1F) == 0 && v->HasEngineType() && (Engine::Get(v->engine_type)->callbacks_used & SGCU_VEHICLE_32DAY_CALLBACK) != 0) {
			uint16_t callback = GetVehicleCallback(CBID_VEHICLE_32DAY_CALLBACK, 0, 0, v->engine_type, v);
			if (callback != CALLBACK_FAILED) {
				if (HasBit(callback, 0)) {
					TriggerVehicle(v, VEHICLE_TRIGGER_CALLBACK_32); // Trigger vehicle trigger 10
				}

				/* After a vehicle trigger, the graphics and properties of the vehicle could change.
				 * Note: MarkDirty also invalidates the palette, which is the meaning of bit 1. So, nothing special there. */
				if (callback != 0) v->First()->MarkDirty();

				if (callback & ~3) ErrorUnknownCallbackResult(v->GetGRFID(), CBID_VEHICLE_32DAY_CALLBACK, callback);
			}
		}

		/* This is called once per day for each vehicle, but not in the first tick of the day */
		switch (v->type) {
			case VEH_TRAIN:
				CallVehicleOnNewDay<Train>(v);
				break;
			case VEH_ROAD:
				CallVehicleOnNewDay<RoadVehicle>(v);
				break;
			case VEH_SHIP:
				CallVehicleOnNewDay<Ship>(v);
				break;
			case VEH_AIRCRAFT:
				CallVehicleOnNewDay<Aircraft>(v);
				break;
			default:
				break;
		}
	}
}

/**
 * Increases the day counter for all vehicles and calls 1-day and 32-day handlers.
 * Each tick, it processes vehicles with "index % DAY_TICKS == _date_fract",
 * so each day, all vehicles are processes in DAY_TICKS steps.
 */
static void RunVehicleCalendarDayProc()
{
	if (_game_mode != GM_NORMAL) return;

	Vehicle *v = nullptr;
	SCOPE_INFO_FMT([&v], "RunVehicleCalendarDayProc: {}", VehicleInfoDumper(v));
	for (size_t i = CalTime::CurDateFract(); i < Vehicle::GetPoolSize(); i += DAY_TICKS) {
		v = Vehicle::Get(i);
		if (v == nullptr) continue;

		/* This is called once per day for each vehicle, but not in the first tick of the day */
		switch (v->type) {
			case VEH_TRAIN:
				AgeVehicle(v);
				break;
			case VEH_ROAD:
				if (v->IsFrontEngine()) AgeVehicle(v);
				break;
			case VEH_SHIP:
				if (static_cast<Ship *>(v)->IsPrimaryVehicle()) AgeVehicle(v);
				break;
			case VEH_AIRCRAFT:
				if (static_cast<Aircraft *>(v)->IsNormalAircraft()) AgeVehicle(v);
				break;
			default:
				break;
		}
	}
}

static void ShowAutoReplaceAdviceMessage(const CommandCost &res, const Vehicle *v)
{
	StringID error_message = res.GetErrorMessage();
	if (error_message == STR_ERROR_AUTOREPLACE_NOTHING_TO_DO || error_message == INVALID_STRING_ID) return;

	if (error_message == STR_ERROR_NOT_ENOUGH_CASH_REQUIRES_CURRENCY) error_message = STR_ERROR_AUTOREPLACE_MONEY_LIMIT;

	StringID message;
	if (error_message == STR_ERROR_TRAIN_TOO_LONG_AFTER_REPLACEMENT) {
		message = error_message;
	} else {
		message = STR_NEWS_VEHICLE_AUTORENEW_FAILED;
	}

	SetDParam(0, v->index);
	SetDParam(1, error_message);
	AddVehicleAdviceNewsItem(AdviceType::AutorenewFailed, message, v->index);
}

static std::vector<VehicleID> _train_news_too_heavy_this_tick;

void ShowTrainTooHeavyAdviceMessage(const Vehicle *v)
{
	if (find_index(_train_news_too_heavy_this_tick, v->index) < 0) {
		_train_news_too_heavy_this_tick.push_back(v->index);
		SetDParam(0, v->index);
		AddNewsItem(STR_ERROR_TRAIN_TOO_HEAVY, NewsType::Advice, NewsStyle::Small, {NewsFlag::InColour, NewsFlag::VehicleParam0},
				NewsReferenceType::Vehicle, v->index);
	}
}

bool _tick_caches_valid = false;
std::vector<Train *> _tick_train_front_cache;
std::vector<RoadVehicle *> _tick_road_veh_front_cache;
std::vector<Aircraft *> _tick_aircraft_front_cache;
std::vector<Ship *> _tick_ship_cache;
std::vector<Vehicle *> _tick_other_veh_cache;

std::vector<VehicleID> _remove_from_tick_effect_veh_cache;
btree::btree_set<VehicleID> _tick_effect_veh_cache;

void ClearVehicleTickCaches()
{
	_tick_train_front_cache.clear();
	_tick_road_veh_front_cache.clear();
	_tick_aircraft_front_cache.clear();
	_tick_ship_cache.clear();
	_tick_effect_veh_cache.clear();
	_remove_from_tick_effect_veh_cache.clear();
	_tick_other_veh_cache.clear();
}

void RemoveFromOtherVehicleTickCache(const Vehicle *v)
{
	for (auto &u : _tick_other_veh_cache) {
		if (u == v) u = nullptr;
	}
}

void RebuildVehicleTickCaches()
{
	ClearVehicleTickCaches();

	for (VehicleID i = 0; i < Vehicle::GetPoolSize(); i++) {
		Vehicle *v = Vehicle::Get(i);
		if (v == nullptr) continue;

#if OTTD_UPPER_TAGGED_PTR
		/* Avoid needing to de-reference v */
		uintptr_t ptr = _vehicle_pool.GetRaw(i);
		const VehicleType vtype = VehiclePoolOps::GetVehicleType(ptr);
		const bool is_front = !VehiclePoolOps::IsNonFrontVehiclePtr(ptr);
#else
		const VehicleType vtype = v->type;
		const bool is_front = (v->Previous() == nullptr);
#endif

		switch (vtype) {
			default:
				_tick_other_veh_cache.push_back(v);
				break;

			case VEH_TRAIN:
				if (is_front) _tick_train_front_cache.push_back(Train::From(v));
				break;

			case VEH_ROAD:
				if (is_front) _tick_road_veh_front_cache.push_back(RoadVehicle::From(v));
				break;

			case VEH_AIRCRAFT:
				if (is_front) _tick_aircraft_front_cache.push_back(Aircraft::From(v));
				break;

			case VEH_SHIP:
				if (is_front) _tick_ship_cache.push_back(Ship::From(v));
				break;

			case VEH_EFFECT:
				_tick_effect_veh_cache.insert(i);
				break;
		}
	}
	_tick_caches_valid = true;
}

void ValidateVehicleTickCaches()
{
	if (!_tick_caches_valid) return;

	std::vector<Train *> saved_tick_train_front_cache = std::move(_tick_train_front_cache);
	std::vector<RoadVehicle *> saved_tick_road_veh_front_cache = std::move(_tick_road_veh_front_cache);
	std::vector<Aircraft *> saved_tick_aircraft_front_cache = std::move(_tick_aircraft_front_cache);
	std::vector<Ship *> saved_tick_ship_cache = std::move(_tick_ship_cache);
	btree::btree_set<VehicleID> saved_tick_effect_veh_cache = std::move(_tick_effect_veh_cache);
	for (VehicleID id : _remove_from_tick_effect_veh_cache) {
		saved_tick_effect_veh_cache.erase(id);
	}
	std::vector<Vehicle *> saved_tick_other_veh_cache = std::move(_tick_other_veh_cache);
	saved_tick_other_veh_cache.erase(std::remove(saved_tick_other_veh_cache.begin(), saved_tick_other_veh_cache.end(), nullptr), saved_tick_other_veh_cache.end());

	RebuildVehicleTickCaches();

	assert(saved_tick_train_front_cache == saved_tick_train_front_cache);
	assert(saved_tick_road_veh_front_cache == _tick_road_veh_front_cache);
	assert(saved_tick_aircraft_front_cache == _tick_aircraft_front_cache);
	assert(saved_tick_ship_cache == _tick_ship_cache);
	assert(saved_tick_effect_veh_cache == _tick_effect_veh_cache);
	assert(saved_tick_other_veh_cache == _tick_other_veh_cache);
}

void VehicleTickCargoAging(Vehicle *v)
{
	if (v->vcache.cached_cargo_age_period != 0) {
		v->cargo_age_counter = std::min(v->cargo_age_counter, v->vcache.cached_cargo_age_period);
		if (--v->cargo_age_counter == 0) {
			v->cargo.AgeCargo();
			v->cargo_age_counter = v->vcache.cached_cargo_age_period;
		}
	}
}

void VehicleTickMotion(Vehicle *v, Vehicle *front)
{
	/* Do not play any sound when crashed */
	if (front->vehstatus & VS_CRASHED) return;

	/* Do not play any sound when in depot or tunnel */
	if (v->vehstatus & VS_HIDDEN) return;

	v->motion_counter += front->cur_speed;
	if (_settings_client.sound.vehicle && _settings_client.music.effect_vol != 0) {
		/* Play a running sound if the motion counter passes 256 (Do we not skip sounds?) */
		if (GB(v->motion_counter, 0, 8) < front->cur_speed) PlayVehicleSound(v, VSE_RUNNING);

		/* Play an alternating running sound every 16 ticks */
		if (GB(v->tick_counter, 0, 4) == 0) {
			/* Play running sound when speed > 0 and not braking */
			bool running = (front->cur_speed > 0) && !(front->vehstatus & (VS_STOPPED | VS_TRAIN_SLOWING));
			PlayVehicleSound(v, running ? VSE_RUNNING_16 : VSE_STOPPED_16);
		}
	}
}

void CallVehicleTicks()
{
	_vehicles_to_autoreplace.clear();
	_vehicles_to_templatereplace.clear();
	_vehicles_to_pay_repair.clear();
	_vehicles_to_sell.clear();

	_train_news_too_heavy_this_tick.clear();

	if (TickSkipCounter() == 0) RunVehicleDayProc();

	if (EconTime::UsingWallclockUnits() && !CalTime::IsCalendarFrozen() && CalTime::CurSubDateFract() == 0) {
		RunVehicleCalendarDayProc();
	}

	if (DayLengthFactor() >= 8 && _game_mode == GM_NORMAL) {
		/*
		 * Vehicle::OnPeriodic is decoupled from Vehicle::OnNewDay at day lengths >= 8
		 * Use a fixed interval of 512 ticks (unscaled) instead
		 */

		Vehicle *v = nullptr;
		SCOPE_INFO_FMT([&v], "CallVehicleTicks -> OnPeriodic: {}", VehicleInfoDumper(v));
		for (size_t i = (size_t)(_scaled_tick_counter & 0x1FF); i < Vehicle::GetPoolSize(); i += 0x200) {
			v = Vehicle::Get(i);
			if (v == nullptr) continue;

			/* This is called once per day for each vehicle, but not in the first tick of the day */
			switch (v->type) {
				case VEH_TRAIN:
					Train::From(v)->Train::OnPeriodic();
					break;
				case VEH_ROAD:
					RoadVehicle::From(v)->RoadVehicle::OnPeriodic();
					break;
				case VEH_SHIP:
					Ship::From(v)->Ship::OnPeriodic();
					break;
				case VEH_AIRCRAFT:
					Aircraft::From(v)->Aircraft::OnPeriodic();
					break;
				default:
					break;
			}
		}
	}

	RecordSyncEvent(NSRE_VEH_PERIODIC);

	{
		PerformanceMeasurer framerate(PFE_GL_ECONOMY);
		Station *si_st = nullptr;
		SCOPE_INFO_FMT([&si_st], "CallVehicleTicks: LoadUnloadStation: {}", StationInfoDumper(si_st));
		for (Station *st : Station::Iterate()) {
			si_st = st;
			LoadUnloadStation(st);
		}
	}

	RecordSyncEvent(NSRE_VEH_LOAD_UNLOAD);

	if (!_tick_caches_valid || HasChickenBit(DCBF_VEH_TICK_CACHE)) RebuildVehicleTickCaches();

	if (HasChickenBit(DCBF_WATER_REGION_CLEAR)) {
		DebugInvalidateAllWaterRegions();
	}
	if (HasChickenBit(DCBF_WATER_REGION_INIT_ALL)) {
		DebugInitAllWaterRegions();
	}

	Vehicle *v = nullptr;
	SCOPE_INFO_FMT([&v], "CallVehicleTicks: {}", VehicleInfoDumper(v));
	{
		for (VehicleID id : _remove_from_tick_effect_veh_cache) {
			_tick_effect_veh_cache.erase(id);
		}
		_remove_from_tick_effect_veh_cache.clear();
		for (VehicleID id : _tick_effect_veh_cache) {
			EffectVehicle *u = EffectVehicle::Get(id);
			v = u;
			u->EffectVehicle::Tick();
		}
	}
	if (!_tick_effect_veh_cache.empty()) RecordSyncEvent(NSRE_VEH_EFFECT);
	{
		PerformanceMeasurer framerate(PFE_GL_TRAINS);
		for (Train *front : _tick_train_front_cache) {
			v = front;
			if (!front->Train::Tick()) continue;
			for (Train *u = front; u != nullptr; u = u->Next()) {
				u->tick_counter++;
				VehicleTickCargoAging(u);
				if (u->IsEngine() && !((front->vehstatus & VS_STOPPED) && front->cur_speed == 0)) VehicleTickMotion(u, front);
			}
		}
	}
	RecordSyncEvent(NSRE_VEH_TRAIN);
	{
		PerformanceMeasurer framerate(PFE_GL_ROADVEHS);
		for (RoadVehicle *front : _tick_road_veh_front_cache) {
			v = front;
			if (!front->RoadVehicle::Tick()) continue;
			for (RoadVehicle *u = front; u != nullptr; u = u->Next()) {
				u->tick_counter++;
				VehicleTickCargoAging(u);
			}
			if (!(front->vehstatus & VS_STOPPED)) VehicleTickMotion(front, front);
		}
	}
	if (!_tick_road_veh_front_cache.empty()) RecordSyncEvent(NSRE_VEH_ROAD);
	{
		PerformanceMeasurer framerate(PFE_GL_AIRCRAFT);
		for (Aircraft *front : _tick_aircraft_front_cache) {
			v = front;
			if (!front->Aircraft::Tick()) continue;
			for (Aircraft *u = front; u != nullptr; u = u->Next()) {
				VehicleTickCargoAging(u);
			}
			if (!(front->vehstatus & VS_STOPPED)) VehicleTickMotion(front, front);
		}
	}
	if (!_tick_aircraft_front_cache.empty()) RecordSyncEvent(NSRE_VEH_AIR);
	{
		PerformanceMeasurer framerate(PFE_GL_SHIPS);
		for (Ship *s : _tick_ship_cache) {
			v = s;
			if (!s->Ship::Tick()) continue;
			for (Ship *u = s; u != nullptr; u = u->Next()) {
				VehicleTickCargoAging(u);
			}
			if (!(s->vehstatus & VS_STOPPED)) VehicleTickMotion(s, s);
		}
	}
	if (!_tick_ship_cache.empty()) RecordSyncEvent(NSRE_VEH_SHIP);
	{
		for (Vehicle *u : _tick_other_veh_cache) {
			if (!u) continue;
			v = u;
			u->Tick();
		}
	}
	v = nullptr;
	if (!_tick_other_veh_cache.empty()) RecordSyncEvent(NSRE_VEH_OTHER);

	/* Handle vehicles marked for immediate sale */
	Backup<CompanyID> sell_cur_company(_current_company, FILE_LINE);
	for (VehicleID index : _vehicles_to_sell) {
		Vehicle *v = Vehicle::Get(index);
		SCOPE_INFO_FMT([v], "CallVehicleTicks: sell: {}", VehicleInfoDumper(v));
		const bool is_train = (v->type == VEH_TRAIN);

		sell_cur_company.Change(v->owner);

		int x = v->x_pos;
		int y = v->y_pos;
		int z = v->z_pos;

		CommandCost cost = Command<CMD_SELL_VEHICLE>::Do(DC_EXEC, v->index, SellVehicleFlags::SellChain, INVALID_CLIENT_ID);
		v = nullptr;
		if (!cost.Succeeded()) continue;

		if (IsLocalCompany() && cost.Succeeded()) {
			if (cost.GetCost() != 0) {
				ShowCostOrIncomeAnimation(x, y, z, cost.GetCost());
			}
		}

		if (is_train) _vehicles_to_templatereplace.erase(index);
		_vehicles_to_autoreplace.erase(index);
	}
	sell_cur_company.Restore();
	if (!_vehicles_to_sell.empty()) RecordSyncEvent(NSRE_VEH_SELL);

	/* do Template Replacement */
	Backup<CompanyID> tmpl_cur_company(_current_company, FILE_LINE);
	for (VehicleID index : _vehicles_to_templatereplace) {
		Train *t = Train::Get(index);

		SCOPE_INFO_FMT([t], "CallVehicleTicks: template replace: {}", VehicleInfoDumper(t));

		auto it = _vehicles_to_autoreplace.find(index);
		assert(it != _vehicles_to_autoreplace.end());
		if (it->second) t->vehstatus &= ~VS_STOPPED;
		_vehicles_to_autoreplace.erase(it);

		/* Store the position of the effect as the vehicle pointer will become invalid later */
		int x = t->x_pos;
		int y = t->y_pos;
		int z = t->z_pos;

		tmpl_cur_company.Change(t->owner);


		CommandCost res = Command<CMD_TEMPLATE_REPLACE_VEHICLE>::Do(DC_EXEC, t->index);
		if (res.HasResultData()) {
			t = Train::Get(res.GetResultData());
		}
		const Company *c = Company::Get(_current_company);
		SubtractMoneyFromCompany(CommandCost(EXPENSES_NEW_VEHICLES, (Money)c->settings.engine_renew_money));
		CommandCost res2 = Command<CMD_AUTOREPLACE_VEHICLE>::Do(DC_EXEC, t->index, true);
		if (res2.HasResultData()) {
			t = Train::Get(res2.GetResultData());
		}
		SubtractMoneyFromCompany(CommandCost(EXPENSES_NEW_VEHICLES, -(Money)c->settings.engine_renew_money));
		if (res2.Succeeded() || res.GetCost() == 0) res.AddCost(res2);

		if (!IsLocalCompany()) continue;

		if (res.GetCost() != 0) {
			ShowCostOrIncomeAnimation(x, y, z, res.GetCost());
		}

		if (res.Failed()) {
			ShowAutoReplaceAdviceMessage(res, t);
		}
	}
	tmpl_cur_company.Restore();
	if (!_vehicles_to_templatereplace.empty()) RecordSyncEvent(NSRE_VEH_TBTR);

	/* do Auto Replacement */
	Backup<CompanyID> cur_company(_current_company, FILE_LINE);
	for (auto &it : _vehicles_to_autoreplace) {
		v = Vehicle::Get(it.first);
		/* Autoreplace needs the current company set as the vehicle owner */
		cur_company.Change(v->owner);

		if (v->type == VEH_TRAIN) {
			assert(!_vehicles_to_templatereplace.count(v->index));
		}

		/* Start vehicle if we stopped them in VehicleEnteredDepotThisTick()
		 * We need to stop them between VehicleEnteredDepotThisTick() and here or we risk that
		 * they are already leaving the depot again before being replaced. */
		if (it.second) v->vehstatus &= ~VS_STOPPED;

		/* Store the position of the effect as the vehicle pointer will become invalid later */
		int x = v->x_pos;
		int y = v->y_pos;
		int z = v->z_pos;

		const Company *c = Company::Get(_current_company);
		SubtractMoneyFromCompany(CommandCost(EXPENSES_NEW_VEHICLES, (Money)c->settings.engine_renew_money));
		CommandCost res = Command<CMD_AUTOREPLACE_VEHICLE>::Do(DC_EXEC, v->index, false);
		SubtractMoneyFromCompany(CommandCost(EXPENSES_NEW_VEHICLES, -(Money)c->settings.engine_renew_money));

		if (!IsLocalCompany()) continue;

		if (res.Succeeded()) {
			ShowCostOrIncomeAnimation(x, y, z, res.GetCost());
			continue;
		}

		ShowAutoReplaceAdviceMessage(res, v);
	}
	cur_company.Restore();
	if (!_vehicles_to_autoreplace.empty()) RecordSyncEvent(NSRE_VEH_AUTOREPLACE);

	Backup<CompanyID> repair_cur_company(_current_company, FILE_LINE);
	for (VehicleID index : _vehicles_to_pay_repair) {
		Vehicle *v = Vehicle::Get(index);
		SCOPE_INFO_FMT([v], "CallVehicleTicks: repair: {}", VehicleInfoDumper(v));

		ExpensesType type = INVALID_EXPENSES;
		_current_company = v->owner;
		switch (v->type) {
			case VEH_AIRCRAFT:
				type = EXPENSES_AIRCRAFT_RUN;
				break;

			case VEH_TRAIN:
				type = EXPENSES_TRAIN_RUN;
				break;

			case VEH_SHIP:
				type = EXPENSES_SHIP_RUN;
				break;

			case VEH_ROAD:
				type = EXPENSES_ROADVEH_RUN;
				break;

			default:
				NOT_REACHED();
		}
		dbg_assert(type != INVALID_EXPENSES);

		Money vehicle_new_value = v->GetEngine()->GetCost();

		// The static cast is to fix compilation on (old) MSVC as the overload for OverflowSafeInt operator / is ambiguous.
		Money repair_cost = (v->breakdowns_since_last_service * vehicle_new_value / static_cast<uint>(_settings_game.vehicle.repair_cost)) + 1;
		if (v->age > v->max_age) repair_cost <<= 1;
		CommandCost cost(type, repair_cost);
		v->First()->profit_this_year -= cost.GetCost() << 8;
		SubtractMoneyFromCompany(cost);
		ShowCostOrIncomeAnimation(v->x_pos, v->y_pos, v->z_pos, cost.GetCost());
		v->breakdowns_since_last_service = 0;
	}
	repair_cur_company.Restore();
	if (!_vehicles_to_pay_repair.empty()) RecordSyncEvent(NSRE_VEH_REPAIR);
	_vehicles_to_pay_repair.clear();
}

void RemoveVirtualTrainsOfUser(uint32_t user)
{
	if (!_tick_caches_valid || HasChickenBit(DCBF_VEH_TICK_CACHE)) RebuildVehicleTickCaches();

	Backup<CompanyID> cur_company(_current_company, FILE_LINE);
	for (const Train *front : _tick_train_front_cache) {
		if (front->IsVirtual() && front->motion_counter == user) {
			cur_company.Change(front->owner);
			Command<CMD_DELETE_VIRTUAL_TRAIN>::Post(front->index);
		}
	}
	cur_company.Restore();
}

/**
 * Add vehicle sprite for drawing to the screen.
 * @param v Vehicle to draw.
 */
static void DoDrawVehicle(const Vehicle *v)
{
	PaletteID pal = PAL_NONE;

	if (v->vehstatus & VS_DEFPAL) pal = (v->vehstatus & VS_CRASHED) ? PALETTE_CRASH : GetVehiclePalette(v);

	/* Check whether the vehicle shall be transparent due to the game state */
	bool shadowed = (v->vehstatus & (VS_SHADOW | VS_HIDDEN)) != 0;

	if (v->type == VEH_EFFECT) {
		/* Check whether the vehicle shall be transparent/invisible due to GUI settings.
		 * However, transparent smoke and bubbles look weird, so always hide them. */
		TransparencyOption to = EffectVehicle::From(v)->GetTransparencyOption();
		if (to != TO_INVALID && (IsTransparencySet(to) || IsInvisibilitySet(to))) return;
	}

	{
		Vehicle *v_mutable = const_cast<Vehicle *>(v);
		if (HasBit(v_mutable->vcache.cached_veh_flags, VCF_IMAGE_REFRESH) && v_mutable->cur_image_valid_dir != INVALID_DIR) {
			VehicleSpriteSeq seq;
			v_mutable->GetImage(v_mutable->cur_image_valid_dir, EIT_ON_MAP, &seq);
			v_mutable->sprite_seq = seq;
			v_mutable->UpdateSpriteSeqBound();
			ClrBit(v_mutable->vcache.cached_veh_flags, VCF_IMAGE_REFRESH);
		}
	}

	ViewportSortableSpriteSpecialFlags special_flags = IsDiagonalDirection(v->direction) ? VSSF_NONE : VSSSF_SORT_SPECIAL | VSSSF_SORT_DIAG_VEH;

	StartSpriteCombine();
	for (uint i = 0; i < v->sprite_seq.count; ++i) {
		PaletteID pal2 = v->sprite_seq.seq[i].pal;
		if (!pal2 || (v->vehstatus & VS_CRASHED)) pal2 = pal;
		AddSortableSpriteToDraw(v->sprite_seq.seq[i].sprite, pal2, v->x_pos + v->x_offs, v->y_pos + v->y_offs,
			v->x_extent, v->y_extent, v->z_extent, v->z_pos, shadowed, v->x_bb_offs, v->y_bb_offs, 0, nullptr, special_flags);
	}
	EndSpriteCombine();
}

struct ViewportHashBound {
	int xl, xu, yl, yu;
};

static const int VHB_BASE_MARGIN = 70;

static ViewportHashBound GetViewportHashBound(int l, int r, int t, int b, int x_margin, int y_margin) {
	int xl = (l - ((VHB_BASE_MARGIN + x_margin) * ZOOM_BASE)) >> (7 + ZOOM_BASE_SHIFT);
	int xu = (r + (x_margin * ZOOM_BASE))                     >> (7 + ZOOM_BASE_SHIFT);
	/* compare after shifting instead of before, so that lower bits don't affect comparison result */
	if (xu - xl < (1 << 6)) {
		xl &= 0x3F;
		xu &= 0x3F;
	} else {
		/* scan whole hash row */
		xl = 0;
		xu = 0x3F;
	}

	int yl = (t - ((VHB_BASE_MARGIN + y_margin) * ZOOM_BASE)) >> (6 + ZOOM_BASE_SHIFT);
	int yu = (b + (y_margin * ZOOM_BASE))                     >> (6 + ZOOM_BASE_SHIFT);
	/* compare after shifting instead of before, so that lower bits don't affect comparison result */
	if (yu - yl < (1 << 6)) {
		yl = (yl & 0x3F) << 6;
		yu = (yu & 0x3F) << 6;
	} else {
		/* scan whole column */
		yl = 0;
		yu = 0x3F << 6;
	}
	return { xl, xu, yl, yu };
};

template <bool update_vehicles>
void ViewportAddVehiclesIntl(DrawPixelInfo *dpi)
{
	/* The bounding rectangle */
	const int l = dpi->left;
	const int r = dpi->left + dpi->width;
	const int t = dpi->top;
	const int b = dpi->top + dpi->height;

	/* The hash area to scan */
	const ViewportHashBound vhb = GetViewportHashBound(l, r, t, b,
			update_vehicles ? MAX_VEHICLE_PIXEL_X - VHB_BASE_MARGIN : 0, update_vehicles ? MAX_VEHICLE_PIXEL_Y - VHB_BASE_MARGIN : 0);

	const int ul = l - (MAX_VEHICLE_PIXEL_X * ZOOM_BASE);
	const int ur = r + (MAX_VEHICLE_PIXEL_X * ZOOM_BASE);
	const int ut = t - (MAX_VEHICLE_PIXEL_Y * ZOOM_BASE);
	const int ub = b + (MAX_VEHICLE_PIXEL_Y * ZOOM_BASE);

	for (int y = vhb.yl;; y = (y + (1 << 6)) & (0x3F << 6)) {
		for (int x = vhb.xl;; x = (x + 1) & 0x3F) {
			const Vehicle *v = _vehicle_viewport_hash[x + y]; // already masked & 0xFFF

			while (v != nullptr) {
				if (v->IsDrawn()) {
					if (update_vehicles &&
							HasBit(v->vcache.cached_veh_flags, VCF_IMAGE_REFRESH) &&
							ul <= v->coord.right &&
							ut <= v->coord.bottom &&
							ur >= v->coord.left &&
							ub >= v->coord.top) {
						Vehicle *v_mutable = const_cast<Vehicle *>(v);
						switch (v->type) {
							case VEH_TRAIN:       Train::From(v_mutable)->UpdateImageStateUsingMapDirection(v_mutable->sprite_seq); break;
							case VEH_ROAD:  RoadVehicle::From(v_mutable)->UpdateImageStateUsingMapDirection(v_mutable->sprite_seq); break;
							case VEH_SHIP:         Ship::From(v_mutable)->UpdateImageStateUsingMapDirection(v_mutable->sprite_seq); break;
							case VEH_AIRCRAFT: Aircraft::From(v_mutable)->UpdateImageStateUsingMapDirection(v_mutable->sprite_seq); break;
							default: break;
						}
						v_mutable->UpdateSpriteSeqBound();
						v_mutable->UpdateViewportDeferred();
					}

					if (l <= v->coord.right &&
							t <= v->coord.bottom &&
							r >= v->coord.left &&
							b >= v->coord.top) {
						DoDrawVehicle(v);
					}
				}
				v = v->hash_viewport_next;
			}

			if (x == vhb.xu) break;
		}

		if (y == vhb.yu) break;
	}

	if (update_vehicles) ProcessDeferredUpdateVehicleViewportHashes();
}

/**
 * Add the vehicle sprites that should be drawn at a part of the screen.
 * @param dpi Rectangle being drawn.
 * @param update_vehicles Whether to update vehicles around drawing rectangle.
 */
void ViewportAddVehicles(DrawPixelInfo *dpi, bool update_vehicles)
{
	if (update_vehicles) {
		ViewportAddVehiclesIntl<true>(dpi);
	} else {
		ViewportAddVehiclesIntl<false>(dpi);
	}
}

void ViewportMapDrawVehicles(DrawPixelInfo *dpi, Viewport *vp)
{
	/* The save rectangle */
	const int l = vp->virtual_left;
	const int r = vp->virtual_left + vp->virtual_width;
	const int t = vp->virtual_top;
	const int b = vp->virtual_top + vp->virtual_height;

	/* The hash area to scan */
	const ViewportHashBound vhb = GetViewportHashBound(l, r, t, b, 0, 0);

	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	for (int y = vhb.yl;; y = (y + (1 << 6)) & (0x3F << 6)) {
		if (vp->map_draw_vehicles_cache.done_hash_bits[y >> 6] != UINT64_MAX) {
			for (int x = vhb.xl;; x = (x + 1) & 0x3F) {
				if (!HasBit(vp->map_draw_vehicles_cache.done_hash_bits[y >> 6], x)) {
					SetBit(vp->map_draw_vehicles_cache.done_hash_bits[y >> 6], x);
					const Vehicle *v = _vehicle_viewport_hash[x + y]; // already masked & 0xFFF

					while (v != nullptr) {
						if (!(v->vehstatus & (VS_HIDDEN | VS_UNCLICKABLE)) && (v->type != VEH_EFFECT)) {
							Point pt = { v->coord.left, v->coord.top };
							if (pt.x >= l && pt.x < r && pt.y >= t && pt.y < b) {
								const int pixel_x = UnScaleByZoomLower(pt.x - l, dpi->zoom);
								const int pixel_y = UnScaleByZoomLower(pt.y - t, dpi->zoom);
								const int pos = pixel_x + (pixel_y) * vp->width;
								SetBit(vp->map_draw_vehicles_cache.vehicle_pixels[pos / VP_BLOCK_BITS], pos % VP_BLOCK_BITS);
							}
						}
						v = v->hash_viewport_next;
					}
				}

				if (x == vhb.xu) break;
			}
		}

		if (y == vhb.yu) break;
	}

	/* The drawing rectangle */
	int mask = ScaleByZoom(-1, vp->zoom);
	const int dl = UnScaleByZoomLower(dpi->left - (vp->virtual_left & mask), dpi->zoom);
	const int dr = UnScaleByZoomLower(dpi->left + dpi->width - (vp->virtual_left & mask), dpi->zoom);
	const int dt = UnScaleByZoomLower(dpi->top - (vp->virtual_top & mask), dpi->zoom);
	const int db = UnScaleByZoomLower(dpi->top + dpi->height - (vp->virtual_top & mask), dpi->zoom);
	int y_ptr = vp->width * dt;
	for (int y = dt; y < db; y++, y_ptr += vp->width) {
		const uint row_start = static_cast<uint>(y_ptr + dl);
		const uint row_end = static_cast<uint>(y_ptr + dr);

		ViewPortBlockT ignore_mask = GetBitMaskSC<ViewPortBlockT>(0, row_start % VP_BLOCK_BITS);
		const ViewPortBlockT *ptr = vp->map_draw_vehicles_cache.vehicle_pixels.data() + (row_start / VP_BLOCK_BITS);
		for (uint block = row_start - (row_start % VP_BLOCK_BITS); block < row_end; block += VP_BLOCK_BITS, ignore_mask = 0, ptr++) {
			const ViewPortBlockT value = *ptr & ~ignore_mask;
			for (uint8_t bit : SetBitIterator(value)) {
				uint pos = block + bit;
				if (pos >= row_end) break;
				blitter->SetPixel32(dpi->dst_ptr, pos - row_start, y - dt, PC_WHITE, Colour(0xFC, 0xFC, 0xFC).data);
			}
		}
	}
}

/**
 * Find the vehicle close to the clicked coordinates.
 * @param vp Viewport clicked in.
 * @param x  X coordinate in the viewport.
 * @param y  Y coordinate in the viewport.
 * @return Closest vehicle, or \c nullptr if none found.
 */
Vehicle *CheckClickOnVehicle(const Viewport *vp, int x, int y)
{
	Vehicle *found = nullptr;
	uint dist, best_dist = UINT_MAX;

	if ((uint)(x -= vp->left) >= (uint)vp->width || (uint)(y -= vp->top) >= (uint)vp->height) return nullptr;

	x = ScaleByZoom(x, vp->zoom) + vp->virtual_left;
	y = ScaleByZoom(y, vp->zoom) + vp->virtual_top;

	/* The hash area to scan */
	const ViewportHashBound vhb = GetViewportHashBound(x, x, y, y, 0, 0);

	for (int hy = vhb.yl;; hy = (hy + (1 << 6)) & (0x3F << 6)) {
		for (int hx = vhb.xl;; hx = (hx + 1) & 0x3F) {
			Vehicle *v = _vehicle_viewport_hash[hx + hy]; // already masked & 0xFFF

			while (v != nullptr) {
				if (((v->vehstatus & VS_UNCLICKABLE) == 0) && v->IsDrawn() &&
					x >= v->coord.left && x <= v->coord.right &&
					y >= v->coord.top && y <= v->coord.bottom) {

					dist = std::max(
						abs(((v->coord.left + v->coord.right) >> 1) - x),
						abs(((v->coord.top + v->coord.bottom) >> 1) - y)
					);

					if (dist < best_dist) {
						found = v;
						best_dist = dist;
					}
				}
				v = v->hash_viewport_next;
			}

			if (hx == vhb.xu) break;
		}

		if (hy == vhb.yu) break;
	}

	return found;
}

/**
 * Decrease the value of a vehicle.
 * @param v %Vehicle to devaluate.
 */
void DecreaseVehicleValue(Vehicle *v)
{
	v->value -= v->value >> 8;
	SetWindowDirty(WC_VEHICLE_DETAILS, v->index);
}

/** The chances for the different types of vehicles to suffer from different types of breakdowns
 * The chance for a given breakdown type n is _breakdown_chances[vehtype][n] - _breakdown_chances[vehtype][n-1] */
static const uint8_t _breakdown_chances[4][4] = {
	{ //Trains:
		25,  ///< 10% chance for BREAKDOWN_CRITICAL.
		51,  ///< 10% chance for BREAKDOWN_EM_STOP.
		127, ///< 30% chance for BREAKDOWN_LOW_SPEED.
		255, ///< 50% chance for BREAKDOWN_LOW_POWER.
	},
	{ //Road Vehicles:
		51,  ///< 20% chance for BREAKDOWN_CRITICAL.
		76,  ///< 10% chance for BREAKDOWN_EM_STOP.
		153, ///< 30% chance for BREAKDOWN_LOW_SPEED.
		255, ///< 40% chance for BREAKDOWN_LOW_POWER.
	},
	{ //Ships:
		51,  ///< 20% chance for BREAKDOWN_CRITICAL.
		76,  ///< 10% chance for BREAKDOWN_EM_STOP.
		178, ///< 40% chance for BREAKDOWN_LOW_SPEED.
		255, ///< 30% chance for BREAKDOWN_LOW_POWER.
	},
	{ //Aircraft:
		178, ///< 70% chance for BREAKDOWN_AIRCRAFT_SPEED.
		229, ///< 20% chance for BREAKDOWN_AIRCRAFT_DEPOT.
		255, ///< 10% chance for BREAKDOWN_AIRCRAFT_EM_LANDING.
		255, ///< Aircraft have only 3 breakdown types, so anything above 0% here will cause a crash.
	},
};

/**
 * Determine the type of breakdown a vehicle will have.
 * Results are saved in breakdown_type and breakdown_severity.
 * @param v the vehicle in question.
 * @param r the random number to use. (Note that bits 0..6 are already used)
 */
void DetermineBreakdownType(Vehicle *v, uint32_t r) {
	/* if 'improved breakdowns' is off, just do the classic breakdown */
	if (!_settings_game.vehicle.improved_breakdowns) {
		v->breakdown_type = BREAKDOWN_CRITICAL;
		v->breakdown_severity = 40; //only used by aircraft (321 km/h)
		return;
	}
	uint8_t rand = GB(r, 8, 8);
	const uint8_t *breakdown_type_chance = _breakdown_chances[v->type];

	if (v->type == VEH_AIRCRAFT) {
		if (rand <= breakdown_type_chance[BREAKDOWN_AIRCRAFT_SPEED]) {
			v->breakdown_type = BREAKDOWN_AIRCRAFT_SPEED;
			/* all speed values here are 1/8th of the real max speed in km/h */
			uint8_t max_speed = std::max(1, std::min(v->vcache.cached_max_speed >> 3, 255));
			uint8_t min_speed = std::max(1, std::min(15 + (max_speed >> 2), v->vcache.cached_max_speed >> 4));
			v->breakdown_severity = min_speed + (((v->reliability + GB(r, 16, 16)) * (max_speed - min_speed)) >> 17);
		} else if (rand <= breakdown_type_chance[BREAKDOWN_AIRCRAFT_DEPOT]) {
			v->breakdown_type = BREAKDOWN_AIRCRAFT_DEPOT;
		} else if (rand <= breakdown_type_chance[BREAKDOWN_AIRCRAFT_EM_LANDING]) {
			/* emergency landings only happen when reliability < 87% */
			if (v->reliability < 0xDDDD) {
				v->breakdown_type = BREAKDOWN_AIRCRAFT_EM_LANDING;
			} else {
				/* try again */
				DetermineBreakdownType(v, Random());
			}
		} else {
			NOT_REACHED();
		}
		return;
	}

	if (rand <= breakdown_type_chance[BREAKDOWN_CRITICAL]) {
		v->breakdown_type = BREAKDOWN_CRITICAL;
	} else if (rand <= breakdown_type_chance[BREAKDOWN_EM_STOP]) {
		/* Non-front engines cannot have emergency stops */
		if (v->type == VEH_TRAIN && !(Train::From(v)->IsFrontEngine())) {
			return DetermineBreakdownType(v, Random());
		}
		v->breakdown_type = BREAKDOWN_EM_STOP;
		v->breakdown_delay >>= 2; //emergency stops don't last long (1/4 of normal)
	} else if (rand <= breakdown_type_chance[BREAKDOWN_LOW_SPEED]) {
		v->breakdown_type = BREAKDOWN_LOW_SPEED;
		/* average of random and reliability */
		uint16_t rand2 = (GB(r, 16, 16) + v->reliability) >> 1;
		uint16_t max_speed =
			(v->type == VEH_TRAIN) ?
			GetVehicleProperty(v, PROP_TRAIN_SPEED, RailVehInfo(v->engine_type)->max_speed) :
			(v->type == VEH_ROAD ) ?
			GetVehicleProperty(v, PROP_ROADVEH_SPEED, RoadVehInfo(v->engine_type)->max_speed) :
			(v->type == VEH_SHIP) ?
			GetVehicleProperty(v, PROP_SHIP_SPEED, ShipVehInfo(v->engine_type)->max_speed ) :
			GetVehicleProperty(v, PROP_AIRCRAFT_SPEED, AircraftVehInfo(v->engine_type)->max_speed);
		uint8_t min_speed = std::min(41, max_speed >> 2);
		/* we use the min() function here because we want to use the real value of max_speed for the min_speed calculation */
		max_speed = std::min<uint16_t>(max_speed, 255);
		v->breakdown_severity = Clamp((max_speed * rand2) >> 16, min_speed, max_speed);
	} else if (rand <= breakdown_type_chance[BREAKDOWN_LOW_POWER]) {
		v->breakdown_type = BREAKDOWN_LOW_POWER;
		/** within this type there are two possibilities: (50/50)
		 * power reduction (10-90%), or no power at all */
		if (GB(r, 7, 1)) {
			v->breakdown_severity = Clamp((GB(r, 16, 16) + v->reliability) >> 9, 26, 231);
		} else {
			v->breakdown_severity = 0;
		}
	} else {
		NOT_REACHED();
	}
}

void CheckVehicleBreakdown(Vehicle *v)
{
	int rel, rel_old;

	/* decrease reliability */
	if (!_settings_game.order.no_servicing_if_no_breakdowns ||
			_settings_game.difficulty.vehicle_breakdowns != 0) {
		v->reliability = rel = std::max((rel_old = v->reliability) - v->reliability_spd_dec, 0);
		if ((rel_old >> 8) != (rel >> 8)) SetWindowDirty(WC_VEHICLE_DETAILS, v->First()->index);
	}

	if (v->breakdown_ctr != 0 || (v->First()->vehstatus & VS_STOPPED) ||
			_settings_game.difficulty.vehicle_breakdowns < 1 ||
			v->First()->cur_speed < 5 || _game_mode == GM_MENU ||
			(v->type == VEH_AIRCRAFT && ((Aircraft*)v)->state != FLYING) ||
			(v->type == VEH_TRAIN && !(Train::From(v)->IsFrontEngine()) && !_settings_game.vehicle.improved_breakdowns)) {
		return;
	}

	uint32_t r = Random();

	/* increase chance of failure */
	int chance = v->breakdown_chance + 1;
	if (Chance16I(1, 25, r)) chance += 25;
	chance = ClampTo<uint8_t>(chance);
	v->breakdown_chance = chance;

	if (_settings_game.vehicle.improved_breakdowns) {
		if (v->type == VEH_TRAIN && Train::From(v)->IsMultiheaded()) {
			/* Dual engines have their breakdown chances reduced to 70% of the normal value */
			chance = chance * 7 / 10;
		}
		chance *= v->First()->breakdown_chance_factor;
		chance >>= 7;
	}
	/**
	 * Chance is (1 - reliability) * breakdown_setting * breakdown_chance / 10.
	 * breakdown_setting is scaled by 2 to support a value of 1/2 (setting value 64).
	 * Chance is (1 - reliability) * breakdown_scaling_x2 * breakdown_chance / 20.
	 *
	 * At 90% reliabilty, normal setting (2) and average breakdown_chance (128),
	 * a vehicle will break down (on average) every 100 days.
	 * This *should* mean that vehicles break down about as often as (or a little less than) they used to.
	 * However, because breakdowns are no longer by definition a complete stop,
	 * their impact will be significantly less.
	 */
	uint32_t r1 = Random();
	uint32_t breakdown_scaling_x2 = (_settings_game.difficulty.vehicle_breakdowns == 64) ? 1 : (_settings_game.difficulty.vehicle_breakdowns * 2);
	if ((uint32_t) (0xffff - v->reliability) * breakdown_scaling_x2 * chance > GB(r1, 0, 24) * 10 * 2) {
		uint32_t r2 = Random();
		v->breakdown_ctr = GB(r1, 24, 6) + 0xF;
		if (v->type == VEH_TRAIN) SetBit(Train::From(v)->First()->flags, VRF_CONSIST_BREAKDOWN);
		v->breakdown_delay = GB(r2, 0, 7) + 0x80;
		v->breakdown_chance = 0;
		DetermineBreakdownType(v, r2);
	}
}

/**
 * Handle all of the aspects of a vehicle breakdown
 * This includes adding smoke and sounds, and ending the breakdown when appropriate.
 * @return true iff the vehicle is stopped because of a breakdown
 * @note This function always returns false for aircraft, since these never stop for breakdowns
 */
bool Vehicle::HandleBreakdown()
{
	/* Possible states for Vehicle::breakdown_ctr
	 * 0  - vehicle is running normally
	 * 1  - vehicle is currently broken down
	 * 2  - vehicle is going to break down now
	 * >2 - vehicle is counting down to the actual breakdown event */
	switch (this->breakdown_ctr) {
		case 0:
			return false;

		case 2:
			this->breakdown_ctr = 1;

			if (this->breakdowns_since_last_service != 255) {
				this->breakdowns_since_last_service++;
			}

			if (this->type == VEH_AIRCRAFT) {
				this->MarkDirty();
				assert(this->breakdown_type <= BREAKDOWN_AIRCRAFT_EM_LANDING);
				/* Aircraft just need this flag, the rest is handled elsewhere */
				this->vehstatus |= VS_AIRCRAFT_BROKEN;
				if(this->breakdown_type == BREAKDOWN_AIRCRAFT_SPEED ||
						(this->current_order.IsType(OT_GOTO_DEPOT) &&
						(this->current_order.GetDepotOrderType() & ODTFB_BREAKDOWN) &&
						GetTargetAirportIfValid(Aircraft::From(this)) != nullptr)) return false;
				FindBreakdownDestination(Aircraft::From(this));
			} else if (this->type == VEH_TRAIN) {
				if (this->breakdown_type == BREAKDOWN_LOW_POWER ||
						this->First()->cur_speed <= ((this->breakdown_type == BREAKDOWN_LOW_SPEED) ? this->breakdown_severity : 0)) {
					switch (this->breakdown_type) {
						case BREAKDOWN_RV_CRASH:
							if (_settings_game.vehicle.improved_breakdowns) SetBit(Train::From(this)->flags, VRF_HAS_HIT_RV);
						/* FALL THROUGH */
						case BREAKDOWN_CRITICAL:
							if (!PlayVehicleSound(this, VSE_BREAKDOWN)) {
								SndPlayVehicleFx((_settings_game.game_creation.landscape != LandscapeType::Toyland) ? SND_10_BREAKDOWN_TRAIN_SHIP : SND_3A_BREAKDOWN_TRAIN_SHIP_TOYLAND, this);
							}
							if (!(this->vehstatus & VS_HIDDEN) && !EngInfo(this->engine_type)->misc_flags.Test(EngineMiscFlag::NoBreakdownSmoke) && this->breakdown_delay > 0) {
								EffectVehicle *u = CreateEffectVehicleRel(this, 4, 4, 5, EV_BREAKDOWN_SMOKE);
								if (u != nullptr) u->animation_state = this->breakdown_delay * 2;
							}
							/* Max Speed reduction*/
							if (_settings_game.vehicle.improved_breakdowns) {
								if (!HasBit(Train::From(this)->flags, VRF_NEED_REPAIR)) {
									SetBit(Train::From(this)->flags, VRF_NEED_REPAIR);
									Train::From(this)->critical_breakdown_count = 1;
								} else if (Train::From(this)->critical_breakdown_count != 255) {
									Train::From(this)->critical_breakdown_count++;
								}
								Train::From(this->First())->ConsistChanged(CCF_TRACK);
							}
						/* FALL THROUGH */
						case BREAKDOWN_EM_STOP:
							CheckBreakdownFlags(Train::From(this->First()));
							SetBit(Train::From(this->First())->flags, VRF_BREAKDOWN_STOPPED);
							break;
						case BREAKDOWN_BRAKE_OVERHEAT:
							CheckBreakdownFlags(Train::From(this->First()));
							SetBit(Train::From(this->First())->flags, VRF_BREAKDOWN_STOPPED);
							break;
						case BREAKDOWN_LOW_SPEED:
							CheckBreakdownFlags(Train::From(this->First()));
							SetBit(Train::From(this->First())->flags, VRF_BREAKDOWN_SPEED);
							break;
						case BREAKDOWN_LOW_POWER:
							SetBit(Train::From(this->First())->flags, VRF_BREAKDOWN_POWER);
							break;
						default: NOT_REACHED();
					}
					this->First()->MarkDirty();
					SetWindowDirty(WC_VEHICLE_VIEW, this->index);
					SetWindowDirty(WC_VEHICLE_DETAILS, this->index);
				} else {
					this->breakdown_ctr = 2; // wait until slowdown
					this->breakdowns_since_last_service--;
					SetBit(Train::From(this)->flags, VRF_BREAKDOWN_BRAKING);
					return false;
				}
				if ((!(this->vehstatus & VS_HIDDEN)) && (this->breakdown_type == BREAKDOWN_LOW_SPEED || this->breakdown_type == BREAKDOWN_LOW_POWER)
						&& !EngInfo(this->engine_type)->misc_flags.Test(EngineMiscFlag::NoBreakdownSmoke)) {
					EffectVehicle *u = CreateEffectVehicleRel(this, 0, 0, 2, EV_BREAKDOWN_SMOKE); //some grey clouds to indicate a broken engine
					if (u != nullptr) u->animation_state = 25;
				}
			} else {
				switch (this->breakdown_type) {
					case BREAKDOWN_CRITICAL:
						if (!PlayVehicleSound(this, VSE_BREAKDOWN)) {
							bool train_or_ship = this->type == VEH_TRAIN || this->type == VEH_SHIP;
							SndPlayVehicleFx((_settings_game.game_creation.landscape != LandscapeType::Toyland) ?
								(train_or_ship ? SND_10_BREAKDOWN_TRAIN_SHIP : SND_0F_BREAKDOWN_ROADVEHICLE) :
								(train_or_ship ? SND_3A_BREAKDOWN_TRAIN_SHIP_TOYLAND : SND_35_BREAKDOWN_ROADVEHICLE_TOYLAND), this);
						}
						if (!(this->vehstatus & VS_HIDDEN) && !EngInfo(this->engine_type)->misc_flags.Test(EngineMiscFlag::NoBreakdownSmoke) && this->breakdown_delay > 0) {
							EffectVehicle *u = CreateEffectVehicleRel(this, 4, 4, 5, EV_BREAKDOWN_SMOKE);
							if (u != nullptr) u->animation_state = this->breakdown_delay * 2;
						}
						if (_settings_game.vehicle.improved_breakdowns) {
							if (this->type == VEH_ROAD) {
								if (RoadVehicle::From(this)->critical_breakdown_count != 255) {
									RoadVehicle::From(this)->critical_breakdown_count++;
								}
							} else if (this->type == VEH_SHIP) {
								if (Ship::From(this)->critical_breakdown_count != 255) {
									Ship::From(this)->critical_breakdown_count++;
								}
							}
						}
					/* FALL THROUGH */
					case BREAKDOWN_EM_STOP:
						this->cur_speed = 0;
						break;
					case BREAKDOWN_LOW_SPEED:
					case BREAKDOWN_LOW_POWER:
						/* do nothing */
						break;
					default: NOT_REACHED();
				}
				if ((!(this->vehstatus & VS_HIDDEN)) &&
						(this->breakdown_type == BREAKDOWN_LOW_SPEED || this->breakdown_type == BREAKDOWN_LOW_POWER) &&
						!EngInfo(this->engine_type)->misc_flags.Test(EngineMiscFlag::NoBreakdownSmoke)) {
					/* Some gray clouds to indicate a broken RV */
					EffectVehicle *u = CreateEffectVehicleRel(this, 0, 0, 2, EV_BREAKDOWN_SMOKE);
					if (u != nullptr) u->animation_state = 25;
				}
				this->First()->MarkDirty();
				SetWindowDirty(WC_VEHICLE_VIEW, this->index);
				SetWindowDirty(WC_VEHICLE_DETAILS, this->index);
				return (this->breakdown_type == BREAKDOWN_CRITICAL || this->breakdown_type == BREAKDOWN_EM_STOP);
			}

			[[fallthrough]];
		case 1:
			/* Aircraft breakdowns end only when arriving at the airport */
			if (this->type == VEH_AIRCRAFT) return false;

			/* For trains this function is called twice per tick, so decrease v->breakdown_delay at half the rate */
			if ((this->tick_counter & (this->type == VEH_TRAIN ? 3 : 1)) == 0) {
				if (--this->breakdown_delay == 0) {
					this->breakdown_ctr = 0;
					if (this->type == VEH_TRAIN) {
						CheckBreakdownFlags(Train::From(this->First()));
						this->First()->MarkDirty();
						SetWindowDirty(WC_VEHICLE_VIEW, this->First()->index);
					} else {
						this->MarkDirty();
						SetWindowDirty(WC_VEHICLE_VIEW, this->index);
					}
				}
			}
			return (this->breakdown_type == BREAKDOWN_CRITICAL || this->breakdown_type == BREAKDOWN_EM_STOP ||
					this->breakdown_type == BREAKDOWN_RV_CRASH || this->breakdown_type == BREAKDOWN_BRAKE_OVERHEAT);

		default:
			if (!this->current_order.IsType(OT_LOADING)) this->breakdown_ctr--;
			return false;
	}
}

/**
 * Update economy age of a vehicle.
 * @param v Vehicle to update.
 */
void EconomyAgeVehicle(Vehicle *v)
{
	/* Stop if a virtual vehicle */
	if (HasBit(v->subtype, GVSF_VIRTUAL)) return;

	if (v->economy_age < EconTime::MAX_DATE.AsDelta()) {
		v->economy_age++;
		if (v->IsPrimaryVehicle() && v->economy_age == VEHICLE_PROFIT_MIN_AGE + 1) GroupStatistics::VehicleReachedMinAge(v);
	}
}

/**
 * Update age of a vehicle.
 * @param v Vehicle to update.
 */
void AgeVehicle(Vehicle *v)
{
	/* Stop if a virtual vehicle */
	if (HasBit(v->subtype, GVSF_VIRTUAL)) return;

	if (v->age < CalTime::MAX_DATE.AsDelta()) v->age++;

	if (!v->IsPrimaryVehicle() && (v->type != VEH_TRAIN || !Train::From(v)->IsEngine())) return;

	CalTime::DateDelta age = v->age - v->max_age;
	for (int i = 0; i <= 4; i++) {
		if (age == CalTime::DateAtStartOfYear(CalTime::Year{i}).AsDelta()) {
			v->reliability_spd_dec <<= 1;
			break;
		}
	}

	SetWindowDirty(WC_VEHICLE_DETAILS, v->index);

	/* Don't warn if warnings are disabled */
	if (!_settings_client.gui.old_vehicle_warn) return;

	/* Don't warn about vehicles which are non-primary (e.g., part of an articulated vehicle), don't belong to us, are crashed, or are stopped */
	if (v->Previous() != nullptr || v->owner != _local_company || (v->vehstatus & VS_CRASHED) != 0 || (v->vehstatus & VS_STOPPED) != 0) return;

	const Company *c = Company::Get(v->owner);
	/* Don't warn if a renew is active */
	if (c->settings.engine_renew && v->GetEngine()->company_avail.Any()) return;
	/* Don't warn if a replacement is active */
	if (EngineHasReplacementForCompany(c, v->engine_type, v->group_id)) return;

	StringID str;
	if (age == -DAYS_IN_LEAP_YEAR) {
		str = STR_NEWS_VEHICLE_IS_GETTING_OLD;
	} else if (age == 0) {
		str = STR_NEWS_VEHICLE_IS_GETTING_VERY_OLD;
	} else if (age > 0 && (age % DAYS_IN_LEAP_YEAR) == 0) {
		str = STR_NEWS_VEHICLE_IS_GETTING_VERY_OLD_AND;
	} else {
		return;
	}

	SetDParam(0, v->index);
	AddVehicleAdviceNewsItem(AdviceType::VehicleOld, str, v->index);
}

/**
 * Calculates how full a vehicle is.
 * @param front The front vehicle of the consist to check.
 * @param colour The string to show depending on if we are unloading or loading
 * @return A percentage of how full the Vehicle is.
 *         Percentages are rounded towards 50%, so that 0% and 100% are only returned
 *         if the vehicle is completely empty or full.
 *         This is useful for both display and conditional orders.
 */
uint8_t CalcPercentVehicleFilled(const Vehicle *front, StringID *colour)
{
	int count = 0;
	int max = 0;
	int cars = 0;
	int unloading = 0;
	bool loading = false;

	bool is_loading = front->current_order.IsType(OT_LOADING);

	/* The station may be nullptr when the (colour) string does not need to be set. */
	const Station *st = Station::GetIfValid(front->last_station_visited);
	assert(colour == nullptr || (st != nullptr && is_loading));

	bool order_no_load = is_loading && (front->current_order.GetLoadType() & OLFB_NO_LOAD);
	bool order_full_load = is_loading && (front->current_order.GetLoadType() & OLFB_FULL_LOAD);

	/* Count up max and used */
	for (const Vehicle *v = front; v != nullptr; v = v->Next()) {
		count += v->cargo.StoredCount();
		max += v->cargo_cap;
		if (v->cargo_cap != 0 && colour != nullptr) {
			unloading += HasBit(v->vehicle_flags, VF_CARGO_UNLOADING) ? 1 : 0;
			loading |= !order_no_load &&
					(order_full_load || st->goods[v->cargo_type].HasRating()) &&
					!HasBit(front->vehicle_flags, VF_LOADING_FINISHED) && !HasBit(front->vehicle_flags, VF_STOP_LOADING);
			cars++;
		}
	}

	if (colour != nullptr) {
		if (unloading == 0 && loading) {
			*colour = STR_PERCENT_UP;
		} else if (unloading == 0 && !loading) {
			*colour = STR_PERCENT_NONE;
		} else if (cars == unloading || !loading) {
			*colour = STR_PERCENT_DOWN;
		} else {
			*colour = STR_PERCENT_UP_DOWN;
		}
	}

	/* Train without capacity */
	if (max == 0) return 100;

	/* Return the percentage */
	if (count * 2 < max) {
		/* Less than 50%; round up, so that 0% means really empty. */
		return CeilDiv(count * 100, max);
	} else {
		/* More than 50%; round down, so that 100% means really full. */
		return (count * 100) / max;
	}
}

uint8_t CalcPercentVehicleFilledOfCargo(const Vehicle *front, CargoType cargo)
{
	int count = 0;
	int max = 0;

	/* Count up max and used */
	for (const Vehicle *v = front; v != nullptr; v = v->Next()) {
		if (v->cargo_type != cargo) continue;
		count += v->cargo.StoredCount();
		max += v->cargo_cap;
	}

	/* Train without capacity */
	if (max == 0) return 0;

	/* Return the percentage */
	if (count * 2 < max) {
		/* Less than 50%; round up, so that 0% means really empty. */
		return CeilDiv(count * 100, max);
	} else {
		/* More than 50%; round down, so that 100% means really full. */
		return (count * 100) / max;
	}
}

/**
 * Vehicle entirely entered the depot, update its status, orders, vehicle windows, service it, etc.
 * @param v Vehicle that entered a depot.
 */
void VehicleEnterDepot(Vehicle *v)
{
	/* Always work with the front of the vehicle */
	dbg_assert(v == v->First());

	switch (v->type) {
		case VEH_TRAIN: {
			Train *t = Train::From(v);
			/* Clear path reservation */
			SetDepotReservation(t->tile, false);
			if (_settings_client.gui.show_track_reservation) MarkTileDirtyByTile(t->tile, VMDF_NOT_MAP_MODE);

			UpdateSignalsOnSegment(t->tile, INVALID_DIAGDIR, t->owner);
			t->wait_counter = 0;
			t->force_proceed = TFP_NONE;
			ClrBit(t->flags, VRF_TOGGLE_REVERSE);
			t->ConsistChanged(CCF_ARRANGE);
			t->reverse_distance = 0;
			t->UpdateTrainSpeedAdaptationLimit(0);
			t->lookahead.reset();
			if (!(t->vehstatus & VS_CRASHED)) {
				t->crash_anim_pos = 0;
			}
			break;
		}

		case VEH_ROAD:
			break;

		case VEH_SHIP: {
			Ship *ship = Ship::From(v);
			ship->state = TRACK_BIT_DEPOT;
			ship->UpdateCache();
			ship->UpdateViewport(true, true);
			SetWindowDirty(WC_VEHICLE_DEPOT, v->tile.base());
			break;
		}

		case VEH_AIRCRAFT:
			HandleAircraftEnterHangar(Aircraft::From(v));
			break;
		default: NOT_REACHED();
	}
	SetWindowDirty(WC_VEHICLE_VIEW, v->index);
	DirtyVehicleListWindowForVehicle(v);

	if (v->type != VEH_TRAIN) {
		/* Trains update the vehicle list when the first unit enters the depot and calls VehicleEnterDepot() when the last unit enters.
		 * We only increase the number of vehicles when the first one enters, so we will not need to search for more vehicles in the depot */
		InvalidateWindowData(WC_VEHICLE_DEPOT, v->tile.base());
	}
	SetWindowDirty(WC_VEHICLE_DEPOT, v->tile.base());

	v->vehstatus |= VS_HIDDEN;
	v->UpdateIsDrawn();
	v->cur_speed = 0;

	VehicleServiceInDepot(v);

	/* After a vehicle trigger, the graphics and properties of the vehicle could change. */
	TriggerVehicle(v, VEHICLE_TRIGGER_DEPOT);
	v->MarkDirty();

	InvalidateWindowData(WC_VEHICLE_VIEW, v->index);

	if (v->current_order.IsType(OT_GOTO_DEPOT)) {
		SetWindowDirty(WC_VEHICLE_VIEW, v->index);

		const Order *real_order = v->GetOrder(v->cur_real_order_index);

		/* Test whether we are heading for this depot. If not, do nothing.
		 * Note: The target depot for nearest-/manual-depot-orders is only updated on junctions, but we want to accept every depot. */
		if ((v->current_order.GetDepotOrderType() & ODTFB_PART_OF_ORDERS) &&
				real_order != nullptr && !(real_order->GetDepotActionType() & ODATFB_NEAREST_DEPOT) &&
				(v->type == VEH_AIRCRAFT ? v->current_order.GetDestination() != GetStationIndex(v->tile) : v->dest_tile != v->tile)) {
			/* We are heading for another depot, keep driving. */
			return;
		}

		/* Test whether we are heading for this depot. If not, do nothing. */
		if ((v->current_order.GetDepotExtraFlags() & ODEFB_SPECIFIC) &&
				(v->type == VEH_AIRCRAFT ? v->current_order.GetDestination() != GetStationIndex(v->tile) : v->dest_tile != v->tile)) {
			/* We are heading for another depot, keep driving. */
			return;
		}

		if (v->current_order.GetDepotActionType() & ODATFB_SELL) {
			_vehicles_to_sell.insert(v->index);
			return;
		}

		if (v->current_order.IsRefit()) {
			Backup<CompanyID> cur_company(_current_company, v->owner, FILE_LINE);
			CommandCost cost = Command<CMD_REFIT_VEHICLE>::Do(DC_EXEC, v->index, v->current_order.GetRefitCargo(), 0xFF, false, false, 0);
			cur_company.Restore();

			if (cost.Failed()) {
				_vehicles_to_autoreplace[v->index] = false;
				if (v->owner == _local_company) {
					/* Notify the user that we stopped the vehicle */
					SetDParam(0, v->index);
					AddVehicleAdviceNewsItem(AdviceType::RefitFailed, STR_NEWS_ORDER_REFIT_FAILED, v->index);
				}
			} else if (cost.GetCost() != 0) {
				v->profit_this_year -= cost.GetCost() << 8;
				if (v->owner == _local_company) {
					ShowCostOrIncomeAnimation(v->x_pos, v->y_pos, v->z_pos, cost.GetCost());
				}
			}
		}

		/* Handle the ODTFB_PART_OF_ORDERS case. If there is a timetabled wait time, hold the train, otherwise skip to the next order.
		Note that if there is a only a travel_time, but no wait_time defined for the order, and the train arrives to the depot sooner as scheduled,
		he doesn't wait in it, as it would in stations. Thus, the original behaviour is maintained if there's no defined wait_time.*/
		if (v->current_order.GetDepotOrderType() & ODTFB_PART_OF_ORDERS) {
			v->DeleteUnreachedImplicitOrders();
			UpdateVehicleTimetable(v, true);
			if (v->current_order.IsWaitTimetabled() && !(v->current_order.GetDepotActionType() & ODATFB_HALT)) {
				v->current_order.MakeWaiting();
				v->current_order.SetNonStopType(ONSF_NO_STOP_AT_ANY_STATION);
				return;
			} else {
				v->IncrementImplicitOrderIndex();
			}
		}

		if (v->current_order.GetDepotActionType() & ODATFB_HALT) {
			/* Vehicles are always stopped on entering depots. Do not restart this one. */
			_vehicles_to_autoreplace[v->index] = false;
			/* Invalidate last_loading_station. As the link from the station
			 * before the stop to the station after the stop can't be predicted
			 * we shouldn't construct it when the vehicle visits the next stop. */
			v->last_loading_station = INVALID_STATION;
			ClrBit(v->vehicle_flags, VF_LAST_LOAD_ST_SEP);

			/* Clear unbunching data. */
			v->ResetDepotUnbunching();

			/* Announce that the vehicle is waiting to players and AIs. */
			if (v->owner == _local_company) {
				SetDParam(0, v->index);
				AddVehicleAdviceNewsItem(AdviceType::VehicleWaiting, STR_NEWS_TRAIN_IS_WAITING + v->type, v->index);
			}
			AI::NewEvent(v->owner, new ScriptEventVehicleWaitingInDepot(v->index));
		}

		/* If we've entered our unbunching depot, record the round trip duration. */
		if (v->current_order.GetDepotActionType() & ODATFB_UNBUNCH && v->unbunch_state != nullptr && v->unbunch_state->depot_unbunching_last_departure != INVALID_STATE_TICKS) {
			Ticks measured_round_trip = (_state_ticks - v->unbunch_state->depot_unbunching_last_departure).AsTicks();
			Ticks &rtt = v->unbunch_state->round_trip_time;
			if (rtt == 0) {
				/* This might be our first round trip. */
				rtt = measured_round_trip;
			} else {
				/* If we have a previous trip, smooth the effects of outlier trip calculations caused by jams or other interference. */
				rtt = Clamp(measured_round_trip, (rtt / 2), ClampTo<Ticks>(rtt * 2));
			}
		}

		v->current_order.MakeDummy();
	}
}

/**
 * Update the vehicle on the viewport, updating the right hash and setting the
 *  new coordinates.
 * @param dirty Mark the (new and old) coordinates of the vehicle as dirty.
 */
void Vehicle::UpdateViewport(bool dirty)
{
	/* Skip updating sprites on dedicated servers without screen */
	if (IsHeadless()) return;

	Rect new_coord = ConvertRect<Rect16, Rect>(this->sprite_seq_bounds);

	Point pt = RemapCoords(this->x_pos + this->x_offs, this->y_pos + this->y_offs, this->z_pos);
	new_coord.left   += pt.x;
	new_coord.top    += pt.y;
	new_coord.right  += pt.x + 2 * ZOOM_BASE;
	new_coord.bottom += pt.y + 2 * ZOOM_BASE;

	UpdateVehicleViewportHash(this, new_coord.left, new_coord.top);

	Rect old_coord = this->coord;
	this->coord = new_coord;

	if (dirty) {
		if (old_coord.left == INVALID_COORD) {
			this->MarkAllViewportsDirty();
		} else {
			::MarkAllViewportsDirty(
					std::min(old_coord.left,   this->coord.left),
					std::min(old_coord.top,    this->coord.top),
					std::max(old_coord.right,  this->coord.right),
					std::max(old_coord.bottom, this->coord.bottom),
					VMDF_NOT_LANDSCAPE | (this->type != VEH_EFFECT ? VMDF_NONE : VMDF_NOT_MAP_MODE)
			);
		}
	}
}

void Vehicle::UpdateViewportDeferred()
{
	Rect new_coord = ConvertRect<Rect16, Rect>(this->sprite_seq_bounds);

	Point pt = RemapCoords(this->x_pos + this->x_offs, this->y_pos + this->y_offs, this->z_pos);
	new_coord.left   += pt.x;
	new_coord.top    += pt.y;
	new_coord.right  += pt.x + 2 * ZOOM_BASE;
	new_coord.bottom += pt.y + 2 * ZOOM_BASE;

	UpdateVehicleViewportHashDeferred(this, new_coord.left, new_coord.top);

	this->coord = new_coord;
}

/**
 * Update the position of the vehicle, and update the viewport.
 */
void Vehicle::UpdatePositionAndViewport()
{
	this->UpdatePosition();
	this->UpdateViewport(true);
}

/**
 * Marks viewports dirty where the vehicle's image is.
 */
void Vehicle::MarkAllViewportsDirty() const
{
	::MarkAllViewportsDirty(this->coord.left, this->coord.top, this->coord.right, this->coord.bottom, VMDF_NOT_LANDSCAPE | (this->type != VEH_EFFECT ? VMDF_NONE : VMDF_NOT_MAP_MODE));
}

VehicleOrderID Vehicle::GetFirstWaitingLocation(bool require_wait_timetabled) const
{
	for (int i = 0; i < this->GetNumOrders(); ++i) {
		const Order* order = this->GetOrder(i);

		if (order->IsWaitTimetabled() && !order->IsType(OT_IMPLICIT) && !order->IsType(OT_CONDITIONAL)) {
			return i;
		}
		if (order->IsType(OT_GOTO_STATION)) {
			return (order->IsWaitTimetabled() || !require_wait_timetabled) ? i : INVALID_VEH_ORDER_ID;
		}
	}
	return INVALID_VEH_ORDER_ID;
}

/**
 * Get position information of a vehicle when moving one pixel in the direction it is facing
 * @param v Vehicle to move
 * @return Position information after the move
 */
GetNewVehiclePosResult GetNewVehiclePos(const Vehicle *v)
{
	static const int8_t _delta_coord[16] = {
		-1,-1,-1, 0, 1, 1, 1, 0, /* x */
		-1, 0, 1, 1, 1, 0,-1,-1, /* y */
	};

	int x = v->x_pos + _delta_coord[v->direction];
	int y = v->y_pos + _delta_coord[v->direction + 8];

	GetNewVehiclePosResult gp;
	gp.x = x;
	gp.y = y;
	gp.old_tile = v->tile;
	gp.new_tile = TileVirtXY(x, y);
	return gp;
}

static const Direction _new_direction_table[] = {
	DIR_N,  DIR_NW, DIR_W,
	DIR_NE, DIR_SE, DIR_SW,
	DIR_E,  DIR_SE, DIR_S
};

Direction GetDirectionTowards(const Vehicle *v, int x, int y)
{
	int i = 0;

	if (y >= v->y_pos) {
		if (y != v->y_pos) i += 3;
		i += 3;
	}

	if (x >= v->x_pos) {
		if (x != v->x_pos) i++;
		i++;
	}

	Direction dir = v->direction;

	DirDiff dirdiff = DirDifference(_new_direction_table[i], dir);
	if (dirdiff == DIRDIFF_SAME) return dir;
	return ChangeDir(dir, dirdiff > DIRDIFF_REVERSE ? DIRDIFF_45LEFT : DIRDIFF_45RIGHT);
}

/**
 * Call the tile callback function for a vehicle entering a tile
 * @param v    Vehicle entering the tile
 * @param tile Tile entered
 * @param x    X position
 * @param y    Y position
 * @return Some meta-data over the to be entered tile.
 * @see VehicleEnterTileStatus to see what the bits in the return value mean.
 */
VehicleEnterTileStatus VehicleEnterTile(Vehicle *v, TileIndex tile, int x, int y)
{
	return _tile_type_procs[GetTileType(tile)]->vehicle_enter_tile_proc(v, tile, x, y);
}

/**
 * Find first unused unit number.
 * This does not mark the unit number as used.
 * @returns First unused unit number.
 */
UnitID FreeUnitIDGenerator::NextID() const
{
	for (auto it = std::begin(this->used_bitmap); it != std::end(this->used_bitmap); ++it) {
		BitmapStorage available = ~(*it);
		if (available == 0) continue;
		return static_cast<UnitID>(std::distance(std::begin(this->used_bitmap), it) * BITMAP_SIZE + FindFirstBit(available) + 1);
	}
	return static_cast<UnitID>(this->used_bitmap.size() * BITMAP_SIZE + 1);
}

/**
 * Use a unit number. If the unit number is not valid it is ignored.
 * @param index Unit number to use.
 * @returns Unit number used.
 */
UnitID FreeUnitIDGenerator::UseID(UnitID index)
{
	if (index == 0 || index == UINT16_MAX) return index;

	index--;

	size_t slot = index / BITMAP_SIZE;
	if (slot >= this->used_bitmap.size()) this->used_bitmap.resize(slot + 1);
	SetBit(this->used_bitmap[index / BITMAP_SIZE], index % BITMAP_SIZE);

	return index + 1;
}

/**
 * Release a unit number. If the unit number is not valid it is ignored.
 * @param index Unit number to release.
 */
void FreeUnitIDGenerator::ReleaseID(UnitID index)
{
	if (index == 0 || index == UINT16_MAX) return;

	index--;

	assert(index / BITMAP_SIZE < this->used_bitmap.size());
	ClrBit(this->used_bitmap[index / BITMAP_SIZE], index % BITMAP_SIZE);
}

/**
 * Get an unused unit number for a vehicle (if allowed).
 * @param type Type of vehicle
 * @return A unused unit number for the given type of vehicle if it is allowed to build one, else \c UINT16_MAX.
 */
UnitID GetFreeUnitNumber(VehicleType type)
{
	/* Check whether it is allowed to build another vehicle. */
	uint max_veh;
	switch (type) {
		case VEH_TRAIN:    max_veh = _settings_game.vehicle.max_trains;   break;
		case VEH_ROAD:     max_veh = _settings_game.vehicle.max_roadveh;  break;
		case VEH_SHIP:     max_veh = _settings_game.vehicle.max_ships;    break;
		case VEH_AIRCRAFT: max_veh = _settings_game.vehicle.max_aircraft; break;
		default: NOT_REACHED();
	}

	const Company *c = Company::Get(_current_company);
	if (c->group_all[type].num_vehicle >= max_veh) return UINT16_MAX; // Currently already at the limit, no room to make a new one.

	return c->freeunits[type].NextID();
}


/**
 * Check whether we can build infrastructure for the given
 * vehicle type. This to disable building stations etc. when
 * you are not allowed/able to have the vehicle type yet.
 * @param type the vehicle type to check this for
 * @return true if there is any reason why you may build
 *         the infrastructure for the given vehicle type
 */
bool CanBuildVehicleInfrastructure(VehicleType type, uint8_t subtype)
{
	assert(IsCompanyBuildableVehicleType(type));

	if (!Company::IsValidID(_local_company)) return false;

	UnitID max;
	switch (type) {
		case VEH_TRAIN:
			if (!HasAnyRailTypesAvail(_local_company)) return false;
			max = _settings_game.vehicle.max_trains;
			break;
		case VEH_ROAD:
			if (!HasAnyRoadTypesAvail(_local_company, (RoadTramType)subtype)) return false;
			max = _settings_game.vehicle.max_roadveh;
			break;
		case VEH_SHIP:     max = _settings_game.vehicle.max_ships; break;
		case VEH_AIRCRAFT: max = _settings_game.vehicle.max_aircraft; break;
		default: NOT_REACHED();
	}

	/* We can build vehicle infrastructure when we may build the vehicle type */
	if (max > 0) {
		/* Can we actually build the vehicle type? */
		for (const Engine *e : Engine::IterateType(type)) {
			if (type == VEH_ROAD && GetRoadTramType(e->u.road.roadtype) != (RoadTramType)subtype) continue;
			if (e->company_avail.Test(_local_company)) return true;
		}
		return false;
	}

	/* We should be able to build infrastructure when we have the actual vehicle type */
	for (const Vehicle *v : Vehicle::IterateType(type)) {
		if (type == VEH_ROAD && GetRoadTramType(RoadVehicle::From(v)->roadtype) != (RoadTramType)subtype) continue;
		if (v->owner == _local_company) return true;
	}

	return false;
}


/**
 * Determines the #LiveryScheme for a vehicle.
 * @param engine_type Engine of the vehicle.
 * @param parent_engine_type Engine of the front vehicle, #INVALID_ENGINE if vehicle is at front itself.
 * @param v the vehicle, \c nullptr if in purchase list etc.
 * @return livery scheme to use.
 */
LiveryScheme GetEngineLiveryScheme(EngineID engine_type, EngineID parent_engine_type, const Vehicle *v)
{
	CargoType cargo_type = v == nullptr ? INVALID_CARGO : v->cargo_type;
	const Engine *e = Engine::Get(engine_type);
	switch (e->type) {
		default: NOT_REACHED();
		case VEH_TRAIN:
			if (v != nullptr && parent_engine_type != INVALID_ENGINE && (UsesWagonOverride(v) || (v->IsArticulatedPart() && e->u.rail.railveh_type != RAILVEH_WAGON))) {
				/* Wagonoverrides use the colour scheme of the front engine.
				 * Articulated parts use the colour scheme of the first part. (Not supported for articulated wagons) */
				engine_type = parent_engine_type;
				e = Engine::Get(engine_type);
				/* Note: Luckily cargo_type is not needed for engines */
			}

			if (!IsValidCargoType(cargo_type)) cargo_type = e->GetDefaultCargoType();
			if (!IsValidCargoType(cargo_type)) cargo_type = GetCargoTypeByLabel(CT_GOODS); // The vehicle does not carry anything, let's pick some freight cargo
			assert(IsValidCargoType(cargo_type));
			if (e->u.rail.railveh_type == RAILVEH_WAGON) {
				if (!CargoSpec::Get(cargo_type)->is_freight) {
					if (parent_engine_type == INVALID_ENGINE) {
						return LS_PASSENGER_WAGON_STEAM;
					} else {
						bool is_mu = EngInfo(parent_engine_type)->misc_flags.Test(EngineMiscFlag::RailIsMU);
						switch (RailVehInfo(parent_engine_type)->engclass) {
							default: NOT_REACHED();
							case EC_STEAM:    return LS_PASSENGER_WAGON_STEAM;
							case EC_DIESEL:   return is_mu ? LS_DMU : LS_PASSENGER_WAGON_DIESEL;
							case EC_ELECTRIC: return is_mu ? LS_EMU : LS_PASSENGER_WAGON_ELECTRIC;
							case EC_MONORAIL: return LS_PASSENGER_WAGON_MONORAIL;
							case EC_MAGLEV:   return LS_PASSENGER_WAGON_MAGLEV;
						}
					}
				} else {
					return LS_FREIGHT_WAGON;
				}
			} else {
				bool is_mu = e->info.misc_flags.Test(EngineMiscFlag::RailIsMU);

				switch (e->u.rail.engclass) {
					default: NOT_REACHED();
					case EC_STEAM:    return LS_STEAM;
					case EC_DIESEL:   return is_mu ? LS_DMU : LS_DIESEL;
					case EC_ELECTRIC: return is_mu ? LS_EMU : LS_ELECTRIC;
					case EC_MONORAIL: return LS_MONORAIL;
					case EC_MAGLEV:   return LS_MAGLEV;
				}
			}

		case VEH_ROAD:
			/* Always use the livery of the front */
			if (v != nullptr && parent_engine_type != INVALID_ENGINE) {
				engine_type = parent_engine_type;
				e = Engine::Get(engine_type);
				cargo_type = v->First()->cargo_type;
			}
			if (!IsValidCargoType(cargo_type)) cargo_type = e->GetDefaultCargoType();
			if (!IsValidCargoType(cargo_type)) cargo_type = GetCargoTypeByLabel(CT_GOODS); // The vehicle does not carry anything, let's pick some freight cargo
			assert(IsValidCargoType(cargo_type));

			/* Important: Use Tram Flag of front part. Luckily engine_type refers to the front part here. */
			if (e->info.misc_flags.Test(EngineMiscFlag::RoadIsTram)) {
				/* Tram */
				return IsCargoInClass(cargo_type, CC_PASSENGERS) ? LS_PASSENGER_TRAM : LS_FREIGHT_TRAM;
			} else {
				/* Bus or truck */
				return IsCargoInClass(cargo_type, CC_PASSENGERS) ? LS_BUS : LS_TRUCK;
			}

		case VEH_SHIP:
			if (!IsValidCargoType(cargo_type)) cargo_type = e->GetDefaultCargoType();
			if (!IsValidCargoType(cargo_type)) cargo_type = GetCargoTypeByLabel(CT_GOODS); // The vehicle does not carry anything, let's pick some freight cargo
			assert(IsValidCargoType(cargo_type));
			return IsCargoInClass(cargo_type, CC_PASSENGERS) ? LS_PASSENGER_SHIP : LS_FREIGHT_SHIP;

		case VEH_AIRCRAFT:
			switch (e->u.air.subtype) {
				case AIR_HELI: return LS_HELICOPTER;
				case AIR_CTOL: return LS_SMALL_PLANE;
				case AIR_CTOL | AIR_FAST: return LS_LARGE_PLANE;
				default: NOT_REACHED();
			}
	}
}

/**
 * Determines the livery for a vehicle.
 * @param engine_type EngineID of the vehicle
 * @param company Owner of the vehicle
 * @param parent_engine_type EngineID of the front vehicle. INVALID_VEHICLE if vehicle is at front itself.
 * @param v the vehicle. nullptr if in purchase list etc.
 * @param livery_setting The livery settings to use for acquiring the livery information.
 * @param ignore_group Ignore group overrides.
 * @return livery to use
 */
const Livery *GetEngineLivery(EngineID engine_type, CompanyID company, EngineID parent_engine_type, const Vehicle *v, uint8_t livery_setting, bool ignore_group)
{
	const Company *c = Company::Get(company);
	LiveryScheme scheme = LS_DEFAULT;

	if (livery_setting == LIT_ALL || (livery_setting == LIT_COMPANY && company == _local_company)) {
		if (v != nullptr && !ignore_group) {
			const Group *g = Group::GetIfValid(v->First()->group_id);
			if (g != nullptr) {
				/* Traverse parents until we find a livery or reach the top */
				while (g->livery.in_use == 0 && g->parent != INVALID_GROUP) {
					g = Group::Get(g->parent);
				}
				if (g->livery.in_use != 0) return &g->livery;
			}
		}

		/* The default livery is always available for use, but its in_use flag determines
		 * whether any _other_ liveries are in use. */
		if (c->livery[LS_DEFAULT].in_use != 0) {
			/* Determine the livery scheme to use */
			scheme = GetEngineLiveryScheme(engine_type, parent_engine_type, v);
		}
	}

	return &c->livery[scheme];
}


static PaletteID GetEngineColourMap(EngineID engine_type, CompanyID company, EngineID parent_engine_type, const Vehicle *v, bool ignore_group = false)
{
	PaletteID map = (v != nullptr && !ignore_group) ? v->colourmap : PAL_NONE;

	/* Return cached value if any */
	if (map != PAL_NONE) return map;

	const Engine *e = Engine::Get(engine_type);

	/* Check if we should use the colour map callback */
	if (e->info.callback_mask.Test(VehicleCallbackMask::ColourRemap)) {
		uint16_t callback = GetVehicleCallback(CBID_VEHICLE_COLOUR_MAPPING, 0, 0, engine_type, v);
		/* Failure means "use the default two-colour" */
		if (callback != CALLBACK_FAILED) {
			static_assert(PAL_NONE == 0); // Returning 0x4000 (resp. 0xC000) coincidences with default value (PAL_NONE)
			map = GB(callback, 0, 14);
			/* If bit 14 is set, then the company colours are applied to the
			 * map else it's returned as-is. */
			if (!HasBit(callback, 14)) {
				/* Update cache */
				if (v != nullptr) const_cast<Vehicle *>(v)->colourmap = map;
				return map;
			}
		}
	}

	bool twocc = e->info.misc_flags.Test(EngineMiscFlag::Uses2CC);

	if (map == PAL_NONE) map = twocc ? (PaletteID)SPR_2CCMAP_BASE : (PaletteID)PALETTE_RECOLOUR_START;

	/* Spectator has news shown too, but has invalid company ID - as well as dedicated server */
	if (!Company::IsValidID(company)) return map;

	const Livery *livery = GetEngineLivery(engine_type, company, parent_engine_type, v, _settings_client.gui.liveries, ignore_group);

	map += livery->colour1;
	if (twocc) map += livery->colour2 * 16;

	/* Update cache */
	if (v != nullptr && !ignore_group) const_cast<Vehicle *>(v)->colourmap = map;
	return map;
}

/**
 * Get the colour map for an engine. This used for unbuilt engines in the user interface.
 * @param engine_type ID of engine
 * @param company ID of company
 * @return A ready-to-use palette modifier
 */
PaletteID GetEnginePalette(EngineID engine_type, CompanyID company)
{
	return GetEngineColourMap(engine_type, company, INVALID_ENGINE, nullptr);
}

/**
 * Get the colour map for a vehicle.
 * @param v Vehicle to get colour map for
 * @return A ready-to-use palette modifier
 */
PaletteID GetVehiclePalette(const Vehicle *v)
{
	if (v->IsGroundVehicle()) {
		return GetEngineColourMap(v->engine_type, v->owner, v->GetGroundVehicleCache()->first_engine, v);
	}

	return GetEngineColourMap(v->engine_type, v->owner, INVALID_ENGINE, v);
}

/**
 * Get the uncached colour map for a train, ignoring the vehicle's group.
 * @param v Vehicle to get colour map for
 * @return A ready-to-use palette modifier
 */
PaletteID GetUncachedTrainPaletteIgnoringGroup(const Train *v)
{
	return GetEngineColourMap(v->engine_type, v->owner, v->GetGroundVehicleCache()->first_engine, v, true);
}

/**
 * Delete all implicit orders which were not reached.
 */
void Vehicle::DeleteUnreachedImplicitOrders()
{
	if (this->IsGroundVehicle()) {
		uint16_t &gv_flags = this->GetGroundVehicleFlags();
		if (HasBit(gv_flags, GVF_SUPPRESS_IMPLICIT_ORDERS)) {
			/* Do not delete orders, only skip them */
			ClrBit(gv_flags, GVF_SUPPRESS_IMPLICIT_ORDERS);
			this->cur_implicit_order_index = this->cur_real_order_index;
			if (this->cur_timetable_order_index != this->cur_real_order_index) {
				Order *real_timetable_order = this->cur_timetable_order_index != INVALID_VEH_ORDER_ID ? this->GetOrder(this->cur_timetable_order_index) : nullptr;
				if (real_timetable_order == nullptr || !real_timetable_order->IsType(OT_CONDITIONAL)) {
					/* Timetable order ID was not the real order or a conditional order, to avoid updating the wrong timetable, just clear the timetable index */
					this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
				}
			}
			InvalidateVehicleOrder(this, 0);
			return;
		}
	}

	const Order *order = this->GetOrder(this->cur_implicit_order_index);
	while (order != nullptr) {
		if (this->cur_implicit_order_index == this->cur_real_order_index) break;

		if (order->IsType(OT_IMPLICIT)) {
			DeleteOrder(this, this->cur_implicit_order_index);
			/* DeleteOrder does various magic with order_indices, so resync 'order' with 'cur_implicit_order_index' */
		} else {
			/* Skip non-implicit orders, e.g. service-orders */
			this->cur_implicit_order_index++;
		}

		/* Wrap around */
		if (this->cur_implicit_order_index >= this->orders->GetNumOrders()) this->cur_implicit_order_index = 0;

		order = this->GetOrder(this->cur_implicit_order_index);
	}
}

/**
 * Increase capacity for all link stats associated with vehicles in the given consist.
 * @param st Station to get the link stats from.
 * @param front First vehicle in the consist.
 * @param next_station_id Station the consist will be travelling to next.
 */
static void VehicleIncreaseStats(const Vehicle *front)
{
	for (const Vehicle *v = front; v != nullptr; v = v->Next()) {
		StationID last_loading_station = HasBit(front->vehicle_flags, VF_LAST_LOAD_ST_SEP) ? v->last_loading_station : front->last_loading_station;
		StateTicks loading_tick = HasBit(front->vehicle_flags, VF_LAST_LOAD_ST_SEP) ? v->last_loading_tick : front->last_loading_tick;
		if (v->refit_cap > 0 &&
				last_loading_station != INVALID_STATION &&
				last_loading_station != front->last_station_visited &&
				((front->current_order.GetCargoLoadType(v->cargo_type) & OLFB_NO_LOAD) == 0 ||
				(front->current_order.GetCargoUnloadType(v->cargo_type) & OUFB_NO_UNLOAD) == 0)) {
			/* The cargo count can indeed be higher than the refit_cap if
			 * wagons have been auto-replaced and subsequently auto-
			 * refitted to a higher capacity. The cargo gets redistributed
			 * among the wagons in that case.
			 * As usage is not such an important figure anyway we just
			 * ignore the additional cargo then.*/
			EdgeUpdateMode restricted_mode = EUM_INCREASE;
			if (v->type == VEH_AIRCRAFT) restricted_mode |= EUM_AIRCRAFT;
			IncreaseStats(Station::Get(last_loading_station), v->cargo_type, front->last_station_visited, v->refit_cap,
				std::min<uint>(v->refit_cap, v->cargo.StoredCount()), (_state_ticks - loading_tick).AsTicksT<uint32_t>(), restricted_mode);
		}
	}
}

/**
 * Prepare everything to begin the loading when arriving at a station.
 * @pre IsTileType(this->tile, MP_STATION) || this->type == VEH_SHIP.
 */
void Vehicle::BeginLoading()
{
	if (this->type == VEH_TRAIN) {
		assert_tile(IsTileType(Train::From(this)->GetStationLoadingVehicle()->tile, MP_STATION), Train::From(this)->GetStationLoadingVehicle()->tile);
	} else {
		assert_tile(IsTileType(this->tile, MP_STATION) || this->type == VEH_SHIP, this->tile);
	}

	bool no_load_prepare = false;
	if (this->current_order.IsType(OT_GOTO_STATION) &&
			this->current_order.GetDestination() == this->last_station_visited) {
		this->DeleteUnreachedImplicitOrders();

		/* Now both order indices point to the destination station, and we can start loading */
		this->current_order.MakeLoading(true);
		UpdateVehicleTimetable(this, true);

		/* Furthermore add the Non Stop flag to mark that this station
		 * is the actual destination of the vehicle, which is (for example)
		 * necessary to be known for HandleTrainLoading to determine
		 * whether the train is lost or not; not marking a train lost
		 * that arrives at random stations is bad. */
		this->current_order.SetNonStopType(ONSF_NO_STOP_AT_ANY_STATION);
	} else if (this->current_order.IsType(OT_LOADING_ADVANCE)) {
		this->current_order.MakeLoading(true);
		this->current_order.SetNonStopType(ONSF_NO_STOP_AT_ANY_STATION);
		no_load_prepare = true;
	} else {
		/* We weren't scheduled to stop here. Insert an implicit order
		 * to show that we are stopping here.
		 * While only groundvehicles have implicit orders, e.g. aircraft might still enter
		 * the 'wrong' terminal when skipping orders etc. */
		Order *in_list = this->GetOrder(this->cur_implicit_order_index);
		if (this->IsGroundVehicle() &&
				(in_list == nullptr || !in_list->IsType(OT_IMPLICIT) ||
				in_list->GetDestination() != this->last_station_visited)) {
			bool suppress_implicit_orders = HasBit(this->GetGroundVehicleFlags(), GVF_SUPPRESS_IMPLICIT_ORDERS);
			/* Do not create consecutive duplicates of implicit orders */
			const Order *prev_order = this->cur_implicit_order_index > 0 ? this->GetOrder(this->cur_implicit_order_index - 1) : (this->GetNumOrders() > 1 ? this->GetLastOrder() : nullptr);
			if (prev_order == nullptr ||
					(!prev_order->IsType(OT_IMPLICIT) && !prev_order->IsType(OT_GOTO_STATION)) ||
					prev_order->GetDestination() != this->last_station_visited) {

				/* Prefer deleting implicit orders instead of inserting new ones,
				 * so test whether the right order follows later. In case of only
				 * implicit orders treat the last order in the list like an
				 * explicit one, except if the overall number of orders surpasses
				 * IMPLICIT_ORDER_ONLY_CAP. */
				int target_index = this->cur_implicit_order_index;
				bool found = false;
				while (target_index != this->cur_real_order_index || this->GetNumManualOrders() == 0) {
					const Order *order = this->GetOrder(target_index);
					if (order == nullptr) break; // No orders.
					if (order->IsType(OT_IMPLICIT) && order->GetDestination() == this->last_station_visited) {
						found = true;
						break;
					}
					target_index++;
					if (target_index >= this->orders->GetNumOrders()) {
						if (this->GetNumManualOrders() == 0 &&
								this->GetNumOrders() < IMPLICIT_ORDER_ONLY_CAP) {
							break;
						}
						target_index = 0;
					}
					if (target_index == this->cur_implicit_order_index) break; // Avoid infinite loop.
				}

				if (found) {
					if (suppress_implicit_orders) {
						/* Skip to the found order */
						this->cur_implicit_order_index = target_index;
						InvalidateVehicleOrder(this, 0);
					} else {
						/* Delete all implicit orders up to the station we just reached */
						const Order *order = this->GetOrder(this->cur_implicit_order_index);
						while (!order->IsType(OT_IMPLICIT) || order->GetDestination() != this->last_station_visited) {
							if (order->IsType(OT_IMPLICIT)) {
								DeleteOrder(this, this->cur_implicit_order_index);
								/* DeleteOrder does various magic with order_indices, so resync 'order' with 'cur_implicit_order_index' */
							} else {
								/* Skip non-implicit orders, e.g. service-orders */
								this->cur_implicit_order_index++;
							}

							/* Wrap around */
							if (this->cur_implicit_order_index >= this->orders->GetNumOrders()) this->cur_implicit_order_index = 0;

							order = this->GetOrder(this->cur_implicit_order_index);

							assert(order != nullptr);
						}
					}
				} else if (!suppress_implicit_orders &&
						((this->orders == nullptr ? OrderList::CanAllocateItem() : this->orders->GetNumOrders() < MAX_VEH_ORDER_ID))) {
					/* Insert new implicit order */
					Order implicit_order;
					implicit_order.MakeImplicit(this->last_station_visited);
					InsertOrder(this, std::move(implicit_order), this->cur_implicit_order_index);
					if (this->cur_implicit_order_index > 0) --this->cur_implicit_order_index;

					/* InsertOrder disabled creation of implicit orders for all vehicles with the same implicit order.
					 * Re-enable it for this vehicle */
					uint16_t &gv_flags = this->GetGroundVehicleFlags();
					ClrBit(gv_flags, GVF_SUPPRESS_IMPLICIT_ORDERS);
				}
			}
		}
		this->current_order.MakeLoading(false);
	}

	if (!no_load_prepare) {
		VehicleIncreaseStats(this);

		PrepareUnload(this);
	}

	DirtyVehicleListWindowForVehicle(this);
	SetWindowWidgetDirty(WC_VEHICLE_VIEW, this->index, WID_VV_START_STOP);
	SetWindowDirty(WC_VEHICLE_DETAILS, this->index);
	SetWindowDirty(WC_STATION_VIEW, this->last_station_visited);

	Station::Get(this->last_station_visited)->MarkTilesDirty(true);
	this->cur_speed = 0;
	this->MarkDirty();
}

/**
 * Return all reserved cargo packets to the station and reset all packets
 * staged for transfer.
 * @param st the station where the reserved packets should go.
 */
void Vehicle::CancelReservation(StationID next, Station *st)
{
	for (Vehicle *v = this; v != nullptr; v = v->next) {
		VehicleCargoList &cargo = v->cargo;
		if (cargo.ActionCount(VehicleCargoList::MTA_LOAD) > 0) {
			Debug(misc, 1, "cancelling cargo reservation");
			cargo.Return(UINT_MAX, &st->goods[v->cargo_type].CreateData().cargo, next, v->tile);
		}
		cargo.KeepAll();
	}
}

CargoTypes Vehicle::GetLastLoadingStationValidCargoMask() const
{
	if (!HasBit(this->vehicle_flags, VF_LAST_LOAD_ST_SEP)) {
		return (this->last_loading_station != INVALID_STATION) ? ALL_CARGOTYPES : 0;
	} else {
		CargoTypes cargo_mask = 0;
		for (const Vehicle *u = this; u != nullptr; u = u->Next()) {
			if (u->cargo_type < NUM_CARGO && u->last_loading_station != INVALID_STATION) {
				SetBit(cargo_mask, u->cargo_type);
			}
		}
		return cargo_mask;
	}
}

/**
 * Perform all actions when leaving a station.
 * @pre this->current_order.IsType(OT_LOADING)
 */
void Vehicle::LeaveStation()
{
	assert(this->current_order.IsAnyLoadingType());

	delete this->cargo_payment;
	dbg_assert(this->cargo_payment == nullptr); // cleared by ~CargoPayment

	ClrBit(this->vehicle_flags, VF_COND_ORDER_WAIT);
	ClrBit(this->vehicle_flags, VF_STOP_LOADING);

	TileIndex station_tile = INVALID_TILE;

	if (this->type == VEH_TRAIN) {
		station_tile = Train::From(this)->GetStationLoadingVehicle()->tile;
		for (Train *v = Train::From(this); v != nullptr; v = v->Next()) {
			ClrBit(v->flags, VRF_BEYOND_PLATFORM_END);
			ClrBit(v->flags, VRF_NOT_YET_IN_PLATFORM);
			ClrBit(v->vehicle_flags, VF_CARGO_UNLOADING);
		}
	}

	/* Only update the timetable if the vehicle was supposed to stop here. */
	if (this->current_order.GetNonStopType() != ONSF_STOP_EVERYWHERE) UpdateVehicleTimetable(this, false);

	CargoTypes cargoes_can_load_unload = this->current_order.FilterLoadUnloadTypeCargoMask([&](const Order *o, CargoType cargo) {
		return ((o->GetCargoLoadType(cargo) & OLFB_NO_LOAD) == 0) || ((o->GetCargoUnloadType(cargo) & OUFB_NO_UNLOAD) == 0);
	});
	CargoTypes has_cargo_mask = this->GetLastLoadingStationValidCargoMask();
	CargoTypes cargoes_can_leave_with_cargo = FilterCargoMask([&](CargoType cargo) {
		return this->current_order.CanLeaveWithCargo(HasBit(has_cargo_mask, cargo), cargo);
	}, cargoes_can_load_unload);

	if (cargoes_can_load_unload != 0) {
		if (cargoes_can_leave_with_cargo != 0) {
			/* Refresh next hop stats to make sure we've done that at least once
			 * during the stop and that refit_cap == cargo_cap for each vehicle in
			 * the consist. */
			this->ResetRefitCaps();
			LinkRefresher::Run(this, true, false, cargoes_can_leave_with_cargo);
		}

		if (cargoes_can_leave_with_cargo == ALL_CARGOTYPES) {
			/* can leave with all cargoes */

			/* if the vehicle could load here or could stop with cargo loaded set the last loading station */
			this->last_loading_station = this->last_station_visited;
			this->last_loading_tick = _state_ticks;
			ClrBit(this->vehicle_flags, VF_LAST_LOAD_ST_SEP);
		} else if (cargoes_can_leave_with_cargo == 0) {
			/* can leave with no cargoes */

			/* if the vehicle couldn't load and had to unload or transfer everything
			 * set the last loading station to invalid as it will leave empty. */
			this->last_loading_station = INVALID_STATION;
			ClrBit(this->vehicle_flags, VF_LAST_LOAD_ST_SEP);
		} else {
			/* mix of cargoes loadable or could not leave with all cargoes */

			/* NB: this is saved here as we overwrite it on the first iteration of the loop below */
			StationID head_last_loading_station = this->last_loading_station;
			StateTicks head_last_loading_tick = this->last_loading_tick;
			for (Vehicle *u = this; u != nullptr; u = u->Next()) {
				StationID last_loading_station = HasBit(this->vehicle_flags, VF_LAST_LOAD_ST_SEP) ? u->last_loading_station : head_last_loading_station;
				StateTicks last_loading_tick = HasBit(this->vehicle_flags, VF_LAST_LOAD_ST_SEP) ? u->last_loading_tick : head_last_loading_tick;
				if (u->cargo_type < NUM_CARGO && HasBit(cargoes_can_load_unload, u->cargo_type)) {
					if (HasBit(cargoes_can_leave_with_cargo, u->cargo_type)) {
						u->last_loading_station = this->last_station_visited;
						u->last_loading_tick = _state_ticks;
					} else {
						u->last_loading_station = INVALID_STATION;
					}
				} else {
					u->last_loading_station = last_loading_station;
					u->last_loading_tick = last_loading_tick;
				}
			}
			SetBit(this->vehicle_flags, VF_LAST_LOAD_ST_SEP);
		}
	}

	this->current_order.MakeLeaveStation();
	Station *st = Station::Get(this->last_station_visited);
	this->CancelReservation(INVALID_STATION, st);
	st->loading_vehicles.erase(std::remove(st->loading_vehicles.begin(), st->loading_vehicles.end(), this), st->loading_vehicles.end());

	HideFillingPercent(&this->fill_percent_te_id);
	trip_occupancy = CalcPercentVehicleFilled(this, nullptr);

	if (this->type == VEH_TRAIN && !(this->vehstatus & VS_CRASHED)) {
		/* Trigger station animation (trains only) */
		if (IsRailStationTile(station_tile)) {
			TriggerStationRandomisation(st, station_tile, SRT_TRAIN_DEPARTS);
			TriggerStationAnimation(st, station_tile, SAT_TRAIN_DEPARTS);
		}

		SetBit(Train::From(this)->flags, VRF_LEAVING_STATION);
		if (Train::From(this)->lookahead != nullptr) Train::From(this)->lookahead->zpos_refresh_remaining = 0;
	}
	if (this->type == VEH_ROAD && !(this->vehstatus & VS_CRASHED)) {
		/* Trigger road stop animation */
		if (IsAnyRoadStopTile(this->tile)) {
			TriggerRoadStopRandomisation(st, this->tile, RSRT_VEH_DEPARTS);
			TriggerRoadStopAnimation(st, this->tile, SAT_TRAIN_DEPARTS);
		}
	}

	if (this->cur_real_order_index < this->GetNumOrders()) {
		Order *real_current_order = this->GetOrder(this->cur_real_order_index);
		if (real_current_order->IsType(OT_GOTO_STATION) && real_current_order->GetDestination() == this->last_station_visited) {
			uint current_occupancy = CalcPercentVehicleFilled(this, nullptr);
			uint old_occupancy = real_current_order->GetOccupancy();
			uint new_occupancy;
			if (old_occupancy == 0) {
				new_occupancy = current_occupancy;
			} else {
				Company *owner = Company::GetIfValid(this->owner);
				uint8_t occupancy_smoothness = owner ? owner->settings.order_occupancy_smoothness : 0;
				// Exponential weighted moving average using occupancy_smoothness
				new_occupancy = (old_occupancy - 1) * occupancy_smoothness;
				new_occupancy += current_occupancy * (100 - occupancy_smoothness);
				new_occupancy += 50; // round to nearest integer percent, rather than just floor
				new_occupancy /= 100;
			}
			if (new_occupancy + 1 != old_occupancy) {
				this->order_occupancy_average = 0;
				real_current_order->SetOccupancy(static_cast<uint8_t>(new_occupancy + 1));
				for (const Vehicle *v = this->FirstShared(); v != nullptr; v = v->NextShared()) {
					SetWindowDirty(WC_VEHICLE_ORDERS, v->index);
				}
			}
		}
	}

	this->MarkDirty();
}
/**
 * Perform all actions when switching to advancing within a station for loading/unloading
 * @pre this->current_order.IsType(OT_LOADING)
 * @pre this->type == VEH_TRAIN
 */
void Vehicle::AdvanceLoadingInStation()
{
	assert(this->current_order.IsType(OT_LOADING));
	dbg_assert(this->type == VEH_TRAIN);

	ClrBit(Train::From(this)->flags, VRF_ADVANCE_IN_PLATFORM);

	for (Train *v = Train::From(this); v != nullptr; v = v->Next()) {
		if (HasBit(v->flags, VRF_NOT_YET_IN_PLATFORM)) {
			ClrBit(v->flags, VRF_NOT_YET_IN_PLATFORM);
		} else {
			SetBit(v->flags, VRF_BEYOND_PLATFORM_END);
		}
	}

	HideFillingPercent(&this->fill_percent_te_id);
	this->current_order.MakeLoadingAdvance(this->last_station_visited);
	this->current_order.SetNonStopType(ONSF_NO_STOP_AT_ANY_STATION);
	if (Train::From(this)->lookahead != nullptr) Train::From(this)->lookahead->zpos_refresh_remaining = 0;
	this->MarkDirty();
}

void Vehicle::RecalculateOrderOccupancyAverage()
{
	uint num_valid = 0;
	uint total = 0;
	uint order_count = this->GetNumOrders();
	for (uint i = 0; i < order_count; i++) {
		const Order *order = this->GetOrder(i);
		uint occupancy = order->GetOccupancy();
		if (occupancy > 0 && order->UseOccupancyValueForAverage()) {
			num_valid++;
			total += (occupancy - 1);
		}
	}
	if (num_valid > 0) {
		this->order_occupancy_average = 16 + ((total + (num_valid / 2)) / num_valid);
	} else {
		this->order_occupancy_average = 1;
	}
}

/**
 * Reset all refit_cap in the consist to cargo_cap.
 */
void Vehicle::ResetRefitCaps()
{
	for (Vehicle *v = this; v != nullptr; v = v->Next()) v->refit_cap = v->cargo_cap;
}

/**
 * Release the vehicle's unit number.
 */
void Vehicle::ReleaseUnitNumber()
{
	if (this->unitnumber != 0) {
		Company::Get(this->owner)->freeunits[this->type].ReleaseID(this->unitnumber);
		this->unitnumber = 0;
	}
}

static bool ShouldVehicleContinueWaiting(Vehicle *v)
{
	if (v->GetNumOrders() < 1) return false;

	/* Rate-limit re-checking of conditional order loop */
	if (HasBit(v->vehicle_flags, VF_COND_ORDER_WAIT) && v->tick_counter % 32 != 0) return true;

	/* Don't use implicit orders for waiting loops */
	if (v->cur_implicit_order_index < v->GetNumOrders() && v->GetOrder(v->cur_implicit_order_index)->IsType(OT_IMPLICIT)) return false;

	/* If conditional orders lead back to this order, just keep waiting without leaving the order */
	bool loop = AdvanceOrderIndexDeferred(v, v->cur_implicit_order_index + 1) == v->cur_implicit_order_index;
	FlushAdvanceOrderIndexDeferred(v, loop);
	if (loop) SetBit(v->vehicle_flags, VF_COND_ORDER_WAIT);
	return loop;
}

/**
 * Handle the loading of the vehicle; when not it skips through dummy
 * orders and does nothing in all other cases.
 * @param mode is the non-first call for this vehicle in this tick?
 */
void Vehicle::HandleLoading(bool mode)
{
	switch (this->current_order.GetType()) {
		case OT_LOADING: {
			TimetableTicks wait_time = std::max<int>(this->current_order.GetTimetabledWait() - this->lateness_counter, 0);

			/* Save time just loading took since that is what goes into the timetable */
			if (!HasBit(this->vehicle_flags, VF_LOADING_FINISHED)) {
				this->current_loading_time = this->current_order_time;
			}

			/* Pay the loading fee for using someone else's station, if appropriate */
			if (!mode && this->type != VEH_TRAIN) PayStationSharingFee(this, Station::Get(this->last_station_visited));

			/* Not the first call for this tick, or still loading */
			if (mode || !HasBit(this->vehicle_flags, VF_LOADING_FINISHED) || (this->current_order_time < wait_time && this->current_order.GetLeaveType() != OLT_LEAVE_EARLY) || ShouldVehicleContinueWaiting(this)) {
				if (!mode && this->type == VEH_TRAIN && HasBit(Train::From(this)->flags, VRF_ADVANCE_IN_PLATFORM)) this->AdvanceLoadingInStation();
				return;
			}

			this->LeaveStation();

			/* Only advance to next order if we just loaded at the current one */
			const Order *order = this->GetOrder(this->cur_implicit_order_index);
			if (order == nullptr ||
					(!order->IsType(OT_IMPLICIT) && !order->IsType(OT_GOTO_STATION)) ||
					order->GetDestination() != this->last_station_visited) {
				return;
			}
			break;
		}

		case OT_DUMMY: break;

		default: return;
	}

	this->IncrementImplicitOrderIndex();
}

/**
 * Handle the waiting time everywhere else as in stations (basically in depot but, eventually, also elsewhere ?)
 * Function is called when order's wait_time is defined.
 * @param stop_waiting should we stop waiting (or definitely avoid) even if there is still time left to wait ?
 * @param process_orders whether to call ProcessOrders when exiting a waiting order
 */
void Vehicle::HandleWaiting(bool stop_waiting, bool process_orders)
{
	switch (this->current_order.GetType()) {
		case OT_WAITING: {
			uint wait_time = std::max<int>(this->current_order.GetTimetabledWait() - this->lateness_counter, 0);
			/* Vehicles holds on until waiting Timetabled time expires. */
			if (!stop_waiting && this->current_order_time < wait_time && this->current_order.GetLeaveType() != OLT_LEAVE_EARLY) {
				return;
			}
			if (!stop_waiting && process_orders && ShouldVehicleContinueWaiting(this)) {
				return;
			}

			/* When wait_time is expired, we move on. */
			ClrBit(this->vehicle_flags, VF_COND_ORDER_WAIT);
			UpdateVehicleTimetable(this, false);
			this->IncrementImplicitOrderIndex();
			this->current_order.MakeDummy();
			if (this->type == VEH_TRAIN) Train::From(this)->force_proceed = TFP_NONE;
			if (process_orders) ProcessOrders(this);
			break;
		}

		default:
			return;
	}
}

/**
 * Check if the current vehicle has a full load order.
 * @return true Iff this vehicle has a full load order.
 */
bool Vehicle::HasFullLoadOrder() const
{
	for (const Order *o : this->Orders()) {
		if (o->IsType(OT_GOTO_STATION) && o->GetLoadType() & (OLFB_FULL_LOAD | OLF_FULL_LOAD_ANY)) return true;
		if (o->IsType(OT_GOTO_STATION) && o->GetLoadType() == OLFB_CARGO_TYPE_LOAD) {
			for (CargoType cid = 0; cid < NUM_CARGO; cid++) {
				if (o->GetCargoLoadType(cid) & (OLFB_FULL_LOAD | OLF_FULL_LOAD_ANY)) return true;
			}
		}
	}
	return false;
}

/**
 * Check if the current vehicle has a conditional order.
 * @return true Iff this vehicle has a conditional order.
 */
bool Vehicle::HasConditionalOrder() const
{
	for (const Order *o : this->Orders()) {
		if (o->IsType(OT_CONDITIONAL)) return true;
	}
	return false;
}

/**
 * Check if the current vehicle has an unbunching order.
 * @return true Iff this vehicle has an unbunching order.
 */
bool Vehicle::HasUnbunchingOrder() const
{
	for (const Order *o : this->Orders()) {
		if (o->IsType(OT_GOTO_DEPOT) && o->GetDepotActionType() & ODATFB_UNBUNCH) return true;
	}
	return false;
}

/**
 * Check if the previous order is a depot unbunching order.
 * @return true Iff the previous order is a depot order with the unbunch flag.
 */
static bool PreviousOrderIsUnbunching(const Vehicle *v)
{
	/* If we are headed for the first order, we must wrap around back to the last order. */
	bool is_first_order = (v->GetOrder(v->cur_implicit_order_index) == v->GetFirstOrder());
	const Order *previous_order = (is_first_order) ? v->GetLastOrder() : v->GetOrder(v->cur_implicit_order_index - 1);

	if (previous_order == nullptr || !previous_order->IsType(OT_GOTO_DEPOT)) return false;
	return (previous_order->GetDepotActionType() & ODATFB_UNBUNCH) != 0;
}

/**
 * Leave an unbunching depot and calculate the next departure time for shared order vehicles.
 */
void Vehicle::LeaveUnbunchingDepot()
{
	/* Don't do anything if this is not our unbunching order. */
	if (!PreviousOrderIsUnbunching(this)) return;

	if (this->unbunch_state == nullptr) this->unbunch_state.reset(new VehicleUnbunchState());

	/* Set the start point for this round trip time. */
	this->unbunch_state->depot_unbunching_last_departure = _state_ticks;

	/* Tell the timetable we are now "on time." */
	this->lateness_counter = 0;
	SetWindowDirty(WC_VEHICLE_TIMETABLE, this->index);

	/* Find the average travel time of vehicles that we share orders with. */
	int num_vehicles = 0;
	Ticks total_travel_time = 0;

	Vehicle *u = this->FirstShared();
	for (; u != nullptr; u = u->NextShared()) {
		/* Ignore vehicles that are manually stopped or crashed. */
		if (u->vehstatus & (VS_STOPPED | VS_CRASHED)) continue;

		num_vehicles++;
		if (u->unbunch_state != nullptr) total_travel_time += u->unbunch_state->round_trip_time;
	}

	/* Make sure we cannot divide by 0. */
	num_vehicles = std::max(num_vehicles, 1);

	/* Calculate the separation by finding the average travel time, then calculating equal separation (minimum 1 tick) between vehicles. */
	Ticks separation = std::max((total_travel_time / num_vehicles / num_vehicles), 1);
	StateTicks next_departure = _state_ticks + separation;

	/* Set the departure time of all vehicles that we share orders with. */
	u = this->FirstShared();
	for (; u != nullptr; u = u->NextShared()) {
		/* Ignore vehicles that are manually stopped or crashed. */
		if (u->vehstatus & (VS_STOPPED | VS_CRASHED)) continue;

		if (u->unbunch_state == nullptr) u->unbunch_state.reset(new VehicleUnbunchState());
		u->unbunch_state->depot_unbunching_next_departure = next_departure;
		SetWindowDirty(WC_VEHICLE_VIEW, u->index);
	}
}

/**
 * Check whether a vehicle inside a depot is waiting for unbunching.
 * @return True if the vehicle must continue waiting, or false if it may try to leave the depot.
 */
bool Vehicle::IsWaitingForUnbunching() const
{
	assert(this->IsInDepot());

	/* Don't bother if there are no vehicles sharing orders. */
	if (!this->IsOrderListShared()) return false;

	/* Don't do anything if there aren't enough orders. */
	if (this->GetNumOrders() <= 1) return false;

	/* Don't do anything if this is not our unbunching order. */
	if (!PreviousOrderIsUnbunching(this)) return false;

	return (this->unbunch_state != nullptr) && (this->unbunch_state->depot_unbunching_next_departure > _state_ticks);
};

/**
 * Send this vehicle to the depot using the given command(s).
 * @param flags   the command flags (like execute and such).
 * @param command the command to execute.
 * @param specific_depot specific depot to use, if DepotCommandFlags::Specific is set.
 * @return the cost of the depot action.
 */
CommandCost Vehicle::SendToDepot(DoCommandFlag flags, DepotCommandFlags command, TileIndex specific_depot)
{
	CommandCost ret = CheckOwnership(this->owner);
	if (ret.Failed()) return ret;

	if (this->vehstatus & VS_CRASHED) return CMD_ERROR;
	if (this->IsStoppedInDepot()) {
		if (command.Test(DepotCommandFlag::Sell) && !command.Test(DepotCommandFlag::Cancel) && (!command.Test(DepotCommandFlag::Specific) || specific_depot == this->tile)) {
			/* Sell vehicle immediately */

			if (flags & DC_EXEC) {
				int x = this->x_pos;
				int y = this->y_pos;
				int z = this->z_pos;

				CommandCost cost = Command<CMD_SELL_VEHICLE>::Do(flags, this->index, SellVehicleFlags::SellChain, INVALID_CLIENT_ID);
				if (cost.Succeeded()) {
					if (IsLocalCompany()) {
						if (cost.GetCost() != 0) {
							ShowCostOrIncomeAnimation(x, y, z, cost.GetCost());
						}
					}
					SubtractMoneyFromCompany(cost);
				}
			}
			return CommandCost();
		}
		return CMD_ERROR;
	}

	/* No matter why we're headed to the depot, unbunching data is no longer valid. */
	if (flags & DC_EXEC) this->ResetDepotUnbunching();

	auto cancel_order = [&]() {
		if (flags & DC_EXEC) {
			/* If the orders to 'goto depot' are in the orders list (forced servicing),
			 * then skip to the next order; effectively cancelling this forced service */
			if (this->current_order.GetDepotOrderType() & ODTFB_PART_OF_ORDERS) this->IncrementRealOrderIndex();

			if (this->IsGroundVehicle()) {
				uint16_t &gv_flags = this->GetGroundVehicleFlags();
				SetBit(gv_flags, GVF_SUPPRESS_IMPLICIT_ORDERS);
			}

			/* We don't cancel a breakdown-related goto depot order, we only change whether to halt or not */
			if (this->current_order.GetDepotOrderType() & ODTFB_BREAKDOWN) {
				this->current_order.SetDepotActionType(this->current_order.GetDepotActionType() == ODATFB_HALT ? ODATF_SERVICE_ONLY : ODATFB_HALT);
			} else {
				this->ClearSeparation();
				if (HasBit(this->vehicle_flags, VF_TIMETABLE_SEPARATION)) ClrBit(this->vehicle_flags, VF_TIMETABLE_STARTED);

				this->current_order.MakeDummy();
				SetWindowWidgetDirty(WC_VEHICLE_VIEW, this->index, WID_VV_START_STOP);
			}

			/* prevent any attempt to update timetable for current order, as actual travel time will be incorrect due to depot command */
			this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
		}
	};

	if (command.Test(DepotCommandFlag::Cancel)) {
		if (this->current_order.IsType(OT_GOTO_DEPOT)) {
			cancel_order();
			return CommandCost();
		} else {
			return CMD_ERROR;
		}
	}

	if (this->current_order.IsType(OT_GOTO_DEPOT) && !command.Test(DepotCommandFlag::Specific)) {
		bool halt_in_depot = (this->current_order.GetDepotActionType() & ODATFB_HALT) != 0;
		bool sell_in_depot = (this->current_order.GetDepotActionType() & ODATFB_SELL) != 0;
		if (command.Test(DepotCommandFlag::Service) == halt_in_depot || command.Test(DepotCommandFlag::Sell) != sell_in_depot) {
			/* We called with a different DepotCommandFlag::Service or DepotCommandFlag::Sell setting.
			 * Now we change the setting to apply the new one and let the vehicle head for the same depot.
			 * Note: the if is (true for requesting service == true for ordered to stop in depot)          */
			if (flags & DC_EXEC) {
				if (!(this->current_order.GetDepotOrderType() & ODTFB_BREAKDOWN)) this->current_order.SetDepotOrderType(ODTF_MANUAL);
				this->current_order.SetDepotActionType(command.Test(DepotCommandFlag::Sell) ? ODATFB_HALT | ODATFB_SELL : (command.Test(DepotCommandFlag::Service) ? ODATF_SERVICE_ONLY : ODATFB_HALT));
				this->ClearSeparation();
				if (HasBit(this->vehicle_flags, VF_TIMETABLE_SEPARATION)) ClrBit(this->vehicle_flags, VF_TIMETABLE_STARTED);
				SetWindowWidgetDirty(WC_VEHICLE_VIEW, this->index, WID_VV_START_STOP);
			}
			return CommandCost();
		}

		if (command.Test(DepotCommandFlag::DontCancel)) return CMD_ERROR; // Requested no cancellation of depot orders
		cancel_order();
		return CommandCost();
	}

	ClosestDepot closest_depot;
	static const StringID no_depot[] = {STR_ERROR_UNABLE_TO_FIND_ROUTE_TO, STR_ERROR_UNABLE_TO_FIND_LOCAL_DEPOT, STR_ERROR_UNABLE_TO_FIND_LOCAL_DEPOT, STR_ERROR_CAN_T_SEND_AIRCRAFT_TO_HANGAR};
	if (command.Test(DepotCommandFlag::Specific)) {
		if (!(IsDepotTile(specific_depot) && GetDepotVehicleType(specific_depot) == this->type &&
				IsInfraTileUsageAllowed(this->type, this->owner, specific_depot))) {
			return CommandCost(no_depot[this->type]);
		}
		if ((this->type == VEH_ROAD && (GetPresentRoadTypes(tile) & RoadVehicle::From(this)->compatible_roadtypes) == 0) ||
				(this->type == VEH_TRAIN && !HasBit(Train::From(this)->compatible_railtypes, GetRailType(tile)))) {
			return CommandCost(no_depot[this->type]);
		}
		closest_depot.location = specific_depot;
		closest_depot.destination = (this->type == VEH_AIRCRAFT) ? GetStationIndex(specific_depot) : GetDepotIndex(specific_depot);
		closest_depot.reverse = false;
	} else {
		closest_depot = this->FindClosestDepot();
		if (!closest_depot.found) return CommandCost(no_depot[this->type]);
	}

	if (flags & DC_EXEC) {
		if (this->current_order.IsAnyLoadingType()) this->LeaveStation();
		if (this->current_order.IsType(OT_WAITING)) this->HandleWaiting(true);

		if (this->type == VEH_TRAIN) {
			for (Train *v = Train::From(this); v != nullptr; v = v->Next()) {
				ClrBit(v->flags, VRF_BEYOND_PLATFORM_END);
			}
		}

		if (this->IsGroundVehicle() && this->GetNumManualOrders() > 0) {
			uint16_t &gv_flags = this->GetGroundVehicleFlags();
			SetBit(gv_flags, GVF_SUPPRESS_IMPLICIT_ORDERS);
		}

		this->SetDestTile(closest_depot.location);
		this->current_order.MakeGoToDepot(closest_depot.destination.ToDepotID(), ODTF_MANUAL);
		if (command.Test(DepotCommandFlag::Sell)) {
			this->current_order.SetDepotActionType(ODATFB_HALT | ODATFB_SELL);
		} else if (!command.Test(DepotCommandFlag::Service)) {
			this->current_order.SetDepotActionType(ODATFB_HALT);
		}
		if (command.Test(DepotCommandFlag::Specific)) {
			this->current_order.SetDepotExtraFlags(ODEFB_SPECIFIC);
		}
		SetWindowWidgetDirty(WC_VEHICLE_VIEW, this->index, WID_VV_START_STOP);

		/* Prevent any attempt to update timetable for current order, as actual travel time will be incorrect due to depot command. */
		this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;

		/* If there is no depot in front and the train is not already reversing, reverse automatically (trains only) */
		if (this->type == VEH_TRAIN && (closest_depot.reverse ^ HasBit(Train::From(this)->flags, VRF_REVERSING))) {
			Command<CMD_REVERSE_TRAIN_DIRECTION>::Do(DC_EXEC, this->index, false);
		}

		if (this->type == VEH_AIRCRAFT) {
			Aircraft *a = Aircraft::From(this);
			if (a->state == FLYING && a->targetairport != closest_depot.destination) {
				/* The aircraft is now heading for a different hangar than the next in the orders */
				AircraftNextAirportPos_and_Order(a);
			}
		}
	}

	return CommandCost();

}

/**
 * Update the cached visual effect.
 * @param allow_power_change true if the wagon-is-powered-state may change.
 */
void Vehicle::UpdateVisualEffect(bool allow_power_change)
{
	bool powered_before = HasBit(this->vcache.cached_vis_effect, VE_DISABLE_WAGON_POWER);
	const Engine *e = this->GetEngine();

	/* Evaluate properties */
	uint8_t visual_effect;
	switch (e->type) {
		case VEH_TRAIN: visual_effect = e->u.rail.visual_effect; break;
		case VEH_ROAD:  visual_effect = e->u.road.visual_effect; break;
		case VEH_SHIP:  visual_effect = e->u.ship.visual_effect; break;
		default:        visual_effect = 1 << VE_DISABLE_EFFECT;  break;
	}

	/* Check powered wagon / visual effect callback */
	if (e->info.callback_mask.Test(VehicleCallbackMask::VisualEffect)) {
		uint16_t callback = GetVehicleCallback(CBID_VEHICLE_VISUAL_EFFECT, 0, 0, this->engine_type, this);

		if (callback != CALLBACK_FAILED) {
			if (callback >= 0x100 && e->GetGRF()->grf_version >= 8) ErrorUnknownCallbackResult(e->GetGRFID(), CBID_VEHICLE_VISUAL_EFFECT, callback);

			callback = GB(callback, 0, 8);
			/* Avoid accidentally setting 'visual_effect' to the default value
			 * Since bit 6 (disable effects) is set anyways, we can safely erase some bits. */
			if (callback == VE_DEFAULT) {
				assert(HasBit(callback, VE_DISABLE_EFFECT));
				SB(callback, VE_TYPE_START, VE_TYPE_COUNT, 0);
			}
			visual_effect = callback;
		}
	}

	/* Apply default values */
	if (visual_effect == VE_DEFAULT ||
			(!HasBit(visual_effect, VE_DISABLE_EFFECT) && GB(visual_effect, VE_TYPE_START, VE_TYPE_COUNT) == VE_TYPE_DEFAULT)) {
		/* Only train engines have default effects.
		 * Note: This is independent of whether the engine is a front engine or articulated part or whatever. */
		if (e->type != VEH_TRAIN || e->u.rail.railveh_type == RAILVEH_WAGON || !IsInsideMM(e->u.rail.engclass, EC_STEAM, EC_MONORAIL)) {
			if (visual_effect == VE_DEFAULT) {
				visual_effect = 1 << VE_DISABLE_EFFECT;
			} else {
				SetBit(visual_effect, VE_DISABLE_EFFECT);
			}
		} else {
			if (visual_effect == VE_DEFAULT) {
				/* Also set the offset */
				visual_effect = (VE_OFFSET_CENTRE - (e->u.rail.engclass == EC_STEAM ? 4 : 0)) << VE_OFFSET_START;
			}
			SB(visual_effect, VE_TYPE_START, VE_TYPE_COUNT, e->u.rail.engclass - EC_STEAM + VE_TYPE_STEAM);
		}
	}

	this->vcache.cached_vis_effect = visual_effect;

	if (!allow_power_change && powered_before != HasBit(this->vcache.cached_vis_effect, VE_DISABLE_WAGON_POWER)) {
		ToggleBit(this->vcache.cached_vis_effect, VE_DISABLE_WAGON_POWER);
		ShowNewGrfVehicleError(this->engine_type, STR_NEWGRF_BROKEN, STR_NEWGRF_BROKEN_POWERED_WAGON, GRFBug::VehPoweredWagon, false);
	}
}

static const int8_t _vehicle_smoke_pos[8] = {
	1, 1, 1, 0, -1, -1, -1, 0
};

/**
 * Call CBID_VEHICLE_SPAWN_VISUAL_EFFECT and spawn requested effects.
 * @param v Vehicle to create effects for.
 */
static void SpawnAdvancedVisualEffect(const Vehicle *v)
{
	uint16_t callback = GetVehicleCallback(CBID_VEHICLE_SPAWN_VISUAL_EFFECT, 0, Random(), v->engine_type, v);
	if (callback == CALLBACK_FAILED) return;

	uint count = GB(callback, 0, 2);
	bool auto_center = HasBit(callback, 13);
	bool auto_rotate = !HasBit(callback, 14);

	int8_t l_center = 0;
	if (auto_center) {
		/* For road vehicles: Compute offset from vehicle position to vehicle center */
		if (v->type == VEH_ROAD) l_center = -(int)(VEHICLE_LENGTH - RoadVehicle::From(v)->gcache.cached_veh_length) / 2;
	} else {
		/* For trains: Compute offset from vehicle position to sprite position */
		if (v->type == VEH_TRAIN) l_center = (VEHICLE_LENGTH - Train::From(v)->gcache.cached_veh_length) / 2;
	}

	Direction l_dir = v->direction;
	if (v->type == VEH_TRAIN && HasBit(Train::From(v)->flags, VRF_REVERSE_DIRECTION)) l_dir = ReverseDir(l_dir);
	Direction t_dir = ChangeDir(l_dir, DIRDIFF_90RIGHT);

	int8_t x_center = _vehicle_smoke_pos[l_dir] * l_center;
	int8_t y_center = _vehicle_smoke_pos[t_dir] * l_center;

	for (uint i = 0; i < count; i++) {
		uint32_t reg = GetRegister(0x100 + i);
		uint type = GB(reg,  0, 8);
		int8_t x    = GB(reg,  8, 8);
		int8_t y    = GB(reg, 16, 8);
		int8_t z    = GB(reg, 24, 8);

		if (auto_rotate) {
			int8_t l = x;
			int8_t t = y;
			x = _vehicle_smoke_pos[l_dir] * l + _vehicle_smoke_pos[t_dir] * t;
			y = _vehicle_smoke_pos[t_dir] * l - _vehicle_smoke_pos[l_dir] * t;
		}

		if (type >= 0xF0) {
			switch (type) {
				case 0xF1: CreateEffectVehicleRel(v, x_center + x, y_center + y, z, EV_STEAM_SMOKE); break;
				case 0xF2: CreateEffectVehicleRel(v, x_center + x, y_center + y, z, EV_DIESEL_SMOKE); break;
				case 0xF3: CreateEffectVehicleRel(v, x_center + x, y_center + y, z, EV_ELECTRIC_SPARK); break;
				case 0xFA: CreateEffectVehicleRel(v, x_center + x, y_center + y, z, EV_BREAKDOWN_SMOKE_AIRCRAFT); break;
				default: break;
			}
		}
	}
}

int ReversingDistanceTargetSpeed(const Train *v);

/**
 * Draw visual effects (smoke and/or sparks) for a vehicle chain.
 * @param max_speed The speed as limited by underground and orders, UINT_MAX if not already known
 * @pre this->IsPrimaryVehicle()
 */
void Vehicle::ShowVisualEffect(uint max_speed) const
{
	dbg_assert(this->IsPrimaryVehicle());
	bool sound = false;

	/* Do not show any smoke when:
	 * - vehicle smoke is disabled by the player
	 * - the vehicle is slowing down or stopped (by the player)
	 * - the vehicle is moving very slowly
	 */
	if (_settings_game.vehicle.smoke_amount == 0 ||
			this->vehstatus & (VS_TRAIN_SLOWING | VS_STOPPED) ||
			this->cur_speed < 2) {
		return;
	}

	if (max_speed == UINT_MAX) max_speed = this->GetCurrentMaxSpeed();

	if (this->type == VEH_TRAIN) {
		const Train *t = Train::From(this);
		/* For trains, do not show any smoke when:
		 * - the train is reversing
		 * - the train is exceeding the max speed
		 * - is entering a station with an order to stop there and its speed is equal to maximum station entering speed
		 * - is approaching a reversing point and its speed is equal to maximum approach speed
		 */
		if (HasBit(t->flags, VRF_REVERSING) ||
				t->cur_speed > max_speed ||
				(HasStationTileRail(t->tile) && t->IsFrontEngine() && t->current_order.ShouldStopAtStation(t, GetStationIndex(t->tile), IsRailWaypoint(t->tile)) &&
				t->cur_speed >= max_speed) ||
				(t->reverse_distance >= 1 && (int)t->cur_speed >= ReversingDistanceTargetSpeed(t))) {
			return;
		}
	}

	const Vehicle *v = this;

	do {
		bool advanced = HasBit(v->vcache.cached_vis_effect, VE_ADVANCED_EFFECT);
		int effect_offset = GB(v->vcache.cached_vis_effect, VE_OFFSET_START, VE_OFFSET_COUNT) - VE_OFFSET_CENTRE;
		VisualEffectSpawnModel effect_model = VESM_NONE;
		if (advanced) {
			effect_offset = VE_OFFSET_CENTRE;
			effect_model = (VisualEffectSpawnModel)GB(v->vcache.cached_vis_effect, 0, VE_ADVANCED_EFFECT);
			if (effect_model >= VESM_END) effect_model = VESM_NONE; // unknown spawning model
		} else {
			effect_model = (VisualEffectSpawnModel)GB(v->vcache.cached_vis_effect, VE_TYPE_START, VE_TYPE_COUNT);
			assert(effect_model != (VisualEffectSpawnModel)VE_TYPE_DEFAULT); // should have been resolved by UpdateVisualEffect
			static_assert((uint)VESM_STEAM    == (uint)VE_TYPE_STEAM);
			static_assert((uint)VESM_DIESEL   == (uint)VE_TYPE_DIESEL);
			static_assert((uint)VESM_ELECTRIC == (uint)VE_TYPE_ELECTRIC);
		}

		/* Show no smoke when:
		 * - Smoke has been disabled for this vehicle
		 * - The vehicle is not visible
		 * - The vehicle is under a bridge
		 * - The vehicle is on a depot tile
		 * - The vehicle is on a tunnel tile
		 * - The vehicle is a train engine that is currently unpowered */
		if (effect_model == VESM_NONE ||
				v->vehstatus & VS_HIDDEN ||
				IsBridgeAbove(v->tile) ||
				IsDepotTile(v->tile) ||
				IsTunnelTile(v->tile) ||
				(v->type == VEH_TRAIN &&
				!HasPowerOnRail(Train::From(v)->railtype, GetTileRailTypeByTrackBit(v->tile, Train::From(v)->track)))) {
			if (HasBit(v->vcache.cached_veh_flags, VCF_LAST_VISUAL_EFFECT)) break;
			continue;
		}

		EffectVehicleType evt = EV_END;
		switch (effect_model) {
			case VESM_STEAM:
				/* Steam smoke - amount is gradually falling until vehicle reaches its maximum speed, after that it's normal.
				 * Details: while vehicle's current speed is gradually increasing, steam plumes' density decreases by one third each
				 * third of its maximum speed spectrum. Steam emission finally normalises at very close to vehicle's maximum speed.
				 * REGULATION:
				 * - instead of 1, 4 / 2^smoke_amount (max. 2) is used to provide sufficient regulation to steam puffs' amount. */
				if (GB(v->tick_counter, 0, ((4 >> _settings_game.vehicle.smoke_amount) + ((this->cur_speed * 3) / max_speed))) == 0) {
					evt = EV_STEAM_SMOKE;
				}
				break;

			case VESM_DIESEL: {
				/* Diesel smoke - thicker when vehicle is starting, gradually subsiding till it reaches its maximum speed
				 * when smoke emission stops.
				 * Details: Vehicle's (max.) speed spectrum is divided into 32 parts. When max. speed is reached, chance for smoke
				 * emission erodes by 32 (1/4). For trains, power and weight come in handy too to either increase smoke emission in
				 * 6 steps (1000HP each) if the power is low or decrease smoke emission in 6 steps (512 tonnes each) if the train
				 * isn't overweight. Power and weight contributions are expressed in a way that neither extreme power, nor
				 * extreme weight can ruin the balance (e.g. FreightWagonMultiplier) in the formula. When the vehicle reaches
				 * maximum speed no diesel_smoke is emitted.
				 * REGULATION:
				 * - up to which speed a diesel vehicle is emitting smoke (with reduced/small setting only until 1/2 of max_speed),
				 * - in Chance16 - the last value is 512 / 2^smoke_amount (max. smoke when 128 = smoke_amount of 2). */
				int power_weight_effect = 0;
				if (v->type == VEH_TRAIN) {
					power_weight_effect = (32 >> (Train::From(this)->gcache.cached_power >> 10)) - (32 >> (Train::From(this)->gcache.cached_weight >> 9));
				}
				if (this->cur_speed < (max_speed >> (2 >> _settings_game.vehicle.smoke_amount)) &&
						Chance16((64 - ((this->cur_speed << 5) / max_speed) + power_weight_effect), (512 >> _settings_game.vehicle.smoke_amount))) {
					evt = EV_DIESEL_SMOKE;
				}
				break;
			}

			case VESM_ELECTRIC:
				/* Electric train's spark - more often occurs when train is departing (more load)
				 * Details: Electric locomotives are usually at least twice as powerful as their diesel counterparts, so spark
				 * emissions are kept simple. Only when starting, creating huge force are sparks more likely to happen, but when
				 * reaching its max. speed, quarter by quarter of it, chance decreases until the usual 2,22% at train's top speed.
				 * REGULATION:
				 * - in Chance16 the last value is 360 / 2^smoke_amount (max. sparks when 90 = smoke_amount of 2). */
				if (GB(v->tick_counter, 0, 2) == 0 &&
						Chance16((6 - ((this->cur_speed << 2) / max_speed)), (360 >> _settings_game.vehicle.smoke_amount))) {
					evt = EV_ELECTRIC_SPARK;
				}
				break;

			default:
				NOT_REACHED();
		}

		if (evt != EV_END && advanced) {
			sound = true;
			SpawnAdvancedVisualEffect(v);
		} else if (evt != EV_END) {
			sound = true;

			/* The effect offset is relative to a point 4 units behind the vehicle's
			 * front (which is the center of an 8/8 vehicle). Shorter vehicles need a
			 * correction factor. */
			if (v->type == VEH_TRAIN) effect_offset += (VEHICLE_LENGTH - Train::From(v)->gcache.cached_veh_length) / 2;

			int x = _vehicle_smoke_pos[v->direction] * effect_offset;
			int y = _vehicle_smoke_pos[(v->direction + 2) % 8] * effect_offset;

			if (v->type == VEH_TRAIN && HasBit(Train::From(v)->flags, VRF_REVERSE_DIRECTION)) {
				x = -x;
				y = -y;
			}

			CreateEffectVehicleRel(v, x, y, 10, evt);
		}

		if (HasBit(v->vcache.cached_veh_flags, VCF_LAST_VISUAL_EFFECT)) break;
	} while ((v = v->Next()) != nullptr);

	if (sound) PlayVehicleSound(this, VSE_VISUAL_EFFECT);
}

/**
 * Set the next vehicle of this vehicle.
 * @param next the next vehicle. nullptr removes the next vehicle.
 */
void Vehicle::SetNext(Vehicle *next)
{
	dbg_assert(this != next);

	if (this->next != nullptr) {
		/* We had an old next vehicle. Update the first and previous pointers */
		for (Vehicle *v = this->next; v != nullptr; v = v->Next()) {
			v->first = this->next;
		}
		this->next->previous = nullptr;
#if OTTD_UPPER_TAGGED_PTR
		VehiclePoolOps::SetIsNonFrontVehiclePtr(_vehicle_pool.GetRawRef(this->next->index), false);
#endif
	}

	this->next = next;

	if (this->next != nullptr) {
		/* A new next vehicle. Update the first and previous pointers */
		if (this->next->previous != nullptr) this->next->previous->next = nullptr;
		this->next->previous = this;
#if OTTD_UPPER_TAGGED_PTR
		VehiclePoolOps::SetIsNonFrontVehiclePtr(_vehicle_pool.GetRawRef(this->next->index), true);
#endif
		for (Vehicle *v = this->next; v != nullptr; v = v->Next()) {
			v->first = this->first;
		}
	}
}

/**
 * Gets the running cost of a vehicle  that can be sent into SetDParam for string processing.
 * @return the vehicle's running cost
 */
Money Vehicle::GetDisplayRunningCost() const
{
	Money cost = this->GetRunningCost() >> 8;
	if (_settings_client.gui.show_running_costs_calendar_year) cost *= DayLengthFactor();
	return cost;
}

/**
 * Adds this vehicle to a shared vehicle chain.
 * @param shared_chain a vehicle of the chain with shared vehicles.
 * @pre !this->IsOrderListShared()
 */
void Vehicle::AddToShared(Vehicle *shared_chain)
{
	dbg_assert(this->previous_shared == nullptr && this->next_shared == nullptr);

	if (shared_chain->orders == nullptr) {
		dbg_assert(shared_chain->previous_shared == nullptr);
		dbg_assert(shared_chain->next_shared == nullptr);
		this->orders = shared_chain->orders = new OrderList(nullptr, shared_chain);
	}

	this->next_shared     = shared_chain->next_shared;
	this->previous_shared = shared_chain;

	shared_chain->next_shared = this;

	if (this->next_shared != nullptr) this->next_shared->previous_shared = this;

	shared_chain->orders->AddVehicle(this);
}

/**
 * Removes the vehicle from the shared order list.
 */
void Vehicle::RemoveFromShared()
{
	/* Remember if we were first and the old window number before RemoveVehicle()
	 * as this changes first if needed. */
	bool were_first = (this->FirstShared() == this);
	VehicleListIdentifier vli(VL_SHARED_ORDERS, this->type, this->owner, this->FirstShared()->index);

	this->orders->RemoveVehicle(this);

	if (!were_first) {
		/* We are not the first shared one, so only relink our previous one. */
		this->previous_shared->next_shared = this->NextShared();
	}

	if (this->next_shared != nullptr) this->next_shared->previous_shared = this->previous_shared;


	if (this->orders->GetNumVehicles() == 1) InvalidateVehicleOrder(this->FirstShared(), VIWD_MODIFY_ORDERS);

	if (this->orders->GetNumVehicles() == 1 && !_settings_client.gui.enable_single_veh_shared_order_gui) {
		/* When there is only one vehicle, remove the shared order list window. */
		CloseWindowById(GetWindowClassForVehicleType(this->type), vli.ToWindowNumber());
	} else if (were_first) {
		/* If we were the first one, update to the new first one.
		 * Note: FirstShared() is already the new first */
		InvalidateWindowData(GetWindowClassForVehicleType(this->type), vli.ToWindowNumber(), this->FirstShared()->index | (1U << 31));
	}

	this->next_shared     = nullptr;
	this->previous_shared = nullptr;

	this->ClearSeparation();
	if (HasBit(this->vehicle_flags, VF_TIMETABLE_SEPARATION)) ClrBit(this->vehicle_flags, VF_TIMETABLE_STARTED);
}

template <typename T, typename U>
void DumpVehicleFlagsGeneric(const Vehicle *v, T dump, U dump_header)
{
	if (v->IsGroundVehicle()) {
		dump_header("st:", "subtype:");
		dump('F', "GVSF_FRONT",            HasBit(v->subtype, GVSF_FRONT));
		dump('A', "GVSF_ARTICULATED_PART", HasBit(v->subtype, GVSF_ARTICULATED_PART));
		dump('W', "GVSF_WAGON",            HasBit(v->subtype, GVSF_WAGON));
		dump('E', "GVSF_ENGINE",           HasBit(v->subtype, GVSF_ENGINE));
		dump('f', "GVSF_FREE_WAGON",       HasBit(v->subtype, GVSF_FREE_WAGON));
		dump('M', "GVSF_MULTIHEADED",      HasBit(v->subtype, GVSF_MULTIHEADED));
		dump('V', "GVSF_VIRTUAL",          HasBit(v->subtype, GVSF_VIRTUAL));
	}
	dump_header("vs:", "vehstatus:");
	dump('H', "VS_HIDDEN",          v->vehstatus & VS_HIDDEN);
	dump('S', "VS_STOPPED",         v->vehstatus & VS_STOPPED);
	dump('U', "VS_UNCLICKABLE",     v->vehstatus & VS_UNCLICKABLE);
	dump('D', "VS_DEFPAL",          v->vehstatus & VS_DEFPAL);
	dump('s', "VS_TRAIN_SLOWING",   v->vehstatus & VS_TRAIN_SLOWING);
	dump('X', "VS_SHADOW",          v->vehstatus & VS_SHADOW);
	dump('B', "VS_AIRCRAFT_BROKEN", v->vehstatus & VS_AIRCRAFT_BROKEN);
	dump('C', "VS_CRASHED",         v->vehstatus & VS_CRASHED);
	dump_header("vf:", "vehicle_flags:");
	dump('F', "VF_LOADING_FINISHED",        HasBit(v->vehicle_flags, VF_LOADING_FINISHED));
	dump('U', "VF_CARGO_UNLOADING",         HasBit(v->vehicle_flags, VF_CARGO_UNLOADING));
	dump('P', "VF_BUILT_AS_PROTOTYPE",      HasBit(v->vehicle_flags, VF_BUILT_AS_PROTOTYPE));
	dump('T', "VF_TIMETABLE_STARTED",       HasBit(v->vehicle_flags, VF_TIMETABLE_STARTED));
	dump('A', "VF_AUTOFILL_TIMETABLE",      HasBit(v->vehicle_flags, VF_AUTOFILL_TIMETABLE));
	dump('w', "VF_AUTOFILL_PRES_WAIT_TIME", HasBit(v->vehicle_flags, VF_AUTOFILL_PRES_WAIT_TIME));
	dump('S', "VF_STOP_LOADING",            HasBit(v->vehicle_flags, VF_STOP_LOADING));
	dump('L', "VF_PATHFINDER_LOST",         HasBit(v->vehicle_flags, VF_PATHFINDER_LOST));
	dump('c', "VF_SERVINT_IS_CUSTOM",       HasBit(v->vehicle_flags, VF_SERVINT_IS_CUSTOM));
	dump('p', "VF_SERVINT_IS_PERCENT",      HasBit(v->vehicle_flags, VF_SERVINT_IS_PERCENT));
	dump('z', "VF_SEPARATION_ACTIVE",       HasBit(v->vehicle_flags, VF_SEPARATION_ACTIVE));
	dump('D', "VF_SCHEDULED_DISPATCH",      HasBit(v->vehicle_flags, VF_SCHEDULED_DISPATCH));
	dump('x', "VF_LAST_LOAD_ST_SEP",        HasBit(v->vehicle_flags, VF_LAST_LOAD_ST_SEP));
	dump('s', "VF_TIMETABLE_SEPARATION",    HasBit(v->vehicle_flags, VF_TIMETABLE_SEPARATION));
	dump('a', "VF_AUTOMATE_TIMETABLE",      HasBit(v->vehicle_flags, VF_AUTOMATE_TIMETABLE));
	dump('Q', "VF_HAVE_SLOT",               HasBit(v->vehicle_flags, VF_HAVE_SLOT));
	dump('W', "VF_COND_ORDER_WAIT",         HasBit(v->vehicle_flags, VF_COND_ORDER_WAIT));
	dump('r', "VF_REPLACEMENT_PENDING",     HasBit(v->vehicle_flags, VF_REPLACEMENT_PENDING));
	dump_header("vcf:", "cached_veh_flags:");
	dump('l', "VCF_LAST_VISUAL_EFFECT",     HasBit(v->vcache.cached_veh_flags, VCF_LAST_VISUAL_EFFECT));
	dump('z', "VCF_GV_ZERO_SLOPE_RESIST",   HasBit(v->vcache.cached_veh_flags, VCF_GV_ZERO_SLOPE_RESIST));
	dump('d', "VCF_IS_DRAWN",               HasBit(v->vcache.cached_veh_flags, VCF_IS_DRAWN));
	dump('t', "VCF_REDRAW_ON_TRIGGER",      HasBit(v->vcache.cached_veh_flags, VCF_REDRAW_ON_TRIGGER));
	dump('s', "VCF_REDRAW_ON_SPEED_CHANGE", HasBit(v->vcache.cached_veh_flags, VCF_REDRAW_ON_SPEED_CHANGE));
	dump('R', "VCF_IMAGE_REFRESH",          HasBit(v->vcache.cached_veh_flags, VCF_IMAGE_REFRESH));
	dump('N', "VCF_IMAGE_REFRESH_NEXT",     HasBit(v->vcache.cached_veh_flags, VCF_IMAGE_REFRESH_NEXT));
	dump('c', "VCF_IMAGE_CURVATURE",        HasBit(v->vcache.cached_veh_flags, VCF_IMAGE_CURVATURE));
	if (v->IsGroundVehicle()) {
		uint16_t gv_flags = v->GetGroundVehicleFlags();
		dump_header("gvf:", "GroundVehicleFlags:");
		dump('u', "GVF_GOINGUP_BIT",              HasBit(gv_flags, GVF_GOINGUP_BIT));
		dump('d', "GVF_GOINGDOWN_BIT",            HasBit(gv_flags, GVF_GOINGDOWN_BIT));
		dump('s', "GVF_SUPPRESS_IMPLICIT_ORDERS", HasBit(gv_flags, GVF_SUPPRESS_IMPLICIT_ORDERS));
		dump('c', "GVF_CHUNNEL_BIT",              HasBit(gv_flags, GVF_CHUNNEL_BIT));
	}
	if (v->type == VEH_TRAIN) {
		const Train *t = Train::From(v);
		dump_header("tf:", "train flags:");
		dump('R', "VRF_REVERSING",                     HasBit(t->flags, VRF_REVERSING));
		dump('W', "VRF_WAITING_RESTRICTION",           HasBit(t->flags, VRF_WAITING_RESTRICTION));
		dump('P', "VRF_POWEREDWAGON",                  HasBit(t->flags, VRF_POWEREDWAGON));
		dump('r', "VRF_REVERSE_DIRECTION",             HasBit(t->flags, VRF_REVERSE_DIRECTION));
		dump('h', "VRF_HAS_HIT_RV",                    HasBit(t->flags, VRF_HAS_HIT_RV));
		dump('e', "VRF_EL_ENGINE_ALLOWED_NORMAL_RAIL", HasBit(t->flags, VRF_EL_ENGINE_ALLOWED_NORMAL_RAIL));
		dump('q', "VRF_TOGGLE_REVERSE",                HasBit(t->flags, VRF_TOGGLE_REVERSE));
		dump('s', "VRF_TRAIN_STUCK",                   HasBit(t->flags, VRF_TRAIN_STUCK));
		dump('L', "VRF_LEAVING_STATION",               HasBit(t->flags, VRF_LEAVING_STATION));
		dump('b', "VRF_BREAKDOWN_BRAKING",             HasBit(t->flags, VRF_BREAKDOWN_BRAKING));
		dump('p', "VRF_BREAKDOWN_POWER",               HasBit(t->flags, VRF_BREAKDOWN_POWER));
		dump('v', "VRF_BREAKDOWN_SPEED",               HasBit(t->flags, VRF_BREAKDOWN_SPEED));
		dump('z', "VRF_BREAKDOWN_STOPPED",             HasBit(t->flags, VRF_BREAKDOWN_STOPPED));
		dump('F', "VRF_NEED_REPAIR",                   HasBit(t->flags, VRF_NEED_REPAIR));
		dump('B', "VRF_BEYOND_PLATFORM_END",           HasBit(t->flags, VRF_BEYOND_PLATFORM_END));
		dump('Y', "VRF_NOT_YET_IN_PLATFORM",           HasBit(t->flags, VRF_NOT_YET_IN_PLATFORM));
		dump('A', "VRF_ADVANCE_IN_PLATFORM",           HasBit(t->flags, VRF_ADVANCE_IN_PLATFORM));
		dump('K', "VRF_CONSIST_BREAKDOWN",             HasBit(t->flags, VRF_CONSIST_BREAKDOWN));
		dump('J', "VRF_CONSIST_SPEED_REDUCTION",       HasBit(t->flags, VRF_CONSIST_SPEED_REDUCTION));
		dump('X', "VRF_PENDING_SPEED_RESTRICTION",     HasBit(t->flags, VRF_PENDING_SPEED_RESTRICTION));
		dump('c', "VRF_SPEED_ADAPTATION_EXEMPT",       HasBit(t->flags, VRF_SPEED_ADAPTATION_EXEMPT));
	}
	if (v->type == VEH_ROAD) {
		const RoadVehicle *rv = RoadVehicle::From(v);
		dump_header("rvf:", "road vehicle flags:");
		dump('L', "RVF_ON_LEVEL_CROSSING",             HasBit(rv->rvflags, RVF_ON_LEVEL_CROSSING));
	}
}

void Vehicle::DumpVehicleFlags(format_target &buffer, bool include_tile) const
{
	bool first_header = true;
	auto dump = [&](char c, const char *name, bool flag) {
		if (flag) buffer.push_back(c);
	};
	auto dump_header = [&](const char* header, const char *header_long) {
		if (first_header) {
			first_header = false;
		} else {
			buffer.append(", ");
		}
		buffer.append(header);
	};
	if (!this->IsGroundVehicle()) {
		buffer.format("st: {:X}", this->subtype);
		first_header = false;
	}
	DumpVehicleFlagsGeneric(this, dump, dump_header);
	if (this->type == VEH_TRAIN) {
		const Train *t = Train::From(this);
		buffer.format(", trk: 0x{:02X}", (uint)t->track);
		if (t->reverse_distance > 0) buffer.format(", rev: {}", t->reverse_distance);
	} else if (this->type == VEH_ROAD) {
		const RoadVehicle *r = RoadVehicle::From(this);
		buffer.format(", rvs: {:X}, rvf: {:X}", r->state, r->frame);
	}
	if (include_tile) {
		buffer.append(", [");
		DumpTileInfo(buffer, this->tile);
		buffer.push_back(']');
		TileIndex vtile = TileVirtXY(this->x_pos, this->y_pos);
		if (this->tile != vtile) buffer.format(", VirtXYTile: {:X} ({} x {})", vtile, TileX(vtile), TileY(vtile));
	}
	if (this->cargo_payment) buffer.append(", CP");
}


void Vehicle::DumpVehicleFlagsMultiline(format_target &buffer, const char *base_indent, const char *extra_indent) const
{
	auto dump = [&](char c, const char *name, bool flag) {
		if (flag) buffer.format("{}{}{}\n", base_indent, extra_indent, name);
	};
	auto dump_header = [&](const char* header, const char *header_long) {
		buffer.format("{}{}\n", base_indent, header_long);
	};
	if (!this->IsGroundVehicle()) {
		buffer.format("{}subtype: {:X}\n", base_indent, this->subtype);
	}
	DumpVehicleFlagsGeneric(this, dump, dump_header);
	if (this->type == VEH_TRAIN) {
		const Train *t = Train::From(this);
		buffer.format("{}track: 0x{:02X}\n", base_indent, (uint)t->track);
		if (t->reverse_distance > 0) buffer.format("{}reverse_distance: {}\n", base_indent, t->reverse_distance);
	} else if (this->type == VEH_ROAD) {
		const RoadVehicle *r = RoadVehicle::From(this);
		buffer.format("{}RV state: {:X}\n{}RV frame: {:X}\n", base_indent, r->state, base_indent, r->frame);
	}
	if (this->cargo_payment) buffer.format("{}cargo_payment present\n", base_indent);
}

void VehiclesYearlyLoop()
{
	for (Vehicle *v : Vehicle::IterateFrontOnly()) {
		if (v->IsPrimaryVehicle()) {
			/* show warning if vehicle is not generating enough income last 2 years (corresponds to a red icon in the vehicle list) */
			Money profit = v->GetDisplayProfitThisYear();
			if (v->economy_age >= VEHICLE_PROFIT_MIN_AGE && profit < 0) {
				if (_settings_client.gui.vehicle_income_warn && v->owner == _local_company) {
					SetDParam(0, v->index);
					SetDParam(1, profit);
					AddVehicleAdviceNewsItem(AdviceType::VehicleUnprofitable, EconTime::UsingWallclockUnits() ? STR_NEWS_VEHICLE_UNPROFITABLE_PERIOD : STR_NEWS_VEHICLE_UNPROFITABLE_YEAR, v->index);
				}
				AI::NewEvent(v->owner, new ScriptEventVehicleUnprofitable(v->index));
			}

			v->profit_last_year = v->profit_this_year;
			v->profit_lifetime += v->profit_this_year;
			v->profit_this_year = 0;
			SetWindowDirty(WC_VEHICLE_DETAILS, v->index);
		}
	}
	GroupStatistics::UpdateProfits();
	SetWindowClassesDirty(WC_TRAINS_LIST);
	SetWindowClassesDirty(WC_TRACE_RESTRICT_SLOTS);
	SetWindowClassesDirty(WC_SHIPS_LIST);
	SetWindowClassesDirty(WC_ROADVEH_LIST);
	SetWindowClassesDirty(WC_AIRCRAFT_LIST);
}


/**
 * Can this station be used by the given engine type?
 * @param engine_type the type of vehicles to test
 * @param st the station to test for
 * @return true if and only if the vehicle of the type can use this station.
 * @note For road vehicles the Vehicle is needed to determine whether it can
 *       use the station. This function will return true for road vehicles
 *       when at least one of the facilities is available.
 */
bool CanVehicleUseStation(EngineID engine_type, const Station *st)
{
	const Engine *e = Engine::GetIfValid(engine_type);
	dbg_assert(e != nullptr);

	switch (e->type) {
		case VEH_TRAIN:
			return (st->facilities & FACIL_TRAIN) != 0;

		case VEH_ROAD:
			/* For road vehicles we need the vehicle to know whether it can actually
			 * use the station, but if it doesn't have facilities for RVs it is
			 * certainly not possible that the station can be used. */
			return (st->facilities & (FACIL_BUS_STOP | FACIL_TRUCK_STOP)) != 0;

		case VEH_SHIP:
			return (st->facilities & FACIL_DOCK) != 0;

		case VEH_AIRCRAFT:
			return (st->facilities & FACIL_AIRPORT) != 0 &&
					(st->airport.GetFTA()->flags & (e->u.air.subtype & AIR_CTOL ? AirportFTAClass::AIRPLANES : AirportFTAClass::HELICOPTERS)) != 0;

		default:
			return false;
	}
}

/**
 * Can this station be used by the given vehicle?
 * @param v the vehicle to test
 * @param st the station to test for
 * @return true if and only if the vehicle can use this station.
 */
bool CanVehicleUseStation(const Vehicle *v, const Station *st)
{
	if (v->type == VEH_ROAD) return st->GetPrimaryRoadStop(RoadVehicle::From(v)) != nullptr;

	return CanVehicleUseStation(v->engine_type, st);
}

/**
 * Get reason string why this station can't be used by the given vehicle.
 * @param v The vehicle to test.
 * @param st The station to test for.
 * @return The string explaining why the vehicle cannot use the station.
 */
StringID GetVehicleCannotUseStationReason(const Vehicle *v, const Station *st)
{
	switch (v->type) {
		case VEH_TRAIN:
			return STR_ERROR_NO_RAIL_STATION;

		case VEH_ROAD: {
			const RoadVehicle *rv = RoadVehicle::From(v);
			RoadStop *rs = st->GetPrimaryRoadStop(rv->IsBus() ? RoadStopType::Bus : RoadStopType::Truck);

			StringID err = rv->IsBus() ? STR_ERROR_NO_BUS_STATION : STR_ERROR_NO_TRUCK_STATION;

			for (; rs != nullptr; rs = rs->next) {
				/* Articulated vehicles cannot use bay road stops, only drive-through. Make sure the vehicle can actually use this bay stop */
				if (HasTileAnyRoadType(rs->xy, rv->compatible_roadtypes) && IsBayRoadStopTile(rs->xy) && rv->HasArticulatedPart()) {
					err = STR_ERROR_NO_STOP_ARTICULATED_VEHICLE;
					continue;
				}

				/* Bay stop errors take precedence, but otherwise the vehicle may not be compatible with the roadtype/tramtype of this station tile.
				 * We give bay stop errors precedence because they are usually a bus sent to a tram station or vice versa. */
				if (!HasTileAnyRoadType(rs->xy, rv->compatible_roadtypes) && err != STR_ERROR_NO_STOP_ARTICULATED_VEHICLE) {
					err = RoadTypeIsRoad(rv->roadtype) ? STR_ERROR_NO_STOP_COMPATIBLE_ROAD_TYPE : STR_ERROR_NO_STOP_COMPATIBLE_TRAM_TYPE;
					continue;
				}
			}

			return err;
		}

		case VEH_SHIP:
			return STR_ERROR_NO_DOCK;

		case VEH_AIRCRAFT:
			if ((st->facilities & FACIL_AIRPORT) == 0) return STR_ERROR_NO_AIRPORT;
			if (v->GetEngine()->u.air.subtype & AIR_CTOL) {
				return STR_ERROR_AIRPORT_NO_PLANES;
			} else {
				return STR_ERROR_AIRPORT_NO_HELICOPTERS;
			}

		default:
			return INVALID_STRING_ID;
	}
}

/**
 * Access the ground vehicle cache of the vehicle.
 * @pre The vehicle is a #GroundVehicle.
 * @return #GroundVehicleCache of the vehicle.
 */
GroundVehicleCache *Vehicle::GetGroundVehicleCache()
{
	dbg_assert(this->IsGroundVehicle());
	if (this->type == VEH_TRAIN) {
		return &Train::From(this)->gcache;
	} else {
		return &RoadVehicle::From(this)->gcache;
	}
}

/**
 * Access the ground vehicle cache of the vehicle.
 * @pre The vehicle is a #GroundVehicle.
 * @return #GroundVehicleCache of the vehicle.
 */
const GroundVehicleCache *Vehicle::GetGroundVehicleCache() const
{
	dbg_assert(this->IsGroundVehicle());
	if (this->type == VEH_TRAIN) {
		return &Train::From(this)->gcache;
	} else {
		return &RoadVehicle::From(this)->gcache;
	}
}

/**
 * Access the ground vehicle flags of the vehicle.
 * @pre The vehicle is a #GroundVehicle.
 * @return #GroundVehicleFlags of the vehicle.
 */
uint16_t &Vehicle::GetGroundVehicleFlags()
{
	dbg_assert(this->IsGroundVehicle());
	if (this->type == VEH_TRAIN) {
		return Train::From(this)->gv_flags;
	} else {
		return RoadVehicle::From(this)->gv_flags;
	}
}

/**
 * Access the ground vehicle flags of the vehicle.
 * @pre The vehicle is a #GroundVehicle.
 * @return #GroundVehicleFlags of the vehicle.
 */
const uint16_t &Vehicle::GetGroundVehicleFlags() const
{
	dbg_assert(this->IsGroundVehicle());
	if (this->type == VEH_TRAIN) {
		return Train::From(this)->gv_flags;
	} else {
		return RoadVehicle::From(this)->gv_flags;
	}
}

/**
 * Calculates the set of vehicles that will be affected by a given selection.
 * @param[in,out] set Set of affected vehicles.
 * @param v First vehicle of the selection.
 * @param num_vehicles Number of vehicles in the selection (not counting articulated parts).
 * @pre \a set must be empty.
 * @post \a set will contain the vehicles that will be refitted.
 */
void GetVehicleSet(VehicleSet &set, Vehicle *v, uint8_t num_vehicles)
{
	if (v->type == VEH_TRAIN) {
		Train *u = Train::From(v);
		/* Only include whole vehicles, so start with the first articulated part */
		u = u->GetFirstEnginePart();

		/* Include num_vehicles vehicles, not counting articulated parts */
		for (; u != nullptr && num_vehicles > 0; num_vehicles--) {
			do {
				/* Include current vehicle in the selection. */
				include(set, u->index);

				/* If the vehicle is multiheaded, add the other part too. */
				if (u->IsMultiheaded()) include(set, u->other_multiheaded_part->index);

				u = u->Next();
			} while (u != nullptr && u->IsArticulatedPart());
		}
	}
}

void DumpVehicleStats(struct format_target &buffer)
{
	struct vtypestats {
		uint count[2] = { 0, 0 };

		bool IsEmpty() const { return (count[0] | count[1]) == 0; }

		vtypestats &operator+=(const vtypestats &other)
		{
			this->count[0] += other.count[0];
			this->count[1] += other.count[1];
			return *this;
		}
	};
	struct cstats {
		vtypestats vstats[VEH_END];
		vtypestats virt_train;
		vtypestats template_train;
	};
	std::map<Owner, cstats> cstatmap;

	for (Vehicle *v : Vehicle::Iterate()) {
		cstats &cs = cstatmap[v->owner];
		vtypestats &vs = ((v->type == VEH_TRAIN) && Train::From(v)->IsVirtual()) ? cs.virt_train : cs.vstats[v->type];
		vs.count[v->Previous() != nullptr ? 1 : 0]++;
	}

	for (const TemplateVehicle *tv : TemplateVehicle::Iterate()) {
		cstats &cs = cstatmap[tv->owner];
		cs.template_train.count[tv->Prev() != nullptr ? 1 : 0]++;
	}

	auto print_stats = [&](const cstats &cs, bool show_non_company) {
		auto line = [&](const vtypestats &vs, const char *type) {
			if (vs.count[0] || vs.count[1]) {
				buffer.format("  {:10}: primary: {:5}, secondary: {:5}\n", type, vs.count[0], vs.count[1]);
			}
		};
		line(cs.vstats[VEH_TRAIN], "train");
		line(cs.vstats[VEH_ROAD], "road");
		line(cs.vstats[VEH_SHIP], "ship");
		line(cs.vstats[VEH_AIRCRAFT], "aircraft");
		if (show_non_company) {
			line(cs.vstats[VEH_EFFECT], "effect");
			line(cs.vstats[VEH_DISASTER], "disaster");
		}
		line(cs.virt_train, "virt train");
		line(cs.template_train, "tmpl train");
		buffer.push_back('\n');
	};

	cstats totals{};
	for (auto &it : cstatmap) {
		buffer.format("{}: ", it.first);
		SetDParam(0, it.first);
		buffer.append(GetString(STR_COMPANY_NAME));
		buffer.push_back('\n');
		print_stats(it.second, false);

		for (VehicleType vt = VEH_BEGIN; vt != VEH_END; vt++) {
			totals.vstats[vt] += it.second.vstats[vt];
		}
		totals.virt_train += it.second.virt_train;
		totals.template_train += it.second.template_train;
	}
	buffer.append("Totals\n");
	print_stats(totals, true);
	buffer.format("Total vehicles: {}\n", Vehicle::GetNumItems());
}

void AdjustVehicleStateTicksBase(StateTicksDelta delta)
{
	for (Vehicle *v : Vehicle::Iterate()) {
		if (v->timetable_start != 0) v->timetable_start += delta;
		if (v->last_loading_tick != 0) v->last_loading_tick += delta;
		if (v->unbunch_state != nullptr) {
			if (v->unbunch_state->depot_unbunching_last_departure != INVALID_STATE_TICKS) v->unbunch_state->depot_unbunching_last_departure += delta;
			if (v->unbunch_state->depot_unbunching_next_departure != INVALID_STATE_TICKS) v->unbunch_state->depot_unbunching_next_departure += delta;
		}
		for (auto &it : v->dispatch_records) {
			it.second.dispatched += delta;
		}
	}

	for (OrderList *order_list : OrderList::Iterate()) {
		for (DispatchSchedule &ds : order_list->GetScheduledDispatchScheduleSet()) {
			ds.SetScheduledDispatchStartTick(ds.GetScheduledDispatchStartTick() + delta);
		}
	}

	for (OrderBackup *ob : OrderBackup::Iterate()) {
		for (auto &it : ob->dispatch_records) {
			it.second.dispatched += delta;
		}
	}
}

void ShiftVehicleDates(EconTime::DateDelta interval)
{
	for (Vehicle *v : Vehicle::Iterate()) {
		v->date_of_last_service = std::max<EconTime::Date>(v->date_of_last_service + interval, EconTime::Date{0});
	}
	/* date_of_last_service_newgrf is not updated here as it must stay stable
	 * for vehicles outside of a depot. */
}

/**
 * Calculates the maximum weight of the ground vehicle when loaded.
 * @return Weight in tonnes
 */
uint32_t Vehicle::GetDisplayMaxWeight() const
{
	uint32_t max_weight = 0;

	for (const Vehicle *u = this; u != nullptr; u = u->Next()) {
		max_weight += u->GetMaxWeight();
	}

	return max_weight;
}

/**
 * Calculates the minimum power-to-weight ratio using the maximum weight of the ground vehicle
 * @return power-to-weight ratio in 10ths of hp(I) per tonne
 */
uint32_t Vehicle::GetDisplayMinPowerToWeight() const
{
	uint32_t max_weight = GetDisplayMaxWeight();
	if (max_weight == 0) return 0;
	return GetGroundVehicleCache()->cached_power * 10u / max_weight;
}

/**
 * Checks if two vehicle chains have the same list of engines.
 * @param v1 First vehicle chain.
 * @param v1 Second vehicle chain.
 * @return True if same, false if different.
 */
bool VehiclesHaveSameEngineList(const Vehicle *v1, const Vehicle *v2)
{
	while (true) {
		if (v1 == nullptr && v2 == nullptr) return true;
		if (v1 == nullptr || v2 == nullptr) return false;
		if (v1->GetEngine() != v2->GetEngine()) return false;
		v1 = v1->GetNextVehicle();
		v2 = v2->GetNextVehicle();
	}
}

/**
 * Checks if two vehicles have the same list of orders.
 * @param v1 First vehicles.
 * @param v1 Second vehicles.
 * @return True if same, false if different.
 */
bool VehiclesHaveSameOrderList(const Vehicle *v1, const Vehicle *v2)
{
	const Order *o1 = v1->GetFirstOrder();
	const Order *o2 = v2->GetFirstOrder();
	while (true) {
		if (o1 == nullptr && o2 == nullptr) return true;
		if (o1 == nullptr || o2 == nullptr) return false;
		if (!o1->Equals(*o2)) return false;
		o1 = v1->orders->GetNextNoWrap(o1);
		o2 = v2->orders->GetNextNoWrap(o2);
	}
}
