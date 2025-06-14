/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file yapf_road.cpp The road pathfinding. */

#include "../../stdafx.h"
#include "yapf.hpp"
#include "yapf_node_road.hpp"
#include "../../roadstop_base.h"
#include "../../vehicle_func.h"

#include "../../safeguards.h"

/**
 * This used to be MAX_MAP_SIZE, but is now its own constant.
 * This is due to the addition of the extra-large maps patch,
 * which increases MAX_MAP_SIZE by several orders of magnitude.
 * This is no longer a sensible value for pathfinding as it
 * leads to major performance issues if a path is not found.
 */
const uint MAX_RV_PF_TILES = 1 << 11;

const int MAX_RV_LEADER_TARGETS = 4;

template <class Types>
class CYapfCostRoadT
{
public:
	typedef typename Types::Tpf Tpf; ///< pathfinder (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower; ///< track follower helper
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables

protected:
	int max_cost;

	CYapfCostRoadT() : max_cost(0) {};

	/** to access inherited path finder */
	Tpf &Yapf()
	{
		/* use two lines to avoid false-positive Undefined Behavior Sanitizer warnings when alignof(Tpf) > alignof(*this) and *this does not meet alignof(Tpf) */
		Tpf *p = static_cast<Tpf *>(this);
		return *p;
	}

	int SlopeCost(TileIndex tile, TileIndex next_tile, Trackdir)
	{
		/* height of the center of the current tile */
		int x1 = TileX(tile) * TILE_SIZE;
		int y1 = TileY(tile) * TILE_SIZE;
		int z1 = GetSlopePixelZ(x1 + TILE_SIZE / 2, y1 + TILE_SIZE / 2, true);

		/* height of the center of the next tile */
		int x2 = TileX(next_tile) * TILE_SIZE;
		int y2 = TileY(next_tile) * TILE_SIZE;
		int z2 = GetSlopePixelZ(x2 + TILE_SIZE / 2, y2 + TILE_SIZE / 2, true);

		if (z2 - z1 > 1) {
			/* Slope up */
			return Yapf().PfGetSettings().road_slope_penalty;
		}
		return 0;
	}

	/** return one tile cost */
	inline int OneTileCost(TileIndex tile, Trackdir trackdir, const TrackFollower *tf)
	{
		int cost = 0;

		bool predicted_occupied = false;
		for (int i = 0; i < MAX_RV_LEADER_TARGETS && Yapf().leader_targets[i] != INVALID_TILE; ++i) {
			if (Yapf().leader_targets[i] != tile) continue;
			cost += Yapf().PfGetSettings().road_curve_penalty;
			predicted_occupied = true;
			break;
		}

		/* set base cost */
		if (IsDiagonalTrackdir(trackdir)) {
			cost += YAPF_TILE_LENGTH;
			switch (GetTileType(tile)) {
				case MP_ROAD:
					/* Increase the cost for level crossings */
					if (IsLevelCrossing(tile)) {
						cost += Yapf().PfGetSettings().road_crossing_penalty;
					}
					break;

				case MP_STATION: {
					if (IsRoadWaypoint(tile)) break;

					const RoadStop *rs = RoadStop::GetByTile(tile, GetRoadStopType(tile));
					if (IsDriveThroughStopTile(tile)) {
						/* Increase the cost for drive-through road stops */
						cost += Yapf().PfGetSettings().road_stop_penalty;
						DiagDirection dir = TrackdirToExitdir(trackdir);
						if (!RoadStop::IsDriveThroughRoadStopContinuation(tile, tile - TileOffsByDiagDir(dir))) {
							/* When we're the first road stop in a 'queue' of them we increase
							 * cost based on the fill percentage of the whole queue. */
							const RoadStop::Entry *entry = rs->GetEntry(dir);
							if (GetDriveThroughStopDisallowedRoadDirections(tile) != DRD_NONE && !tf->IsTram()) {
								cost += (entry->GetOccupied() + rs->GetEntry(ReverseDiagDir(dir))->GetOccupied()) * Yapf().PfGetSettings().road_stop_occupied_penalty / (2 * entry->GetLength());
							} else {
								cost += entry->GetOccupied() * Yapf().PfGetSettings().road_stop_occupied_penalty / entry->GetLength();
							}
						}

						if (predicted_occupied) {
							cost += Yapf().PfGetSettings().road_stop_occupied_penalty;
						}
					} else {
						/* Increase cost for filled road stops */
						cost += Yapf().PfGetSettings().road_stop_bay_occupied_penalty * (!rs->IsFreeBay(0) + !rs->IsFreeBay(1)) / 2;
						if (predicted_occupied) {
							cost += Yapf().PfGetSettings().road_stop_bay_occupied_penalty;
						}
					}

					break;
				}

				default:
					break;
			}
		} else {
			/* non-diagonal trackdir */
			cost = YAPF_TILE_CORNER_LENGTH + Yapf().PfGetSettings().road_curve_penalty;
		}
		return cost;
	}

public:
	inline void SetMaxCost(int max_cost)
	{
		this->max_cost = max_cost;
	}

