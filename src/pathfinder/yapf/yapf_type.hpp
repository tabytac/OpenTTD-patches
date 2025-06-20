/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file yapf_type.hpp Types used by YAPF. */

#ifndef YAPF_TYPE_HPP
#define YAPF_TYPE_HPP

#include "../../core/enum_type.hpp"

/* Enum used in PfCalcCost() to see why was the segment closed. */
enum class EndSegmentReason : uint8_t {
	/* The following reasons can be saved into cached segment */
	DeadEnd, ///< track ends here
	DeadEndEol, ///< track ends here bit refers to the next tile, the last tile of the segment itself is usable
	RailType, ///< the next tile has a different rail type than our tiles
	InfiniteLoop, ///< infinite loop detected
	SegmentTooLong, ///< the segment is too long (possible infinite loop)
	ChoiceFollows, ///< the next tile contains a choice (the track splits to more than one segments)
	Depot, ///< stop in the depot (could be a target next time)
	Waypoint, ///< waypoint encountered (could be a target next time)
	Station, ///< station encountered (could be a target next time)
	SafeTile, ///< safe waiting position found (could be a target)

	/* The following reasons are used only internally by PfCalcCost().
	 *  They should not be found in the cached segment. */
	PathTooLong, ///< the path is too long (searching for the nearest depot in the given radius)
	FirstTwoWayRed, ///< first signal was 2-way and it was red
	LookAheadEnd, ///< we have just passed the last look-ahead signal
	TargetReached, ///< we have just reached the destination
	Reverse, ///< we should reverse after this point
};
using EndSegmentReasons = EnumBitSet<EndSegmentReason, uint16_t>;

/* What reasons mean that the target can be found and needs to be detected. */
static constexpr EndSegmentReasons ESRF_POSSIBLE_TARGET = {
	EndSegmentReason::Depot,
	EndSegmentReason::Waypoint,
	EndSegmentReason::Station,
	EndSegmentReason::SafeTile,
};

/* What reasons can be stored back into cached segment. */
static constexpr EndSegmentReasons ESRF_CACHED_MASK = {
	EndSegmentReason::DeadEnd,
	EndSegmentReason::DeadEndEol,
	EndSegmentReason::RailType,
	EndSegmentReason::InfiniteLoop,
	EndSegmentReason::SegmentTooLong,
	EndSegmentReason::ChoiceFollows,
	EndSegmentReason::Depot,
	EndSegmentReason::Waypoint,
	EndSegmentReason::Station,
	EndSegmentReason::SafeTile,
	EndSegmentReason::Reverse,
};

/* Reasons to abort pathfinding in this direction. */
static constexpr EndSegmentReasons ESRF_ABORT_PF_MASK = {
	EndSegmentReason::DeadEnd,
	EndSegmentReason::PathTooLong,
	EndSegmentReason::InfiniteLoop,
	EndSegmentReason::FirstTwoWayRed,
};

/* Reasons to abort pathfinding in this direction, when reversing is pending. */
static constexpr EndSegmentReasons ESRF_ABORT_PF_MASK_PENDING_REVERSE = EndSegmentReasons(ESRF_ABORT_PF_MASK).Reset(EndSegmentReason::DeadEnd);

std::string ValueStr(EndSegmentReasons flags);

#endif /* YAPF_TYPE_HPP */