	/**
	 * Called by YAPF to calculate the cost from the origin to the given node.
	 *  Calculates only the cost of given node, adds it to the parent node cost
	 *  and stores the result into Node::cost member
	 */
	inline bool PfCalcCost(Node &n, const TrackFollower *tf)
	{
		/* this is to handle the case where the starting tile is a junction custom bridge head,
		 * and we have advanced across the bridge in the initial step */
		int segment_cost = tf->tiles_skipped * YAPF_TILE_LENGTH;

		uint tiles = 0;
		/* start at n.key.tile / n.key.td and walk to the end of segment */
		TileIndex tile = n.key.tile;
		Trackdir trackdir = n.key.td;
		int parent_cost = (n.parent != nullptr) ? n.parent->cost : 0;

		for (;;) {
			/* base tile cost depending on distance between edges */
			segment_cost += Yapf().OneTileCost(tile, trackdir, tf);

			const RoadVehicle *v = Yapf().GetVehicle();
			/* we have reached the vehicle's destination - segment should end here to avoid target skipping */
			if (Yapf().PfDetectDestinationTile(tile, trackdir)) break;

			/* Finish if we already exceeded the maximum path cost (i.e. when
			 * searching for the nearest depot). */
			if (this->max_cost > 0 && (parent_cost + segment_cost) > this->max_cost) {
				return false;
			}

			/* stop if we have just entered the depot */
			if (IsRoadDepotTile(tile) && trackdir == DiagDirToDiagTrackdir(ReverseDiagDir(GetRoadDepotDirection(tile)))) {
				/* next time we will reverse and leave the depot */
				break;
			}

			/* if there are no reachable trackdirs on new tile, we have end of road */
			TrackFollower F(Yapf().GetVehicle());
			if (!F.Follow(tile, trackdir)) break;

			/* if we skipped some tunnel tiles, add their cost */
			/* with custom bridge heads, this cost must be added before checking if the segment has ended */
			segment_cost += F.tiles_skipped * YAPF_TILE_LENGTH;
			tiles += F.tiles_skipped + 1;

			/* if there are more trackdirs available & reachable, we are at the end of segment */
			if (KillFirstBit(F.new_td_bits) != TRACKDIR_BIT_NONE) break;
			if (tiles > MAX_RV_PF_TILES) break;

			Trackdir new_td = (Trackdir)FindFirstBit(F.new_td_bits);

			/* stop if RV is on simple loop with no junctions */
			if (F.new_tile == n.key.tile && new_td == n.key.td) return false;

			/* add hilly terrain penalty */
			segment_cost += Yapf().SlopeCost(tile, F.new_tile, trackdir);

			/* add min/max speed penalties */
			int min_speed = 0;
			int max_veh_speed = std::min<int>(v->GetDisplayMaxSpeed(), v->current_order.GetMaxSpeed() * 2);
			int max_speed = F.GetSpeedLimit(&min_speed);
			if (max_speed < max_veh_speed) segment_cost += YAPF_TILE_LENGTH * (max_veh_speed - max_speed) * (4 + F.tiles_skipped) / max_veh_speed;
			if (min_speed > max_veh_speed) segment_cost += YAPF_TILE_LENGTH * (min_speed - max_veh_speed);

			/* move to the next tile */
			tile = F.new_tile;
			trackdir = new_td;
		}

		/* save end of segment back to the node */
		n.segment_last_tile = tile;
		n.segment_last_td = trackdir;

		/* save also tile cost */
		n.cost = parent_cost + segment_cost;
		return true;
	}
};


template <class Types>
class CYapfDestinationAnyDepotRoadT
{
public:
	typedef typename Types::Tpf Tpf; ///< the pathfinder class (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower;
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables

	/** to access inherited path finder */
	Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

	/** Called by YAPF to detect if node ends in the desired destination */
	inline bool PfDetectDestination(Node &n)
	{
		return IsRoadDepotTile(n.segment_last_tile);
	}

	inline bool PfDetectDestinationTile(TileIndex tile, Trackdir)
	{
		return IsRoadDepotTile(tile);
	}

	/**
	 * Called by YAPF to calculate cost estimate. Calculates distance to the destination
	 *  adds it to the actual cost from origin and stores the sum to the Node::estimate
	 */
	inline bool PfCalcEstimate(Node &n)
	{
		n.estimate = n.cost;
		return true;
	}
};


template <class Types>
class CYapfDestinationTileRoadT
{
public:
	typedef typename Types::Tpf Tpf; ///< the pathfinder class (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower;
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables

protected:
	TileIndex dest_tile;
	TrackdirBits dest_trackdirs;
	StationID dest_station;
	StationType station_type;
	bool non_artic;

public:
	void SetDestination(const RoadVehicle *v)
	{
		auto set_trackdirs = [&]() {
			DiagDirection dir = v->current_order.GetRoadVehTravelDirection();
			this->dest_trackdirs = (dir == INVALID_DIAGDIR) ? INVALID_TRACKDIR_BIT : TrackdirToTrackdirBits(DiagDirToDiagTrackdir(dir));
		};
		if (v->current_order.IsType(OT_GOTO_STATION)) {
			this->dest_station   = v->current_order.GetDestination().ToStationID();
			set_trackdirs();
			this->station_type   = v->IsBus() ? StationType::Bus : StationType::Truck;
			this->dest_tile      = CalcClosestStationTile(this->dest_station, v->tile, this->station_type);
			this->non_artic      = !v->HasArticulatedPart();
		} else if (v->current_order.IsType(OT_GOTO_WAYPOINT)) {
			this->dest_station   = v->current_order.GetDestination().ToStationID();
			set_trackdirs();
			this->station_type   = StationType::RoadWaypoint;
			this->dest_tile      = CalcClosestStationTile(this->dest_station, v->tile, this->station_type);
			this->non_artic      = !v->HasArticulatedPart();
		} else {
			this->dest_station   = INVALID_STATION;
			this->dest_tile      = v->dest_tile;
			this->dest_trackdirs = GetTileTrackdirBits(v->dest_tile, TRANSPORT_ROAD, GetRoadTramType(v->roadtype));
		}
	}

	const Station *GetDestinationStation() const
	{
		return this->dest_station != INVALID_STATION ? Station::GetIfValid(this->dest_station) : nullptr;
	}

protected:
	/** to access inherited path finder */
	Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

public:
	/** Called by YAPF to detect if node ends in the desired destination */
	inline bool PfDetectDestination(Node &n)
	{
		return this->PfDetectDestinationTile(n.segment_last_tile, n.segment_last_td);
	}

	inline bool PfDetectDestinationTile(TileIndex tile, Trackdir trackdir)
	{
		if (this->dest_station != INVALID_STATION) {
			return IsTileType(tile, MP_STATION) &&
				GetStationIndex(tile) == this->dest_station &&
				(this->station_type == GetStationType(tile)) &&
				(this->non_artic || IsDriveThroughStopTile(tile)) &&
				(this->dest_trackdirs == INVALID_TRACKDIR_BIT ||
				(IsDriveThroughStopTile(tile) ? HasTrackdir(this->dest_trackdirs, trackdir) : HasTrackdir(this->dest_trackdirs, DiagDirToDiagTrackdir(ReverseDiagDir(GetBayRoadStopDir(tile))))));
		}

		return tile == this->dest_tile && HasTrackdir(this->dest_trackdirs, trackdir);
	}

	/**
	 * Called by YAPF to calculate cost estimate. Calculates distance to the destination
	 *  adds it to the actual cost from origin and stores the sum to the Node::estimate
	 */
	inline bool PfCalcEstimate(Node &n)
	{
		static const int dg_dir_to_x_offs[] = {-1, 0, 1, 0};
		static const int dg_dir_to_y_offs[] = {0, 1, 0, -1};
		if (this->PfDetectDestination(n)) {
			n.estimate = n.cost;
			return true;
		}

		TileIndex tile = n.segment_last_tile;
		DiagDirection exitdir = TrackdirToExitdir(n.segment_last_td);
		int x1 = 2 * TileX(tile) + dg_dir_to_x_offs[(int)exitdir];
		int y1 = 2 * TileY(tile) + dg_dir_to_y_offs[(int)exitdir];
		int x2 = 2 * TileX(this->dest_tile);
		int y2 = 2 * TileY(this->dest_tile);
		int dx = abs(x1 - x2);
		int dy = abs(y1 - y2);
		int dmin = std::min(dx, dy);
		int dxy = abs(dx - dy);
		int d = dmin * YAPF_TILE_CORNER_LENGTH + (dxy - 1) * (YAPF_TILE_LENGTH / 2);
		n.estimate = n.cost + d;
		assert(n.estimate >= n.parent->estimate);
		return true;
	}
};

struct FindVehiclesOnTileProcData {
	const Vehicle *origin_vehicle;
	TileIndex (*targets)[MAX_RV_LEADER_TARGETS];
};

static Vehicle * FindVehiclesOnTileProc(Vehicle *v, void *_data)
{
	FindVehiclesOnTileProcData *data = (FindVehiclesOnTileProcData*)(_data);

	const Vehicle *front = v->First();

	if (data->origin_vehicle == front) {
		return nullptr;
	}

	/* only consider vehicles going to the same station as us */
	if (!front->current_order.IsType(OT_GOTO_STATION) || data->origin_vehicle->current_order.GetDestination() != front->current_order.GetDestination()) {
		return nullptr;
	}

	TileIndex ti = v->tile + TileOffsByDir(v->direction);

	for (int i = 0; i < MAX_RV_LEADER_TARGETS; i++) {
		if ((*data->targets)[i] == INVALID_TILE) {
			(*data->targets)[i] = ti;
			break;
		}
		if ((*data->targets)[i] == ti) {
			break;
		}
	}

	return nullptr;
}

template <class Types>
class CYapfFollowRoadT
{
public:
	typedef typename Types::Tpf Tpf; ///< the pathfinder class (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower;
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables

protected:
	/** to access inherited path finder */
	inline Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

public:

	/**
	 * Called by YAPF to move from the given node to the next tile. For each
	 *  reachable trackdir on the new tile creates new node, initializes it
	 *  and adds it to the open list by calling Yapf().AddNewNode(n)
	 */
	inline void PfFollowNode(Node &old_node)
	{
		TrackFollower F(Yapf().GetVehicle());
		if (F.Follow(old_node.segment_last_tile, old_node.segment_last_td)) {
			Yapf().AddMultipleNodes(&old_node, F);
		}
	}

	/** return debug report character to identify the transportation type */
	inline char TransportTypeChar() const
	{
		return 'r';
	}

	static Trackdir stChooseRoadTrack(const RoadVehicle *v, TileIndex tile, DiagDirection enterdir, bool &path_found, RoadVehPathCache &path_cache)
	{
		Tpf pf;
		return pf.ChooseRoadTrack(v, tile, enterdir, path_found, path_cache);
	}

	inline Trackdir ChooseRoadTrack(const RoadVehicle *v, TileIndex tile, DiagDirection enterdir, bool &path_found, RoadVehPathCache &path_cache)
	{
		/* Handle special case - when next tile is destination tile.
		 * However, when going to a station the (initial) destination
		 * tile might not be a station, but a junction, in which case
		 * this method forces the vehicle to jump in circles. */
		if (tile == v->dest_tile && !v->current_order.IsType(OT_GOTO_STATION)) {
			/* choose diagonal trackdir reachable from enterdir */
			return DiagDirToDiagTrackdir(enterdir);
		}
		/* our source tile will be the next vehicle tile (should be the given one) */
		TileIndex src_tile = tile;
		/* get available trackdirs on the start tile */
		TrackdirBits src_trackdirs = GetTrackdirBitsForRoad(tile, GetRoadTramType(v->roadtype));
		/* select reachable trackdirs only */
		src_trackdirs &= DiagdirReachesTrackdirs(enterdir);

		/* set origin and destination nodes */
		Yapf().SetOrigin(src_tile, src_trackdirs);
		Yapf().SetDestination(v);

		bool multiple_targets = false;
		TileArea non_cached_area;
		const Station *st = Yapf().GetDestinationStation();
		if (st) {
			const RoadStop *stop = st->GetPrimaryRoadStop(v);
			if (stop != nullptr && (IsDriveThroughStopTile(stop->xy) || stop->GetNextRoadStop(v) != nullptr)) {
				multiple_targets = true;
				non_cached_area = v->IsBus() ? st->bus_station : st->truck_station;
				non_cached_area.Expand(YAPF_ROADVEH_PATH_CACHE_DESTINATION_LIMIT);
			}
		}

		Yapf().leader_targets[0] = INVALID_TILE;
		if (multiple_targets && non_cached_area.Contains(tile)) {
			/* Destination station has at least 2 usable road stops, or first is a drive-through stop,
			 * check for other vehicles headin to the same destination directly in front */
			for (int i = 1; i < MAX_RV_LEADER_TARGETS; ++i) {
				Yapf().leader_targets[i] = INVALID_TILE;
			}
			FindVehiclesOnTileProcData data;
			data.origin_vehicle = v;
			data.targets = &Yapf().leader_targets;
			FindVehicleOnPos(tile, VEH_ROAD, &data, &FindVehiclesOnTileProc);
		}

		/* find the best path */
		path_found = Yapf().FindPath(v);

		/* if path not found - return INVALID_TRACKDIR */
		Trackdir next_trackdir = INVALID_TRACKDIR;
		Node *pNode = Yapf().GetBestNode();
		if (pNode != nullptr) {
			/* path was found or at least suggested
			 * walk through the path back to its origin */
			while (pNode->parent != nullptr) {
				if (pNode->GetIsChoice()) {
					path_cache.push_front(pNode->GetTile(), pNode->GetTrackdir());
				}
				pNode = pNode->parent;
			}
			/* return trackdir from the best origin node (one of start nodes) */
			Node &best_next_node = *pNode;
			assert(best_next_node.GetTile() == tile);
			next_trackdir = best_next_node.GetTrackdir();
			/* remove last element for the special case when tile == dest_tile */
			if (path_found && !path_cache.empty() && tile == v->dest_tile) {
				path_cache.pop_back();
			}
			path_cache.layout_ctr = _road_layout_change_counter;

			/* Check if target is a station, and cached path ends within 8 tiles of the dest tile */
			if (multiple_targets) {
				/* Destination station has at least 2 usable road stops, or first is a drive-through stop,
				 * trim end of path cache within a number of tiles of road stop tile area */
				while (!path_cache.empty() && non_cached_area.Contains(path_cache.back_tile())) {
					path_cache.pop_back();
				}
			}
		}
		return next_trackdir;
	}

	inline uint DistanceToTile(const RoadVehicle *v, TileIndex dst_tile)
	{
		/* handle special case - when current tile is the destination tile */
		if (dst_tile == v->tile) {
			/* distance is zero in this case */
			return 0;
		}

		if (!this->SetOriginFromVehiclePos(v)) return UINT_MAX;

		/* get available trackdirs on the destination tile */
		Yapf().SetDestination(v);

		/* if path not found - return distance = UINT_MAX */
		uint dist = UINT_MAX;

		/* find the best path */
		if (!Yapf().FindPath(v)) return dist;

		Node *pNode = Yapf().GetBestNode();
		if (pNode != nullptr) {
			/* path was found
			 * get the path cost estimate */
			dist = pNode->GetCostEstimate();
		}

		return dist;
	}

	/** Return true if the valid origin (tile/trackdir) was set from the current vehicle position. */
	inline bool SetOriginFromVehiclePos(const RoadVehicle *v)
	{
		/* set origin (tile, trackdir) */
		TileIndex src_tile = v->tile;
		Trackdir src_td = v->GetVehicleTrackdir();
		if (!HasTrackdir(GetTrackdirBitsForRoad(src_tile, Yapf().IsTram() ? RTT_TRAM : RTT_ROAD), src_td)) {
			/* sometimes the roadveh is not on the road (it resides on non-existing track)
			 * how should we handle that situation? */
			return false;
		}
		Yapf().SetOrigin(src_tile, TrackdirToTrackdirBits(src_td));
		return true;
	}

	static FindDepotData stFindNearestDepot(const RoadVehicle *v, TileIndex tile, Trackdir td, int max_distance)
	{
		Tpf pf;
		return pf.FindNearestDepot(v, tile, td, max_distance);
	}

	/**
	 * Find the best depot for a road vehicle.
	 * @param v Vehicle
	 * @param tile Tile of the vehicle.
	 * @param td Trackdir of the vehicle.
	 * @param max_distance max length (penalty) for paths.
	 */
	inline FindDepotData FindNearestDepot(const RoadVehicle *v, TileIndex tile, Trackdir td, int max_distance)
	{
		/* Set origin. */
		Yapf().SetOrigin(tile, TrackdirToTrackdirBits(td));
		Yapf().SetMaxCost(max_distance);

		/* Find the best path and return if no depot is found. */
		if (!Yapf().FindPath(v)) return FindDepotData();

		/* Return the cost of the best path and its depot. */
		Node *n = Yapf().GetBestNode();
		return FindDepotData(n->segment_last_tile, n->cost);
	}
};

template <class Tpf_, class Tnode_list, template <class Types> class Tdestination>
struct CYapfRoad_TypesT
{
	typedef CYapfRoad_TypesT<Tpf_, Tnode_list, Tdestination>  Types;

	typedef Tpf_                              Tpf;
	typedef CFollowTrackRoad                  TrackFollower;
	typedef Tnode_list                        NodeList;
	typedef RoadVehicle                       VehicleType;
	typedef CYapfBaseT<Types>                 PfBase;
	typedef CYapfFollowRoadT<Types>           PfFollow;
	typedef CYapfOriginTileT<Types>           PfOrigin;
	typedef Tdestination<Types>               PfDestination;
	typedef CYapfSegmentCostCacheNoneT<Types> PfCache;
	typedef CYapfCostRoadT<Types>             PfCost;
};

template <class Types>
struct CYapfRoadCommon : CYapfT<Types> {
	TileIndex leader_targets[MAX_RV_LEADER_TARGETS]; ///< the tiles targeted by vehicles in front of the current vehicle
};

struct CYapfRoad1         : CYapfRoadCommon<CYapfRoad_TypesT<CYapfRoad1        , CRoadNodeListTrackDir, CYapfDestinationTileRoadT    > > {};
struct CYapfRoad2         : CYapfRoadCommon<CYapfRoad_TypesT<CYapfRoad2        , CRoadNodeListExitDir , CYapfDestinationTileRoadT    > > {};

struct CYapfRoadAnyDepot1 : CYapfRoadCommon<CYapfRoad_TypesT<CYapfRoadAnyDepot1, CRoadNodeListTrackDir, CYapfDestinationAnyDepotRoadT> > {};
struct CYapfRoadAnyDepot2 : CYapfRoadCommon<CYapfRoad_TypesT<CYapfRoadAnyDepot2, CRoadNodeListExitDir , CYapfDestinationAnyDepotRoadT> > {};


Trackdir YapfRoadVehicleChooseTrack(const RoadVehicle *v, TileIndex tile, DiagDirection enterdir, TrackdirBits trackdirs, bool &path_found, RoadVehPathCache &path_cache)
{
	Trackdir td_ret = _settings_game.pf.yapf.disable_node_optimization
		? CYapfRoad1::stChooseRoadTrack(v, tile, enterdir, path_found, path_cache) // Trackdir
		: CYapfRoad2::stChooseRoadTrack(v, tile, enterdir, path_found, path_cache); // ExitDir, allow 90-deg

	return (td_ret != INVALID_TRACKDIR) ? td_ret : (Trackdir)FindFirstBit(trackdirs);
}

FindDepotData YapfRoadVehicleFindNearestDepot(const RoadVehicle *v, int max_distance)
{
	TileIndex tile = v->tile;
	Trackdir trackdir = v->GetVehicleTrackdir();

	if (!HasTrackdir(GetTrackdirBitsForRoad(tile, GetRoadTramType(v->roadtype)), trackdir)) {
		return FindDepotData();
	}

	return _settings_game.pf.yapf.disable_node_optimization
		? CYapfRoadAnyDepot1::stFindNearestDepot(v, tile, trackdir, max_distance) // Trackdir
		: CYapfRoadAnyDepot2::stFindNearestDepot(v, tile, trackdir, max_distance); // ExitDir
}
