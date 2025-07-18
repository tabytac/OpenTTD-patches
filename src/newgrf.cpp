/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file newgrf.cpp Base of all NewGRF support. */

#include "stdafx.h"

#include "newgrf_internal.h"
#include "core/backup_type.hpp"
#include "core/container_func.hpp"
#include "core/bit_cast.hpp"
#include "debug.h"
#include "fileio_func.h"
#include "engine_func.h"
#include "engine_base.h"
#include "engine_override.h"
#include "bridge.h"
#include "town.h"
#include "newgrf_engine.h"
#include "newgrf_text.h"
#include "fontcache.h"
#include "currency.h"
#include "landscape.h"
#include "newgrf_badge.h"
#include "newgrf_badge_type.h"
#include "newgrf_cargo.h"
#include "newgrf_house.h"
#include "newgrf_sound.h"
#include "newgrf_station.h"
#include "industrytype.h"
#include "industry_map.h"
#include "newgrf_act5.h"
#include "newgrf_canal.h"
#include "newgrf_townname.h"
#include "newgrf_industries.h"
#include "newgrf_airporttiles.h"
#include "newgrf_airport.h"
#include "newgrf_object.h"
#include "newgrf_newsignals.h"
#include "newgrf_newlandscape.h"
#include "newgrf_extension.h"
#include "rev.h"
#include "fios.h"
#include "strings_func.h"
#include "date_func.h"
#include "string_func.h"
#include "network/core/config.h"
#include "smallmap_gui.h"
#include "genworld.h"
#include "error.h"
#include "error_func.h"
#include "vehicle_func.h"
#include "language.h"
#include "vehicle_base.h"
#include "road.h"
#include "newgrf_roadstop.h"
#include "debug_settings.h"

#include "table/strings.h"
#include "table/build_industry.h"

#include "3rdparty/cpp-btree/btree_map.h"
#include "3rdparty/robin_hood/robin_hood.h"

#include <variant>

#include "safeguards.h"

/* TTDPatch extended GRF format codec
 * (c) Petr Baudis 2004 (GPL'd)
 * Changes by Florian octo Forster are (c) by the OpenTTD development team.
 *
 * Contains portions of documentation by TTDPatch team.
 * Thanks especially to Josef Drexler for the documentation as well as a lot
 * of help at #tycoon. Also thanks to Michael Blunck for his GRF files which
 * served as subject to the initial testing of this codec. */

constexpr uint16_t GROUPID_CALLBACK_FAILED = 0x7FFF; ///< Explicit "failure" result.

/** List of all loaded GRF files */
static std::vector<GRFFile *> _grf_files;
static robin_hood::unordered_map<uint32_t, GRFFile *> _grf_file_map;

const std::vector<GRFFile *> &GetAllGRFFiles()
{
	return _grf_files;
}

static robin_hood::unordered_map<uint16_t, const CallbackResultSpriteGroup *> _callback_result_cache;

/** Miscellaneous GRF features, set by Action 0x0D, parameter 0x9E */
uint8_t _misc_grf_features = 0;

/** 32 * 8 = 256 flags. Apparently TTDPatch uses this many.. */
static uint32_t _ttdpatch_flags[8];
static uint32_t _observed_ttdpatch_flags[8];

/** Indicates which are the newgrf features currently loaded ingame */
GRFLoadedFeatures _loaded_newgrf_features;

GrfProcessingState _cur;


/**
 * Helper to check whether an image index is valid for a particular NewGRF vehicle.
 * @tparam T The type of vehicle.
 * @param image_index The image index to check.
 * @return True iff the image index is valid, or 0xFD (use new graphics).
 */
template <VehicleType T>
static inline bool IsValidNewGRFImageIndex(uint8_t image_index)
{
	return image_index == 0xFD || IsValidImageIndex<T>(image_index);
}

class OTTDByteReaderSignal { };

/** Class to read from a NewGRF file */
class ByteReader {
protected:
	uint8_t *data;
	uint8_t *end;

public:
	ByteReader(uint8_t *data, uint8_t *end) : data(data), end(end) { }

	inline uint8_t *ReadBytes(size_t size)
	{
		if (data + size >= end) {
			/* Put data at the end, as would happen if every byte had been individually read. */
			data = end;
			throw OTTDByteReaderSignal();
		}

		uint8_t *ret = data;
		data += size;
		return ret;
	}

	inline uint8_t ReadByte()
	{
		if (data < end) return *(data)++;
		throw OTTDByteReaderSignal();
	}

	uint16_t ReadWord()
	{
		uint16_t val = ReadByte();
		return val | (ReadByte() << 8);
	}

	uint16_t ReadExtendedByte()
	{
		uint16_t val = ReadByte();
		return val == 0xFF ? ReadWord() : val;
	}

	uint32_t ReadDWord()
	{
		uint32_t val = ReadWord();
		return val | (ReadWord() << 16);
	}

	uint32_t PeekDWord()
	{
		AutoRestoreBackup backup(this->data, this->data);
		return this->ReadDWord();
	}

	uint32_t ReadVarSize(uint8_t size)
	{
		switch (size) {
			case 1: return ReadByte();
			case 2: return ReadWord();
			case 4: return ReadDWord();
			default:
				NOT_REACHED();
				return 0;
		}
	}

	std::string_view ReadString()
	{
		char *string = reinterpret_cast<char *>(data);
		size_t string_length = ttd_strnlen(string, Remaining());

		/* Skip past the terminating NUL byte if it is present, but not more than remaining. */
		Skip(std::min(string_length + 1, Remaining()));

		return std::string_view(string, string_length);
	}

	inline size_t Remaining() const
	{
		return end - data;
	}

	inline bool HasData(size_t count = 1) const
	{
		return data + count <= end;
	}

	inline uint8_t *Data()
	{
		return data;
	}

	inline void Skip(size_t len)
	{
		data += len;
		/* It is valid to move the buffer to exactly the end of the data,
		 * as there may not be any more data read. */
		if (data > end) throw OTTDByteReaderSignal();
	}

	inline void ResetReadPosition(uint8_t *pos)
	{
		data = pos;
	}
};

typedef void (*SpecialSpriteHandler)(ByteReader &buf);

/** The maximum amount of stations a single GRF is allowed to add */
static const uint NUM_STATIONS_PER_GRF = UINT16_MAX - 1;

/** Temporary engine data used when loading only */
struct GRFTempEngineData {
	/** Summary state of refittability properties */
	enum Refittability : uint8_t {
		UNSET    =  0,  ///< No properties assigned. Default refit masks shall be activated.
		EMPTY,          ///< GRF defined vehicle as not-refittable. The vehicle shall only carry the default cargo.
		NONEMPTY,       ///< GRF defined the vehicle as refittable. If the refitmask is empty after translation (cargotypes not available), disable the vehicle.
	};

	CargoClasses cargo_allowed;          ///< Bitmask of cargo classes that are allowed as a refit.
	CargoClasses cargo_allowed_required; ///< Bitmask of cargo classes that are required to be all present to allow a cargo as a refit.
	CargoClasses cargo_disallowed;       ///< Bitmask of cargo classes that are disallowed as a refit.
	RailTypeLabel railtypelabel;
	uint8_t roadtramtype;
	const GRFFile *defaultcargo_grf; ///< GRF defining the cargo translation table to use if the default cargo is the 'first refittable'.
	Refittability refittability;     ///< Did the newgrf set any refittability property? If not, default refittability will be applied.
	uint8_t rv_max_speed;        ///< Temporary storage of RV prop 15, maximum speed in mph/0.8
	CargoTypes ctt_include_mask; ///< Cargo types always included in the refit mask.
	CargoTypes ctt_exclude_mask; ///< Cargo types always excluded from the refit mask.

	/**
	 * Update the summary refittability on setting a refittability property.
	 * @param non_empty true if the GRF sets the vehicle to be refittable.
	 */
	void UpdateRefittability(bool non_empty)
	{
		if (non_empty) {
			this->refittability = NONEMPTY;
		} else if (this->refittability == UNSET) {
			this->refittability = EMPTY;
		}
	}
};

static std::vector<GRFTempEngineData> _gted;  ///< Temporary engine data used during NewGRF loading

/**
 * Contains the GRF ID of the owner of a vehicle if it has been reserved.
 * GRM for vehicles is only used if dynamic engine allocation is disabled,
 * so 256 is the number of original engines. */
static uint32_t _grm_engines[256];

/** Contains the GRF ID of the owner of a cargo if it has been reserved */
static uint32_t _grm_cargoes[NUM_CARGO * 2];

struct GRFLocation {
	uint32_t grfid;
	uint32_t nfoline;

	GRFLocation() { }
	GRFLocation(uint32_t grfid, uint32_t nfoline) : grfid(grfid), nfoline(nfoline) { }

	bool operator<(const GRFLocation &other) const
	{
		return this->grfid < other.grfid || (this->grfid == other.grfid && this->nfoline < other.nfoline);
	}

	bool operator == (const GRFLocation &other) const
	{
		return this->grfid == other.grfid && this->nfoline == other.nfoline;
	}
};

static btree::btree_map<GRFLocation, std::pair<SpriteID, uint16_t>> _grm_sprites;
typedef btree::btree_map<GRFLocation, std::unique_ptr<uint8_t[]>> GRFLineToSpriteOverride;
static GRFLineToSpriteOverride _grf_line_to_action6_sprite_override;
static bool _action6_override_active = false;

/**
 * Debug() function dedicated to newGRF debugging messages
 * Function is essentially the same as Debug(grf, severity, ...) with the
 * addition of file:line information when parsing grf files.
 * NOTE: for the above reason(s) GrfMsg() should ONLY be used for
 * loading/parsing grf files, not for runtime debug messages as there
 * is no file information available during that time.
 * @param severity debugging severity level, see debug.h
 * @param msg the message
 * @param msg format arguments
 */
void GrfInfoVFmt(int severity, fmt::string_view msg, fmt::format_args args)
{
	format_buffer buf;
	buf.format("[{}:{}] ", _cur.grfconfig->filename, _cur.nfo_line);
	buf.vformat(msg, args);
	debug_print(DebugLevelID::grf, severity, buf);
}

/**
 * Obtain a NewGRF file by its grfID
 * @param grfid The grfID to obtain the file for
 * @return The file.
 */
GRFFile *GetFileByGRFID(uint32_t grfid)
{
	auto iter = _grf_file_map.find(grfid);
	if (iter != _grf_file_map.end()) return iter->second;
	return nullptr;
}

/**
 * Obtain a NewGRF file by its grfID,  expect it to usually be the current GRF's grfID
 * @param grfid The grfID to obtain the file for
 * @return The file.
 */
GRFFile *GetFileByGRFIDExpectCurrent(uint32_t grfid)
{
	if (_cur.grffile->grfid == grfid) return _cur.grffile;
	return GetFileByGRFID(grfid);
}

/**
 * Obtain a NewGRF file by its filename
 * @param filename The filename to obtain the file for.
 * @return The file.
 */
static GRFFile *GetFileByFilename(const std::string &filename)
{
	for (GRFFile * const file : _grf_files) {
		if (file->filename == filename) return file;
	}
	return nullptr;
}

/** Reset all NewGRFData that was used only while processing data */
static void ClearTemporaryNewGRFData(GRFFile *gf)
{
	gf->labels.clear();
}

/**
 * Disable a GRF
 * @param message Error message or STR_NULL.
 * @param config GRFConfig to disable, nullptr for current.
 * @return Error message of the GRF for further customisation.
 */
static GRFError *DisableGrf(StringID message = STR_NULL, GRFConfig *config = nullptr)
{
	GRFFile *file;
	if (config != nullptr) {
		file = GetFileByGRFID(config->ident.grfid);
	} else {
		config = _cur.grfconfig;
		file = _cur.grffile;
	}

	config->status = GCS_DISABLED;
	if (file != nullptr) ClearTemporaryNewGRFData(file);
	if (config == _cur.grfconfig) _cur.skip_sprites = -1;

	if (message == STR_NULL) return nullptr;

	config->error = {STR_NEWGRF_ERROR_MSG_FATAL, message};
	if (config == _cur.grfconfig) config->error->param_value[0] = _cur.nfo_line;
	return &config->error.value();
}

using StringIDMappingHandler = void(*)(StringID, uintptr_t);

/**
 * Information for mapping static StringIDs.
 */
struct StringIDMapping {
	const GRFFile *grf;          ///< Source NewGRF.
	GRFStringID source;          ///< Source grf-local GRFStringID.
	StringIDMappingHandler func; ///< Function for mapping result.
	uintptr_t func_data;         ///< Data for func.

	StringIDMapping(const GRFFile *grf, GRFStringID source, uintptr_t func_data, StringIDMappingHandler func) : grf(grf), source(source), func(func), func_data(func_data) { }
};

/** Strings to be mapped during load. */
static std::vector<StringIDMapping> _string_to_grf_mapping;

/**
 * Record a static StringID for getting translated later.
 * @param source Source grf-local GRFStringID.
 * @param target Destination for the mapping result.
 */
static void AddStringForMapping(GRFStringID source, StringID *target)
{
	*target = STR_UNDEFINED;
	_string_to_grf_mapping.emplace_back(_cur.grffile, source, reinterpret_cast<uintptr_t>(target), nullptr);
}

/**
 * Record a static StringID for getting translated later.
 * @param source Source grf-local GRFStringID.
 * @param data Arbitrary data (e.g pointer), must fit into a uintptr_t.
 * @param func Function to call to set the mapping result.
 */
template <typename T, typename F>
static void AddStringForMapping(GRFStringID source, T data, F func)
{
	static_assert(sizeof(T) <= sizeof(uintptr_t));

	func(STR_UNDEFINED, data);

	_string_to_grf_mapping.emplace_back(_cur.grffile, source, bit_cast_to_storage<uintptr_t>(data), [](StringID str, uintptr_t func_data) {
		F handler;
		handler(str, bit_cast_from_storage<T>(func_data));
	});
}

/**
 * Perform a mapping from TTDPatch's string IDs to OpenTTD's
 * string IDs, but only for the ones we are aware off; the rest
 * like likely unused and will show a warning.
 * @param str Grf-local GRFStringID to convert.
 * @return the converted string ID
 */
static StringID TTDPStringIDToOTTDStringIDMapping(GRFStringID str)
{
	/* StringID table for TextIDs 0x4E->0x6D */
	static const StringID units_volume[] = {
		STR_ITEMS,      STR_PASSENGERS, STR_TONS,       STR_BAGS,
		STR_LITERS,     STR_ITEMS,      STR_CRATES,     STR_TONS,
		STR_TONS,       STR_TONS,       STR_TONS,       STR_BAGS,
		STR_TONS,       STR_TONS,       STR_TONS,       STR_BAGS,
		STR_TONS,       STR_TONS,       STR_BAGS,       STR_LITERS,
		STR_TONS,       STR_LITERS,     STR_TONS,       STR_ITEMS,
		STR_BAGS,       STR_LITERS,     STR_TONS,       STR_ITEMS,
		STR_TONS,       STR_ITEMS,      STR_LITERS,     STR_ITEMS
	};

	/* A string straight from a NewGRF; this was already translated by MapGRFStringID(). */
	assert(!IsInsideMM(str.base(), 0xD000, 0xD7FF));

#define TEXTID_TO_STRINGID(begin, end, stringid, stringend) \
	static_assert(stringend - stringid == end - begin); \
	if (str.base() >= begin && str.base() <= end) return StringID{str.base() + (stringid - begin)}

	/* We have some changes in our cargo strings, resulting in some missing. */
	TEXTID_TO_STRINGID(0x000E, 0x002D, STR_CARGO_PLURAL_NOTHING,                      STR_CARGO_PLURAL_FIZZY_DRINKS);
	TEXTID_TO_STRINGID(0x002E, 0x004D, STR_CARGO_SINGULAR_NOTHING,                    STR_CARGO_SINGULAR_FIZZY_DRINK);
	if (str.base() >= 0x004E && str.base() <= 0x006D) return units_volume[str.base() - 0x004E];
	TEXTID_TO_STRINGID(0x006E, 0x008D, STR_QUANTITY_NOTHING,                          STR_QUANTITY_FIZZY_DRINKS);
	TEXTID_TO_STRINGID(0x008E, 0x00AD, STR_ABBREV_NOTHING,                            STR_ABBREV_FIZZY_DRINKS);
	TEXTID_TO_STRINGID(0x00D1, 0x00E0, STR_COLOUR_DARK_BLUE,                          STR_COLOUR_WHITE);

	/* Map building names according to our lang file changes. There are several
	 * ranges of house ids, all of which need to be remapped to allow newgrfs
	 * to use original house names. */
	TEXTID_TO_STRINGID(0x200F, 0x201F, STR_TOWN_BUILDING_NAME_TALL_OFFICE_BLOCK_1,    STR_TOWN_BUILDING_NAME_OLD_HOUSES_1);
	TEXTID_TO_STRINGID(0x2036, 0x2041, STR_TOWN_BUILDING_NAME_COTTAGES_1,             STR_TOWN_BUILDING_NAME_SHOPPING_MALL_1);
	TEXTID_TO_STRINGID(0x2059, 0x205C, STR_TOWN_BUILDING_NAME_IGLOO_1,                STR_TOWN_BUILDING_NAME_PIGGY_BANK_1);

	/* Same thing for industries */
	TEXTID_TO_STRINGID(0x4802, 0x4826, STR_INDUSTRY_NAME_COAL_MINE,                   STR_INDUSTRY_NAME_SUGAR_MINE);
	TEXTID_TO_STRINGID(0x482D, 0x482E, STR_NEWS_INDUSTRY_CONSTRUCTION,                STR_NEWS_INDUSTRY_PLANTED);
	TEXTID_TO_STRINGID(0x4832, 0x4834, STR_NEWS_INDUSTRY_CLOSURE_GENERAL,             STR_NEWS_INDUSTRY_CLOSURE_LACK_OF_TREES);
	TEXTID_TO_STRINGID(0x4835, 0x4838, STR_NEWS_INDUSTRY_PRODUCTION_INCREASE_GENERAL, STR_NEWS_INDUSTRY_PRODUCTION_INCREASE_FARM);
	TEXTID_TO_STRINGID(0x4839, 0x483A, STR_NEWS_INDUSTRY_PRODUCTION_DECREASE_GENERAL, STR_NEWS_INDUSTRY_PRODUCTION_DECREASE_FARM);

	switch (str.base()) {
		case 0x4830: return STR_ERROR_CAN_T_CONSTRUCT_THIS_INDUSTRY;
		case 0x4831: return STR_ERROR_FOREST_CAN_ONLY_BE_PLANTED;
		case 0x483B: return STR_ERROR_CAN_ONLY_BE_POSITIONED;
	}
#undef TEXTID_TO_STRINGID

	if (str.base() == 0) return STR_EMPTY;

	Debug(grf, 0, "Unknown StringID 0x{:04X} remapped to STR_EMPTY. Please open a Feature Request if you need it", str);

	return STR_EMPTY;
}

/**
 * Used when setting an object's property to map to the GRF's strings
 * while taking in consideration the "drift" between TTDPatch string system and OpenTTD's one
 * @param grfid Id of the grf file.
 * @param str GRF-local GRFStringID that we want to have the equivalent in OpenTTD.
 * @return The properly adjusted StringID.
 */
template <typename T>
StringID MapGRFStringIDCommon(T grfid, GRFStringID str)
{
	if (IsInsideMM(str.base(), 0xD800, 0x10000)) {
		/* General text provided by NewGRF.
		 * In the specs this is called the 0xDCxx range (misc persistent texts),
		 * but we meanwhile extended the range to 0xD800-0xFFFF.
		 * Note: We are not involved in the "persistent" business, since we do not store
		 * any NewGRF strings in savegames. */
		return GetGRFStringID(grfid, str);
	} else if (IsInsideMM(str.base(), 0xD000, 0xD800)) {
		/* Callback text provided by NewGRF.
		 * In the specs this is called the 0xD0xx range (misc graphics texts).
		 * These texts can be returned by various callbacks.
		 *
		 * Due to how TTDP implements the GRF-local- to global-textid translation
		 * texts included via 0x80 or 0x81 control codes have to add 0x400 to the textid.
		 * We do not care about that difference and just mask out the 0x400 bit.
		 */
		str = GRFStringID(str.base() & ~0x400);
		return GetGRFStringID(grfid, str);
	} else {
		/* The NewGRF wants to include/reference an original TTD string.
		 * Try our best to find an equivalent one. */
		return TTDPStringIDToOTTDStringIDMapping(str);
	}
}

StringID MapGRFStringID(uint32_t grfid, GRFStringID str)
{
	return MapGRFStringIDCommon(grfid, str);
}

/* This form should be preferred over the uint32_t grfid form, to avoid redundant GRFID to GRF lookups */
StringID MapGRFStringID(const GRFFile *grf, GRFStringID str)
{
	return MapGRFStringIDCommon(grf, str);
}

static robin_hood::unordered_flat_map<uint32_t, uint32_t> _grf_id_overrides;

/**
 * Set the override for a NewGRF
 * @param source_grfid The grfID which wants to override another NewGRF.
 * @param target_grfid The grfID which is being overridden.
 */
static void SetNewGRFOverride(uint32_t source_grfid, uint32_t target_grfid)
{
	if (target_grfid == 0) {
		_grf_id_overrides.erase(source_grfid);
		GrfMsg(5, "SetNewGRFOverride: Removed override of 0x{:X}", std::byteswap(source_grfid));
	} else {
		_grf_id_overrides[source_grfid] = target_grfid;
		GrfMsg(5, "SetNewGRFOverride: Added override of 0x{:X} to 0x{:X}", std::byteswap(source_grfid), std::byteswap(target_grfid));
	}
}

/**
 * Get overridden GRF for current GRF if present.
 * @return Overridden GRFFile if present, or nullptr.
 */
static GRFFile *GetCurrentGRFOverride()
{
	auto found = _grf_id_overrides.find(_cur.grffile->grfid);
	if (found != std::end(_grf_id_overrides)) {
		GRFFile *grffile = GetFileByGRFID(found->second);
		if (grffile != nullptr) return grffile;
	}
	return nullptr;
}

/**
 * Returns the engine associated to a certain internal_id, resp. allocates it.
 * @param file NewGRF that wants to change the engine.
 * @param type Vehicle type.
 * @param internal_id Engine ID inside the NewGRF.
 * @param static_access If the engine is not present, return nullptr instead of allocating a new engine. (Used for static Action 0x04).
 * @return The requested engine.
 */
static Engine *GetNewEngine(const GRFFile *file, VehicleType type, uint16_t internal_id, bool static_access = false)
{
	/* Hack for add-on GRFs that need to modify another GRF's engines. This lets
	 * them use the same engine slots. */
	uint32_t scope_grfid = INVALID_GRFID; // If not using dynamic_engines, all newgrfs share their ID range
	if (_settings_game.vehicle.dynamic_engines) {
		/* If dynamic_engies is enabled, there can be multiple independent ID ranges. */
		scope_grfid = file->grfid;
		if (auto it = _grf_id_overrides.find(file->grfid); it != std::end(_grf_id_overrides)) {
			scope_grfid = it->second;
			const GRFFile *grf_match = GetFileByGRFID(scope_grfid);
			if (grf_match == nullptr) {
				GrfMsg(5, "Tried mapping from GRFID {:x} to {:x} but target is not loaded", std::byteswap(file->grfid), std::byteswap(scope_grfid));
			} else {
				GrfMsg(5, "Mapping from GRFID {:x} to {:x}", std::byteswap(file->grfid), std::byteswap(scope_grfid));
			}
		}

		/* Check if the engine is registered in the override manager */
		EngineID engine = _engine_mngr.GetID(type, internal_id, scope_grfid);
		if (engine != INVALID_ENGINE) {
			Engine *e = Engine::Get(engine);
			if (!e->grf_prop.HasGrfFile()) {
				e->grf_prop.grfid = file->grfid;
				e->grf_prop.grffile = file;
			}
			return e;
		}
	}

	/* Check if there is an unreserved slot */
	EngineID engine = _engine_mngr.GetID(type, internal_id, INVALID_GRFID);
	if (engine != INVALID_ENGINE) {
		Engine *e = Engine::Get(engine);

		if (!e->grf_prop.HasGrfFile()) {
			e->grf_prop.grfid = file->grfid;
			e->grf_prop.grffile = file;
			GrfMsg(5, "Replaced engine at index {} for GRFID {:x}, type {}, index {}", e->index, std::byteswap(file->grfid), type, internal_id);
		}

		/* Reserve the engine slot */
		if (!static_access) {
			_engine_mngr.RemoveFromIndex(engine);
			EngineIDMapping &eid = _engine_mngr.mappings[engine];
			eid.grfid = scope_grfid; // Note: this is INVALID_GRFID if dynamic_engines is disabled, so no reservation
			_engine_mngr.AddToIndex(engine);
		}

		return e;
	}

	if (static_access) return nullptr;

	if (!Engine::CanAllocateItem()) {
		GrfMsg(0, "Can't allocate any more engines");
		return nullptr;
	}

	size_t engine_pool_size = Engine::GetPoolSize();

	/* ... it's not, so create a new one based off an existing engine */
	Engine *e = new Engine(type, internal_id);
	e->grf_prop.grfid = file->grfid;
	e->grf_prop.grffile = file;

	/* Reserve the engine slot */
	assert(_engine_mngr.mappings.size() == e->index);
	_engine_mngr.mappings.push_back({
			scope_grfid, // Note: this is INVALID_GRFID if dynamic_engines is disabled, so no reservation
			internal_id,
			type,
			std::min<uint8_t>(internal_id, _engine_counts[type]) // substitute_id == _engine_counts[subtype] means "no substitute"
	});
	_engine_mngr.AddToIndex(e->index);

	if (engine_pool_size != Engine::GetPoolSize()) {
		/* Resize temporary engine data ... */
		_gted.resize(Engine::GetPoolSize());
	}
	if (type == VEH_TRAIN) {
		_gted[e->index].railtypelabel = GetRailTypeInfo(e->u.rail.railtype)->label;
	}

	GrfMsg(5, "Created new engine at index {} for GRFID {:x}, type {}, index {}", e->index, std::byteswap(file->grfid), type, internal_id);

	return e;
}

/**
 * Return the ID of a new engine
 * @param file The NewGRF file providing the engine.
 * @param type The Vehicle type.
 * @param internal_id NewGRF-internal ID of the engine.
 * @return The new EngineID.
 * @note depending on the dynamic_engine setting and a possible override
 *       property the grfID may be unique or overwriting or partially re-defining
 *       properties of an existing engine.
 */
EngineID GetNewEngineID(const GRFFile *file, VehicleType type, uint16_t internal_id)
{
	uint32_t scope_grfid = INVALID_GRFID; // If not using dynamic_engines, all newgrfs share their ID range
	if (_settings_game.vehicle.dynamic_engines) {
		scope_grfid = file->grfid;
		if (auto it = _grf_id_overrides.find(file->grfid); it != std::end(_grf_id_overrides)) {
			scope_grfid = it->second;
		}
	}

	return _engine_mngr.GetID(type, internal_id, scope_grfid);
}

/**
 * Map the colour modifiers of TTDPatch to those that Open is using.
 * @param grf_sprite Pointer to the structure been modified.
 */
static void MapSpriteMappingRecolour(PalSpriteID *grf_sprite)
{
	if (HasBit(grf_sprite->pal, 14)) {
		ClrBit(grf_sprite->pal, 14);
		SetBit(grf_sprite->sprite, SPRITE_MODIFIER_OPAQUE);
	}

	if (HasBit(grf_sprite->sprite, 14)) {
		ClrBit(grf_sprite->sprite, 14);
		SetBit(grf_sprite->sprite, PALETTE_MODIFIER_TRANSPARENT);
	}

	if (HasBit(grf_sprite->sprite, 15)) {
		ClrBit(grf_sprite->sprite, 15);
		SetBit(grf_sprite->sprite, PALETTE_MODIFIER_COLOUR);
	}
}

/**
 * Read a sprite and a palette from the GRF and convert them into a format
 * suitable to OpenTTD.
 * @param buf                 Input stream.
 * @param read_flags          Whether to read TileLayoutFlags.
 * @param invert_action1_flag Set to true, if palette bit 15 means 'not from action 1'.
 * @param use_cur_spritesets  Whether to use currently referenceable action 1 sets.
 * @param feature             GrfSpecFeature to use spritesets from.
 * @param[out] grf_sprite     Read sprite and palette.
 * @param[out] max_sprite_offset  Optionally returns the number of sprites in the spriteset of the sprite. (0 if no spritset)
 * @param[out] max_palette_offset Optionally returns the number of sprites in the spriteset of the palette. (0 if no spritset)
 * @return Read TileLayoutFlags.
 */
static TileLayoutFlags ReadSpriteLayoutSprite(ByteReader &buf, bool read_flags, bool invert_action1_flag, bool use_cur_spritesets, int feature, PalSpriteID *grf_sprite, uint16_t *max_sprite_offset = nullptr, uint16_t *max_palette_offset = nullptr)
{
	grf_sprite->sprite = buf.ReadWord();
	grf_sprite->pal = buf.ReadWord();
	TileLayoutFlags flags = read_flags ? (TileLayoutFlags)buf.ReadWord() : TLF_NOTHING;

	MapSpriteMappingRecolour(grf_sprite);

	bool custom_sprite = HasBit(grf_sprite->pal, 15) != invert_action1_flag;
	ClrBit(grf_sprite->pal, 15);

	if (custom_sprite) {
		/* Use sprite from Action 1 */
		uint index = GB(grf_sprite->sprite, 0, 14);
		SpriteSetInfo sprite_set_info;
		if (use_cur_spritesets) sprite_set_info = _cur.GetSpriteSetInfo(feature, index);
		if (use_cur_spritesets && (!sprite_set_info.IsValid() || sprite_set_info.GetNumEnts() == 0)) {
			GrfMsg(1, "ReadSpriteLayoutSprite: Spritelayout uses undefined custom spriteset {}", index);
			grf_sprite->sprite = SPR_IMG_QUERY;
			grf_sprite->pal = PAL_NONE;
		} else {
			SpriteID sprite = use_cur_spritesets ? sprite_set_info.GetSprite() : index;
			if (max_sprite_offset != nullptr) *max_sprite_offset = use_cur_spritesets ? sprite_set_info.GetNumEnts() : UINT16_MAX;
			SB(grf_sprite->sprite, 0, SPRITE_WIDTH, sprite);
			SetBit(grf_sprite->sprite, SPRITE_MODIFIER_CUSTOM_SPRITE);
		}
	} else if ((flags & TLF_SPRITE_VAR10) && !(flags & TLF_SPRITE_REG_FLAGS)) {
		GrfMsg(1, "ReadSpriteLayoutSprite: Spritelayout specifies var10 value for non-action-1 sprite");
		DisableGrf(STR_NEWGRF_ERROR_INVALID_SPRITE_LAYOUT);
		return flags;
	}

	if (flags & TLF_CUSTOM_PALETTE) {
		/* Use palette from Action 1 */
		uint index = GB(grf_sprite->pal, 0, 14);
		SpriteSetInfo sprite_set_info;
		if (use_cur_spritesets) sprite_set_info = _cur.GetSpriteSetInfo(feature, index);
		if (use_cur_spritesets && (!sprite_set_info.IsValid() || sprite_set_info.GetNumEnts() == 0)) {
			GrfMsg(1, "ReadSpriteLayoutSprite: Spritelayout uses undefined custom spriteset {} for 'palette'", index);
			grf_sprite->pal = PAL_NONE;
		} else {
			SpriteID sprite = use_cur_spritesets ? sprite_set_info.GetSprite() : index;
			if (max_palette_offset != nullptr) *max_palette_offset = use_cur_spritesets ? sprite_set_info.GetNumEnts() : UINT16_MAX;
			SB(grf_sprite->pal, 0, SPRITE_WIDTH, sprite);
			SetBit(grf_sprite->pal, SPRITE_MODIFIER_CUSTOM_SPRITE);
		}
	} else if ((flags & TLF_PALETTE_VAR10) && !(flags & TLF_PALETTE_REG_FLAGS)) {
		GrfMsg(1, "ReadSpriteLayoutRegisters: Spritelayout specifies var10 value for non-action-1 palette");
		DisableGrf(STR_NEWGRF_ERROR_INVALID_SPRITE_LAYOUT);
		return flags;
	}

	return flags;
}

/**
 * Preprocess the TileLayoutFlags and read register modifiers from the GRF.
 * @param buf        Input stream.
 * @param flags      TileLayoutFlags to process.
 * @param is_parent  Whether the sprite is a parentsprite with a bounding box.
 * @param dts        Sprite layout to insert data into.
 * @param index      Sprite index to process; 0 for ground sprite.
 */
static void ReadSpriteLayoutRegisters(ByteReader &buf, TileLayoutFlags flags, bool is_parent, NewGRFSpriteLayout *dts, uint index)
{
	if (!(flags & TLF_DRAWING_FLAGS)) return;

	if (dts->registers == nullptr) dts->AllocateRegisters();
	TileLayoutRegisters &regs = const_cast<TileLayoutRegisters&>(dts->registers[index]);
	regs.flags = flags & TLF_DRAWING_FLAGS;

	if (flags & TLF_DODRAW)  regs.dodraw  = buf.ReadByte();
	if (flags & TLF_SPRITE)  regs.sprite  = buf.ReadByte();
	if (flags & TLF_PALETTE) regs.palette = buf.ReadByte();

	if (is_parent) {
		if (flags & TLF_BB_XY_OFFSET) {
			regs.delta.parent[0] = buf.ReadByte();
			regs.delta.parent[1] = buf.ReadByte();
		}
		if (flags & TLF_BB_Z_OFFSET)    regs.delta.parent[2] = buf.ReadByte();
	} else {
		if (flags & TLF_CHILD_X_OFFSET) regs.delta.child[0]  = buf.ReadByte();
		if (flags & TLF_CHILD_Y_OFFSET) regs.delta.child[1]  = buf.ReadByte();
	}

	if (flags & TLF_SPRITE_VAR10) {
		regs.sprite_var10 = buf.ReadByte();
		if (regs.sprite_var10 > TLR_MAX_VAR10) {
			GrfMsg(1, "ReadSpriteLayoutRegisters: Spritelayout specifies var10 ({}) exceeding the maximal allowed value {}", regs.sprite_var10, TLR_MAX_VAR10);
			DisableGrf(STR_NEWGRF_ERROR_INVALID_SPRITE_LAYOUT);
			return;
		}
	}

	if (flags & TLF_PALETTE_VAR10) {
		regs.palette_var10 = buf.ReadByte();
		if (regs.palette_var10 > TLR_MAX_VAR10) {
			GrfMsg(1, "ReadSpriteLayoutRegisters: Spritelayout specifies var10 ({}) exceeding the maximal allowed value {}", regs.palette_var10, TLR_MAX_VAR10);
			DisableGrf(STR_NEWGRF_ERROR_INVALID_SPRITE_LAYOUT);
			return;
		}
	}
}

/**
 * Read a spritelayout from the GRF.
 * @param buf                  Input
 * @param num_building_sprites Number of building sprites to read
 * @param use_cur_spritesets   Whether to use currently referenceable action 1 sets.
 * @param feature              GrfSpecFeature to use spritesets from.
 * @param allow_var10          Whether the spritelayout may specify var10 values for resolving multiple action-1-2-3 chains
 * @param no_z_position        Whether bounding boxes have no Z offset
 * @param dts                  Layout container to output into
 * @return True on error (GRF was disabled).
 */
static bool ReadSpriteLayout(ByteReader &buf, uint num_building_sprites, bool use_cur_spritesets, uint8_t feature, bool allow_var10, bool no_z_position, NewGRFSpriteLayout *dts)
{
	bool has_flags = HasBit(num_building_sprites, 6);
	ClrBit(num_building_sprites, 6);
	TileLayoutFlags valid_flags = TLF_KNOWN_FLAGS;
	if (!allow_var10) valid_flags &= ~TLF_VAR10_FLAGS;
	dts->Allocate(num_building_sprites); // allocate before reading groundsprite flags

	TempBufferT<uint16_t, 16> max_sprite_offset(num_building_sprites + 1, 0);
	TempBufferT<uint16_t, 16> max_palette_offset(num_building_sprites + 1, 0);

	/* Groundsprite */
	TileLayoutFlags flags = ReadSpriteLayoutSprite(buf, has_flags, false, use_cur_spritesets, feature, &dts->ground, max_sprite_offset, max_palette_offset);
	if (_cur.skip_sprites < 0) return true;

	if (flags & ~(valid_flags & ~TLF_NON_GROUND_FLAGS)) {
		GrfMsg(1, "ReadSpriteLayout: Spritelayout uses invalid flag 0x{:X} for ground sprite", flags & ~(valid_flags & ~TLF_NON_GROUND_FLAGS));
		DisableGrf(STR_NEWGRF_ERROR_INVALID_SPRITE_LAYOUT);
		return true;
	}

	ReadSpriteLayoutRegisters(buf, flags, false, dts, 0);
	if (_cur.skip_sprites < 0) return true;

	for (uint i = 0; i < num_building_sprites; i++) {
		DrawTileSeqStruct *seq = const_cast<DrawTileSeqStruct*>(&dts->seq[i]);

		flags = ReadSpriteLayoutSprite(buf, has_flags, false, use_cur_spritesets, feature, &seq->image, max_sprite_offset + i + 1, max_palette_offset + i + 1);
		if (_cur.skip_sprites < 0) return true;

		if (flags & ~valid_flags) {
			GrfMsg(1, "ReadSpriteLayout: Spritelayout uses unknown flag 0x{:X}", flags & ~valid_flags);
			DisableGrf(STR_NEWGRF_ERROR_INVALID_SPRITE_LAYOUT);
			return true;
		}

		seq->delta_x = buf.ReadByte();
		seq->delta_y = buf.ReadByte();

		if (!no_z_position) seq->delta_z = buf.ReadByte();

		if (seq->IsParentSprite()) {
			seq->size_x = buf.ReadByte();
			seq->size_y = buf.ReadByte();
			seq->size_z = buf.ReadByte();
		}

		ReadSpriteLayoutRegisters(buf, flags, seq->IsParentSprite(), dts, i + 1);
		if (_cur.skip_sprites < 0) return true;
	}

	/* Check if the number of sprites per spriteset is consistent */
	bool is_consistent = true;
	dts->consistent_max_offset = 0;
	for (uint i = 0; i < num_building_sprites + 1; i++) {
		if (max_sprite_offset[i] > 0) {
			if (dts->consistent_max_offset == 0) {
				dts->consistent_max_offset = max_sprite_offset[i];
			} else if (dts->consistent_max_offset != max_sprite_offset[i]) {
				is_consistent = false;
				break;
			}
		}
		if (max_palette_offset[i] > 0) {
			if (dts->consistent_max_offset == 0) {
				dts->consistent_max_offset = max_palette_offset[i];
			} else if (dts->consistent_max_offset != max_palette_offset[i]) {
				is_consistent = false;
				break;
			}
		}
	}

	/* When the Action1 sets are unknown, everything should be 0 (no spriteset usage) or UINT16_MAX (some spriteset usage) */
	assert(use_cur_spritesets || (is_consistent && (dts->consistent_max_offset == 0 || dts->consistent_max_offset == UINT16_MAX)));

	if (!is_consistent || dts->registers != nullptr) {
		dts->consistent_max_offset = 0;
		if (dts->registers == nullptr) dts->AllocateRegisters();

		for (uint i = 0; i < num_building_sprites + 1; i++) {
			TileLayoutRegisters &regs = const_cast<TileLayoutRegisters&>(dts->registers[i]);
			regs.max_sprite_offset = max_sprite_offset[i];
			regs.max_palette_offset = max_palette_offset[i];
		}
	}

	return false;
}

/**
 * Translate the refit mask. refit_mask is uint32_t as it has not been mapped to CargoTypes.
 */
static CargoTypes TranslateRefitMask(uint32_t refit_mask)
{
	CargoTypes result = 0;
	for (uint8_t bit : SetBitIterator(refit_mask)) {
		CargoType cargo = GetCargoTranslation(bit, _cur.grffile, true);
		if (IsValidCargoType(cargo)) SetBit(result, cargo);
	}
	return result;
}

/**
 * Converts TTD(P) Base Price pointers into the enum used by OTTD
 * See http://wiki.ttdpatch.net/tiki-index.php?page=BaseCosts
 * @param base_pointer TTD(P) Base Price Pointer
 * @param error_location Function name for grf error messages
 * @param[out] index If \a base_pointer is valid, \a index is assigned to the matching price; else it is left unchanged
 */
static void ConvertTTDBasePrice(uint32_t base_pointer, const char *error_location, Price *index)
{
	/* Special value for 'none' */
	if (base_pointer == 0) {
		*index = INVALID_PRICE;
		return;
	}

	static const uint32_t start = 0x4B34; ///< Position of first base price
	static const uint32_t size  = 6;      ///< Size of each base price record

	if (base_pointer < start || (base_pointer - start) % size != 0 || (base_pointer - start) / size >= PR_END) {
		GrfMsg(1, "{}: Unsupported running cost base 0x{:04X}, ignoring", error_location, base_pointer);
		return;
	}

	*index = (Price)((base_pointer - start) / size);
}

/** Possible return values for the FeatureChangeInfo functions */
enum ChangeInfoResult : uint8_t {
	CIR_SUCCESS,    ///< Variable was parsed and read
	CIR_DISABLED,   ///< GRF was disabled due to error
	CIR_UNHANDLED,  ///< Variable was parsed but unread
	CIR_UNKNOWN,    ///< Variable is unknown
	CIR_INVALID_ID, ///< Attempt to modify an invalid ID
};

using ChangeInfoHandler = ChangeInfoResult(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf);

static ChangeInfoResult HandleAction0PropertyDefault(ByteReader &buf, int prop)
{
	if (prop == A0RPI_UNKNOWN_ERROR) {
		return CIR_DISABLED;
	} else if (prop < A0RPI_UNKNOWN_IGNORE) {
		return CIR_UNKNOWN;
	} else {
		buf.Skip(buf.ReadExtendedByte());
		return CIR_SUCCESS;
	}
}

static bool MappedPropertyLengthMismatch(ByteReader &buf, uint expected_size, const GRFFilePropertyRemapEntry *mapping_entry)
{
	uint length = buf.ReadExtendedByte();
	if (length != expected_size) {
		if (mapping_entry != nullptr) {
			GrfMsg(2, "Ignoring use of mapped property: {}, feature: {}, mapped to: {:X}{}, with incorrect data size: {} instead of {}",
					mapping_entry->name, GetFeatureString(mapping_entry->feature),
					mapping_entry->property_id, mapping_entry->extended ? " (extended)" : "",
					length, expected_size);
		}
		buf.Skip(length);
		return true;
	} else {
		return false;
	}
}

/**
 * Define properties common to all vehicles
 * @param ei Engine info.
 * @param prop The property to change.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult CommonVehicleChangeInfo(EngineInfo *ei, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	switch (prop) {
		case 0x00: // Introduction date
			ei->base_intro = CalTime::DAYS_TILL_ORIGINAL_BASE_YEAR + buf.ReadWord();
			break;

		case 0x02: // Decay speed
			ei->decay_speed = buf.ReadByte();
			break;

		case 0x03: // Vehicle life
			ei->lifelength = CalTime::YearDelta{buf.ReadByte()};
			break;

		case 0x04: // Model life
			ei->base_life = CalTime::YearDelta{buf.ReadByte()};
			break;

		case 0x06: // Climates available
			ei->climates = LandscapeTypes{buf.ReadByte()};
			break;

		case PROP_VEHICLE_LOAD_AMOUNT: // 0x07 Loading speed
			/* Amount of cargo loaded during a vehicle's "loading tick" */
			ei->load_amount = buf.ReadByte();
			break;

		default:
			return HandleAction0PropertyDefault(buf, prop);
	}

	return CIR_SUCCESS;
}

/**
 * Skip a list of badges.
 * @param buf Buffer reader containing list of badges to skip.
 */
static void SkipBadgeList(ByteReader &buf)
{
	uint16_t count = buf.ReadWord();
	while (count-- > 0) {
		buf.ReadWord();
	}
}

/**
 * Read a list of badges.
 * @param buf Buffer reader containing list of badges to read.
 * @param feature The feature of the badge list.
 * @returns list of badges.
 */
static std::vector<BadgeID> ReadBadgeList(ByteReader &buf, GrfSpecFeature feature)
{
	uint16_t count = buf.ReadWord();

	std::vector<BadgeID> badges;
	badges.reserve(count);

	while (count-- > 0) {
		uint16_t local_index = buf.ReadWord();
		if (local_index >= std::size(_cur.grffile->badge_list)) {
			GrfMsg(1, "ReadBadgeList: Badge label {} out of range (max {}), skipping.", local_index, std::size(_cur.grffile->badge_list) - 1);
			continue;
		}

		BadgeID index = _cur.grffile->badge_list[local_index];

		/* Is badge already present? */
		if (std::ranges::find(badges, index) != std::end(badges)) continue;

		badges.push_back(index);
		MarkBadgeSeen(index, feature);
	}

	return badges;
}

/**
 * Define properties for rail vehicles
 * @param first Local ID of the first vehicle.
 * @param last Local ID of the last vehicle.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult RailVehicleChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	for (uint id = first; id < last; ++id) {
		Engine *e = GetNewEngine(_cur.grffile, VEH_TRAIN, id);
		if (e == nullptr) return CIR_INVALID_ID; // No engine could be allocated, so neither can any next vehicles

		EngineInfo *ei = &e->info;
		RailVehicleInfo *rvi = &e->u.rail;

		switch (prop) {
			case 0x05: { // Track type
				uint8_t tracktype = buf.ReadByte();

				if (tracktype < _cur.grffile->railtype_list.size()) {
					_gted[e->index].railtypelabel = _cur.grffile->railtype_list[tracktype];
					break;
				}

				switch (tracktype) {
					case 0: _gted[e->index].railtypelabel = rvi->engclass >= 2 ? RAILTYPE_LABEL_ELECTRIC : RAILTYPE_LABEL_RAIL; break;
					case 1: _gted[e->index].railtypelabel = RAILTYPE_LABEL_MONO; break;
					case 2: _gted[e->index].railtypelabel = RAILTYPE_LABEL_MAGLEV; break;
					default:
						GrfMsg(1, "RailVehicleChangeInfo: Invalid track type {} specified, ignoring", tracktype);
						break;
				}
				break;
			}

			case 0x08: // AI passenger service
				/* Tells the AI that this engine is designed for
				 * passenger services and shouldn't be used for freight. */
				rvi->ai_passenger_only = buf.ReadByte();
				break;

			case PROP_TRAIN_SPEED: { // 0x09 Speed (1 unit is 1 km-ish/h)
				uint16_t speed = buf.ReadWord();
				if (speed == 0xFFFF) speed = 0;

				rvi->max_speed = speed;
				break;
			}

			case PROP_TRAIN_POWER: // 0x0B Power
				rvi->power = buf.ReadWord();

				/* Set engine / wagon state based on power */
				if (rvi->power != 0) {
					if (rvi->railveh_type == RAILVEH_WAGON) {
						rvi->railveh_type = RAILVEH_SINGLEHEAD;
					}
				} else {
					rvi->railveh_type = RAILVEH_WAGON;
				}
				break;

			case PROP_TRAIN_RUNNING_COST_FACTOR: // 0x0D Running cost factor
				rvi->running_cost = buf.ReadByte();
				break;

			case 0x0E: // Running cost base
				ConvertTTDBasePrice(buf.ReadDWord(), "RailVehicleChangeInfo", &rvi->running_cost_class);
				break;

			case 0x12: { // Sprite ID
				uint8_t spriteid = buf.ReadByte();
				uint8_t orig_spriteid = spriteid;

				/* TTD sprite IDs point to a location in a 16bit array, but we use it
				 * as an array index, so we need it to be half the original value. */
				if (spriteid < 0xFD) spriteid >>= 1;

				if (IsValidNewGRFImageIndex<VEH_TRAIN>(spriteid)) {
					rvi->image_index = spriteid;
				} else {
					GrfMsg(1, "RailVehicleChangeInfo: Invalid Sprite {} specified, ignoring", orig_spriteid);
					rvi->image_index = 0;
				}
				break;
			}

			case 0x13: { // Dual-headed
				uint8_t dual = buf.ReadByte();

				if (dual != 0) {
					rvi->railveh_type = RAILVEH_MULTIHEAD;
				} else {
					rvi->railveh_type = rvi->power == 0 ?
						RAILVEH_WAGON : RAILVEH_SINGLEHEAD;
				}
				break;
			}

			case PROP_TRAIN_CARGO_CAPACITY: // 0x14 Cargo capacity
				rvi->capacity = buf.ReadByte();
				break;

			case 0x15: { // Cargo type
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				uint8_t ctype = buf.ReadByte();

				if (ctype == 0xFF) {
					/* 0xFF is specified as 'use first refittable' */
					ei->cargo_type = INVALID_CARGO;
				} else {
					/* Use translated cargo. Might result in INVALID_CARGO (first refittable), if cargo is not defined. */
					ei->cargo_type = GetCargoTranslation(ctype, _cur.grffile);
					if (ei->cargo_type == INVALID_CARGO) GrfMsg(2, "RailVehicleChangeInfo: Invalid cargo type {}, using first refittable", ctype);
				}
				ei->cargo_label = CT_INVALID;
				break;
			}

			case PROP_TRAIN_WEIGHT: // 0x16 Weight
				SB(rvi->weight, 0, 8, buf.ReadByte());
				break;

			case PROP_TRAIN_COST_FACTOR: // 0x17 Cost factor
				rvi->cost_factor = buf.ReadByte();
				break;

			case 0x18: // AI rank
				GrfMsg(2, "RailVehicleChangeInfo: Property 0x18 'AI rank' not used by NoAI, ignored.");
				buf.ReadByte();
				break;

			case 0x19: { // Engine traction type
				/* What do the individual numbers mean?
				 * 0x00 .. 0x07: Steam
				 * 0x08 .. 0x27: Diesel
				 * 0x28 .. 0x31: Electric
				 * 0x32 .. 0x37: Monorail
				 * 0x38 .. 0x41: Maglev
				 */
				uint8_t traction = buf.ReadByte();
				EngineClass engclass;

				if (traction <= 0x07) {
					engclass = EC_STEAM;
				} else if (traction <= 0x27) {
					engclass = EC_DIESEL;
				} else if (traction <= 0x31) {
					engclass = EC_ELECTRIC;
				} else if (traction <= 0x37) {
					engclass = EC_MONORAIL;
				} else if (traction <= 0x41) {
					engclass = EC_MAGLEV;
				} else {
					break;
				}

				if (_cur.grffile->railtype_list.empty()) {
					/* Use traction type to select between normal and electrified
					 * rail only when no translation list is in place. */
					if (_gted[e->index].railtypelabel == RAILTYPE_LABEL_RAIL     && engclass >= EC_ELECTRIC) _gted[e->index].railtypelabel = RAILTYPE_LABEL_ELECTRIC;
					if (_gted[e->index].railtypelabel == RAILTYPE_LABEL_ELECTRIC && engclass  < EC_ELECTRIC) _gted[e->index].railtypelabel = RAILTYPE_LABEL_RAIL;
				}

				rvi->engclass = engclass;
				break;
			}

			case 0x1A: // Alter purchase list sort order
				AlterVehicleListOrder(e->index, buf.ReadExtendedByte());
				break;

			case 0x1B: // Powered wagons power bonus
				rvi->pow_wag_power = buf.ReadWord();
				break;

			case 0x1C: // Refit cost
				ei->refit_cost = buf.ReadByte();
				break;

			case 0x1D: { // Refit cargo
				uint32_t mask = buf.ReadDWord();
				_gted[e->index].UpdateRefittability(mask != 0);
				ei->refit_mask = TranslateRefitMask(mask);
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				break;
			}

			case 0x1E: { // Callback
				auto mask = ei->callback_mask.base();
				SB(mask, 0, 8, buf.ReadByte());
				ei->callback_mask = VehicleCallbackMasks{mask};
				break;
			}

			case PROP_TRAIN_TRACTIVE_EFFORT: // 0x1F Tractive effort coefficient
				rvi->tractive_effort = buf.ReadByte();
				break;

			case 0x20: // Air drag
				rvi->air_drag = buf.ReadByte();
				break;

			case PROP_TRAIN_SHORTEN_FACTOR: // 0x21 Shorter vehicle
				rvi->shorten_factor = buf.ReadByte();
				break;

			case 0x22: // Visual effect
				rvi->visual_effect = buf.ReadByte();
				/* Avoid accidentally setting visual_effect to the default value
				 * Since bit 6 (disable effects) is set anyways, we can safely erase some bits. */
				if (rvi->visual_effect == VE_DEFAULT) {
					assert(HasBit(rvi->visual_effect, VE_DISABLE_EFFECT));
					SB(rvi->visual_effect, VE_TYPE_START, VE_TYPE_COUNT, 0);
				}
				break;

			case 0x23: // Powered wagons weight bonus
				rvi->pow_wag_weight = buf.ReadByte();
				break;

			case 0x24: { // High byte of vehicle weight
				uint8_t weight = buf.ReadByte();

				if (weight > 4) {
					GrfMsg(2, "RailVehicleChangeInfo: Nonsensical weight of {} tons, ignoring", weight << 8);
				} else {
					SB(rvi->weight, 8, 8, weight);
				}
				break;
			}

			case PROP_TRAIN_USER_DATA: // 0x25 User-defined bit mask to set when checking veh. var. 42
				rvi->user_def_data = buf.ReadByte();
				break;

			case 0x26: // Retire vehicle early
				ei->retire_early = buf.ReadByte();
				break;

			case 0x27: // Miscellaneous flags
				ei->misc_flags = static_cast<EngineMiscFlags>(buf.ReadByte());
				_loaded_newgrf_features.has_2CC |= ei->misc_flags.Test(EngineMiscFlag::Uses2CC);
				break;

			case 0x28: // Cargo classes allowed
				_gted[e->index].cargo_allowed = buf.ReadWord();
				_gted[e->index].UpdateRefittability(_gted[e->index].cargo_allowed != 0);
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				break;

			case 0x29: // Cargo classes disallowed
				_gted[e->index].cargo_disallowed = buf.ReadWord();
				_gted[e->index].UpdateRefittability(false);
				break;

			case 0x2A: // Long format introduction date (days since year 0)
				ei->base_intro = CalTime::Date(static_cast<int32_t>(buf.ReadDWord()));
				break;

			case PROP_TRAIN_CARGO_AGE_PERIOD: // 0x2B Cargo aging period
				ei->cargo_age_period = buf.ReadWord();
				break;

			case 0x2C:   // CTT refit include list
			case 0x2D: { // CTT refit exclude list
				uint8_t count = buf.ReadByte();
				_gted[e->index].UpdateRefittability(prop == 0x2C && count != 0);
				if (prop == 0x2C) _gted[e->index].defaultcargo_grf = _cur.grffile;
				CargoTypes &ctt = prop == 0x2C ? _gted[e->index].ctt_include_mask : _gted[e->index].ctt_exclude_mask;
				ctt = 0;
				while (count--) {
					CargoType ctype = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
					if (IsValidCargoType(ctype)) SetBit(ctt, ctype);
				}
				break;
			}

			case PROP_TRAIN_CURVE_SPEED_MOD: // 0x2E Curve speed modifier
				rvi->curve_speed_mod = buf.ReadWord();
				break;

			case 0x2F: // Engine variant
				ei->variant_id = buf.ReadWord();
				break;

			case 0x30: // Extra miscellaneous flags
				ei->extra_flags = static_cast<ExtraEngineFlags>(buf.ReadDWord());
				break;

			case 0x31: { // Callback additional mask
				auto mask = ei->callback_mask.base();
				SB(mask, 8, 8, buf.ReadByte());
				ei->callback_mask = VehicleCallbackMasks{mask};
				break;
			}

			case 0x32: // Cargo classes required for a refit.
				_gted[e->index].cargo_allowed_required = buf.ReadWord();
				break;

			case 0x33: // Badge list
				e->badges = ReadBadgeList(buf, GSF_TRAINS);
				break;

			default:
				ret = CommonVehicleChangeInfo(ei, prop, mapping_entry, buf);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for road vehicles
 * @param first Local ID of the first vehicle.
 * @param last Local ID of the last vehicle.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult RoadVehicleChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	for (uint id = first; id < last; ++id) {
		Engine *e = GetNewEngine(_cur.grffile, VEH_ROAD, id);
		if (e == nullptr) return CIR_INVALID_ID; // No engine could be allocated, so neither can any next vehicles

		EngineInfo *ei = &e->info;
		RoadVehicleInfo *rvi = &e->u.road;

		switch (prop) {
			case 0x05: // Road/tram type
				/* RoadTypeLabel is looked up later after the engine's road/tram
				 * flag is set, however 0 means the value has not been set. */
				_gted[e->index].roadtramtype = buf.ReadByte() + 1;
				break;

			case 0x08: // Speed (1 unit is 0.5 kmh)
				rvi->max_speed = buf.ReadByte();
				break;

			case PROP_ROADVEH_RUNNING_COST_FACTOR: // 0x09 Running cost factor
				rvi->running_cost = buf.ReadByte();
				break;

			case 0x0A: // Running cost base
				ConvertTTDBasePrice(buf.ReadDWord(), "RoadVehicleChangeInfo", &rvi->running_cost_class);
				break;

			case 0x0E: { // Sprite ID
				uint8_t spriteid = buf.ReadByte();
				uint8_t orig_spriteid = spriteid;

				/* cars have different custom id in the GRF file */
				if (spriteid == 0xFF) spriteid = 0xFD;

				if (spriteid < 0xFD) spriteid >>= 1;

				if (IsValidNewGRFImageIndex<VEH_ROAD>(spriteid)) {
					rvi->image_index = spriteid;
				} else {
					GrfMsg(1, "RoadVehicleChangeInfo: Invalid Sprite {} specified, ignoring", orig_spriteid);
					rvi->image_index = 0;
				}
				break;
			}

			case PROP_ROADVEH_CARGO_CAPACITY: // 0x0F Cargo capacity
				rvi->capacity = buf.ReadByte();
				break;

			case 0x10: { // Cargo type
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				uint8_t ctype = buf.ReadByte();

				if (ctype == 0xFF) {
					/* 0xFF is specified as 'use first refittable' */
					ei->cargo_type = INVALID_CARGO;
				} else {
					/* Use translated cargo. Might result in INVALID_CARGO (first refittable), if cargo is not defined. */
					ei->cargo_type = GetCargoTranslation(ctype, _cur.grffile);
					if (ei->cargo_type == INVALID_CARGO) GrfMsg(2, "RoadVehicleChangeInfo: Invalid cargo type {}, using first refittable", ctype);
				}
				ei->cargo_label = CT_INVALID;
				break;
			}

			case PROP_ROADVEH_COST_FACTOR: // 0x11 Cost factor
				rvi->cost_factor = buf.ReadByte();
				break;

			case 0x12: // SFX
				rvi->sfx = GetNewGRFSoundID(_cur.grffile, buf.ReadByte());
				break;

			case PROP_ROADVEH_POWER: // Power in units of 10 HP.
				rvi->power = buf.ReadByte();
				break;

			case PROP_ROADVEH_WEIGHT: // Weight in units of 1/4 tons.
				rvi->weight = buf.ReadByte();
				break;

			case PROP_ROADVEH_SPEED: // Speed in mph/0.8
				_gted[e->index].rv_max_speed = buf.ReadByte();
				break;

			case 0x16: { // Cargoes available for refitting
				uint32_t mask = buf.ReadDWord();
				_gted[e->index].UpdateRefittability(mask != 0);
				ei->refit_mask = TranslateRefitMask(mask);
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				break;
			}

			case 0x17: { // Callback mask
				auto mask = ei->callback_mask.base();
				SB(mask, 0, 8, buf.ReadByte());
				ei->callback_mask = VehicleCallbackMasks{mask};
				break;
			}

			case PROP_ROADVEH_TRACTIVE_EFFORT: // Tractive effort coefficient in 1/256.
				rvi->tractive_effort = buf.ReadByte();
				break;

			case 0x19: // Air drag
				rvi->air_drag = buf.ReadByte();
				break;

			case 0x1A: // Refit cost
				ei->refit_cost = buf.ReadByte();
				break;

			case 0x1B: // Retire vehicle early
				ei->retire_early = buf.ReadByte();
				break;

			case 0x1C: // Miscellaneous flags
				ei->misc_flags = static_cast<EngineMiscFlags>(buf.ReadByte());
				_loaded_newgrf_features.has_2CC |= ei->misc_flags.Test(EngineMiscFlag::Uses2CC);
				break;

			case 0x1D: // Cargo classes allowed
				_gted[e->index].cargo_allowed = buf.ReadWord();
				_gted[e->index].UpdateRefittability(_gted[e->index].cargo_allowed != 0);
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				break;

			case 0x1E: // Cargo classes disallowed
				_gted[e->index].cargo_disallowed = buf.ReadWord();
				_gted[e->index].UpdateRefittability(false);
				break;

			case 0x1F: // Long format introduction date (days since year 0)
				ei->base_intro = CalTime::Date(static_cast<int32_t>(buf.ReadDWord()));
				break;

			case 0x20: // Alter purchase list sort order
				AlterVehicleListOrder(e->index, buf.ReadExtendedByte());
				break;

			case 0x21: // Visual effect
				rvi->visual_effect = buf.ReadByte();
				/* Avoid accidentally setting visual_effect to the default value
				 * Since bit 6 (disable effects) is set anyways, we can safely erase some bits. */
				if (rvi->visual_effect == VE_DEFAULT) {
					assert(HasBit(rvi->visual_effect, VE_DISABLE_EFFECT));
					SB(rvi->visual_effect, VE_TYPE_START, VE_TYPE_COUNT, 0);
				}
				break;

			case PROP_ROADVEH_CARGO_AGE_PERIOD: // 0x22 Cargo aging period
				ei->cargo_age_period = buf.ReadWord();
				break;

			case PROP_ROADVEH_SHORTEN_FACTOR: // 0x23 Shorter vehicle
				rvi->shorten_factor = buf.ReadByte();
				break;

			case 0x24:   // CTT refit include list
			case 0x25: { // CTT refit exclude list
				uint8_t count = buf.ReadByte();
				_gted[e->index].UpdateRefittability(prop == 0x24 && count != 0);
				if (prop == 0x24) _gted[e->index].defaultcargo_grf = _cur.grffile;
				CargoTypes &ctt = prop == 0x24 ? _gted[e->index].ctt_include_mask : _gted[e->index].ctt_exclude_mask;
				ctt = 0;
				while (count--) {
					CargoType ctype = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
					if (IsValidCargoType(ctype)) SetBit(ctt, ctype);
				}
				break;
			}

			case 0x26: // Engine variant
				ei->variant_id = buf.ReadWord();
				break;

			case 0x27: // Extra miscellaneous flags
				ei->extra_flags = static_cast<ExtraEngineFlags>(buf.ReadDWord());
				break;

			case 0x28: { // Callback additional mask
				auto mask = ei->callback_mask.base();
				SB(mask, 8, 8, buf.ReadByte());
				ei->callback_mask = VehicleCallbackMasks{mask};
				break;
			}

			case 0x29: // Cargo classes required for a refit.
				_gted[e->index].cargo_allowed_required = buf.ReadWord();
				break;

			case 0x2A: // Badge list
				e->badges = ReadBadgeList(buf, GSF_ROADVEHICLES);
				break;

			default:
				ret = CommonVehicleChangeInfo(ei, prop, mapping_entry, buf);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for ships
 * @param first Local ID of the first vehicle.
 * @param last Local ID of the last vehicle.
 * @param prop The property to change.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult ShipVehicleChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	for (uint id = first; id < last; ++id) {
		Engine *e = GetNewEngine(_cur.grffile, VEH_SHIP, id);
		if (e == nullptr) return CIR_INVALID_ID; // No engine could be allocated, so neither can any next vehicles

		EngineInfo *ei = &e->info;
		ShipVehicleInfo *svi = &e->u.ship;

		switch (prop) {
			case 0x08: { // Sprite ID
				uint8_t spriteid = buf.ReadByte();
				uint8_t orig_spriteid = spriteid;

				/* ships have different custom id in the GRF file */
				if (spriteid == 0xFF) spriteid = 0xFD;

				if (spriteid < 0xFD) spriteid >>= 1;

				if (IsValidNewGRFImageIndex<VEH_SHIP>(spriteid)) {
					svi->image_index = spriteid;
				} else {
					GrfMsg(1, "ShipVehicleChangeInfo: Invalid Sprite {} specified, ignoring", orig_spriteid);
					svi->image_index = 0;
				}
				break;
			}

			case 0x09: // Refittable
				svi->old_refittable = (buf.ReadByte() != 0);
				break;

			case PROP_SHIP_COST_FACTOR: // 0x0A Cost factor
				svi->cost_factor = buf.ReadByte();
				break;

			case PROP_SHIP_SPEED: // 0x0B Speed (1 unit is 0.5 km-ish/h). Use 0x23 to achieve higher speeds.
				svi->max_speed = buf.ReadByte();
				break;

			case 0x0C: { // Cargo type
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				uint8_t ctype = buf.ReadByte();

				if (ctype == 0xFF) {
					/* 0xFF is specified as 'use first refittable' */
					ei->cargo_type = INVALID_CARGO;
				} else {
					/* Use translated cargo. Might result in INVALID_CARGO (first refittable), if cargo is not defined. */
					ei->cargo_type = GetCargoTranslation(ctype, _cur.grffile);
					if (ei->cargo_type == INVALID_CARGO) GrfMsg(2, "ShipVehicleChangeInfo: Invalid cargo type {}, using first refittable", ctype);
				}
				ei->cargo_label = CT_INVALID;
				break;
			}

			case PROP_SHIP_CARGO_CAPACITY: // 0x0D Cargo capacity
				svi->capacity = buf.ReadWord();
				break;

			case PROP_SHIP_RUNNING_COST_FACTOR: // 0x0F Running cost factor
				svi->running_cost = buf.ReadByte();
				break;

			case 0x10: // SFX
				svi->sfx = GetNewGRFSoundID(_cur.grffile, buf.ReadByte());
				break;

			case 0x11: { // Cargoes available for refitting
				uint32_t mask = buf.ReadDWord();
				_gted[e->index].UpdateRefittability(mask != 0);
				ei->refit_mask = TranslateRefitMask(mask);
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				break;
			}

			case 0x12: { // Callback mask
				auto mask = ei->callback_mask.base();
				SB(mask, 0, 8, buf.ReadByte());
				ei->callback_mask = VehicleCallbackMasks{mask};
				break;
			}

			case 0x13: // Refit cost
				ei->refit_cost = buf.ReadByte();
				break;

			case 0x14: // Ocean speed fraction
				svi->ocean_speed_frac = buf.ReadByte();
				break;

			case 0x15: // Canal speed fraction
				svi->canal_speed_frac = buf.ReadByte();
				break;

			case 0x16: // Retire vehicle early
				ei->retire_early = buf.ReadByte();
				break;

			case 0x17: // Miscellaneous flags
				ei->misc_flags = static_cast<EngineMiscFlags>(buf.ReadByte());
				_loaded_newgrf_features.has_2CC |= ei->misc_flags.Test(EngineMiscFlag::Uses2CC);
				break;

			case 0x18: // Cargo classes allowed
				_gted[e->index].cargo_allowed = buf.ReadWord();
				_gted[e->index].UpdateRefittability(_gted[e->index].cargo_allowed != 0);
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				break;

			case 0x19: // Cargo classes disallowed
				_gted[e->index].cargo_disallowed = buf.ReadWord();
				_gted[e->index].UpdateRefittability(false);
				break;

			case 0x1A: // Long format introduction date (days since year 0)
				ei->base_intro = CalTime::Date(static_cast<int32_t>(buf.ReadDWord()));
				break;

			case 0x1B: // Alter purchase list sort order
				AlterVehicleListOrder(e->index, buf.ReadExtendedByte());
				break;

			case 0x1C: // Visual effect
				svi->visual_effect = buf.ReadByte();
				/* Avoid accidentally setting visual_effect to the default value
				 * Since bit 6 (disable effects) is set anyways, we can safely erase some bits. */
				if (svi->visual_effect == VE_DEFAULT) {
					assert(HasBit(svi->visual_effect, VE_DISABLE_EFFECT));
					SB(svi->visual_effect, VE_TYPE_START, VE_TYPE_COUNT, 0);
				}
				break;

			case PROP_SHIP_CARGO_AGE_PERIOD: // 0x1D Cargo aging period
				ei->cargo_age_period = buf.ReadWord();
				break;

			case 0x1E:   // CTT refit include list
			case 0x1F: { // CTT refit exclude list
				uint8_t count = buf.ReadByte();
				_gted[e->index].UpdateRefittability(prop == 0x1E && count != 0);
				if (prop == 0x1E) _gted[e->index].defaultcargo_grf = _cur.grffile;
				CargoTypes &ctt = prop == 0x1E ? _gted[e->index].ctt_include_mask : _gted[e->index].ctt_exclude_mask;
				ctt = 0;
				while (count--) {
					CargoType ctype = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
					if (IsValidCargoType(ctype)) SetBit(ctt, ctype);
				}
				break;
			}

			case 0x20: // Engine variant
				ei->variant_id = buf.ReadWord();
				break;

			case 0x21: // Extra miscellaneous flags
				ei->extra_flags = static_cast<ExtraEngineFlags>(buf.ReadDWord());
				break;

			case 0x22: { // Callback additional mask
				auto mask = ei->callback_mask.base();
				SB(mask, 8, 8, buf.ReadByte());
				ei->callback_mask = VehicleCallbackMasks{mask};
				break;
			}

			case 0x23: // Speed (1 unit is 0.5 km-ish/h)
				svi->max_speed = buf.ReadWord();
				break;

			case 0x24: // Acceleration (1 unit is 0.5 km-ish/h per tick)
				svi->acceleration = std::max<uint8_t>(1, buf.ReadByte());
				break;

			case 0x25: // Cargo classes required for a refit.
				_gted[e->index].cargo_allowed_required = buf.ReadWord();
				break;

			case 0x26: // Badge list
				e->badges = ReadBadgeList(buf, GSF_SHIPS);
				break;

			default:
				ret = CommonVehicleChangeInfo(ei, prop, mapping_entry, buf);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for aircraft
 * @param first Local ID of the first vehicle.
 * @param last Local ID of the last vehicle.
 * @param prop The property to change.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult AircraftVehicleChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	for (uint id = first; id < last; ++id) {
		Engine *e = GetNewEngine(_cur.grffile, VEH_AIRCRAFT, id);
		if (e == nullptr) return CIR_INVALID_ID; // No engine could be allocated, so neither can any next vehicles

		EngineInfo *ei = &e->info;
		AircraftVehicleInfo *avi = &e->u.air;

		switch (prop) {
			case 0x08: { // Sprite ID
				uint8_t spriteid = buf.ReadByte();
				uint8_t orig_spriteid = spriteid;

				/* aircraft have different custom id in the GRF file */
				if (spriteid == 0xFF) spriteid = 0xFD;

				if (spriteid < 0xFD) spriteid >>= 1;

				if (IsValidNewGRFImageIndex<VEH_AIRCRAFT>(spriteid)) {
					avi->image_index = spriteid;
				} else {
					GrfMsg(1, "AircraftVehicleChangeInfo: Invalid Sprite {} specified, ignoring", orig_spriteid);
					avi->image_index = 0;
				}
				break;
			}

			case 0x09: // Helicopter
				if (buf.ReadByte() == 0) {
					avi->subtype = AIR_HELI;
				} else {
					SB(avi->subtype, 0, 1, 1); // AIR_CTOL
				}
				break;

			case 0x0A: // Large
				AssignBit(avi->subtype, 1, buf.ReadByte() != 0); // AIR_FAST
				break;

			case PROP_AIRCRAFT_COST_FACTOR: // 0x0B Cost factor
				avi->cost_factor = buf.ReadByte();
				break;

			case PROP_AIRCRAFT_SPEED: // 0x0C Speed (1 unit is 8 mph, we translate to 1 unit is 1 km-ish/h)
				avi->max_speed = (buf.ReadByte() * 128) / 10;
				break;

			case 0x0D: // Acceleration
				avi->acceleration = buf.ReadByte();
				break;

			case PROP_AIRCRAFT_RUNNING_COST_FACTOR: // 0x0E Running cost factor
				avi->running_cost = buf.ReadByte();
				break;

			case PROP_AIRCRAFT_PASSENGER_CAPACITY: // 0x0F Passenger capacity
				avi->passenger_capacity = buf.ReadWord();
				break;

			case PROP_AIRCRAFT_MAIL_CAPACITY: // 0x11 Mail capacity
				avi->mail_capacity = buf.ReadByte();
				break;

			case 0x12: // SFX
				avi->sfx = GetNewGRFSoundID(_cur.grffile, buf.ReadByte());
				break;

			case 0x13: { // Cargoes available for refitting
				uint32_t mask = buf.ReadDWord();
				_gted[e->index].UpdateRefittability(mask != 0);
				ei->refit_mask = TranslateRefitMask(mask);
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				break;
			}

			case 0x14: { // Callback mask
				auto mask = ei->callback_mask.base();
				SB(mask, 0, 8, buf.ReadByte());
				ei->callback_mask = VehicleCallbackMasks{mask};
				break;
			}

			case 0x15: // Refit cost
				ei->refit_cost = buf.ReadByte();
				break;

			case 0x16: // Retire vehicle early
				ei->retire_early = buf.ReadByte();
				break;

			case 0x17: // Miscellaneous flags
				ei->misc_flags = static_cast<EngineMiscFlags>(buf.ReadByte());
				_loaded_newgrf_features.has_2CC |= ei->misc_flags.Test(EngineMiscFlag::Uses2CC);
				break;

			case 0x18: // Cargo classes allowed
				_gted[e->index].cargo_allowed = buf.ReadWord();
				_gted[e->index].UpdateRefittability(_gted[e->index].cargo_allowed != 0);
				_gted[e->index].defaultcargo_grf = _cur.grffile;
				break;

			case 0x19: // Cargo classes disallowed
				_gted[e->index].cargo_disallowed = buf.ReadWord();
				_gted[e->index].UpdateRefittability(false);
				break;

			case 0x1A: // Long format introduction date (days since year 0)
				ei->base_intro = CalTime::Date(static_cast<int32_t>(buf.ReadDWord()));
				break;

			case 0x1B: // Alter purchase list sort order
				AlterVehicleListOrder(e->index, buf.ReadExtendedByte());
				break;

			case PROP_AIRCRAFT_CARGO_AGE_PERIOD: // 0x1C Cargo aging period
				ei->cargo_age_period = buf.ReadWord();
				break;

			case 0x1D:   // CTT refit include list
			case 0x1E: { // CTT refit exclude list
				uint8_t count = buf.ReadByte();
				_gted[e->index].UpdateRefittability(prop == 0x1D && count != 0);
				if (prop == 0x1D) _gted[e->index].defaultcargo_grf = _cur.grffile;
				CargoTypes &ctt = prop == 0x1D ? _gted[e->index].ctt_include_mask : _gted[e->index].ctt_exclude_mask;
				ctt = 0;
				while (count--) {
					CargoType ctype = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
					if (IsValidCargoType(ctype)) SetBit(ctt, ctype);
				}
				break;
			}

			case PROP_AIRCRAFT_RANGE: // 0x1F Max aircraft range
				avi->max_range = buf.ReadWord();
				break;

			case 0x20: // Engine variant
				ei->variant_id = buf.ReadWord();
				break;

			case 0x21: // Extra miscellaneous flags
				ei->extra_flags = static_cast<ExtraEngineFlags>(buf.ReadDWord());
				break;

			case 0x22: { // Callback additional mask
				auto mask = ei->callback_mask.base();
				SB(mask, 8, 8, buf.ReadByte());
				ei->callback_mask = VehicleCallbackMasks{mask};
				break;
			}

			case 0x23: // Cargo classes required for a refit.
				_gted[e->index].cargo_allowed_required = buf.ReadWord();
				break;

			case 0x24: // Badge list
				e->badges = ReadBadgeList(buf, GSF_AIRCRAFT);
				break;

			default:
				ret = CommonVehicleChangeInfo(ei, prop, mapping_entry, buf);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for stations
 * @param first Local ID of the first station.
 * @param last Local ID of the last station.
 * @param prop The property to change.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult StationChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > NUM_STATIONS_PER_GRF) {
		GrfMsg(1, "StationChangeInfo: Station {} is invalid, max {}, ignoring", last, NUM_STATIONS_PER_GRF);
		return CIR_INVALID_ID;
	}

	/* Allocate station specs if necessary */
	if (_cur.grffile->stations.size() < last) _cur.grffile->stations.resize(last);

	for (uint id = first; id < last; ++id) {
		StationSpec *statspec = _cur.grffile->stations[id].get();

		/* Check that the station we are modifying is defined. */
		if (statspec == nullptr && prop != 0x08) {
			GrfMsg(2, "StationChangeInfo: Attempt to modify undefined station {}, ignoring", id);
			return CIR_INVALID_ID;
		}

		switch (prop) {
			case 0x08: { // Class ID
				/* Property 0x08 is special; it is where the station is allocated */
				if (statspec == nullptr) {
					_cur.grffile->stations[id] = std::make_unique<StationSpec>();
					statspec = _cur.grffile->stations[id].get();
				}

				/* Swap classid because we read it in BE meaning WAYP or DFLT */
				uint32_t classid = buf.ReadDWord();
				statspec->class_index = StationClass::Allocate(std::byteswap(classid));
				break;
			}

			case 0x09: { // Define sprite layout
				uint16_t tiles = buf.ReadExtendedByte();
				statspec->renderdata.clear(); // delete earlier loaded stuff
				statspec->renderdata.reserve(tiles);

				for (uint t = 0; t < tiles; t++) {
					NewGRFSpriteLayout *dts = &statspec->renderdata.emplace_back();
					dts->consistent_max_offset = UINT16_MAX; // Spritesets are unknown, so no limit.

					if (buf.HasData(4) && buf.PeekDWord() == 0) {
						buf.Skip(4);
						extern const DrawTileSprites _station_display_datas_rail[8];
						dts->Clone(&_station_display_datas_rail[t % 8]);
						continue;
					}

					ReadSpriteLayoutSprite(buf, false, false, false, GSF_STATIONS, &dts->ground);
					/* On error, bail out immediately. Temporary GRF data was already freed */
					if (_cur.skip_sprites < 0) return CIR_DISABLED;

					static std::vector<DrawTileSeqStruct> tmp_layout;
					tmp_layout.clear();
					for (;;) {
						/* no relative bounding box support */
						DrawTileSeqStruct &dtss = tmp_layout.emplace_back();
						MemSetT(&dtss, 0);

						dtss.delta_x = buf.ReadByte();
						if (dtss.IsTerminator()) break;
						dtss.delta_y = buf.ReadByte();
						dtss.delta_z = buf.ReadByte();
						dtss.size_x = buf.ReadByte();
						dtss.size_y = buf.ReadByte();
						dtss.size_z = buf.ReadByte();

						ReadSpriteLayoutSprite(buf, false, true, false, GSF_STATIONS, &dtss.image);
						/* On error, bail out immediately. Temporary GRF data was already freed */
						if (_cur.skip_sprites < 0) return CIR_DISABLED;
					}
					dts->Clone(tmp_layout.data());
				}

				/* Number of layouts must be even, alternating X and Y */
				if (statspec->renderdata.size() & 1) {
					GrfMsg(1, "StationChangeInfo: Station {} defines an odd number of sprite layouts, dropping the last item", id);
					statspec->renderdata.pop_back();
				}
				break;
			}

			case 0x0A: { // Copy sprite layout
				uint16_t srcid = buf.ReadExtendedByte();
				const StationSpec *srcstatspec = srcid >= _cur.grffile->stations.size() ? nullptr : _cur.grffile->stations[srcid].get();

				if (srcstatspec == nullptr) {
					GrfMsg(1, "StationChangeInfo: Station {} is not defined, cannot copy sprite layout to {}.", srcid, id);
					continue;
				}

				statspec->renderdata.clear(); // delete earlier loaded stuff
				statspec->renderdata.reserve(srcstatspec->renderdata.size());

				for (const auto &it : srcstatspec->renderdata) {
					NewGRFSpriteLayout *dts = &statspec->renderdata.emplace_back();
					dts->Clone(&it);
				}
				break;
			}

			case 0x0B: // Callback mask
				statspec->callback_mask = static_cast<StationCallbackMasks>(buf.ReadByte());
				break;

			case 0x0C: // Disallowed number of platforms
				statspec->disallowed_platforms = buf.ReadByte();
				break;

			case 0x0D: // Disallowed platform lengths
				statspec->disallowed_lengths = buf.ReadByte();
				break;

			case 0x0E: // Define custom layout
				while (buf.HasData()) {
					uint8_t length = buf.ReadByte();
					uint8_t number = buf.ReadByte();

					if (length == 0 || number == 0) break;

					const uint8_t *buf_layout = buf.ReadBytes(length * number);

					/* Create entry in layouts and assign the layout to it. */
					auto &layout = statspec->layouts[GetStationLayoutKey(number, length)];
					layout.assign(buf_layout, buf_layout + length * number);

					/* Ensure the first bit, axis, is zero. The rest of the value is validated during rendering, as we don't know the range yet. */
					for (auto &tile : layout) {
						if ((tile & ~1U) != tile) {
							GrfMsg(1, "StationChangeInfo: Invalid tile {} in layout {}x{}", tile, length, number);
							tile &= ~1U;
						}
					}
				}
				break;

			case 0x0F: { // Copy custom layout
				uint16_t srcid = buf.ReadExtendedByte();
				const StationSpec *srcstatspec = srcid >= _cur.grffile->stations.size() ? nullptr : _cur.grffile->stations[srcid].get();

				if (srcstatspec == nullptr) {
					GrfMsg(1, "StationChangeInfo: Station {} is not defined, cannot copy tile layout to {}.", srcid, id);
					continue;
				}

				statspec->layouts = srcstatspec->layouts;
				break;
			}

			case 0x10: // Little/lots cargo threshold
				statspec->cargo_threshold = buf.ReadWord();
				break;

			case 0x11: { // Pylon placement
				uint8_t pylons = buf.ReadByte();
				if (statspec->tileflags.size() < 8) statspec->tileflags.resize(8);
				for (int j = 0; j < 8; ++j) {
					if (HasBit(pylons, j)) {
						statspec->tileflags[j].Set(StationSpec::TileFlag::Pylons);
					} else {
						statspec->tileflags[j].Reset(StationSpec::TileFlag::Pylons);
					}
				}
				break;
			}

			case 0x12: // Cargo types for random triggers
				if (_cur.grffile->grf_version >= 7) {
					statspec->cargo_triggers = TranslateRefitMask(buf.ReadDWord());
				} else {
					statspec->cargo_triggers = (CargoTypes)buf.ReadDWord();
				}
				break;

			case 0x13: // General flags
				statspec->flags = StationSpecFlags{buf.ReadByte()};
				break;

			case 0x14: { // Overhead wire placement
				uint8_t wires = buf.ReadByte();
				if (statspec->tileflags.size() < 8) statspec->tileflags.resize(8);
				for (int j = 0; j < 8; ++j) {
					if (HasBit(wires, j)) {
						statspec->tileflags[j].Set(StationSpec::TileFlag::NoWires);
					} else {
						statspec->tileflags[j].Reset(StationSpec::TileFlag::NoWires);
					}
				}
				break;
			}

			case 0x15: { // Blocked tiles
				uint8_t blocked = buf.ReadByte();
				if (statspec->tileflags.size() < 8) statspec->tileflags.resize(8);
				for (int j = 0; j < 8; ++j) {
					if (HasBit(blocked, j)) {
						statspec->tileflags[j].Set(StationSpec::TileFlag::Blocked);
					} else {
						statspec->tileflags[j].Reset(StationSpec::TileFlag::Blocked);
					}
				}
				break;
			}

			case 0x16: // Animation info
				statspec->animation.frames = buf.ReadByte();
				statspec->animation.status = buf.ReadByte();
				break;

			case 0x17: // Animation speed
				statspec->animation.speed = buf.ReadByte();
				break;

			case 0x18: // Animation triggers
				statspec->animation.triggers = buf.ReadWord();
				break;

			/* 0x19 road routing (not implemented) */

			case 0x1A: { // Advanced sprite layout
				uint16_t tiles = buf.ReadExtendedByte();
				statspec->renderdata.clear(); // delete earlier loaded stuff
				statspec->renderdata.reserve(tiles);

				for (uint t = 0; t < tiles; t++) {
					NewGRFSpriteLayout *dts = &statspec->renderdata.emplace_back();
					uint num_building_sprites = buf.ReadByte();
					/* On error, bail out immediately. Temporary GRF data was already freed */
					if (ReadSpriteLayout(buf, num_building_sprites, false, GSF_STATIONS, true, false, dts)) return CIR_DISABLED;
				}

				/* Number of layouts must be even, alternating X and Y */
				if (statspec->renderdata.size() & 1) {
					GrfMsg(1, "StationChangeInfo: Station {} defines an odd number of sprite layouts, dropping the last item", id);
					statspec->renderdata.pop_back();
				}
				break;
			}

			case A0RPI_STATION_MIN_BRIDGE_HEIGHT: {
				statspec->internal_flags.Set(StationSpecIntlFlag::BridgeHeightsSet);
				size_t length = buf.ReadExtendedByte();
				if (statspec->bridge_above_flags.size() < length) statspec->bridge_above_flags.resize(length);
				for (size_t i = 0; i < length; i++) {
					statspec->bridge_above_flags[i].height = buf.ReadByte();
				}
				break;
			}

			case 0x1B: // Minimum height for a bridge above
				statspec->internal_flags.Set(StationSpecIntlFlag::BridgeHeightsSet);
				if (statspec->bridge_above_flags.size() < 8) statspec->bridge_above_flags.resize(8);
				for (uint i = 0; i < 8; i++) {
					statspec->bridge_above_flags[i].height = buf.ReadByte();
				}
				break;

			case A0RPI_STATION_DISALLOWED_BRIDGE_PILLARS: {
				statspec->internal_flags.Set(StationSpecIntlFlag::BridgeDisallowedPillarsSet);
				size_t length = buf.ReadExtendedByte();
				if (statspec->bridge_above_flags.size() < length) statspec->bridge_above_flags.resize(length);
				for (size_t i = 0; i < length; i++) {
					statspec->bridge_above_flags[i].disallowed_pillars = buf.ReadByte();
				}
				break;
			}

			case 0x1C: // Station Name
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &statspec->name);
				break;

			case 0x1D: // Station Class name
				AddStringForMapping(GRFStringID{buf.ReadWord()}, statspec, [](StringID str, StationSpec *statspec) { StationClass::Get(statspec->class_index)->name = str; });
				break;

			case 0x1E: { // Extended tile flags (replaces prop 11, 14 and 15)
				uint16_t tiles = buf.ReadExtendedByte();
				auto flags = reinterpret_cast<const StationSpec::TileFlags *>(buf.ReadBytes(tiles));
				statspec->tileflags.assign(flags, flags + tiles);
				break;
			}

			case 0x1F: // Badge list
				statspec->badges = ReadBadgeList(buf, GSF_STATIONS);
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for water features
 * @param first Local ID of the first water feature.
 * @param last Local ID of the last water feature.
 * @param prop The property to change.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult CanalChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > CF_END) {
		GrfMsg(1, "CanalChangeInfo: Canal feature 0x{:02X} is invalid, max {}, ignoring", last, CF_END);
		return CIR_INVALID_ID;
	}

	for (uint id = first; id < last; ++id) {
		CanalProperties *cp = &_cur.grffile->canal_local_properties[id];

		switch (prop) {
			case 0x08:
				cp->callback_mask = static_cast<CanalCallbackMasks>(buf.ReadByte());
				break;

			case 0x09:
				cp->flags = buf.ReadByte();
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for bridges
 * @param first Local ID of the first bridge.
 * @param last Local ID of the last bridge.
 * @param prop The property to change.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult BridgeChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > MAX_BRIDGES) {
		GrfMsg(1, "BridgeChangeInfo: Bridge {} is invalid, max {}, ignoring", last, MAX_BRIDGES);
		return CIR_INVALID_ID;
	}

	for (uint id = first; id < last; ++id) {
		BridgeSpec *bridge = &_bridge[id];

		switch (prop) {
			case 0x08: { // Year of availability
				/* We treat '0' as always available */
				uint8_t year = buf.ReadByte();
				bridge->avail_year = (year > 0 ? CalTime::ORIGINAL_BASE_YEAR + year : CalTime::Year{0});
				break;
			}

			case 0x09: // Minimum length
				bridge->min_length = buf.ReadByte();
				break;

			case 0x0A: // Maximum length
				bridge->max_length = buf.ReadByte();
				if (bridge->max_length > 16) bridge->max_length = UINT16_MAX;
				break;

			case 0x0B: // Cost factor
				bridge->price = buf.ReadByte();
				break;

			case 0x0C: // Maximum speed
				bridge->speed = buf.ReadWord();
				if (bridge->speed == 0) bridge->speed = UINT16_MAX;
				break;

			case 0x0D: { // Bridge sprite tables
				uint8_t tableid = buf.ReadByte();
				uint8_t numtables = buf.ReadByte();

				if (bridge->sprite_table == nullptr) {
					/* Allocate memory for sprite table pointers and zero out */
					bridge->sprite_table = CallocT<PalSpriteID*>(NUM_BRIDGE_PIECES);
				}

				for (; numtables-- != 0; tableid++) {
					if (tableid >= NUM_BRIDGE_PIECES) { // skip invalid data
						GrfMsg(1, "BridgeChangeInfo: Table {} >= {}, skipping", tableid, NUM_BRIDGE_PIECES);
						for (uint8_t sprite = 0; sprite < SPRITES_PER_BRIDGE_PIECE; sprite++) buf.ReadDWord();
						continue;
					}

					if (bridge->sprite_table[tableid] == nullptr) {
						bridge->sprite_table[tableid] = MallocT<PalSpriteID>(SPRITES_PER_BRIDGE_PIECE);
					}

					for (uint8_t sprite = 0; sprite < SPRITES_PER_BRIDGE_PIECE; sprite++) {
						SpriteID image = buf.ReadWord();
						PaletteID pal  = buf.ReadWord();

						bridge->sprite_table[tableid][sprite].sprite = image;
						bridge->sprite_table[tableid][sprite].pal    = pal;

						MapSpriteMappingRecolour(&bridge->sprite_table[tableid][sprite]);
					}
				}
				if (!HasBit(bridge->ctrl_flags, BSCF_CUSTOM_PILLAR_FLAGS)) SetBit(bridge->ctrl_flags, BSCF_INVALID_PILLAR_FLAGS);
				break;
			}

			case 0x0E: // Flags; bit 0 - disable far pillars
				bridge->flags = buf.ReadByte();
				break;

			case 0x0F: // Long format year of availability (year since year 0)
				bridge->avail_year = CalTime::DeserialiseYearClamped(static_cast<int32_t>(buf.ReadDWord()));
				break;

			case 0x10: // purchase string
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &bridge->material);
				break;

			case 0x11: // description of bridge with rails
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &bridge->transport_name[0]);
				break;

			case 0x12: // description of bridge with roads
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &bridge->transport_name[1]);
				break;

			case 0x13: // 16 bits cost multiplier
				bridge->price = buf.ReadWord();
				break;

			case A0RPI_BRIDGE_MENU_ICON:
				if (MappedPropertyLengthMismatch(buf, 4, mapping_entry)) break;
				[[fallthrough]];
			case 0x14: // purchase sprite
				bridge->sprite = buf.ReadWord();
				bridge->pal    = buf.ReadWord();
				break;

			case A0RPI_BRIDGE_PILLAR_FLAGS:
				if (MappedPropertyLengthMismatch(buf, 12, mapping_entry)) break;
				for (uint i = 0; i < 12; i++) {
					bridge->pillar_flags[i] = buf.ReadByte();
				}
				ClrBit(bridge->ctrl_flags, BSCF_INVALID_PILLAR_FLAGS);
				SetBit(bridge->ctrl_flags, BSCF_CUSTOM_PILLAR_FLAGS);
				break;

			case A0RPI_BRIDGE_AVAILABILITY_FLAGS: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t flags = buf.ReadByte();
				AssignBit(bridge->ctrl_flags, BSCF_NOT_AVAILABLE_TOWN, HasBit(flags, 0));
				AssignBit(bridge->ctrl_flags, BSCF_NOT_AVAILABLE_AI_GS, HasBit(flags, 1));
				break;
			}

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Ignore a house property
 * @param prop Property to read.
 * @param buf Property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult IgnoreTownHouseProperty(int prop, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	switch (prop) {
		case 0x09:
		case 0x0B:
		case 0x0C:
		case 0x0D:
		case 0x0E:
		case 0x0F:
		case 0x11:
		case 0x14:
		case 0x15:
		case 0x16:
		case 0x18:
		case 0x19:
		case 0x1A:
		case 0x1B:
		case 0x1C:
		case 0x1D:
		case 0x1F:
			buf.ReadByte();
			break;

		case 0x0A:
		case 0x10:
		case 0x12:
		case 0x13:
		case 0x21:
		case 0x22:
			buf.ReadWord();
			break;

		case 0x1E:
			buf.ReadDWord();
			break;

		case 0x17:
			for (uint j = 0; j < 4; j++) buf.ReadByte();
			break;

		case 0x20: {
			uint8_t count = buf.ReadByte();
			for (uint8_t j = 0; j < count; j++) buf.ReadByte();
			break;
		}

		case 0x23:
			buf.Skip(buf.ReadByte() * 2);
			break;

		default:
			ret = HandleAction0PropertyDefault(buf, prop);
			break;
	}
	return ret;
}

/**
 * Define properties for houses
 * @param first Local ID of the first house.
 * @param last Local ID of the last house.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult TownHouseChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > NUM_HOUSES_PER_GRF) {
		GrfMsg(1, "TownHouseChangeInfo: Too many houses loaded ({}), max ({}). Ignoring.", last, NUM_HOUSES_PER_GRF);
		return CIR_INVALID_ID;
	}

	/* Allocate house specs if they haven't been allocated already. */
	if (_cur.grffile->housespec.size() < last) _cur.grffile->housespec.resize(last);

	for (uint id = first; id < last; ++id) {
		HouseSpec *housespec = _cur.grffile->housespec[id].get();

		if (prop != 0x08 && housespec == nullptr) {
			/* If the house property 08 is not yet set, ignore this property */
			ChangeInfoResult cir = IgnoreTownHouseProperty(prop, buf);
			if (cir > ret) ret = cir;
			continue;
		}

		switch (prop) {
			case 0x08: { // Substitute building type, and definition of a new house
				uint8_t subs_id = buf.ReadByte();
				if (subs_id == 0xFF) {
					/* Instead of defining a new house, a substitute house id
					 * of 0xFF disables the old house with the current id. */
					if (id < NEW_HOUSE_OFFSET) HouseSpec::Get(id)->enabled = false;
					continue;
				} else if (subs_id >= NEW_HOUSE_OFFSET) {
					/* The substitute id must be one of the original houses. */
					GrfMsg(2, "TownHouseChangeInfo: Attempt to use new house {} as substitute house for {}. Ignoring.", subs_id, id);
					continue;
				}

				/* Allocate space for this house. */
				if (housespec == nullptr) {
					/* Only the first property 08 setting copies properties; if you later change it, properties will stay. */
					_cur.grffile->housespec[id] = std::make_unique<HouseSpec>(*HouseSpec::Get(subs_id));
					housespec = _cur.grffile->housespec[id].get();

					housespec->enabled = true;
					housespec->grf_prop.local_id = id;
					housespec->grf_prop.subst_id = subs_id;
					housespec->grf_prop.grfid = _cur.grffile->grfid;
					housespec->grf_prop.grffile = _cur.grffile;
					/* Set default colours for randomization, used if not overridden. */
					housespec->random_colour[0] = COLOUR_RED;
					housespec->random_colour[1] = COLOUR_BLUE;
					housespec->random_colour[2] = COLOUR_ORANGE;
					housespec->random_colour[3] = COLOUR_GREEN;

					/* House flags 40 and 80 are exceptions; these flags are never set automatically. */
					housespec->building_flags.Reset(BuildingFlag::IsChurch).Reset(BuildingFlag::IsStadium);

					/* Make sure that the third cargo type is valid in this
					 * climate. This can cause problems when copying the properties
					 * of a house that accepts food, where the new house is valid
					 * in the temperate climate. */
					CargoType cargo_type = housespec->accepts_cargo[2];
					if (!IsValidCargoType(cargo_type)) cargo_type = GetCargoTypeByLabel(housespec->accepts_cargo_label[2]);
					if (!IsValidCargoType(cargo_type)) {
						housespec->cargo_acceptance[2] = 0;
					}
				}
				break;
			}

			case 0x09: // Building flags
				housespec->building_flags = (BuildingFlags)buf.ReadByte();
				break;

			case 0x0A: { // Availability years
				uint16_t years = buf.ReadWord();
				housespec->min_year = GB(years, 0, 8) > 150 ? CalTime::MAX_YEAR : CalTime::ORIGINAL_BASE_YEAR + GB(years, 0, 8);
				housespec->max_year = GB(years, 8, 8) > 150 ? CalTime::MAX_YEAR : CalTime::ORIGINAL_BASE_YEAR + GB(years, 8, 8);
				break;
			}

			case 0x0B: // Population
				housespec->population = buf.ReadByte();
				break;

			case 0x0C: // Mail generation multiplier
				housespec->mail_generation = buf.ReadByte();
				break;

			case 0x0D: // Passenger acceptance
			case 0x0E: // Mail acceptance
				housespec->cargo_acceptance[prop - 0x0D] = buf.ReadByte();
				break;

			case 0x0F: { // Goods/candy, food/fizzy drinks acceptance
				int8_t goods = buf.ReadByte();

				/* If value of goods is negative, it means in fact food or, if in toyland, fizzy_drink acceptance.
				 * Else, we have "standard" 3rd cargo type, goods or candy, for toyland once more */
				CargoType cargo_type = (goods >= 0) ? ((_settings_game.game_creation.landscape == LandscapeType::Toyland) ? GetCargoTypeByLabel(CT_CANDY) : GetCargoTypeByLabel(CT_GOODS)) :
						((_settings_game.game_creation.landscape == LandscapeType::Toyland) ? GetCargoTypeByLabel(CT_FIZZY_DRINKS) : GetCargoTypeByLabel(CT_FOOD));

				/* Make sure the cargo type is valid in this climate. */
				if (!IsValidCargoType(cargo_type)) goods = 0;

				housespec->accepts_cargo[2] = cargo_type;
				housespec->accepts_cargo_label[2] = CT_INVALID;
				housespec->cargo_acceptance[2] = abs(goods); // but we do need positive value here
				break;
			}

			case 0x10: // Local authority rating decrease on removal
				housespec->remove_rating_decrease = buf.ReadWord();
				break;

			case 0x11: // Removal cost multiplier
				housespec->removal_cost = buf.ReadByte();
				break;

			case 0x12: // Building name ID
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &housespec->building_name);
				break;

			case 0x13: // Building availability mask
				housespec->building_availability = (HouseZones)buf.ReadWord();
				break;

			case 0x14: { // House callback mask
				auto mask = housespec->callback_mask.base();
				SB(mask, 0, 8, buf.ReadByte());
				housespec->callback_mask = HouseCallbackMasks{mask};
				break;
			}

			case 0x15: { // House override byte
				uint8_t override = buf.ReadByte();

				/* The house being overridden must be an original house. */
				if (override >= NEW_HOUSE_OFFSET) {
					GrfMsg(2, "TownHouseChangeInfo: Attempt to override new house {} with house id {}. Ignoring.", override, id);
					continue;
				}

				_house_mngr.Add(id, _cur.grffile->grfid, override);
				break;
			}

			case 0x16: // Periodic refresh multiplier
				housespec->processing_time = std::min<uint8_t>(buf.ReadByte(), 63u);
				break;

			case 0x17: // Four random colours to use
				for (uint j = 0; j < 4; j++) housespec->random_colour[j] = static_cast<Colours>(GB(buf.ReadByte(), 0, 4));
				break;

			case 0x18: // Relative probability of appearing
				housespec->probability = buf.ReadByte();
				break;

			case 0x19: // Extra flags
				housespec->extra_flags = static_cast<HouseExtraFlags>(buf.ReadByte());
				break;

			case 0x1A: // Animation frames
				housespec->animation.frames = buf.ReadByte();
				housespec->animation.status = GB(housespec->animation.frames, 7, 1);
				SB(housespec->animation.frames, 7, 1, 0);
				break;

			case 0x1B: // Animation speed
				housespec->animation.speed = Clamp(buf.ReadByte(), 2, 16);
				break;

			case 0x1C: // Class of the building type
				housespec->class_id = AllocateHouseClassID(buf.ReadByte(), _cur.grffile->grfid);
				break;

			case 0x1D: { // Callback mask part 2
				auto mask = housespec->callback_mask.base();
				SB(mask, 8, 8, buf.ReadByte());
				housespec->callback_mask = HouseCallbackMasks{mask};
				break;
			}

			case 0x1E: { // Accepted cargo types
				uint32_t cargotypes = buf.ReadDWord();

				/* Check if the cargo types should not be changed */
				if (cargotypes == 0xFFFFFFFF) break;

				for (uint j = 0; j < HOUSE_ORIGINAL_NUM_ACCEPTS; j++) {
					/* Get the cargo number from the 'list' */
					uint8_t cargo_part = GB(cargotypes, 8 * j, 8);
					CargoType cargo = GetCargoTranslation(cargo_part, _cur.grffile);

					if (!IsValidCargoType(cargo)) {
						/* Disable acceptance of invalid cargo type */
						housespec->cargo_acceptance[j] = 0;
					} else {
						housespec->accepts_cargo[j] = cargo;
					}
					housespec->accepts_cargo_label[j] = CT_INVALID;
				}
				break;
			}

			case 0x1F: // Minimum life span
				housespec->minimum_life = buf.ReadByte();
				break;

			case 0x20: { // Cargo acceptance watch list
				uint8_t count = buf.ReadByte();
				for (uint8_t j = 0; j < count; j++) {
					CargoType cargo = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
					if (IsValidCargoType(cargo)) SetBit(housespec->watched_cargoes, cargo);
				}
				break;
			}

			case 0x21: // long introduction year
				housespec->min_year = CalTime::Year{buf.ReadWord()};
				break;

			case 0x22: // long maximum year
				housespec->max_year = housespec->max_year = CalTime::Year{buf.ReadWord()};
				if (housespec->max_year == UINT16_MAX) housespec->max_year = CalTime::MAX_YEAR;
				break;

			case 0x23: { // variable length cargo types accepted
				uint count = buf.ReadByte();
				if (count > lengthof(housespec->accepts_cargo)) {
					GRFError *error = DisableGrf(STR_NEWGRF_ERROR_LIST_PROPERTY_TOO_LONG);
					error->param_value[1] = prop;
					return CIR_DISABLED;
				}
				/* Always write the full accepts_cargo array, and check each index for being inside the
				 * provided data. This ensures all values are properly initialized, and also avoids
				 * any risks of array overrun. */
				for (uint i = 0; i < lengthof(housespec->accepts_cargo); i++) {
					if (i < count) {
						housespec->accepts_cargo[i] = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
						housespec->cargo_acceptance[i] = buf.ReadByte();
					} else {
						housespec->accepts_cargo[i] = INVALID_CARGO;
						housespec->cargo_acceptance[i] = 0;
					}
					if (i < std::size(housespec->accepts_cargo_label)) housespec->accepts_cargo_label[i] = CT_INVALID;
				}
				break;
			}

			case 0x24: // Badge list
				housespec->badges = ReadBadgeList(buf, GSF_HOUSES);
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Get the language map associated with a given NewGRF and language.
 * @param grfid       The NewGRF to get the map for.
 * @param language_id The (NewGRF) language ID to get the map for.
 * @return The LanguageMap, or nullptr if it couldn't be found.
 */
/* static */ const LanguageMap *LanguageMap::GetLanguageMap(uint32_t grfid, uint8_t language_id)
{
	/* LanguageID "MAX_LANG", i.e. 7F is any. This language can't have a gender/case mapping, but has to be handled gracefully. */
	const GRFFile *grffile = GetFileByGRFID(grfid);
	if (grffile == nullptr) return nullptr;

	auto it = grffile->language_map.find(language_id);
	if (it == std::end(grffile->language_map)) return nullptr;

	return &it->second;
}

/**
 * Load a cargo- or railtype-translation table.
 * @param first ID of the first translation table entry.
 * @param last ID of the last translation table entry.
 * @param buf The property value.
 * @param gettable Function to get storage for the translation table.
 * @param name Name of the table for debug output.
 * @return ChangeInfoResult.
 */
template <typename T, typename TGetTableFunc>
static ChangeInfoResult LoadTranslationTable(uint first, uint last, ByteReader &buf, TGetTableFunc gettable, std::string_view name)
{
	if (first != 0) {
		GrfMsg(1, "LoadTranslationTable: {} translation table must start at zero", name);
		return CIR_INVALID_ID;
	}

	std::vector<T> &translation_table = gettable(*_cur.grffile);
	translation_table.clear();
	translation_table.reserve(last);
	for (uint id = first; id < last; ++id) {
		translation_table.push_back(T(std::byteswap(buf.ReadDWord())));
	}

	GRFFile *grf_override = GetCurrentGRFOverride();
	if (grf_override != nullptr) {
		/* GRF override is present, copy the translation table to the overridden GRF as well. */
		GrfMsg(1, "LoadTranslationTable: Copying {} translation table to override GRFID '{}'", name, std::byteswap(grf_override->grfid));
		std::vector<T> &override_table = gettable(*grf_override);
		override_table = translation_table;
	}

	return CIR_SUCCESS;
}

static ChangeInfoResult LoadBadgeTranslationTable(uint first, uint last, ByteReader &buf, std::vector<BadgeID> &translation_table, const char *name)
{
	if (first != 0 && first != std::size(translation_table)) {
		GrfMsg(1, "LoadBadgeTranslationTable: {} translation table must start at zero or {}", name, std::size(translation_table));
		return CIR_INVALID_ID;
	}

	if (first == 0) translation_table.clear();
	translation_table.reserve(last);
	for (uint id = first; id < last; ++id) {
		std::string_view label = buf.ReadString();
		translation_table.push_back(GetOrCreateBadge(label).index);
	}

	return CIR_SUCCESS;
}

/**
 * Helper to read a DWord worth of bytes from the reader
 * and to return it as a valid string.
 * @param reader The source of the DWord.
 * @return The read DWord as string.
 */
static std::string ReadDWordAsString(ByteReader &reader)
{
	std::string output;
	for (int i = 0; i < 4; i++) output.push_back(reader.ReadByte());
	return StrMakeValid(output);
}

/**
 * Define properties for global variables
 * @param first ID of the first global var.
 * @param last ID of the last global var.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult GlobalVarChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	/* Properties which are handled as a whole */
	switch (prop) {
		case 0x09: // Cargo Translation Table; loading during both reservation and activation stage (in case it is selected depending on defined cargos)
			return LoadTranslationTable<CargoLabel>(first, last, buf, [](GRFFile &grf) -> std::vector<CargoLabel> & { return grf.cargo_list; }, "Cargo");

		case 0x12: // Rail type translation table; loading during both reservation and activation stage (in case it is selected depending on defined railtypes)
			return LoadTranslationTable<RailTypeLabel>(first, last, buf, [](GRFFile &grf) -> std::vector<RailTypeLabel> & { return grf.railtype_list; }, "Rail type");

		case 0x16: // Road type translation table; loading during both reservation and activation stage (in case it is selected depending on defined roadtypes)
			return LoadTranslationTable<RoadTypeLabel>(first, last, buf, [](GRFFile &grf) -> std::vector<RoadTypeLabel> & { return grf.roadtype_list; }, "Road type");

		case 0x17: // Tram type translation table; loading during both reservation and activation stage (in case it is selected depending on defined tramtypes)
			return LoadTranslationTable<RoadTypeLabel>(first, last, buf, [](GRFFile &grf) -> std::vector<RoadTypeLabel> & { return grf.tramtype_list; }, "Tram type");

		case 0x18: // Badge translation table
			return LoadBadgeTranslationTable(first, last, buf, _cur.grffile->badge_list, "Badge");

		default:
			break;
	}

	/* Properties which are handled per item */
	ChangeInfoResult ret = CIR_SUCCESS;
	for (uint id = first; id < last; ++id) {
		switch (prop) {
			case 0x08: { // Cost base factor
				int factor = buf.ReadByte();

				if (id < PR_END) {
					_cur.grffile->price_base_multipliers[id] = std::min<int>(factor - 8, MAX_PRICE_MODIFIER);
				} else {
					GrfMsg(1, "GlobalVarChangeInfo: Price {} out of range, ignoring", id);
				}
				break;
			}

			case 0x0A: { // Currency display names
				uint curidx = GetNewgrfCurrencyIdConverted(id);
				if (curidx < CURRENCY_END) {
					AddStringForMapping(GRFStringID{buf.ReadWord()}, curidx, [](StringID str, uint curidx) {
						_currency_specs[curidx].name = str;
						_currency_specs[curidx].code.clear();
					});
				} else {
					buf.ReadWord();
				}
				break;
			}

			case 0x0B: { // Currency multipliers
				uint curidx = GetNewgrfCurrencyIdConverted(id);
				uint32_t rate = buf.ReadDWord();

				if (curidx < CURRENCY_END) {
					/* TTDPatch uses a multiple of 1000 for its conversion calculations,
					 * which OTTD does not. For this reason, divide grf value by 1000,
					 * to be compatible */
					_currency_specs[curidx].rate = rate / 1000;
				} else {
					GrfMsg(1, "GlobalVarChangeInfo: Currency multipliers {} out of range, ignoring", curidx);
				}
				break;
			}

			case 0x0C: { // Currency options
				uint curidx = GetNewgrfCurrencyIdConverted(id);
				uint16_t options = buf.ReadWord();

				if (curidx < CURRENCY_END) {
					_currency_specs[curidx].separator.clear();
					_currency_specs[curidx].separator.push_back(GB(options, 0, 8));
					/* By specifying only one bit, we prevent errors,
					 * since newgrf specs said that only 0 and 1 can be set for symbol_pos */
					_currency_specs[curidx].symbol_pos = GB(options, 8, 1);
				} else {
					GrfMsg(1, "GlobalVarChangeInfo: Currency option {} out of range, ignoring", curidx);
				}
				break;
			}

			case 0x0D: { // Currency prefix symbol
				uint curidx = GetNewgrfCurrencyIdConverted(id);
				std::string prefix = ReadDWordAsString(buf);

				if (curidx < CURRENCY_END) {
					_currency_specs[curidx].prefix = prefix;
				} else {
					GrfMsg(1, "GlobalVarChangeInfo: Currency symbol {} out of range, ignoring", curidx);
				}
				break;
			}

			case 0x0E: { // Currency suffix symbol
				uint curidx = GetNewgrfCurrencyIdConverted(id);
				std::string suffix = ReadDWordAsString(buf);

				if (curidx < CURRENCY_END) {
					_currency_specs[curidx].suffix = suffix;
				} else {
					GrfMsg(1, "GlobalVarChangeInfo: Currency symbol {} out of range, ignoring", curidx);
				}
				break;
			}

			case 0x0F: { //  Euro introduction dates
				uint curidx = GetNewgrfCurrencyIdConverted(id);
				CalTime::Year year_euro{buf.ReadWord()};

				if (curidx < CURRENCY_END) {
					_currency_specs[curidx].to_euro = year_euro;
				} else {
					GrfMsg(1, "GlobalVarChangeInfo: Euro intro date {} out of range, ignoring", curidx);
				}
				break;
			}

			case 0x10: // Snow line height table
				if (last > 1 || IsSnowLineSet()) {
					GrfMsg(1, "GlobalVarChangeInfo: The snowline can only be set once ({})", last);
				} else if (buf.Remaining() < SNOW_LINE_MONTHS * SNOW_LINE_DAYS) {
					GrfMsg(1, "GlobalVarChangeInfo: Not enough entries set in the snowline table ({})", buf.Remaining());
				} else {
					auto snow_line = std::make_unique<SnowLine>();

					for (uint i = 0; i < SNOW_LINE_MONTHS; i++) {
						for (uint j = 0; j < SNOW_LINE_DAYS; j++) {
							uint8_t &level = snow_line->table[i][j];
							level = buf.ReadByte();
							if (_cur.grffile->grf_version >= 8) {
								if (level != 0xFF) level = level * (1 + _settings_game.construction.map_height_limit) / 256;
							} else {
								if (level >= 128) {
									/* no snow */
									level = 0xFF;
								} else {
									level = level * (1 + _settings_game.construction.map_height_limit) / 128;
								}
							}

							snow_line->highest_value = std::max(snow_line->highest_value, level);
							snow_line->lowest_value = std::min(snow_line->lowest_value, level);
						}
					}
					SetSnowLine(std::move(snow_line));
				}
				break;

			case 0x11: // GRF match for engine allocation
				/* This is loaded during the reservation stage, so just skip it here. */
				/* Each entry is 8 bytes. */
				buf.Skip(8);
				break;

			case 0x13:   // Gender translation table
			case 0x14:   // Case translation table
			case 0x15: { // Plural form translation
				uint curidx = id; // The current index, i.e. language.
				const LanguageMetadata *lang = curidx < MAX_LANG ? GetLanguage(curidx) : nullptr;
				if (lang == nullptr) {
					GrfMsg(1, "GlobalVarChangeInfo: Language {} is not known, ignoring", curidx);
					/* Skip over the data. */
					if (prop == 0x15) {
						buf.ReadByte();
					} else {
						while (buf.ReadByte() != 0) {
							buf.ReadString();
						}
					}
					break;
				}

				if (prop == 0x15) {
					uint plural_form = buf.ReadByte();
					if (plural_form >= LANGUAGE_MAX_PLURAL) {
						GrfMsg(1, "GlobalVarChanceInfo: Plural form {} is out of range, ignoring", plural_form);
					} else {
						_cur.grffile->language_map[curidx].plural_form = plural_form;
					}
					break;
				}

				uint8_t newgrf_id = buf.ReadByte(); // The NewGRF (custom) identifier.
				while (newgrf_id != 0) {
					std::string_view name = buf.ReadString(); // The name for the OpenTTD identifier.

					/* We'll just ignore the UTF8 identifier character. This is (fairly)
					 * safe as OpenTTD's strings gender/cases are usually in ASCII which
					 * is just a subset of UTF8, or they need the bigger UTF8 characters
					 * such as Cyrillic. Thus we will simply assume they're all UTF8. */
					char32_t c;
					size_t len = Utf8Decode(&c, name.data());
					if (c == NFO_UTF8_IDENTIFIER) name = name.substr(len);

					LanguageMap::Mapping map;
					map.newgrf_id = newgrf_id;
					if (prop == 0x13) {
						map.openttd_id = lang->GetGenderIndex(name.data());
						if (map.openttd_id >= MAX_NUM_GENDERS) {
							GrfMsg(1, "GlobalVarChangeInfo: Gender name {} is not known, ignoring", StrMakeValid(name));
						} else {
							_cur.grffile->language_map[curidx].gender_map.push_back(map);
						}
					} else {
						map.openttd_id = lang->GetCaseIndex(name.data());
						if (map.openttd_id >= MAX_NUM_CASES) {
							GrfMsg(1, "GlobalVarChangeInfo: Case name {} is not known, ignoring", StrMakeValid(name));
						} else {
							_cur.grffile->language_map[curidx].case_map.push_back(map);
						}
					}
					newgrf_id = buf.ReadByte();
				}
				break;
			}

			case A0RPI_GLOBALVAR_EXTRA_STATION_NAMES: {
				if (MappedPropertyLengthMismatch(buf, 4, mapping_entry)) break;
				GRFStringID str = GRFStringID{buf.ReadWord()};
				uint16_t flags = buf.ReadWord();
				if (_extra_station_names.size() < MAX_EXTRA_STATION_NAMES) {
					size_t idx = _extra_station_names.size();
					ExtraStationNameInfo &info = _extra_station_names.emplace_back();
					AddStringForMapping(str, idx, [](StringID str, size_t idx) { _extra_station_names[idx].str = str; });
					info.flags = flags;
				}
				break;
			}

			case A0RPI_GLOBALVAR_EXTRA_STATION_NAMES_PROBABILITY: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				_extra_station_names_probability = buf.ReadByte();
				break;
			}

			case A0RPI_GLOBALVAR_LIGHTHOUSE_GENERATE_AMOUNT:
			case A0RPI_GLOBALVAR_TRANSMITTER_GENERATE_AMOUNT: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				extern std::vector<ObjectSpec> _object_specs;
				ObjectType type = (prop == A0RPI_GLOBALVAR_LIGHTHOUSE_GENERATE_AMOUNT) ? OBJECT_LIGHTHOUSE : OBJECT_TRANSMITTER;
				_object_specs[type].generate_amount = buf.ReadByte();
				break;
			}

			case A0RPI_GLOBALVAR_ALLOW_ROCKS_DESERT: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				extern bool _allow_rocks_desert;
				_allow_rocks_desert = (buf.ReadByte() != 0);
				break;
			}

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

static ChangeInfoResult GlobalVarReserveInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	/* Properties which are handled as a whole */
	switch (prop) {
		case 0x09: // Cargo Translation Table; loading during both reservation and activation stage (in case it is selected depending on defined cargos)
			return LoadTranslationTable<CargoLabel>(first, last, buf, [](GRFFile &grf) -> std::vector<CargoLabel> & { return grf.cargo_list; }, "Cargo");

		case 0x12: // Rail type translation table; loading during both reservation and activation stage (in case it is selected depending on defined railtypes)
			return LoadTranslationTable<RailTypeLabel>(first, last, buf, [](GRFFile &grf) -> std::vector<RailTypeLabel> & { return grf.railtype_list; }, "Rail type");

		case 0x16: // Road type translation table; loading during both reservation and activation stage (in case it is selected depending on defined roadtypes)
			return LoadTranslationTable<RoadTypeLabel>(first, last, buf, [](GRFFile &grf) -> std::vector<RoadTypeLabel> & { return grf.roadtype_list; }, "Road type");

		case 0x17: // Tram type translation table; loading during both reservation and activation stage (in case it is selected depending on defined tramtypes)
			return LoadTranslationTable<RoadTypeLabel>(first, last, buf, [](GRFFile &grf) -> std::vector<RoadTypeLabel> & { return grf.tramtype_list; }, "Tram type");

		case 0x18: // Badge translation table
			return LoadBadgeTranslationTable(first, last, buf, _cur.grffile->badge_list, "Badge");

		default:
			break;
	}

	/* Properties which are handled per item */
	ChangeInfoResult ret = CIR_SUCCESS;

	for (uint id = first; id < last; ++id) {
		switch (prop) {
			case 0x08: // Cost base factor
			case 0x15: // Plural form translation
				buf.ReadByte();
				break;

			case 0x0A: // Currency display names
			case 0x0C: // Currency options
			case 0x0F: // Euro introduction dates
				buf.ReadWord();
				break;

			case 0x0B: // Currency multipliers
			case 0x0D: // Currency prefix symbol
			case 0x0E: // Currency suffix symbol
				buf.ReadDWord();
				break;

			case 0x10: // Snow line height table
				buf.Skip(SNOW_LINE_MONTHS * SNOW_LINE_DAYS);
				break;

			case 0x11: { // GRF match for engine allocation
				uint32_t s = buf.ReadDWord();
				uint32_t t = buf.ReadDWord();
				SetNewGRFOverride(s, t);
				break;
			}

			case 0x13: // Gender translation table
			case 0x14: // Case translation table
				while (buf.ReadByte() != 0) {
					buf.ReadString();
				}
				break;

			case A0RPI_GLOBALVAR_EXTRA_STATION_NAMES:
			case A0RPI_GLOBALVAR_EXTRA_STATION_NAMES_PROBABILITY:
			case A0RPI_GLOBALVAR_LIGHTHOUSE_GENERATE_AMOUNT:
			case A0RPI_GLOBALVAR_TRANSMITTER_GENERATE_AMOUNT:
			case A0RPI_GLOBALVAR_ALLOW_ROCKS_DESERT:
				buf.Skip(buf.ReadExtendedByte());
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}


/**
 * Define properties for cargoes
 * @param first ID of the first cargo.
 * @param last ID of the last cargo.
 * @param prop The property to change.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult CargoChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > NUM_CARGO) {
		GrfMsg(2, "CargoChangeInfo: Cargo type {} out of range (max {})", last, NUM_CARGO - 1);
		return CIR_INVALID_ID;
	}

	for (uint id = first; id < last; ++id) {
		CargoSpec *cs = CargoSpec::Get(id);

		switch (prop) {
			case 0x08: // Bit number of cargo
				cs->bitnum = buf.ReadByte();
				if (cs->IsValid()) {
					cs->grffile = _cur.grffile;
					SetBit(_cargo_mask, id);
				} else {
					ClrBit(_cargo_mask, id);
				}
				BuildCargoLabelMap();
				break;

			case 0x09: // String ID for cargo type name
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->name);
				break;

			case 0x0A: // String for 1 unit of cargo
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->name_single);
				break;

			case 0x0B: // String for singular quantity of cargo (e.g. 1 tonne of coal)
			case 0x1B: // String for cargo units
				/* String for units of cargo. This is different in OpenTTD
				 * (e.g. tonnes) to TTDPatch (e.g. {COMMA} tonne of coal).
				 * Property 1B is used to set OpenTTD's behaviour. */
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->units_volume);
				break;

			case 0x0C: // String for plural quantity of cargo (e.g. 10 tonnes of coal)
			case 0x1C: // String for any amount of cargo
				/* Strings for an amount of cargo. This is different in OpenTTD
				 * (e.g. {WEIGHT} of coal) to TTDPatch (e.g. {COMMA} tonnes of coal).
				 * Property 1C is used to set OpenTTD's behaviour. */
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->quantifier);
				break;

			case 0x0D: // String for two letter cargo abbreviation
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->abbrev);
				break;

			case 0x0E: // Sprite ID for cargo icon
				cs->sprite = buf.ReadWord();
				break;

			case 0x0F: // Weight of one unit of cargo
				cs->weight = buf.ReadByte();
				break;

			case 0x10: // Used for payment calculation
				cs->transit_periods[0] = buf.ReadByte();
				break;

			case 0x11: // Used for payment calculation
				cs->transit_periods[1] = buf.ReadByte();
				break;

			case 0x12: // Base cargo price
				cs->initial_payment = buf.ReadDWord();
				break;

			case 0x13: // Colour for station rating bars
				cs->rating_colour = buf.ReadByte();
				break;

			case 0x14: // Colour for cargo graph
				cs->legend_colour = buf.ReadByte();
				break;

			case 0x15: // Freight status
				cs->is_freight = (buf.ReadByte() != 0);
				break;

			case 0x16: // Cargo classes
				cs->classes = buf.ReadWord();
				break;

			case 0x17: // Cargo label
				cs->label = CargoLabel{std::byteswap(buf.ReadDWord())};
				BuildCargoLabelMap();
				break;

			case 0x18: { // Town growth substitute type
				uint8_t substitute_type = buf.ReadByte();

				switch (substitute_type) {
					case 0x00: cs->town_acceptance_effect = TAE_PASSENGERS; break;
					case 0x02: cs->town_acceptance_effect = TAE_MAIL; break;
					case 0x05: cs->town_acceptance_effect = TAE_GOODS; break;
					case 0x09: cs->town_acceptance_effect = TAE_WATER; break;
					case 0x0B: cs->town_acceptance_effect = TAE_FOOD; break;
					default:
						GrfMsg(1, "CargoChangeInfo: Unknown town growth substitute value {}, setting to none.", substitute_type);
						[[fallthrough]];
					case 0xFF: cs->town_acceptance_effect = TAE_NONE; break;
				}
				break;
			}

			case 0x19: // Town growth coefficient
				buf.ReadWord();
				break;

			case 0x1A: // Bitmask of callbacks to use
				cs->callback_mask = static_cast<CargoCallbackMasks>(buf.ReadByte());
				break;

			case 0x1D: // Vehicle capacity muliplier
				cs->multiplier = std::max<uint16_t>(1u, buf.ReadWord());
				break;

			case 0x1E: { // Town production substitute type
				uint8_t substitute_type = buf.ReadByte();

				switch (substitute_type) {
					case 0x00: cs->town_production_effect = TPE_PASSENGERS; break;
					case 0x02: cs->town_production_effect = TPE_MAIL; break;
					default:
						GrfMsg(1, "CargoChangeInfo: Unknown town production substitute value {}, setting to none.", substitute_type);
						[[fallthrough]];
					case 0xFF: cs->town_production_effect = TPE_NONE; break;
				}
				break;
			}

			case 0x1F: // Town production multiplier
				cs->town_production_multiplier = std::max<uint16_t>(1U, buf.ReadWord());
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}


/**
 * Define properties for sound effects
 * @param first Local ID of the first sound.
 * @param last Local ID of the last sound.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult SoundEffectChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (_cur.grffile->sound_offset == 0) {
		GrfMsg(1, "SoundEffectChangeInfo: No effects defined, skipping");
		return CIR_INVALID_ID;
	}

	if (last - ORIGINAL_SAMPLE_COUNT > _cur.grffile->num_sounds) {
		GrfMsg(1, "SoundEffectChangeInfo: Attempting to change undefined sound effect ({}), max ({}). Ignoring.", last, ORIGINAL_SAMPLE_COUNT + _cur.grffile->num_sounds);
		return CIR_INVALID_ID;
	}

	for (uint id = first; id < last; ++id) {
		SoundEntry *sound = GetSound(first + _cur.grffile->sound_offset - ORIGINAL_SAMPLE_COUNT);

		switch (prop) {
			case 0x08: // Relative volume
				sound->volume = Clamp(buf.ReadByte(), 0, SOUND_EFFECT_MAX_VOLUME);
				break;

			case 0x09: // Priority
				sound->priority = buf.ReadByte();
				break;

			case 0x0A: { // Override old sound
				SoundID orig_sound = buf.ReadByte();

				if (orig_sound >= ORIGINAL_SAMPLE_COUNT) {
					GrfMsg(1, "SoundEffectChangeInfo: Original sound {} not defined (max {})", orig_sound, ORIGINAL_SAMPLE_COUNT);
				} else {
					SoundEntry *old_sound = GetSound(orig_sound);

					/* Literally copy the data of the new sound over the original */
					*old_sound = *sound;
				}
				break;
			}

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Ignore an industry tile property
 * @param prop The property to ignore.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult IgnoreIndustryTileProperty(int prop, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	switch (prop) {
		case 0x09:
		case 0x0D:
		case 0x0E:
		case 0x10:
		case 0x11:
		case 0x12:
			buf.ReadByte();
			break;

		case 0x0A:
		case 0x0B:
		case 0x0C:
		case 0x0F:
			buf.ReadWord();
			break;

		case 0x13:
			buf.Skip(buf.ReadByte() * 2);
			break;

		default:
			ret = HandleAction0PropertyDefault(buf, prop);
			break;
	}
	return ret;
}

/**
 * Define properties for industry tiles
 * @param first Local ID of the first industry tile.
 * @param last Local ID of the last industry tile.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult IndustrytilesChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > NUM_INDUSTRYTILES_PER_GRF) {
		GrfMsg(1, "IndustryTilesChangeInfo: Too many industry tiles loaded ({}), max ({}). Ignoring.", last, NUM_INDUSTRYTILES_PER_GRF);
		return CIR_INVALID_ID;
	}

	/* Allocate industry tile specs if they haven't been allocated already. */
	if (_cur.grffile->indtspec.size() < last) _cur.grffile->indtspec.resize(last);

	for (uint id = first; id < last; ++id) {
		IndustryTileSpec *tsp = _cur.grffile->indtspec[id].get();

		if (prop != 0x08 && tsp == nullptr) {
			ChangeInfoResult cir = IgnoreIndustryTileProperty(prop, buf);
			if (cir > ret) ret = cir;
			continue;
		}

		switch (prop) {
			case 0x08: { // Substitute industry tile type
				uint8_t subs_id = buf.ReadByte();
				if (subs_id >= NEW_INDUSTRYTILEOFFSET) {
					/* The substitute id must be one of the original industry tile. */
					GrfMsg(2, "IndustryTilesChangeInfo: Attempt to use new industry tile {} as substitute industry tile for {}. Ignoring.", subs_id, id);
					continue;
				}

				/* Allocate space for this industry. */
				if (tsp == nullptr) {
					_cur.grffile->indtspec[id] = std::make_unique<IndustryTileSpec>(_industry_tile_specs[subs_id]);
					tsp = _cur.grffile->indtspec[id].get();

					tsp->enabled = true;

					/* A copied tile should not have the animation infos copied too.
					 * The anim_state should be left untouched, though
					 * It is up to the author to animate them */
					tsp->anim_production = INDUSTRYTILE_NOANIM;
					tsp->anim_next = INDUSTRYTILE_NOANIM;

					tsp->grf_prop.local_id = id;
					tsp->grf_prop.subst_id = subs_id;
					tsp->grf_prop.grfid = _cur.grffile->grfid;
					tsp->grf_prop.grffile = _cur.grffile;
					_industile_mngr.AddEntityID(id, _cur.grffile->grfid, subs_id); // pre-reserve the tile slot
				}
				break;
			}

			case 0x09: { // Industry tile override
				uint8_t ovrid = buf.ReadByte();

				/* The industry being overridden must be an original industry. */
				if (ovrid >= NEW_INDUSTRYTILEOFFSET) {
					GrfMsg(2, "IndustryTilesChangeInfo: Attempt to override new industry tile {} with industry tile id {}. Ignoring.", ovrid, id);
					continue;
				}

				_industile_mngr.Add(id, _cur.grffile->grfid, ovrid);
				break;
			}

			case 0x0A: // Tile acceptance
			case 0x0B:
			case 0x0C: {
				uint16_t acctp = buf.ReadWord();
				tsp->accepts_cargo[prop - 0x0A] = GetCargoTranslation(GB(acctp, 0, 8), _cur.grffile);
				tsp->acceptance[prop - 0x0A] = Clamp(GB(acctp, 8, 8), 0, 16);
				tsp->accepts_cargo_label[prop - 0x0A] = CT_INVALID;
				break;
			}

			case 0x0D: // Land shape flags
				tsp->slopes_refused = (Slope)buf.ReadByte();
				break;

			case 0x0E: // Callback mask
				tsp->callback_mask = static_cast<IndustryTileCallbackMasks>(buf.ReadByte());
				break;

			case 0x0F: // Animation information
				tsp->animation.frames = buf.ReadByte();
				tsp->animation.status = buf.ReadByte();
				break;

			case 0x10: // Animation speed
				tsp->animation.speed = buf.ReadByte();
				break;

			case 0x11: // Triggers for callback 25
				tsp->animation.triggers = buf.ReadByte();
				break;

			case 0x12: // Special flags
				tsp->special_flags = IndustryTileSpecialFlags{buf.ReadByte()};
				break;

			case 0x13: { // variable length cargo acceptance
				uint8_t num_cargoes = buf.ReadByte();
				if (num_cargoes > std::size(tsp->acceptance)) {
					GRFError *error = DisableGrf(STR_NEWGRF_ERROR_LIST_PROPERTY_TOO_LONG);
					error->param_value[1] = prop;
					return CIR_DISABLED;
				}
				for (uint i = 0; i < std::size(tsp->acceptance); i++) {
					if (i < num_cargoes) {
						tsp->accepts_cargo[i] = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
						/* Tile acceptance can be negative to counteract the IndustryTileSpecialFlag::AcceptsAllCargo flag */
						tsp->acceptance[i] = (int8_t)buf.ReadByte();
					} else {
						tsp->accepts_cargo[i] = INVALID_CARGO;
						tsp->acceptance[i] = 0;
					}
					if (i < std::size(tsp->accepts_cargo_label)) tsp->accepts_cargo_label[i] = CT_INVALID;
				}
				break;
			}

			case 0x14: // Badge list
				tsp->badges = ReadBadgeList(buf, GSF_INDUSTRYTILES);
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Ignore an industry property
 * @param prop The property to ignore.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult IgnoreIndustryProperty(int prop, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	switch (prop) {
		case 0x09:
		case 0x0B:
		case 0x0F:
		case 0x12:
		case 0x13:
		case 0x14:
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x21:
		case 0x22:
			buf.ReadByte();
			break;

		case 0x0C:
		case 0x0D:
		case 0x0E:
		case 0x10: // INDUSTRY_ORIGINAL_NUM_OUTPUTS bytes
		case 0x1B:
		case 0x1F:
		case 0x24:
			buf.ReadWord();
			break;

		case 0x11: // INDUSTRY_ORIGINAL_NUM_INPUTS bytes + 1
		case 0x1A:
		case 0x1C:
		case 0x1D:
		case 0x1E:
		case 0x20:
		case 0x23:
			buf.ReadDWord();
			break;

		case 0x0A: {
			uint8_t num_table = buf.ReadByte();
			for (uint8_t j = 0; j < num_table; j++) {
				for (uint k = 0;; k++) {
					uint8_t x = buf.ReadByte();
					if (x == 0xFE && k == 0) {
						buf.ReadByte();
						buf.ReadByte();
						break;
					}

					uint8_t y = buf.ReadByte();
					if (x == 0 && y == 0x80) break;

					uint8_t gfx = buf.ReadByte();
					if (gfx == 0xFE) buf.ReadWord();
				}
			}
			break;
		}

		case 0x16:
			for (uint8_t j = 0; j < INDUSTRY_ORIGINAL_NUM_INPUTS; j++) buf.ReadByte();
			break;

		case 0x15:
		case 0x25:
		case 0x26:
		case 0x27:
			buf.Skip(buf.ReadByte());
			break;

		case 0x28: {
			int num_inputs = buf.ReadByte();
			int num_outputs = buf.ReadByte();
			buf.Skip(num_inputs * num_outputs * 2);
			break;
		}

		case 0x29: // Badge list
			SkipBadgeList(buf);
			break;

		default:
			ret = HandleAction0PropertyDefault(buf, prop);
			break;
	}
	return ret;
}

/**
 * Validate the industry layout; e.g. to prevent duplicate tiles.
 * @param layout The layout to check.
 * @return True if the layout is deemed valid.
 */
static bool ValidateIndustryLayout(const IndustryTileLayout &layout)
{
	const size_t size = layout.size();
	if (size == 0) return false;

	for (size_t i = 0; i < size - 1; i++) {
		for (size_t j = i + 1; j < size; j++) {
			if (layout[i].ti.x == layout[j].ti.x &&
					layout[i].ti.y == layout[j].ti.y) {
				return false;
			}
		}
	}

	bool have_regular_tile = false;
	for (const auto &tilelayout : layout) {
		if (tilelayout.gfx != GFX_WATERTILE_SPECIALCHECK) {
			have_regular_tile = true;
			break;
		}
	}

	return have_regular_tile;
}

/**
 * Define properties for industries
 * @param first Local ID of the first industry.
 * @param last Local ID of the last industry.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult IndustriesChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > NUM_INDUSTRYTYPES_PER_GRF) {
		GrfMsg(1, "IndustriesChangeInfo: Too many industries loaded ({}), max ({}). Ignoring.", last, NUM_INDUSTRYTYPES_PER_GRF);
		return CIR_INVALID_ID;
	}

	/* Allocate industry specs if they haven't been allocated already. */
	if (_cur.grffile->industryspec.size() < last) _cur.grffile->industryspec.resize(last);

	for (uint id = first; id < last; ++id) {
		IndustrySpec *indsp = _cur.grffile->industryspec[id].get();

		if (prop != 0x08 && indsp == nullptr) {
			ChangeInfoResult cir = IgnoreIndustryProperty(prop, buf);
			if (cir > ret) ret = cir;
			continue;
		}

		switch (prop) {
			case 0x08: { // Substitute industry type
				uint8_t subs_id = buf.ReadByte();
				if (subs_id == 0xFF) {
					/* Instead of defining a new industry, a substitute industry id
					 * of 0xFF disables the old industry with the current id. */
					_industry_specs[id].enabled = false;
					continue;
				} else if (subs_id >= NEW_INDUSTRYOFFSET) {
					/* The substitute id must be one of the original industry. */
					GrfMsg(2, "_industry_specs: Attempt to use new industry {} as substitute industry for {}. Ignoring.", subs_id, id);
					continue;
				}

				/* Allocate space for this industry.
				 * Only need to do it once. If ever it is called again, it should not
				 * do anything */
				if (indsp == nullptr) {
					_cur.grffile->industryspec[id] = std::make_unique<IndustrySpec>(_origin_industry_specs[subs_id]);
					indsp = _cur.grffile->industryspec[id].get();

					indsp->enabled = true;
					indsp->grf_prop.local_id = id;
					indsp->grf_prop.subst_id = subs_id;
					indsp->grf_prop.grfid = _cur.grffile->grfid;
					indsp->grf_prop.grffile = _cur.grffile;
					/* If the grf industry needs to check its surrounding upon creation, it should
					 * rely on callbacks, not on the original placement functions */
					indsp->check_proc = CHECK_NOTHING;
				}
				break;
			}

			case 0x09: { // Industry type override
				uint8_t ovrid = buf.ReadByte();

				/* The industry being overridden must be an original industry. */
				if (ovrid >= NEW_INDUSTRYOFFSET) {
					GrfMsg(2, "IndustriesChangeInfo: Attempt to override new industry {} with industry id {}. Ignoring.", ovrid, id);
					continue;
				}
				indsp->grf_prop.override = ovrid;
				_industry_mngr.Add(id, _cur.grffile->grfid, ovrid);
				break;
			}

			case 0x0A: { // Set industry layout(s)
				uint8_t new_num_layouts = buf.ReadByte();
				uint32_t definition_size = buf.ReadDWord();
				uint32_t bytes_read = 0;
				std::vector<IndustryTileLayout> new_layouts;
				IndustryTileLayout layout;

				for (uint8_t j = 0; j < new_num_layouts; j++) {
					layout.clear();
					layout.reserve(new_num_layouts);

					for (uint k = 0;; k++) {
						if (bytes_read >= definition_size) {
							GrfMsg(3, "IndustriesChangeInfo: Incorrect size for industry tile layout definition for industry {}.", id);
							/* Avoid warning twice */
							definition_size = UINT32_MAX;
						}

						IndustryTileLayoutTile &it = layout.emplace_back();

						it.ti.x = buf.ReadByte(); // Offsets from northermost tile
						++bytes_read;

						if (it.ti.x == 0xFE && k == 0) {
							/* This means we have to borrow the layout from an old industry */
							IndustryType type = buf.ReadByte();
							uint8_t laynbr = buf.ReadByte();
							bytes_read += 2;

							if (type >= lengthof(_origin_industry_specs)) {
								GrfMsg(1, "IndustriesChangeInfo: Invalid original industry number for layout import, industry {}", id);
								DisableGrf(STR_NEWGRF_ERROR_INVALID_ID);
								return CIR_DISABLED;
							}
							if (laynbr >= _origin_industry_specs[type].layouts.size()) {
								GrfMsg(1, "IndustriesChangeInfo: Invalid original industry layout index for layout import, industry {}", id);
								DisableGrf(STR_NEWGRF_ERROR_INVALID_ID);
								return CIR_DISABLED;
							}
							layout = _origin_industry_specs[type].layouts[laynbr];
							break;
						}

						it.ti.y = buf.ReadByte(); // Or table definition finalisation
						++bytes_read;

						if (it.ti.x == 0 && it.ti.y == 0x80) {
							/* Terminator, remove and finish up */
							layout.pop_back();
							break;
						}

						it.gfx = buf.ReadByte();
						++bytes_read;

						if (it.gfx == 0xFE) {
							/* Use a new tile from this GRF */
							int local_tile_id = buf.ReadWord();
							bytes_read += 2;

							/* Read the ID from the _industile_mngr. */
							int tempid = _industile_mngr.GetID(local_tile_id, _cur.grffile->grfid);

							if (tempid == INVALID_INDUSTRYTILE) {
								GrfMsg(2, "IndustriesChangeInfo: Attempt to use industry tile {} with industry id {}, not yet defined. Ignoring.", local_tile_id, id);
							} else {
								/* Declared as been valid, can be used */
								it.gfx = tempid;
							}
						} else if (it.gfx == GFX_WATERTILE_SPECIALCHECK) {
							it.ti.x = (int8_t)GB(it.ti.x, 0, 8);
							it.ti.y = (int8_t)GB(it.ti.y, 0, 8);

							/* When there were only 256x256 maps, TileIndex was a uint16_t and
							 * it.ti was just a TileIndexDiff that was added to it.
							 * As such negative "x" values were shifted into the "y" position.
							 *   x = -1, y = 1 -> x = 255, y = 0
							 * Since GRF version 8 the position is interpreted as pair of independent int8.
							 * For GRF version < 8 we need to emulate the old shifting behaviour.
							 */
							if (_cur.grffile->grf_version < 8 && it.ti.x < 0) it.ti.y += 1;
						}
					}

					if (!ValidateIndustryLayout(layout)) {
						/* The industry layout was not valid, so skip this one. */
						GrfMsg(1, "IndustriesChangeInfo: Invalid industry layout for industry id {}. Ignoring", id);
						new_num_layouts--;
						j--;
					} else {
						new_layouts.push_back(layout);
					}
				}

				/* Install final layout construction in the industry spec */
				indsp->layouts = new_layouts;
				break;
			}

			case 0x0B: // Industry production flags
				indsp->life_type = IndustryLifeTypes{buf.ReadByte()};
				break;

			case 0x0C: // Industry closure message
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &indsp->closure_text);
				break;

			case 0x0D: // Production increase message
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &indsp->production_up_text);
				break;

			case 0x0E: // Production decrease message
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &indsp->production_down_text);
				break;

			case 0x0F: // Fund cost multiplier
				indsp->cost_multiplier = buf.ReadByte();
				break;

			case 0x10: // Production cargo types
				for (uint8_t j = 0; j < INDUSTRY_ORIGINAL_NUM_OUTPUTS; j++) {
					indsp->produced_cargo[j] = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
					indsp->produced_cargo_label[j] = CT_INVALID;
				}
				break;

			case 0x11: // Acceptance cargo types
				for (uint8_t j = 0; j < INDUSTRY_ORIGINAL_NUM_INPUTS; j++) {
					indsp->accepts_cargo[j] = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
					indsp->accepts_cargo_label[j] = CT_INVALID;
				}
				buf.ReadByte(); // Unnused, eat it up
				break;

			case 0x12: // Production multipliers
			case 0x13:
				indsp->production_rate[prop - 0x12] = buf.ReadByte();
				break;

			case 0x14: // Minimal amount of cargo distributed
				indsp->minimal_cargo = buf.ReadByte();
				break;

			case 0x15: { // Random sound effects
				uint8_t num_sounds = buf.ReadByte();

				std::vector<uint8_t> sounds;
				sounds.reserve(num_sounds);
				for (uint8_t j = 0; j < num_sounds; ++j) {
					sounds.push_back(buf.ReadByte());
				}

				indsp->random_sounds = std::move(sounds);
				break;
			}

			case 0x16: // Conflicting industry types
				for (uint8_t j = 0; j < 3; j++) indsp->conflicting[j] = buf.ReadByte();
				break;

			case 0x17: // Probability in random game
				indsp->appear_creation[to_underlying(_settings_game.game_creation.landscape)] = buf.ReadByte();
				break;

			case 0x18: // Probability during gameplay
				indsp->appear_ingame[to_underlying(_settings_game.game_creation.landscape)] = buf.ReadByte();
				break;

			case 0x19: // Map colour
				indsp->map_colour = buf.ReadByte();
				break;

			case 0x1A: // Special industry flags to define special behavior
				indsp->behaviour = IndustryBehaviours{buf.ReadDWord()};
				break;

			case 0x1B: // New industry text ID
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &indsp->new_industry_text);
				break;

			case 0x1C: // Input cargo multipliers for the three input cargo types
			case 0x1D:
			case 0x1E: {
					uint32_t multiples = buf.ReadDWord();
					indsp->input_cargo_multiplier[prop - 0x1C][0] = GB(multiples, 0, 16);
					indsp->input_cargo_multiplier[prop - 0x1C][1] = GB(multiples, 16, 16);
					break;
				}

			case 0x1F: // Industry name
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &indsp->name);
				break;

			case 0x20: // Prospecting success chance
				indsp->prospecting_chance = buf.ReadDWord();
				break;

			case 0x21:   // Callback mask
			case 0x22: { // Callback additional mask
				auto mask = indsp->callback_mask.base();
				SB(mask, (prop - 0x21) * 8, 8, buf.ReadByte());
				indsp->callback_mask = IndustryCallbackMasks{mask};
				break;
			}

			case 0x23: // removal cost multiplier
				indsp->removal_cost_multiplier = buf.ReadDWord();
				break;

			case 0x24: { // name for nearby station
				GRFStringID str{buf.ReadWord()};
				if (str == 0) {
					indsp->station_name = STR_NULL;
				} else {
					AddStringForMapping(str, &indsp->station_name);
				}
				break;
			}

			case 0x25: { // variable length produced cargoes
				uint8_t num_cargoes = buf.ReadByte();
				if (num_cargoes > std::size(indsp->produced_cargo)) {
					GRFError *error = DisableGrf(STR_NEWGRF_ERROR_LIST_PROPERTY_TOO_LONG);
					error->param_value[1] = prop;
					return CIR_DISABLED;
				}
				for (uint i = 0; i < std::size(indsp->produced_cargo); i++) {
					if (i < num_cargoes) {
						CargoType cargo = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
						indsp->produced_cargo[i] = cargo;
					} else {
						indsp->produced_cargo[i] = INVALID_CARGO;
					}
					if (i < std::size(indsp->produced_cargo_label)) indsp->produced_cargo_label[i] = CT_INVALID;
				}
				break;
			}

			case 0x26: { // variable length accepted cargoes
				uint8_t num_cargoes = buf.ReadByte();
				if (num_cargoes > std::size(indsp->accepts_cargo)) {
					GRFError *error = DisableGrf(STR_NEWGRF_ERROR_LIST_PROPERTY_TOO_LONG);
					error->param_value[1] = prop;
					return CIR_DISABLED;
				}
				for (uint i = 0; i < std::size(indsp->accepts_cargo); i++) {
					if (i < num_cargoes) {
						CargoType cargo = GetCargoTranslation(buf.ReadByte(), _cur.grffile);
						indsp->accepts_cargo[i] = cargo;
					} else {
						indsp->accepts_cargo[i] = INVALID_CARGO;
					}
					if (i < std::size(indsp->accepts_cargo_label)) indsp->accepts_cargo_label[i] = CT_INVALID;
				}
				break;
			}

			case 0x27: { // variable length production rates
				uint8_t num_cargoes = buf.ReadByte();
				if (num_cargoes > std::size(indsp->production_rate)) {
					GRFError *error = DisableGrf(STR_NEWGRF_ERROR_LIST_PROPERTY_TOO_LONG);
					error->param_value[1] = prop;
					return CIR_DISABLED;
				}
				for (uint i = 0; i < std::size(indsp->production_rate); i++) {
					if (i < num_cargoes) {
						indsp->production_rate[i] = buf.ReadByte();
					} else {
						indsp->production_rate[i] = 0;
					}
				}
				break;
			}

			case 0x28: { // variable size input/output production multiplier table
				uint8_t num_inputs = buf.ReadByte();
				uint8_t num_outputs = buf.ReadByte();
				if (num_inputs > std::size(indsp->accepts_cargo) || num_outputs > std::size(indsp->produced_cargo)) {
					GRFError *error = DisableGrf(STR_NEWGRF_ERROR_LIST_PROPERTY_TOO_LONG);
					error->param_value[1] = prop;
					return CIR_DISABLED;
				}
				for (uint i = 0; i < std::size(indsp->accepts_cargo); i++) {
					for (uint j = 0; j < std::size(indsp->produced_cargo); j++) {
						uint16_t mult = 0;
						if (i < num_inputs && j < num_outputs) mult = buf.ReadWord();
						indsp->input_cargo_multiplier[i][j] = mult;
					}
				}
				break;
			}

			case 0x29: // Badge list
				indsp->badges = ReadBadgeList(buf, GSF_INDUSTRIES);
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for airports
 * @param first Local ID of the first airport.
 * @param last Local ID of the last airport.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult AirportChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > NUM_AIRPORTS_PER_GRF) {
		GrfMsg(1, "AirportChangeInfo: Too many airports, trying id ({}), max ({}). Ignoring.", last, NUM_AIRPORTS_PER_GRF);
		return CIR_INVALID_ID;
	}

	/* Allocate industry specs if they haven't been allocated already. */
	if (_cur.grffile->airportspec.size() < last) _cur.grffile->airportspec.resize(last);

	for (uint id = first; id < last; ++id) {
		AirportSpec *as = _cur.grffile->airportspec[id].get();

		if (as == nullptr && prop != 0x08 && prop != 0x09) {
			GrfMsg(2, "AirportChangeInfo: Attempt to modify undefined airport {}, ignoring", id);
			return CIR_INVALID_ID;
		}

		switch (prop) {
			case 0x08: { // Modify original airport
				uint8_t subs_id = buf.ReadByte();
				if (subs_id == 0xFF) {
					/* Instead of defining a new airport, an airport id
					 * of 0xFF disables the old airport with the current id. */
					AirportSpec::GetWithoutOverride(id)->enabled = false;
					continue;
				} else if (subs_id >= NEW_AIRPORT_OFFSET) {
					/* The substitute id must be one of the original airports. */
					GrfMsg(2, "AirportChangeInfo: Attempt to use new airport {} as substitute airport for {}. Ignoring.", subs_id, id);
					continue;
				}

				/* Allocate space for this airport.
				 * Only need to do it once. If ever it is called again, it should not
				 * do anything */
				if (as == nullptr) {
					_cur.grffile->airportspec[id] = std::make_unique<AirportSpec>(*AirportSpec::GetWithoutOverride(subs_id));
					as = _cur.grffile->airportspec[id].get();

					as->enabled = true;
					as->grf_prop.local_id = id;
					as->grf_prop.subst_id = subs_id;
					as->grf_prop.grfid = _cur.grffile->grfid;
					as->grf_prop.grffile = _cur.grffile;
					/* override the default airport */
					_airport_mngr.Add(id, _cur.grffile->grfid, subs_id);
				}
				break;
			}

			case 0x0A: { // Set airport layout
				uint8_t num_layouts = buf.ReadByte();
				buf.ReadDWord(); // Total size of definition, unneeded.
				uint8_t size_x = 0;
				uint8_t size_y = 0;

				std::vector<AirportTileLayout> layouts;
				layouts.reserve(num_layouts);

				for (uint8_t j = 0; j != num_layouts; ++j) {
					auto &layout = layouts.emplace_back();
					layout.rotation = static_cast<Direction>(buf.ReadByte() & 6); // Rotation can only be DIR_NORTH, DIR_EAST, DIR_SOUTH or DIR_WEST.

					for (;;) {
						auto &tile = layout.tiles.emplace_back();
						tile.ti.x = buf.ReadByte();
						tile.ti.y = buf.ReadByte();
						if (tile.ti.x == 0 && tile.ti.y == 0x80) {
							/* Convert terminator to our own. */
							tile.ti.x = -0x80;
							tile.ti.y = 0;
							tile.gfx = 0;
							break;
						}

						tile.gfx = buf.ReadByte();

						if (tile.gfx == 0xFE) {
							/* Use a new tile from this GRF */
							int local_tile_id = buf.ReadWord();

							/* Read the ID from the _airporttile_mngr. */
							uint16_t tempid = _airporttile_mngr.GetID(local_tile_id, _cur.grffile->grfid);

							if (tempid == INVALID_AIRPORTTILE) {
								GrfMsg(2, "AirportChangeInfo: Attempt to use airport tile {} with airport id {}, not yet defined. Ignoring.", local_tile_id, id);
							} else {
								/* Declared as been valid, can be used */
								tile.gfx = tempid;
							}
						} else if (tile.gfx == 0xFF) {
							tile.ti.x = static_cast<int8_t>(GB(tile.ti.x, 0, 8));
							tile.ti.y = static_cast<int8_t>(GB(tile.ti.y, 0, 8));
						}

						/* Determine largest size. */
						if (layout.rotation == DIR_E || layout.rotation == DIR_W) {
							size_x = std::max<uint8_t>(size_x, tile.ti.y + 1);
							size_y = std::max<uint8_t>(size_y, tile.ti.x + 1);
						} else {
							size_x = std::max<uint8_t>(size_x, tile.ti.x + 1);
							size_y = std::max<uint8_t>(size_y, tile.ti.y + 1);
						}
					}
				}
				as->layouts = std::move(layouts);
				as->size_x = size_x;
				as->size_y = size_y;
				break;
			}

			case 0x0C:
				as->min_year = CalTime::Year{buf.ReadWord()};
				as->max_year = CalTime::Year{buf.ReadWord()};
				if (as->max_year == 0xFFFF) as->max_year = CalTime::MAX_YEAR;
				break;

			case 0x0D:
				as->ttd_airport_type = (TTDPAirportType)buf.ReadByte();
				break;

			case 0x0E:
				as->catchment = Clamp(buf.ReadByte(), 1, MAX_CATCHMENT);
				break;

			case 0x0F:
				as->noise_level = buf.ReadByte();
				break;

			case 0x10:
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &as->name);
				break;

			case 0x11: // Maintenance cost factor
				as->maintenance_cost = buf.ReadWord();
				break;

			case 0x12: // Badge list
				as->badges = ReadBadgeList(buf, GSF_AIRPORTS);
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for signals
 * @param first Local ID (unused) first.
 * @param last Local ID (unused) last.
 * @param numinfo Number of subsequent IDs to change the property for.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult SignalsChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	/* Properties which are handled per item */
	ChangeInfoResult ret = CIR_SUCCESS;
	for (uint id = first; id < last; ++id) {
		switch (prop) {
			case A0RPI_SIGNALS_ENABLE_PROGRAMMABLE_SIGNALS:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				AssignBit(_cur.grffile->new_signal_ctrl_flags, NSCF_PROGSIG, buf.ReadByte() != 0);
				break;

			case A0RPI_SIGNALS_ENABLE_NO_ENTRY_SIGNALS:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				AssignBit(_cur.grffile->new_signal_ctrl_flags, NSCF_NOENTRYSIG, buf.ReadByte() != 0);
				break;

			case A0RPI_SIGNALS_ENABLE_RESTRICTED_SIGNALS:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				AssignBit(_cur.grffile->new_signal_ctrl_flags, NSCF_RESTRICTEDSIG, buf.ReadByte() != 0);
				break;

			case A0RPI_SIGNALS_ENABLE_SIGNAL_RECOLOUR:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				AssignBit(_cur.grffile->new_signal_ctrl_flags, NSCF_RECOLOUR_ENABLED, buf.ReadByte() != 0);
				break;

			case A0RPI_SIGNALS_EXTRA_ASPECTS:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				_cur.grffile->new_signal_extra_aspects = std::min<uint8_t>(buf.ReadByte(), NEW_SIGNALS_MAX_EXTRA_ASPECT);
				break;

			case A0RPI_SIGNALS_NO_DEFAULT_STYLE:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				AssignBit(_cur.grffile->new_signal_style_mask, 0, buf.ReadByte() == 0);
				break;

			case A0RPI_SIGNALS_DEFINE_STYLE: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t local_id = buf.ReadByte();
				if (_num_new_signal_styles < MAX_NEW_SIGNAL_STYLES) {
					NewSignalStyle &style = _new_signal_styles[_num_new_signal_styles];
					style = {};
					_num_new_signal_styles++;
					SetBit(_cur.grffile->new_signal_style_mask, _num_new_signal_styles);
					style.grf_local_id = local_id;
					style.grffile = _cur.grffile;
					_cur.grffile->current_new_signal_style = &style;
				} else {
					_cur.grffile->current_new_signal_style = nullptr;
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_NAME: {
				if (MappedPropertyLengthMismatch(buf, 2, mapping_entry)) break;
				GRFStringID str = GRFStringID{buf.ReadWord()};
				if (_cur.grffile->current_new_signal_style != nullptr) {
					AddStringForMapping(str, &(_cur.grffile->current_new_signal_style->name));
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_NO_ASPECT_INCREASE: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t value = buf.ReadByte();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					AssignBit(_cur.grffile->current_new_signal_style->style_flags, NSSF_NO_ASPECT_INC, value != 0);
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_ALWAYS_RESERVE_THROUGH: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t value = buf.ReadByte();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					AssignBit(_cur.grffile->current_new_signal_style->style_flags, NSSF_ALWAYS_RESERVE_THROUGH, value != 0);
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_LOOKAHEAD_EXTRA_ASPECTS: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t value = buf.ReadByte();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					SetBit(_cur.grffile->current_new_signal_style->style_flags, NSSF_LOOKAHEAD_ASPECTS_SET);
					_cur.grffile->current_new_signal_style->lookahead_extra_aspects = value;
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_LOOKAHEAD_SINGLE_SIGNAL_ONLY: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t value = buf.ReadByte();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					AssignBit(_cur.grffile->current_new_signal_style->style_flags, NSSF_LOOKAHEAD_SINGLE_SIGNAL, value != 0);
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_SEMAPHORE_ENABLED: {
				if (MappedPropertyLengthMismatch(buf, 4, mapping_entry)) break;
				uint32_t mask = buf.ReadDWord();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					_cur.grffile->current_new_signal_style->semaphore_mask = (uint8_t)mask;
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_ELECTRIC_ENABLED: {
				if (MappedPropertyLengthMismatch(buf, 4, mapping_entry)) break;
				uint32_t mask = buf.ReadDWord();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					_cur.grffile->current_new_signal_style->electric_mask = (uint8_t)mask;
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_OPPOSITE_SIDE: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t value = buf.ReadByte();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					AssignBit(_cur.grffile->current_new_signal_style->style_flags, NSSF_OPPOSITE_SIDE, value != 0);
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_COMBINED_NORMAL_SHUNT: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t value = buf.ReadByte();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					AssignBit(_cur.grffile->current_new_signal_style->style_flags, NSSF_COMBINED_NORMAL_SHUNT, value != 0);
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_REALISTIC_BRAKING_ONLY: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t value = buf.ReadByte();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					AssignBit(_cur.grffile->current_new_signal_style->style_flags, NSSF_REALISTIC_BRAKING_ONLY, value != 0);
				}
				break;
			}

			case A0RPI_SIGNALS_STYLE_BOTH_SIDES: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t value = buf.ReadByte();
				if (_cur.grffile->current_new_signal_style != nullptr) {
					AssignBit(_cur.grffile->current_new_signal_style->style_flags, NSSF_BOTH_SIDES, value != 0);
				}
				break;
			}

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Ignore properties for objects
 * @param prop The property to ignore.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult IgnoreObjectProperty(uint prop, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	switch (prop) {
		case 0x0B:
		case 0x0C:
		case 0x0D:
		case 0x12:
		case 0x14:
		case 0x16:
		case 0x17:
		case 0x18:
			buf.ReadByte();
			break;

		case 0x09:
		case 0x0A:
		case 0x10:
		case 0x11:
		case 0x13:
		case 0x15:
			buf.ReadWord();
			break;

		case 0x08:
		case 0x0E:
		case 0x0F:
			buf.ReadDWord();
			break;

		case 0x19: // Badge list
			SkipBadgeList(buf);
			break;

		default:
			ret = HandleAction0PropertyDefault(buf, prop);
			break;
	}

	return ret;
}

/**
 * Define properties for objects
 * @param first Local ID of the first object.
 * @param last Local ID of the last object.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult ObjectChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > NUM_OBJECTS_PER_GRF) {
		GrfMsg(1, "ObjectChangeInfo: Too many objects loaded ({}), max ({}). Ignoring.", last, NUM_OBJECTS_PER_GRF);
		return CIR_INVALID_ID;
	}

	/* Allocate object specs if they haven't been allocated already. */
	if (_cur.grffile->objectspec.size() < last) _cur.grffile->objectspec.resize(last);

	for (uint id = first; id < last; ++id) {
		ObjectSpec *spec = _cur.grffile->objectspec[id].get();

		if (prop != 0x08 && spec == nullptr) {
			/* If the object property 08 is not yet set, ignore this property */
			ChangeInfoResult cir = IgnoreObjectProperty(prop, buf);
			if (cir > ret) ret = cir;
			continue;
		}

		switch (prop) {
			case 0x08: { // Class ID
				/* Allocate space for this object. */
				if (spec == nullptr) {
					_cur.grffile->objectspec[id] = std::make_unique<ObjectSpec>();
					spec = _cur.grffile->objectspec[id].get();
					spec->views = 1; // Default for NewGRFs that don't set it.
					spec->size = OBJECT_SIZE_1X1; // Default for NewGRFs that manage to not set it (1x1)
				}

				/* Swap classid because we read it in BE. */
				uint32_t classid = buf.ReadDWord();
				spec->class_index = ObjectClass::Allocate(std::byteswap(classid));
				break;
			}

			case 0x09: { // Class name
				AddStringForMapping(GRFStringID{buf.ReadWord()}, spec, [](StringID str, ObjectSpec *spec) { ObjectClass::Get(spec->class_index)->name = str; });
				break;
			}

			case 0x0A: // Object name
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &spec->name);
				break;

			case 0x0B: // Climate mask
				spec->climate = LandscapeTypes{buf.ReadByte()};
				break;

			case 0x0C: // Size
				spec->size = buf.ReadByte();
				if (GB(spec->size, 0, 4) == 0 || GB(spec->size, 4, 4) == 0) {
					GrfMsg(0, "ObjectChangeInfo: Invalid object size requested (0x{:X}) for object id {}. Ignoring.", spec->size, id);
					spec->size = OBJECT_SIZE_1X1;
				}
				break;

			case 0x0D: // Build cost multiplier
				spec->build_cost_multiplier = buf.ReadByte();
				spec->clear_cost_multiplier = spec->build_cost_multiplier;
				break;

			case 0x0E: // Introduction date
				spec->introduction_date = CalTime::Date(static_cast<int32_t>(buf.ReadDWord()));
				break;

			case 0x0F: // End of life
				spec->end_of_life_date = CalTime::Date(static_cast<int32_t>(buf.ReadDWord()));
				break;

			case 0x10: // Flags
				spec->flags = (ObjectFlags)buf.ReadWord();
				_loaded_newgrf_features.has_2CC |= spec->flags.Test(ObjectFlag::Uses2CC);
				break;

			case 0x11: // Animation info
				spec->animation.frames = buf.ReadByte();
				spec->animation.status = buf.ReadByte();
				break;

			case 0x12: // Animation speed
				spec->animation.speed = buf.ReadByte();
				break;

			case 0x13: // Animation triggers
				spec->animation.triggers = buf.ReadWord();
				break;

			case 0x14: // Removal cost multiplier
				spec->clear_cost_multiplier = buf.ReadByte();
				break;

			case 0x15: // Callback mask
				spec->callback_mask = static_cast<ObjectCallbackMasks>(buf.ReadWord());
				break;

			case 0x16: // Building height
				spec->height = buf.ReadByte();
				break;

			case 0x17: // Views
				spec->views = buf.ReadByte();
				if (spec->views != 1 && spec->views != 2 && spec->views != 4) {
					GrfMsg(2, "ObjectChangeInfo: Invalid number of views ({}) for object id {}. Ignoring.", spec->views, id);
					spec->views = 1;
				}
				break;

			case 0x18: // Amount placed on 256^2 map on map creation
				spec->generate_amount = buf.ReadByte();
				break;

			case 0x19: // Badge list
				spec->badges = ReadBadgeList(buf, GSF_OBJECTS);
				break;

			case A0RPI_OBJECT_USE_LAND_GROUND:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				spec->ctrl_flags.Set(ObjectCtrlFlag::UseLandGround, buf.ReadByte() != 0);
				break;

			case A0RPI_OBJECT_EDGE_FOUNDATION_MODE:
				if (MappedPropertyLengthMismatch(buf, 4, mapping_entry)) break;
				spec->ctrl_flags.Set(ObjectCtrlFlag::EdgeFoundation);
				for (int i = 0; i < 4; i++) {
					spec->edge_foundation[i] = buf.ReadByte();
				}
				break;

			case A0RPI_OBJECT_FLOOD_RESISTANT:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				spec->ctrl_flags.Set(ObjectCtrlFlag::FloodResistant, buf.ReadByte() != 0);
				break;

			case A0RPI_OBJECT_VIEWPORT_MAP_TYPE:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				spec->vport_map_type = (ObjectViewportMapType)buf.ReadByte();
				spec->ctrl_flags.Set(ObjectCtrlFlag::ViewportMapTypeSet);
				break;

			case A0RPI_OBJECT_VIEWPORT_MAP_SUBTYPE:
				if (MappedPropertyLengthMismatch(buf, 2, mapping_entry)) break;
				spec->vport_map_subtype = buf.ReadWord();
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for railtypes
 * @param first Local ID of the first railtype.
 * @param last Local ID of the last railtype.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult RailTypeChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	extern RailTypeInfo _railtypes[RAILTYPE_END];

	if (last > RAILTYPE_END) {
		GrfMsg(1, "RailTypeChangeInfo: Rail type {} is invalid, max {}, ignoring", last, RAILTYPE_END);
		return CIR_INVALID_ID;
	}

	for (uint id = first; id < last; ++id) {
		RailType rt = _cur.grffile->railtype_map[id];
		if (rt == INVALID_RAILTYPE) return CIR_INVALID_ID;

		RailTypeInfo *rti = &_railtypes[rt];

		switch (prop) {
			case 0x08: // Label of rail type
				/* Skipped here as this is loaded during reservation stage. */
				buf.ReadDWord();
				break;

			case 0x09: { // Toolbar caption of railtype (sets name as well for backwards compatibility for grf ver < 8)
				GRFStringID str{buf.ReadWord()};
				AddStringForMapping(str, &rti->strings.toolbar_caption);
				if (_cur.grffile->grf_version < 8) {
					AddStringForMapping(str, &rti->strings.name);
				}
				break;
			}

			case 0x0A: // Menu text of railtype
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.menu_text);
				break;

			case 0x0B: // Build window caption
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.build_caption);
				break;

			case 0x0C: // Autoreplace text
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.replace_text);
				break;

			case 0x0D: // New locomotive text
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.new_loco);
				break;

			case 0x0E: // Compatible railtype list
			case 0x0F: // Powered railtype list
			case 0x18: // Railtype list required for date introduction
			case 0x19: // Introduced railtype list
			{
				/* Rail type compatibility bits are added to the existing bits
				 * to allow multiple GRFs to modify compatibility with the
				 * default rail types. */
				int n = buf.ReadByte();
				for (int j = 0; j != n; j++) {
					RailTypeLabel label = buf.ReadDWord();
					RailType resolved_rt = GetRailTypeByLabel(std::byteswap(label), false);
					if (resolved_rt != INVALID_RAILTYPE) {
						switch (prop) {
							case 0x0F: SetBit(rti->powered_railtypes, resolved_rt);               [[fallthrough]]; // Powered implies compatible.
							case 0x0E: SetBit(rti->compatible_railtypes, resolved_rt);            break;
							case 0x18: SetBit(rti->introduction_required_railtypes, resolved_rt); break;
							case 0x19: SetBit(rti->introduces_railtypes, resolved_rt);            break;
						}
					}
				}
				break;
			}

			case 0x10: // Rail Type flags
				rti->flags = static_cast<RailTypeFlags>(buf.ReadByte());
				break;

			case 0x11: // Curve speed advantage
				rti->curve_speed = buf.ReadByte();
				break;

			case 0x12: // Station graphic
				rti->fallback_railtype = Clamp(buf.ReadByte(), 0, 2);
				break;

			case 0x13: // Construction cost factor
				rti->cost_multiplier = buf.ReadWord();
				break;

			case 0x14: // Speed limit
				rti->max_speed = buf.ReadWord();
				break;

			case 0x15: // Acceleration model
				rti->acceleration_type = Clamp(buf.ReadByte(), 0, 2);
				break;

			case 0x16: // Map colour
				rti->map_colour = buf.ReadByte();
				break;

			case 0x17: // Introduction date
				rti->introduction_date = CalTime::Date(static_cast<int32_t>(buf.ReadDWord()));
				break;

			case 0x1A: // Sort order
				rti->sorting_order = buf.ReadByte();
				break;

			case 0x1B: // Name of railtype (overridden by prop 09 for grf ver < 8)
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.name);
				break;

			case 0x1C: // Maintenance cost factor
				rti->maintenance_multiplier = buf.ReadWord();
				break;

			case 0x1D: // Alternate rail type label list
				/* Skipped here as this is loaded during reservation stage. */
				for (int j = buf.ReadByte(); j != 0; j--) buf.ReadDWord();
				break;

			case 0x1E: // Badge list
				rti->badges = ReadBadgeList(buf, GSF_RAILTYPES);
				break;

			case A0RPI_RAILTYPE_ENABLE_PROGRAMMABLE_SIGNALS:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				rti->ctrl_flags.Set(RailTypeCtrlFlag::SigSpriteProgSig, buf.ReadByte() != 0);
				break;

			case A0RPI_RAILTYPE_ENABLE_NO_ENTRY_SIGNALS:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				rti->ctrl_flags.Set(RailTypeCtrlFlag::SigSpriteNoEntry, buf.ReadByte() != 0);
				break;

			case A0RPI_RAILTYPE_ENABLE_RESTRICTED_SIGNALS:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				rti->ctrl_flags.Set(RailTypeCtrlFlag::SigSpriteRestrictedSig, buf.ReadByte() != 0);
				break;

			case A0RPI_RAILTYPE_DISABLE_REALISTIC_BRAKING:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				rti->ctrl_flags.Set(RailTypeCtrlFlag::NoRealisticBraking, buf.ReadByte() != 0);
				break;

			case A0RPI_RAILTYPE_ENABLE_SIGNAL_RECOLOUR:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				rti->ctrl_flags.Set(RailTypeCtrlFlag::SigSpriteRecolourEnabled, buf.ReadByte() != 0);
				break;

			case A0RPI_RAILTYPE_EXTRA_ASPECTS:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				rti->signal_extra_aspects = std::min<uint8_t>(buf.ReadByte(), NEW_SIGNALS_MAX_EXTRA_ASPECT);
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

static ChangeInfoResult RailTypeReserveInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	extern RailTypeInfo _railtypes[RAILTYPE_END];

	if (last > RAILTYPE_END) {
		GrfMsg(1, "RailTypeReserveInfo: Rail type {} is invalid, max {}, ignoring", last, RAILTYPE_END);
		return CIR_INVALID_ID;
	}

	for (uint id = first; id < last; ++id) {
		switch (prop) {
			case 0x08: // Label of rail type
			{
				RailTypeLabel rtl = buf.ReadDWord();
				rtl = std::byteswap(rtl);

				RailType rt = GetRailTypeByLabel(rtl, false);
				if (rt == INVALID_RAILTYPE) {
					/* Set up new rail type */
					rt = AllocateRailType(rtl);
				}

				_cur.grffile->railtype_map[id] = rt;
				break;
			}

			case 0x09: // Toolbar caption of railtype
			case 0x0A: // Menu text
			case 0x0B: // Build window caption
			case 0x0C: // Autoreplace text
			case 0x0D: // New loco
			case 0x13: // Construction cost
			case 0x14: // Speed limit
			case 0x1B: // Name of railtype
			case 0x1C: // Maintenance cost factor
				buf.ReadWord();
				break;

			case 0x1D: // Alternate rail type label list
				if (_cur.grffile->railtype_map[id] != INVALID_RAILTYPE) {
					int n = buf.ReadByte();
					for (int j = 0; j != n; j++) {
						_railtypes[_cur.grffile->railtype_map[id]].alternate_labels.push_back(std::byteswap(buf.ReadDWord()));
					}
					break;
				}
				GrfMsg(1, "RailTypeReserveInfo: Ignoring property 1D for rail type {} because no label was set", id);
				[[fallthrough]];

			case 0x0E: // Compatible railtype list
			case 0x0F: // Powered railtype list
			case 0x18: // Railtype list required for date introduction
			case 0x19: // Introduced railtype list
				for (int j = buf.ReadByte(); j != 0; j--) buf.ReadDWord();
				break;

			case 0x10: // Rail Type flags
			case 0x11: // Curve speed advantage
			case 0x12: // Station graphic
			case 0x15: // Acceleration model
			case 0x16: // Map colour
			case 0x1A: // Sort order
				buf.ReadByte();
				break;

			case 0x17: // Introduction date
				buf.ReadDWord();
				break;

			case 0x1E: // Badge list
				SkipBadgeList(buf);
				break;

			case A0RPI_RAILTYPE_ENABLE_PROGRAMMABLE_SIGNALS:
			case A0RPI_RAILTYPE_ENABLE_NO_ENTRY_SIGNALS:
			case A0RPI_RAILTYPE_ENABLE_RESTRICTED_SIGNALS:
			case A0RPI_RAILTYPE_DISABLE_REALISTIC_BRAKING:
			case A0RPI_RAILTYPE_ENABLE_SIGNAL_RECOLOUR:
			case A0RPI_RAILTYPE_EXTRA_ASPECTS:
				buf.Skip(buf.ReadExtendedByte());
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Define properties for roadtypes
 * @param first Local ID of the first roadtype.
 * @param last Local ID of the last roadtype.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @param rtt Road/tram type.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult RoadTypeChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf, RoadTramType rtt)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	extern RoadTypeInfo _roadtypes[ROADTYPE_END];
	std::array<RoadType, ROADTYPE_END> &type_map = (rtt == RTT_TRAM) ? _cur.grffile->tramtype_map : _cur.grffile->roadtype_map;

	if (last > ROADTYPE_END) {
		GrfMsg(1, "RoadTypeChangeInfo: Road type {} is invalid, max {}, ignoring", last, ROADTYPE_END);
		return CIR_INVALID_ID;
	}

	for (uint id = first; id < last; ++id) {
		RoadType rt = type_map[id];
		if (rt == INVALID_ROADTYPE) return CIR_INVALID_ID;

		RoadTypeInfo *rti = &_roadtypes[rt];

		switch (prop) {
			case 0x08: // Label of road type
				/* Skipped here as this is loaded during reservation stage. */
				buf.ReadDWord();
				break;

			case 0x09: // Toolbar caption of roadtype
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.toolbar_caption);
				break;

			case 0x0A: // Menu text of roadtype
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.menu_text);
				break;

			case 0x0B: // Build window caption
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.build_caption);
				break;

			case 0x0C: // Autoreplace text
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.replace_text);
				break;

			case 0x0D: // New engine text
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.new_engine);
				break;

			case 0x0F: // Powered roadtype list
			case 0x18: // Roadtype list required for date introduction
			case 0x19: { // Introduced roadtype list
				/* Road type compatibility bits are added to the existing bits
				 * to allow multiple GRFs to modify compatibility with the
				 * default road types. */
				int n = buf.ReadByte();
				for (int j = 0; j != n; j++) {
					RoadTypeLabel label = buf.ReadDWord();
					RoadType resolved_rt = GetRoadTypeByLabel(std::byteswap(label), false);
					if (resolved_rt != INVALID_ROADTYPE) {
						switch (prop) {
							case 0x0F:
								if (GetRoadTramType(resolved_rt) == rtt) {
									SetBit(rti->powered_roadtypes, resolved_rt);
								} else {
									GrfMsg(1, "RoadTypeChangeInfo: Powered road type list: Road type {} road/tram type does not match road type {}, ignoring", resolved_rt, rt);
								}
								break;
							case 0x18: SetBit(rti->introduction_required_roadtypes, resolved_rt); break;
							case 0x19: SetBit(rti->introduces_roadtypes, resolved_rt);            break;
						}
					}
				}
				break;
			}

			case 0x10: // Road Type flags
				rti->flags = static_cast<RoadTypeFlags>(buf.ReadByte());
				break;

			case 0x13: // Construction cost factor
				rti->cost_multiplier = buf.ReadWord();
				break;

			case 0x14: // Speed limit
				rti->max_speed = buf.ReadWord();
				break;

			case 0x16: // Map colour
				rti->map_colour = buf.ReadByte();
				break;

			case 0x17: // Introduction date
				rti->introduction_date = CalTime::Date(static_cast<int32_t>(buf.ReadDWord()));
				break;

			case 0x1A: // Sort order
				rti->sorting_order = buf.ReadByte();
				break;

			case 0x1B: // Name of roadtype
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rti->strings.name);
				break;

			case 0x1C: // Maintenance cost factor
				rti->maintenance_multiplier = buf.ReadWord();
				break;

			case 0x1D: // Alternate road type label list
				/* Skipped here as this is loaded during reservation stage. */
				for (int j = buf.ReadByte(); j != 0; j--) buf.ReadDWord();
				break;

			case 0x1E: // Badge list
				rti->badges = ReadBadgeList(buf, GSF_ROADTYPES);
				break;

			case A0RPI_ROADTYPE_EXTRA_FLAGS:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				rti->extra_flags = static_cast<RoadTypeExtraFlags>(buf.ReadByte());
				break;

			case A0RPI_ROADTYPE_COLLISION_MODE: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				uint8_t collision_mode = buf.ReadByte();
				if (collision_mode < RTCM_END) rti->collision_mode = (RoadTypeCollisionMode)collision_mode;
				break;
			}

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

static ChangeInfoResult RoadTypeChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	return RoadTypeChangeInfo(first, last, prop, mapping_entry, buf, RTT_ROAD);
}

static ChangeInfoResult TramTypeChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	return RoadTypeChangeInfo(first, last, prop, mapping_entry, buf, RTT_TRAM);
}


static ChangeInfoResult RoadTypeReserveInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf, RoadTramType rtt)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	extern RoadTypeInfo _roadtypes[ROADTYPE_END];
	std::array<RoadType, ROADTYPE_END> &type_map = (rtt == RTT_TRAM) ? _cur.grffile->tramtype_map : _cur.grffile->roadtype_map;

	if (last > ROADTYPE_END) {
		GrfMsg(1, "RoadTypeReserveInfo: Road type {} is invalid, max {}, ignoring", last, ROADTYPE_END);
		return CIR_INVALID_ID;
	}

	for (uint id = first; id < last; ++id) {
		switch (prop) {
			case 0x08: { // Label of road type
				RoadTypeLabel rtl = buf.ReadDWord();
				rtl = std::byteswap(rtl);

				RoadType rt = GetRoadTypeByLabel(rtl, false);
				if (rt == INVALID_ROADTYPE) {
					/* Set up new road type */
					rt = AllocateRoadType(rtl, rtt);
				} else if (GetRoadTramType(rt) != rtt) {
					GrfMsg(1, "RoadTypeReserveInfo: Road type {} is invalid type (road/tram), ignoring", id);
					return CIR_INVALID_ID;
				}

				type_map[id] = rt;
				break;
			}
			case 0x09: // Toolbar caption of roadtype
			case 0x0A: // Menu text
			case 0x0B: // Build window caption
			case 0x0C: // Autoreplace text
			case 0x0D: // New loco
			case 0x13: // Construction cost
			case 0x14: // Speed limit
			case 0x1B: // Name of roadtype
			case 0x1C: // Maintenance cost factor
				buf.ReadWord();
				break;

			case 0x1D: // Alternate road type label list
				if (type_map[id] != INVALID_ROADTYPE) {
					int n = buf.ReadByte();
					for (int j = 0; j != n; j++) {
						_roadtypes[type_map[id]].alternate_labels.push_back(std::byteswap(buf.ReadDWord()));
					}
					break;
				}
				GrfMsg(1, "RoadTypeReserveInfo: Ignoring property 1D for road type {} because no label was set", id);
				/* FALL THROUGH */

			case 0x0F: // Powered roadtype list
			case 0x18: // Roadtype list required for date introduction
			case 0x19: // Introduced roadtype list
				for (int j = buf.ReadByte(); j != 0; j--) buf.ReadDWord();
				break;

			case 0x10: // Road Type flags
			case 0x16: // Map colour
			case 0x1A: // Sort order
				buf.ReadByte();
				break;

			case 0x17: // Introduction date
				buf.ReadDWord();
				break;

			case 0x1E: // Badge list
				SkipBadgeList(buf);
				break;

			case A0RPI_ROADTYPE_EXTRA_FLAGS:
				buf.Skip(buf.ReadExtendedByte());
				break;

			case A0RPI_ROADTYPE_COLLISION_MODE:
				buf.Skip(buf.ReadExtendedByte());
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

static ChangeInfoResult RoadTypeReserveInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	return RoadTypeReserveInfo(first, last, prop, mapping_entry, buf, RTT_ROAD);
}

static ChangeInfoResult TramTypeReserveInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	return RoadTypeReserveInfo(first, last, prop, mapping_entry, buf, RTT_TRAM);
}

static ChangeInfoResult AirportTilesChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > NUM_AIRPORTTILES_PER_GRF) {
		GrfMsg(1, "AirportTileChangeInfo: Too many airport tiles loaded ({}), max ({}). Ignoring.", last, NUM_AIRPORTTILES_PER_GRF);
		return CIR_INVALID_ID;
	}

	/* Allocate airport tile specs if they haven't been allocated already. */
	if (_cur.grffile->airtspec.size() < last) _cur.grffile->airtspec.resize(last);

	for (uint id = first; id < last; ++id) {
		AirportTileSpec *tsp = _cur.grffile->airtspec[id].get();

		if (prop != 0x08 && tsp == nullptr) {
			GrfMsg(2, "AirportTileChangeInfo: Attempt to modify undefined airport tile {}. Ignoring.", id);
			return CIR_INVALID_ID;
		}

		switch (prop) {
			case 0x08: { // Substitute airport tile type
				uint8_t subs_id = buf.ReadByte();
				if (subs_id >= NEW_AIRPORTTILE_OFFSET) {
					/* The substitute id must be one of the original airport tiles. */
					GrfMsg(2, "AirportTileChangeInfo: Attempt to use new airport tile {} as substitute airport tile for {}. Ignoring.", subs_id, id);
					continue;
				}

				/* Allocate space for this airport tile. */
				if (tsp == nullptr) {
					_cur.grffile->airtspec[id] = std::make_unique<AirportTileSpec>(*AirportTileSpec::Get(subs_id));
					tsp = _cur.grffile->airtspec[id].get();

					tsp->enabled = true;

					tsp->animation.status = ANIM_STATUS_NO_ANIMATION;

					tsp->grf_prop.local_id = id;
					tsp->grf_prop.subst_id = subs_id;
					tsp->grf_prop.grfid = _cur.grffile->grfid;
					tsp->grf_prop.grffile = _cur.grffile;
					_airporttile_mngr.AddEntityID(id, _cur.grffile->grfid, subs_id); // pre-reserve the tile slot
				}
				break;
			}

			case 0x09: { // Airport tile override
				uint8_t override = buf.ReadByte();

				/* The airport tile being overridden must be an original airport tile. */
				if (override >= NEW_AIRPORTTILE_OFFSET) {
					GrfMsg(2, "AirportTileChangeInfo: Attempt to override new airport tile {} with airport tile id {}. Ignoring.", override, id);
					continue;
				}

				_airporttile_mngr.Add(id, _cur.grffile->grfid, override);
				break;
			}

			case 0x0E: // Callback mask
				tsp->callback_mask = static_cast<AirportTileCallbackMasks>(buf.ReadByte());
				break;

			case 0x0F: // Animation information
				tsp->animation.frames = buf.ReadByte();
				tsp->animation.status = buf.ReadByte();
				break;

			case 0x10: // Animation speed
				tsp->animation.speed = buf.ReadByte();
				break;

			case 0x11: // Animation triggers
				tsp->animation.triggers = buf.ReadByte();
				break;

			case 0x12: // Badge list
				tsp->badges = ReadBadgeList(buf, GSF_TRAMTYPES);
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

/**
 * Ignore properties for roadstops
 * @param prop The property to ignore.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult IgnoreRoadStopProperty(uint prop, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	switch (prop) {
		case 0x09:
		case 0x0C:
		case 0x0F:
		case 0x11:
			buf.ReadByte();
			break;

		case 0x0A:
		case 0x0B:
		case 0x0E:
		case 0x10:
		case 0x15:
			buf.ReadWord();
			break;

		case 0x08:
		case 0x0D:
		case 0x12:
			buf.ReadDWord();
			break;

		case 0x16: // Badge list
			SkipBadgeList(buf);
			break;

		default:
			ret = HandleAction0PropertyDefault(buf, prop);
			break;
	}

	return ret;
}

static ChangeInfoResult RoadStopChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last > NUM_ROADSTOPS_PER_GRF) {
		GrfMsg(1, "RoadStopChangeInfo: RoadStop {} is invalid, max {}, ignoring", last, NUM_ROADSTOPS_PER_GRF);
		return CIR_INVALID_ID;
	}

	if (_cur.grffile->roadstops.size() < last) _cur.grffile->roadstops.resize(last);

	for (uint id = first; id < last; ++id) {
		RoadStopSpec *rs = _cur.grffile->roadstops[id].get();

		if (rs == nullptr && prop != 0x08 && prop != A0RPI_ROADSTOP_CLASS_ID) {
			GrfMsg(1, "RoadStopChangeInfo: Attempt to modify undefined road stop {}, ignoring", id);
			ChangeInfoResult cir = IgnoreRoadStopProperty(prop, buf);
			if (cir > ret) ret = cir;
			continue;
		}

		switch (prop) {
			case A0RPI_ROADSTOP_CLASS_ID:
				if (MappedPropertyLengthMismatch(buf, 4, mapping_entry)) break;
				[[fallthrough]];
			case 0x08: { // Road Stop Class ID
				if (rs == nullptr) {
					_cur.grffile->roadstops[id] = std::make_unique<RoadStopSpec>();
					rs = _cur.grffile->roadstops[id].get();
				}

				uint32_t classid = buf.ReadDWord();
				rs->class_index = RoadStopClass::Allocate(std::byteswap(classid));
				break;
			}

			case A0RPI_ROADSTOP_STOP_TYPE:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				[[fallthrough]];
			case 0x09: // Road stop type
				rs->stop_type = (RoadStopAvailabilityType)buf.ReadByte();
				break;

			case A0RPI_ROADSTOP_STOP_NAME:
				if (MappedPropertyLengthMismatch(buf, 2, mapping_entry)) break;
				[[fallthrough]];
			case 0x0A: // Road Stop Name
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &rs->name);
				break;

			case A0RPI_ROADSTOP_CLASS_NAME:
				if (MappedPropertyLengthMismatch(buf, 2, mapping_entry)) break;
				[[fallthrough]];
			case 0x0B: // Road Stop Class name
				AddStringForMapping(GRFStringID{buf.ReadWord()}, rs, [](StringID str, RoadStopSpec *rs) { RoadStopClass::Get(rs->class_index)->name = str; });
				break;

			case A0RPI_ROADSTOP_DRAW_MODE:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				[[fallthrough]];
			case 0x0C: // The draw mode
				rs->draw_mode = static_cast<RoadStopDrawMode>(buf.ReadByte());
				break;

			case A0RPI_ROADSTOP_TRIGGER_CARGOES:
				if (MappedPropertyLengthMismatch(buf, 4, mapping_entry)) break;
				[[fallthrough]];
			case 0x0D: // Cargo types for random triggers
				rs->cargo_triggers = TranslateRefitMask(buf.ReadDWord());
				break;

			case A0RPI_ROADSTOP_ANIMATION_INFO:
				if (MappedPropertyLengthMismatch(buf, 2, mapping_entry)) break;
				[[fallthrough]];
			case 0x0E: // Animation info
				rs->animation.frames = buf.ReadByte();
				rs->animation.status = buf.ReadByte();
				break;

			case A0RPI_ROADSTOP_ANIMATION_SPEED:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				[[fallthrough]];
			case 0x0F: // Animation speed
				rs->animation.speed = buf.ReadByte();
				break;

			case A0RPI_ROADSTOP_ANIMATION_TRIGGERS:
				if (MappedPropertyLengthMismatch(buf, 2, mapping_entry)) break;
				[[fallthrough]];
			case 0x10: // Animation triggers
				rs->animation.triggers = buf.ReadWord();
				break;

			case A0RPI_ROADSTOP_CALLBACK_MASK:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				[[fallthrough]];
			case 0x11: // Callback mask
				rs->callback_mask = static_cast<RoadStopCallbackMasks>(buf.ReadByte());
				break;

			case A0RPI_ROADSTOP_GENERAL_FLAGS:
				if (MappedPropertyLengthMismatch(buf, 4, mapping_entry)) break;
				[[fallthrough]];
			case 0x12: // General flags
				rs->flags = static_cast<RoadStopSpecFlags>(buf.ReadDWord()); // Future-proofing, size this as 4 bytes, but we only need two byte's worth of flags at present
				break;

			case A0RPI_ROADSTOP_MIN_BRIDGE_HEIGHT:
				if (MappedPropertyLengthMismatch(buf, 6, mapping_entry)) break;
				[[fallthrough]];
			case 0x13: // Minimum height for a bridge above
				rs->internal_flags.Set(RoadStopSpecIntlFlag::BridgeHeightsSet);
				for (uint i = 0; i < 6; i++) {
					rs->bridge_height[i] = buf.ReadByte();
				}
				break;

			case A0RPI_ROADSTOP_DISALLOWED_BRIDGE_PILLARS:
				if (MappedPropertyLengthMismatch(buf, 6, mapping_entry)) break;
				[[fallthrough]];
			case 0x14: // Disallowed bridge pillars
				rs->internal_flags.Set(RoadStopSpecIntlFlag::BridgeDisallowedPillarsSet);
				for (uint i = 0; i < 6; i++) {
					rs->bridge_disallowed_pillars[i] = buf.ReadByte();
				}
				break;

			case A0RPI_ROADSTOP_COST_MULTIPLIERS:
				if (MappedPropertyLengthMismatch(buf, 2, mapping_entry)) break;
				[[fallthrough]];
			case 0x15: // Cost multipliers
				rs->build_cost_multiplier = buf.ReadByte();
				rs->clear_cost_multiplier = buf.ReadByte();
				break;

			case 0x16: // Badge list
				rs->badges = ReadBadgeList(buf, GSF_ROADSTOPS);
				break;

			case A0RPI_ROADSTOP_HEIGHT:
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
//				[[fallthrough]];
//			case 0x16: // Height
				rs->height = buf.ReadByte();
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

static ChangeInfoResult BadgeChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = CIR_SUCCESS;

	if (last >= UINT16_MAX) {
		GrfMsg(1, "BadgeChangeInfo: Tag {} is invalid, max {}, ignoring", last, UINT16_MAX - 1);
		return CIR_INVALID_ID;
	}

	for (uint id = first; id < last; ++id) {
		auto it = _cur.grffile->badge_map.find(id);
		if (prop != 0x08 && it == std::end(_cur.grffile->badge_map)) {
			GrfMsg(1, "BadgeChangeInfo: Attempt to modify undefined tag {}, ignoring", id);
			return CIR_INVALID_ID;
		}

		Badge *badge = nullptr;
		if (prop != 0x08) badge = GetBadge(it->second);

		switch (prop) {
			case 0x08: { // Label
				std::string_view label = buf.ReadString();
				_cur.grffile->badge_map[id] = GetOrCreateBadge(label).index;
				break;
			}

			case 0x09: // Flags
				badge->flags = static_cast<BadgeFlags>(buf.ReadDWord());
				break;

			default:
				ret = CIR_UNKNOWN;
				break;
		}
	}

	return ret;
}

/**
 * Define properties for new landscape
 * @param first Local ID of the first landscape.
 * @param last Local ID of the first landscape.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult NewLandscapeChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	/* Properties which are handled per item */
	ChangeInfoResult ret = CIR_SUCCESS;
	for (uint id = first; id < last; ++id) {
		switch (prop) {
			case A0RPI_NEWLANDSCAPE_ENABLE_RECOLOUR: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				bool enabled = (buf.ReadByte() != 0 ? 1 : 0);
				if (id == NLA3ID_CUSTOM_ROCKS) {
					SB(_cur.grffile->new_landscape_ctrl_flags, NLCF_ROCKS_RECOLOUR_ENABLED, 1, enabled);
				}
				break;
			}

			case A0RPI_NEWLANDSCAPE_ENABLE_DRAW_SNOWY_ROCKS: {
				if (MappedPropertyLengthMismatch(buf, 1, mapping_entry)) break;
				bool enabled = (buf.ReadByte() != 0 ? 1 : 0);
				if (id == NLA3ID_CUSTOM_ROCKS) {
					SB(_cur.grffile->new_landscape_ctrl_flags, NLCF_ROCKS_DRAW_SNOWY_ENABLED, 1, enabled);
				}
				break;
			}

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

static bool HandleChangeInfoResult(const char *caller, ChangeInfoResult cir, GrfSpecFeature feature, int property)
{
	switch (cir) {
		default: NOT_REACHED();

		case CIR_DISABLED:
			/* Error has already been printed; just stop parsing */
			return true;

		case CIR_SUCCESS:
			return false;

		case CIR_UNHANDLED:
			GrfMsg(1, "{}: Ignoring property 0x{:02X} of feature {} (not implemented)", caller, property, GetFeatureString(feature));
			return false;

		case CIR_UNKNOWN:
			GrfMsg(0, "{}: Unknown property 0x{:02X} of feature {}, disabling", caller, property, GetFeatureString(feature));
			[[fallthrough]];

		case CIR_INVALID_ID: {
			/* No debug message for an invalid ID, as it has already been output */
			GRFError *error = DisableGrf(cir == CIR_INVALID_ID ? STR_NEWGRF_ERROR_INVALID_ID : STR_NEWGRF_ERROR_UNKNOWN_PROPERTY);
			if (cir != CIR_INVALID_ID) error->param_value[1] = property;
			return true;
		}
	}
}

static GrfSpecFeatureRef ReadFeature(uint8_t raw_byte, bool allow_48 = false)
{
	if (unlikely(HasBit(_cur.grffile->ctrl_flags, GFCF_HAVE_FEATURE_ID_REMAP))) {
		const GRFFeatureMapRemapSet &remap = _cur.grffile->feature_id_remaps;
		if (remap.remapped_ids[raw_byte]) {
			auto iter = remap.mapping.find(raw_byte);
			const GRFFeatureMapRemapEntry &def = iter->second;
			if (def.feature == GSF_ERROR_ON_USE) {
				GrfMsg(0, "Error: Unimplemented mapped feature: {}, mapped to: {:02X}", def.name, raw_byte);
				GRFError *error = DisableGrf(STR_NEWGRF_ERROR_UNIMPLEMETED_MAPPED_FEATURE_ID);
				error->data = stredup(def.name);
				error->param_value[1] = GSF_INVALID;
				error->param_value[2] = raw_byte;
			} else if (def.feature == GSF_INVALID) {
				GrfMsg(2, "Ignoring unimplemented mapped feature: {}, mapped to: {:02X}", def.name, raw_byte);
			}
			return { def.feature, raw_byte };
		}
	}

	GrfSpecFeature feature;
	if (raw_byte >= GSF_REAL_FEATURE_END && !(allow_48 && raw_byte == 0x48)) {
		feature = GSF_INVALID;
	} else {
		feature = static_cast<GrfSpecFeature>(raw_byte);
	}
	return { feature, raw_byte };
}

static const char *_feature_names[] = {
	"TRAINS",
	"ROADVEHICLES",
	"SHIPS",
	"AIRCRAFT",
	"STATIONS",
	"CANALS",
	"BRIDGES",
	"HOUSES",
	"GLOBALVAR",
	"INDUSTRYTILES",
	"INDUSTRIES",
	"CARGOES",
	"SOUNDFX",
	"AIRPORTS",
	"SIGNALS",
	"OBJECTS",
	"RAILTYPES",
	"AIRPORTTILES",
	"ROADTYPES",
	"TRAMTYPES",
	"ROADSTOPS",
	"BADGES",
	"NEWLANDSCAPE",
	"TOWN",
};
static_assert(lengthof(_feature_names) == GSF_END);

void GetFeatureStringFormatter::fmt_format_value(format_target &output) const
{
	if (this->feature.id < GSF_END) {
		output.format("0x{:02X} ({})", this->feature.raw_byte, _feature_names[this->feature.id]);
	} else {
		if (unlikely(HasBit(_cur.grffile->ctrl_flags, GFCF_HAVE_FEATURE_ID_REMAP))) {
			const GRFFeatureMapRemapSet &remap = _cur.grffile->feature_id_remaps;
			if (remap.remapped_ids[this->feature.raw_byte]) {
				auto iter = remap.mapping.find(this->feature.raw_byte);
				const GRFFeatureMapRemapEntry &def = iter->second;
				output.format("0x{:02X} ({})", this->feature.raw_byte, def.name);
				return;
			}
		}
		output.format("0x{:02X}", this->feature.raw_byte);
	}
}

GetFeatureStringFormatter GetFeatureString(GrfSpecFeatureRef feature)
{
	return GetFeatureStringFormatter(feature);
}

GetFeatureStringFormatter GetFeatureString(GrfSpecFeature feature)
{
	uint8_t raw_byte = feature;
	if (feature >= GSF_REAL_FEATURE_END) {
		for (const auto &entry : _cur.grffile->feature_id_remaps.mapping) {
			if (entry.second.feature == feature) {
				raw_byte = entry.second.raw_id;
				break;
			}
		}
	}
	return GetFeatureStringFormatter(GrfSpecFeatureRef{ feature, raw_byte });
}

struct GRFFilePropertyDescriptor {
	int prop;
	const GRFFilePropertyRemapEntry *entry;

	GRFFilePropertyDescriptor(int prop, const GRFFilePropertyRemapEntry *entry)
			: prop(prop), entry(entry) {}
};

static GRFFilePropertyDescriptor ReadAction0PropertyID(ByteReader &buf, uint8_t feature)
{
	uint8_t raw_prop = buf.ReadByte();
	const GRFFilePropertyRemapSet &remap = _cur.grffile->action0_property_remaps[feature];
	if (remap.remapped_ids[raw_prop]) {
		auto iter = remap.mapping.find(raw_prop);
		assert(iter != remap.mapping.end());
		const GRFFilePropertyRemapEntry &def = iter->second;
		int prop = def.id;
		if (prop == A0RPI_UNKNOWN_ERROR) {
			GrfMsg(0, "Error: Unimplemented mapped property: {}, feature: {}, mapped to: {:X}", def.name, GetFeatureString(def.feature), raw_prop);
			GRFError *error = DisableGrf(STR_NEWGRF_ERROR_UNIMPLEMETED_MAPPED_PROPERTY);
			error->data = stredup(def.name);
			error->param_value[1] = def.feature;
			error->param_value[2] = raw_prop;
		} else if (prop == A0RPI_UNKNOWN_IGNORE) {
			GrfMsg(2, "Ignoring unimplemented mapped property: {}, feature: {}, mapped to: {:X}", def.name, GetFeatureString(def.feature), raw_prop);
		} else if (prop == A0RPI_ID_EXTENSION) {
			uint8_t *outer_data = buf.Data();
			size_t outer_length = buf.ReadExtendedByte();
			uint16_t mapped_id = buf.ReadWord();
			uint8_t *inner_data = buf.Data();
			size_t inner_length = buf.ReadExtendedByte();
			if (inner_length + (inner_data - outer_data) != outer_length) {
				GrfMsg(2, "Ignoring extended ID property with malformed lengths: {}, feature: {}, mapped to: {:X}", def.name, GetFeatureString(def.feature), raw_prop);
				buf.ResetReadPosition(outer_data);
				return GRFFilePropertyDescriptor(A0RPI_UNKNOWN_IGNORE, &def);
			}

			auto ext = _cur.grffile->action0_extended_property_remaps.find((((uint32_t)feature) << 16) | mapped_id);
			if (ext != _cur.grffile->action0_extended_property_remaps.end()) {
				buf.ResetReadPosition(inner_data);
				const GRFFilePropertyRemapEntry &ext_def = ext->second;
				prop = ext_def.id;
				if (prop == A0RPI_UNKNOWN_ERROR) {
					GrfMsg(0, "Error: Unimplemented mapped extended ID property: {}, feature: {}, mapped to: {:X} (via {:X})", ext_def.name, GetFeatureString(ext_def.feature), mapped_id, raw_prop);
					GRFError *error = DisableGrf(STR_NEWGRF_ERROR_UNIMPLEMETED_MAPPED_PROPERTY);
					error->data = stredup(ext_def.name);
					error->param_value[1] = ext_def.feature;
					error->param_value[2] = 0xE0000 | mapped_id;
				} else if (prop == A0RPI_UNKNOWN_IGNORE) {
					GrfMsg(2, "Ignoring unimplemented mapped extended ID property: {}, feature: {}, mapped to: {:X} (via {:X})", ext_def.name, GetFeatureString(ext_def.feature), mapped_id, raw_prop);
				}
				return GRFFilePropertyDescriptor(prop, &ext_def);
			} else {
				GrfMsg(2, "Ignoring unknown extended ID property: {}, feature: {}, mapped to: {:X} (via {:X})", def.name, GetFeatureString(def.feature), mapped_id, raw_prop);
				buf.ResetReadPosition(outer_data);
				return GRFFilePropertyDescriptor(A0RPI_UNKNOWN_IGNORE, &def);
			}
		}
		return GRFFilePropertyDescriptor(prop, &def);
	} else {
		return GRFFilePropertyDescriptor(raw_prop, nullptr);
	}
}

/* Action 0x00 */
static void FeatureChangeInfo(ByteReader &buf)
{
	/* <00> <feature> <num-props> <num-info> <id> (<property <new-info>)...
	 *
	 * B feature
	 * B num-props     how many properties to change per vehicle/station
	 * B num-info      how many vehicles/stations to change
	 * E id            ID of first vehicle/station to change, if num-info is
	 *                 greater than one, this one and the following
	 *                 vehicles/stations will be changed
	 * B property      what property to change, depends on the feature
	 * V new-info      new bytes of info (variable size; depends on properties) */

	static ChangeInfoHandler * const handler[] = {
		/* GSF_TRAINS */        RailVehicleChangeInfo,
		/* GSF_ROADVEHICLES */  RoadVehicleChangeInfo,
		/* GSF_SHIPS */         ShipVehicleChangeInfo,
		/* GSF_AIRCRAFT */      AircraftVehicleChangeInfo,
		/* GSF_STATIONS */      StationChangeInfo,
		/* GSF_CANALS */        CanalChangeInfo,
		/* GSF_BRIDGES */       BridgeChangeInfo,
		/* GSF_HOUSES */        TownHouseChangeInfo,
		/* GSF_GLOBALVAR */     GlobalVarChangeInfo,
		/* GSF_INDUSTRYTILES */ IndustrytilesChangeInfo,
		/* GSF_INDUSTRIES */    IndustriesChangeInfo,
		/* GSF_CARGOES */       nullptr, // Cargo is handled during reservation
		/* GSF_SOUNDFX */       SoundEffectChangeInfo,
		/* GSF_AIRPORTS */      AirportChangeInfo,
		/* GSF_SIGNALS */       SignalsChangeInfo,
		/* GSF_OBJECTS */       ObjectChangeInfo,
		/* GSF_RAILTYPES */     RailTypeChangeInfo,
		/* GSF_AIRPORTTILES */  AirportTilesChangeInfo,
		/* GSF_ROADTYPES */     RoadTypeChangeInfo,
		/* GSF_TRAMTYPES */     TramTypeChangeInfo,
		/* GSF_ROADSTOPS */     RoadStopChangeInfo,
		/* GSF_BADGES */        BadgeChangeInfo,
		/* GSF_NEWLANDSCAPE */  NewLandscapeChangeInfo,
		/* GSF_FAKE_TOWNS */    nullptr,
	};
	static_assert(GSF_END == std::size(handler));
	static_assert(lengthof(handler) == lengthof(_cur.grffile->action0_property_remaps), "Action 0 feature list length mismatch");

	GrfSpecFeatureRef feature_ref = ReadFeature(buf.ReadByte());
	GrfSpecFeature feature = feature_ref.id;
	uint8_t numprops = buf.ReadByte();
	uint numinfo  = buf.ReadByte();
	uint engine   = buf.ReadExtendedByte();

	if (feature >= GSF_END) {
		GrfMsg(1, "FeatureChangeInfo: Unsupported feature {} skipping", GetFeatureString(feature_ref));
		return;
	}

	GrfMsg(6, "FeatureChangeInfo: Feature {}, {} properties, to apply to {}+{}",
	               GetFeatureString(feature_ref), numprops, engine, numinfo);

	if (handler[feature] == nullptr) {
		if (feature != GSF_CARGOES) GrfMsg(1, "FeatureChangeInfo: Unsupported feature {}, skipping", GetFeatureString(feature_ref));
		return;
	}

	/* Mark the feature as used by the grf */
	SetBit(_cur.grffile->grf_features, feature);

	while (numprops-- && buf.HasData()) {
		GRFFilePropertyDescriptor desc = ReadAction0PropertyID(buf, feature);

		ChangeInfoResult cir = handler[feature](engine, engine + numinfo, desc.prop, desc.entry, buf);
		if (HandleChangeInfoResult("FeatureChangeInfo", cir, feature, desc.prop)) return;
	}
}

/* Action 0x00 (GLS_SAFETYSCAN) */
static void SafeChangeInfo(ByteReader &buf)
{
	GrfSpecFeatureRef feature = ReadFeature(buf.ReadByte());
	uint8_t numprops = buf.ReadByte();
	uint numinfo = buf.ReadByte();
	buf.ReadExtendedByte(); // id

	if (feature.id == GSF_BRIDGES && numprops == 1) {
		GRFFilePropertyDescriptor desc = ReadAction0PropertyID(buf, feature.id);
		/* Bridge property 0x0D is redefinition of sprite layout tables, which
		 * is considered safe. */
		if (desc.prop == 0x0D) return;
	} else if (feature.id == GSF_GLOBALVAR && numprops == 1) {
		GRFFilePropertyDescriptor desc = ReadAction0PropertyID(buf, feature.id);
		/* Engine ID Mappings are safe, if the source is static */
		if (desc.prop == 0x11) {
			bool is_safe = true;
			for (uint i = 0; i < numinfo; i++) {
				uint32_t s = buf.ReadDWord();
				buf.ReadDWord(); // dest
				const GRFConfig *grfconfig = GetGRFConfig(s);
				if (grfconfig != nullptr && !grfconfig->flags.Test(GRFConfigFlag::Static)) {
					is_safe = false;
					break;
				}
			}
			if (is_safe) return;
		}
	}

	_cur.grfconfig->flags.Set(GRFConfigFlag::Unsafe);

	/* Skip remainder of GRF */
	_cur.skip_sprites = -1;
}

/* Action 0x00 (GLS_RESERVE) */
static void ReserveChangeInfo(ByteReader &buf)
{
	GrfSpecFeatureRef feature_ref = ReadFeature(buf.ReadByte());
	GrfSpecFeature feature = feature_ref.id;

	if (feature != GSF_CARGOES && feature != GSF_GLOBALVAR && feature != GSF_RAILTYPES && feature != GSF_ROADTYPES && feature != GSF_TRAMTYPES) return;

	uint8_t numprops = buf.ReadByte();
	uint8_t numinfo = buf.ReadByte();
	uint16_t index = buf.ReadExtendedByte();

	while (numprops-- && buf.HasData()) {
		GRFFilePropertyDescriptor desc = ReadAction0PropertyID(buf, feature);
		ChangeInfoResult cir = CIR_SUCCESS;

		switch (feature) {
			default: NOT_REACHED();
			case GSF_CARGOES:
				cir = CargoChangeInfo(index, index + numinfo, desc.prop, desc.entry, buf);
				break;

			case GSF_GLOBALVAR:
				cir = GlobalVarReserveInfo(index, index + numinfo, desc.prop, desc.entry, buf);
				break;

			case GSF_RAILTYPES:
				cir = RailTypeReserveInfo(index, index + numinfo, desc.prop, desc.entry, buf);
				break;

			case GSF_ROADTYPES:
				cir = RoadTypeReserveInfo(index, index + numinfo, desc.prop, desc.entry, buf);
				break;

			case GSF_TRAMTYPES:
				cir = TramTypeReserveInfo(index, index + numinfo, desc.prop, desc.entry, buf);
				break;
		}

		if (HandleChangeInfoResult("ReserveChangeInfo", cir, feature, desc.prop)) return;
	}
}

/* Action 0x01 */
static void NewSpriteSet(ByteReader &buf)
{
	/* Basic format:    <01> <feature> <num-sets> <num-ent>
	 * Extended format: <01> <feature> 00 <first-set> <num-sets> <num-ent>
	 *
	 * B feature       feature to define sprites for
	 *                 0, 1, 2, 3: veh-type, 4: train stations
	 * E first-set     first sprite set to define
	 * B num-sets      number of sprite sets (extended byte in extended format)
	 * E num-ent       how many entries per sprite set
	 *                 For vehicles, this is the number of different
	 *                         vehicle directions in each sprite set
	 *                         Set num-dirs=8, unless your sprites are symmetric.
	 *                         In that case, use num-dirs=4.
	 */

	GrfSpecFeatureRef feature_ref = ReadFeature(buf.ReadByte());
	GrfSpecFeature feature = feature_ref.id;
	uint16_t num_sets  = buf.ReadByte();
	uint16_t first_set = 0;

	if (num_sets == 0 && buf.HasData(3)) {
		/* Extended Action1 format.
		 * Some GRFs define zero sets of zero sprites, though there is actually no use in that. Ignore them. */
		first_set = buf.ReadExtendedByte();
		num_sets = buf.ReadExtendedByte();
	}
	uint16_t num_ents = buf.ReadExtendedByte();

	if (feature >= GSF_END) {
		_cur.skip_sprites = num_sets * num_ents;
		GrfMsg(1, "NewSpriteSet: Unsupported feature {}, skipping {} sprites", GetFeatureString(feature_ref), _cur.skip_sprites);
		return;
	}

	_cur.AddSpriteSets(feature, _cur.spriteid, first_set, num_sets, num_ents);

	GrfMsg(7, "New sprite set at {} of feature {}, consisting of {} sets with {} views each (total {})",
		_cur.spriteid, GetFeatureString(feature), num_sets, num_ents, num_sets * num_ents
	);

	for (int i = 0; i < num_sets * num_ents; i++) {
		_cur.nfo_line++;
		LoadNextSprite(_cur.spriteid++, *_cur.file, _cur.nfo_line);
	}
}

/* Action 0x01 (SKIP) */
static void SkipAct1(ByteReader &buf)
{
	buf.ReadByte();
	uint16_t num_sets  = buf.ReadByte();

	if (num_sets == 0 && buf.HasData(3)) {
		/* Extended Action1 format.
		 * Some GRFs define zero sets of zero sprites, though there is actually no use in that. Ignore them. */
		buf.ReadExtendedByte(); // first_set
		num_sets = buf.ReadExtendedByte();
	}
	uint16_t num_ents = buf.ReadExtendedByte();

	_cur.skip_sprites = num_sets * num_ents;

	GrfMsg(3, "SkipAct1: Skipping {} sprites", _cur.skip_sprites);
}

const CallbackResultSpriteGroup *NewCallbackResultSpriteGroupNoTransform(uint16_t result)
{
	const CallbackResultSpriteGroup *&ptr = _callback_result_cache[result];
	if (ptr == nullptr) {
		assert(CallbackResultSpriteGroup::CanAllocateItem());
		ptr = new CallbackResultSpriteGroup(result);
	}
	return ptr;
}

static const CallbackResultSpriteGroup *NewCallbackResultSpriteGroup(uint16_t groupid)
{
	uint16_t result = CallbackResultSpriteGroup::TransformResultValue(groupid, _cur.grffile->grf_version >= 8);
	return NewCallbackResultSpriteGroupNoTransform(result);
}

static const SpriteGroup *GetGroupFromGroupIDNoCBResult(uint16_t setid, uint8_t type, uint16_t groupid)
{
	if (groupid == GROUPID_CALLBACK_FAILED) return nullptr;

	if ((size_t)groupid >= _cur.spritegroups.size() || _cur.spritegroups[groupid] == nullptr) {
		GrfMsg(1, "GetGroupFromGroupID(0x{:02X}:0x{:02X}): Groupid 0x{:04X} does not exist, leaving empty", setid, type, groupid);
		return nullptr;
	}

	const SpriteGroup *result = _cur.spritegroups[groupid];
	if (likely(!HasBit(_misc_debug_flags, MDF_NEWGRF_SG_SAVE_RAW))) result = PruneTargetSpriteGroup(result);
	return result;
}

/* Helper function to either create a callback or link to a previously
 * defined spritegroup. */
static const SpriteGroup *GetGroupFromGroupID(uint16_t setid, uint8_t type, uint16_t groupid)
{
	if (HasBit(groupid, 15)) {
		return NewCallbackResultSpriteGroup(groupid);
	}

	return GetGroupFromGroupIDNoCBResult(setid, type, groupid);
}

static const SpriteGroup *GetGroupByID(uint16_t groupid)
{
	if ((size_t)groupid >= _cur.spritegroups.size()) return nullptr;

	const SpriteGroup *result = _cur.spritegroups[groupid];
	return result;
}

/**
 * Helper function to either create a callback or a result sprite group.
 * @param feature GrfSpecFeature to define spritegroup for.
 * @param setid SetID of the currently being parsed Action2. (only for debug output)
 * @param type Type of the currently being parsed Action2. (only for debug output)
 * @param spriteid Raw value from the GRF for the new spritegroup; describes either the return value or the referenced spritegroup.
 * @return Created spritegroup.
 */
static const SpriteGroup *CreateGroupFromGroupID(uint8_t feature, uint16_t setid, uint8_t type, uint16_t spriteid)
{
	if (HasBit(spriteid, 15)) {
		return NewCallbackResultSpriteGroup(spriteid);
	}

	const SpriteSetInfo sprite_set_info = _cur.GetSpriteSetInfo(feature, spriteid);

	if (!sprite_set_info.IsValid()) {
		GrfMsg(1, "CreateGroupFromGroupID(0x{:02X}:0x{:02X}): Sprite set {} invalid", setid, type, spriteid);
		return nullptr;
	}

	SpriteID spriteset_start = sprite_set_info.GetSprite();
	uint num_sprites = sprite_set_info.GetNumEnts();

	/* Ensure that the sprites are loeded */
	assert(spriteset_start + num_sprites <= _cur.spriteid);

	assert(ResultSpriteGroup::CanAllocateItem());
	return new ResultSpriteGroup(spriteset_start, num_sprites);
}

static void ProcessDeterministicSpriteGroupRanges(const std::vector<DeterministicSpriteGroupRange> &ranges, std::vector<DeterministicSpriteGroupRange> &ranges_out, const SpriteGroup *default_group)
{
	/* Sort ranges ascending. When ranges overlap, this may required clamping or splitting them */
	std::vector<uint32_t> bounds;
	bounds.reserve(ranges.size());
	for (uint i = 0; i < ranges.size(); i++) {
		bounds.push_back(ranges[i].low);
		if (ranges[i].high != UINT32_MAX) bounds.push_back(ranges[i].high + 1);
	}
	std::sort(bounds.begin(), bounds.end());
	bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

	std::vector<const SpriteGroup *> target;
	target.reserve(bounds.size());
	for (uint j = 0; j < bounds.size(); ++j) {
		uint32_t v = bounds[j];
		const SpriteGroup *t = default_group;
		for (uint i = 0; i < ranges.size(); i++) {
			if (ranges[i].low <= v && v <= ranges[i].high) {
				t = ranges[i].group;
				break;
			}
		}
		target.push_back(t);
	}
	assert(target.size() == bounds.size());

	for (uint j = 0; j < bounds.size(); ) {
		if (target[j] != default_group) {
			DeterministicSpriteGroupRange &r = ranges_out.emplace_back();
			r.group = target[j];
			r.low = bounds[j];
			while (j < bounds.size() && target[j] == r.group) {
				j++;
			}
			r.high = j < bounds.size() ? bounds[j] - 1 : UINT32_MAX;
		} else {
			j++;
		}
	}
}

static VarSpriteGroupScopeOffset ParseRelativeScopeByte(uint8_t relative)
{
	VarSpriteGroupScopeOffset var_scope_count = (GB(relative, 6, 2) << 8);
	if ((relative & 0xF) == 0) {
		SetBit(var_scope_count, 15);
	} else {
		var_scope_count |= (relative & 0xF);
	}
	return var_scope_count;
}

/* Action 0x02 */
static void NewSpriteGroup(ByteReader &buf)
{
	/* <02> <feature> <set-id> <type/num-entries> <feature-specific-data...>
	 *
	 * B feature       see action 1
	 * B set-id        ID of this particular definition
	 *                 This is an extended byte if feature "more_action2_ids" is tested for
	 * B type/num-entries
	 *                 if 80 or greater, this is a randomized or variational
	 *                 list definition, see below
	 *                 otherwise it specifies a number of entries, the exact
	 *                 meaning depends on the feature
	 * V feature-specific-data (huge mess, don't even look it up --pasky) */
	const SpriteGroup *act_group = nullptr;

	GrfSpecFeatureRef feature_ref = ReadFeature(buf.ReadByte());
	GrfSpecFeature feature = feature_ref.id;
	if (feature >= GSF_END) {
		GrfMsg(1, "NewSpriteGroup: Unsupported feature {}, skipping", GetFeatureString(feature_ref));
		return;
	}

	uint16_t setid  = HasBit(_cur.grffile->observed_feature_tests, GFTOF_MORE_ACTION2_IDS) ? buf.ReadExtendedByte() : buf.ReadByte();
	uint8_t type    = buf.ReadByte();

	/* Sprite Groups are created here but they are allocated from a pool, so
	 * we do not need to delete anything if there is an exception from the
	 * ByteReader. */

	/* Decoded sprite type */
	enum SpriteType {
		STYPE_NORMAL,
		STYPE_DETERMINISTIC,
		STYPE_DETERMINISTIC_RELATIVE,
		STYPE_DETERMINISTIC_RELATIVE_2,
		STYPE_RANDOMIZED,
		STYPE_CB_FAILURE,
	};
	SpriteType stype = STYPE_NORMAL;
	switch (type) {
		/* Deterministic Sprite Group */
		case 0x81: // Self scope, byte
		case 0x82: // Parent scope, byte
		case 0x85: // Self scope, word
		case 0x86: // Parent scope, word
		case 0x89: // Self scope, dword
		case 0x8A: // Parent scope, dword
			stype = STYPE_DETERMINISTIC;
			break;

		/* Randomized Sprite Group */
		case 0x80: // Self scope
		case 0x83: // Parent scope
		case 0x84: // Relative scope
			stype = STYPE_RANDOMIZED;
			break;

		/* Extension type */
		case 0x87:
			if (HasBit(_cur.grffile->observed_feature_tests, GFTOF_MORE_VARACTION2_TYPES)) {
				uint8_t subtype = buf.ReadByte();
				switch (subtype) {
					case 0:
						stype = STYPE_CB_FAILURE;
						break;

					case 1:
						stype = STYPE_DETERMINISTIC_RELATIVE;
						break;

					case 2:
						stype = STYPE_DETERMINISTIC_RELATIVE_2;
						break;

					default:
						GrfMsg(1, "NewSpriteGroup: Unknown 0x87 extension subtype {:02X} for feature {}, handling as CB failure", subtype, GetFeatureString(feature));
						stype = STYPE_CB_FAILURE;
						break;
				}
			}
			break;

		default:
			break;
	}

	switch (stype) {
		/* Deterministic Sprite Group */
		case STYPE_DETERMINISTIC:
		case STYPE_DETERMINISTIC_RELATIVE:
		case STYPE_DETERMINISTIC_RELATIVE_2:
		{
			VarSpriteGroupScopeOffset var_scope_count = 0;
			if (stype == STYPE_DETERMINISTIC_RELATIVE) {
				var_scope_count = ParseRelativeScopeByte(buf.ReadByte());
			} else if (stype == STYPE_DETERMINISTIC_RELATIVE_2) {
				uint8_t mode = buf.ReadByte();
				uint8_t offset = buf.ReadByte();
				bool invalid = false;
				if ((mode & 0x7F) >= VSGSRM_END) {
					invalid = true;
				}
				if (HasBit(mode, 7)) {
					/* Use variable 0x100 */
					if (offset != 0) invalid = true;
				}
				if (invalid) {
					GrfMsg(1, "NewSpriteGroup: Unknown 0x87 extension subtype 2 relative mode: {:02X} {:02X} for feature {}, handling as CB failure", mode, offset, GetFeatureString(feature));
					act_group = NewCallbackResultSpriteGroupNoTransform(CALLBACK_FAILED);
					break;
				}
				var_scope_count = (mode << 8) | offset;
			}

			uint8_t varadjust;
			uint8_t varsize;

			bool first_adjust = true;

			assert(DeterministicSpriteGroup::CanAllocateItem());
			DeterministicSpriteGroup *group = new DeterministicSpriteGroup();
			group->nfo_line = _cur.nfo_line;
			group->feature = feature;
			if (_action6_override_active) group->sg_flags |= SGF_ACTION6;
			act_group = group;

			if (stype == STYPE_DETERMINISTIC_RELATIVE || stype == STYPE_DETERMINISTIC_RELATIVE_2) {
				group->var_scope = (feature <= GSF_AIRCRAFT) ? VSG_SCOPE_RELATIVE : VSG_SCOPE_SELF;
				group->var_scope_count = var_scope_count;

				group->size = DSG_SIZE_DWORD;
				varsize = 4;
			} else {
				group->var_scope = HasBit(type, 1) ? VSG_SCOPE_PARENT : VSG_SCOPE_SELF;

				switch (GB(type, 2, 2)) {
					default: NOT_REACHED();
					case 0: group->size = DSG_SIZE_BYTE;  varsize = 1; break;
					case 1: group->size = DSG_SIZE_WORD;  varsize = 2; break;
					case 2: group->size = DSG_SIZE_DWORD; varsize = 4; break;
				}
			}

			const VarAction2AdjustInfo info = { feature, GetGrfSpecFeatureForScope(feature, group->var_scope), varsize };

			DeterministicSpriteGroupShadowCopy *shadow = nullptr;
			if (unlikely(HasBit(_misc_debug_flags, MDF_NEWGRF_SG_SAVE_RAW))) {
				shadow = &(_deterministic_sg_shadows[group]);
			}
			static std::vector<DeterministicSpriteGroupAdjust> current_adjusts;
			current_adjusts.clear();

			VarAction2OptimiseState va2_opt_state;
			/* The initial value is always the constant 0 */
			va2_opt_state.inference = VA2AIF_SIGNED_NON_NEGATIVE | VA2AIF_ONE_OR_ZERO | VA2AIF_HAVE_CONSTANT;
			va2_opt_state.current_constant = 0;

			/* Loop through the var adjusts. Unfortunately we don't know how many we have
			 * from the outset, so we shall have to keep reallocing. */
			do {
				DeterministicSpriteGroupAdjust &adjust = current_adjusts.emplace_back();

				/* The first var adjust doesn't have an operation specified, so we set it to add. */
				adjust.operation = first_adjust ? DSGA_OP_ADD : (DeterministicSpriteGroupAdjustOperation)buf.ReadByte();
				first_adjust = false;
				if (adjust.operation > DSGA_OP_END) adjust.operation = DSGA_OP_END;
				adjust.variable  = buf.ReadByte();
				if (adjust.variable == 0x7E) {
					/* Link subroutine group */
					adjust.subroutine = GetGroupFromGroupIDNoCBResult(setid, type, HasBit(_cur.grffile->observed_feature_tests, GFTOF_MORE_ACTION2_IDS) ? buf.ReadExtendedByte() : buf.ReadByte());
				} else {
					adjust.parameter = IsInsideMM(adjust.variable, 0x60, 0x80) ? buf.ReadByte() : 0;
				}

				varadjust = buf.ReadByte();
				adjust.shift_num = GB(varadjust, 0, 5);
				adjust.type      = (DeterministicSpriteGroupAdjustType)GB(varadjust, 6, 2);
				adjust.and_mask  = buf.ReadVarSize(varsize);

				if (adjust.variable == 0x11) {
					for (const GRFVariableMapEntry &remap : _cur.grffile->grf_variable_remaps) {
						if (remap.feature == info.scope_feature && remap.input_shift == adjust.shift_num && remap.input_mask == adjust.and_mask) {
							adjust.variable = remap.id;
							adjust.shift_num = remap.output_shift;
							adjust.and_mask = remap.output_mask;
							adjust.parameter = remap.output_param;
							break;
						}
					}
				} else if (adjust.variable == 0x7B && adjust.parameter == 0x11) {
					for (const GRFVariableMapEntry &remap : _cur.grffile->grf_variable_remaps) {
						if (remap.feature == info.scope_feature && remap.input_shift == adjust.shift_num && remap.input_mask == adjust.and_mask) {
							adjust.parameter = remap.id;
							adjust.shift_num = remap.output_shift;
							adjust.and_mask = remap.output_mask;
							break;
						}
					}
				}

				if (info.scope_feature == GSF_ROADSTOPS && HasBit(_cur.grffile->observed_feature_tests, GFTOF_ROAD_STOPS)) {
					if (adjust.variable == 0x68) adjust.variable = A2VRI_ROADSTOP_INFO_NEARBY_TILES_EXT;
					if (adjust.variable == 0x7B && adjust.parameter == 0x68) adjust.parameter = A2VRI_ROADSTOP_INFO_NEARBY_TILES_EXT;
				}

				if (adjust.type != DSGA_TYPE_NONE) {
					adjust.add_val    = buf.ReadVarSize(varsize);
					adjust.divmod_val = buf.ReadVarSize(varsize);
					if (adjust.divmod_val == 0) adjust.divmod_val = 1; // Ensure that divide by zero cannot occur
				} else {
					adjust.add_val    = 0;
					adjust.divmod_val = 0;
				}
				if (unlikely(shadow != nullptr)) {
					shadow->adjusts.push_back(adjust);
					/* Pruning was turned off so that the unpruned target could be saved in the shadow, prune now */
					if (adjust.subroutine != nullptr) adjust.subroutine = PruneTargetSpriteGroup(adjust.subroutine);
				}

				OptimiseVarAction2PreCheckAdjust(va2_opt_state, adjust);

				/* Continue reading var adjusts while bit 5 is set. */
			} while (HasBit(varadjust, 5));

			/* shrink_to_fit will be called later */
			group->adjusts.reserve(current_adjusts.size());

			for (const DeterministicSpriteGroupAdjust &adjust : current_adjusts) {
				group->adjusts.push_back(adjust);
				OptimiseVarAction2Adjust(va2_opt_state, info, group, group->adjusts.back());
			}

			std::vector<DeterministicSpriteGroupRange> ranges;
			ranges.resize(buf.ReadByte());
			for (auto &range : ranges) {
				range.group = GetGroupFromGroupID(setid, type, buf.ReadWord());
				range.low   = buf.ReadVarSize(varsize);
				range.high  = buf.ReadVarSize(varsize);
			}

			group->default_group = GetGroupFromGroupID(setid, type, buf.ReadWord());

			if (unlikely(shadow != nullptr)) {
				shadow->calculated_result = ranges.size() == 0;
				ProcessDeterministicSpriteGroupRanges(ranges, shadow->ranges, group->default_group);
				shadow->default_group = group->default_group;

				/* Pruning was turned off so that the unpruned targets could be saved in the shadow ranges, prune now */
				for (DeterministicSpriteGroupRange &range : ranges) {
					range.group = PruneTargetSpriteGroup(range.group);
				}
				group->default_group = PruneTargetSpriteGroup(group->default_group);
			}

			group->error_group = ranges.empty() ? group->default_group : ranges[0].group;
			/* nvar == 0 is a special case -- we turn our value into a callback result */
			if (ranges.empty()) group->dsg_flags |= DSGF_CALCULATED_RESULT;

			ProcessDeterministicSpriteGroupRanges(ranges, group->ranges, group->default_group);

			OptimiseVarAction2DeterministicSpriteGroup(va2_opt_state, info, group, current_adjusts);
			current_adjusts.clear();
			break;
		}

		/* Randomized Sprite Group */
		case STYPE_RANDOMIZED:
		{
			assert(RandomizedSpriteGroup::CanAllocateItem());
			RandomizedSpriteGroup *group = new RandomizedSpriteGroup();
			group->nfo_line = _cur.nfo_line;
			if (_action6_override_active) group->sg_flags |= SGF_ACTION6;
			act_group = group;
			group->var_scope = HasBit(type, 1) ? VSG_SCOPE_PARENT : VSG_SCOPE_SELF;

			if (HasBit(type, 2)) {
				if (feature <= GSF_AIRCRAFT) group->var_scope = VSG_SCOPE_RELATIVE;
				group->var_scope_count = ParseRelativeScopeByte(buf.ReadByte());
			}

			uint8_t triggers = buf.ReadByte();
			group->triggers       = GB(triggers, 0, 7);
			group->cmp_mode       = HasBit(triggers, 7) ? RSG_CMP_ALL : RSG_CMP_ANY;
			group->lowest_randbit = buf.ReadByte();

			uint8_t num_groups = buf.ReadByte();
			if (!HasExactlyOneBit(num_groups)) {
				GrfMsg(1, "NewSpriteGroup: Random Action 2 nrand should be power of 2");
			}

			group->groups.reserve(num_groups);
			for (uint i = 0; i < num_groups; i++) {
				group->groups.push_back(GetGroupFromGroupID(setid, type, buf.ReadWord()));
			}

			if (unlikely(HasBit(_misc_debug_flags, MDF_NEWGRF_SG_SAVE_RAW))) {
				RandomizedSpriteGroupShadowCopy *shadow = &(_randomized_sg_shadows[group]);
				shadow->groups = group->groups;

				/* Pruning was turned off so that the unpruned targets could be saved in the shadow groups, prune now */
				for (const SpriteGroup *&group : group->groups) {
					group = PruneTargetSpriteGroup(group);
				}
			}

			break;
		}

		case STYPE_CB_FAILURE:
			act_group = NewCallbackResultSpriteGroupNoTransform(CALLBACK_FAILED);
			break;

		/* Neither a variable or randomized sprite group... must be a real group */
		case STYPE_NORMAL:
		{
			switch (feature) {
				case GSF_TRAINS:
				case GSF_ROADVEHICLES:
				case GSF_SHIPS:
				case GSF_AIRCRAFT:
				case GSF_STATIONS:
				case GSF_CANALS:
				case GSF_CARGOES:
				case GSF_AIRPORTS:
				case GSF_RAILTYPES:
				case GSF_ROADTYPES:
				case GSF_TRAMTYPES:
				case GSF_BADGES:
				case GSF_SIGNALS:
				case GSF_NEWLANDSCAPE:
				{
					uint8_t num_loaded  = type;
					uint8_t num_loading = buf.ReadByte();

					if (!_cur.HasValidSpriteSets(feature)) {
						GrfMsg(0, "NewSpriteGroup: No sprite set to work on! Skipping");
						return;
					}

					if (num_loaded + num_loading == 0) {
						GrfMsg(1, "NewSpriteGroup: no result, skipping invalid RealSpriteGroup");
						break;
					}

					GrfMsg(6, "NewSpriteGroup: New SpriteGroup 0x{:02X}, {} loaded, {} loading",
							setid, num_loaded, num_loading);

					if (num_loaded + num_loading == 0) {
						GrfMsg(1, "NewSpriteGroup: no result, skipping invalid RealSpriteGroup");
						break;
					}

					if (num_loaded + num_loading == 1) {
						/* Avoid creating 'Real' sprite group if only one option. */
						uint16_t spriteid = buf.ReadWord();
						act_group = CreateGroupFromGroupID(feature, setid, type, spriteid);
						GrfMsg(8, "NewSpriteGroup: one result, skipping RealSpriteGroup = subset {}", spriteid);
						break;
					}

					std::vector<uint16_t> loaded;
					std::vector<uint16_t> loading;

					loaded.reserve(num_loaded);
					for (uint i = 0; i < num_loaded; i++) {
						loaded.push_back(buf.ReadWord());
						GrfMsg(8, "NewSpriteGroup: + rg->loaded[{}]  = subset {}", i, loaded[i]);
					}

					loading.reserve(num_loading);
					for (uint i = 0; i < num_loading; i++) {
						loading.push_back(buf.ReadWord());
						GrfMsg(8, "NewSpriteGroup: + rg->loading[{}] = subset {}", i, loading[i]);
					}

					bool loaded_same = !loaded.empty() && std::adjacent_find(loaded.begin(),  loaded.end(),  std::not_equal_to<>()) == loaded.end();
					bool loading_same = !loading.empty() && std::adjacent_find(loading.begin(), loading.end(), std::not_equal_to<>()) == loading.end();
					if (loaded_same && loading_same && loaded[0] == loading[0]) {
						/* Both lists only contain the same value, so don't create 'Real' sprite group */
						act_group = CreateGroupFromGroupID(feature, setid, type, loaded[0]);
						GrfMsg(8, "NewSpriteGroup: same result, skipping RealSpriteGroup = subset {}", loaded[0]);
						break;
					}

					assert(RealSpriteGroup::CanAllocateItem());
					RealSpriteGroup *group = new RealSpriteGroup();
					group->nfo_line = _cur.nfo_line;
					if (_action6_override_active) group->sg_flags |= SGF_ACTION6;
					act_group = group;

					if (loaded_same && loaded.size() > 1) loaded.resize(1);
					group->loaded.reserve(loaded.size());
					for (uint16_t spriteid : loaded) {
						const SpriteGroup *t = CreateGroupFromGroupID(feature, setid, type, spriteid);
						group->loaded.push_back(t);
					}

					if (loading_same && loading.size() > 1) loading.resize(1);
					group->loading.reserve(loading.size());
					for (uint16_t spriteid : loading) {
						const SpriteGroup *t = CreateGroupFromGroupID(feature, setid, type, spriteid);
						group->loading.push_back(t);
					}

					break;
				}

				case GSF_HOUSES:
				case GSF_AIRPORTTILES:
				case GSF_OBJECTS:
				case GSF_INDUSTRYTILES:
				case GSF_ROADSTOPS: {
					uint8_t num_building_sprites = std::max((uint8_t)1, type);

					assert(TileLayoutSpriteGroup::CanAllocateItem());
					TileLayoutSpriteGroup *group = new TileLayoutSpriteGroup();
					group->nfo_line = _cur.nfo_line;
					if (_action6_override_active) group->sg_flags |= SGF_ACTION6;
					act_group = group;

					/* On error, bail out immediately. Temporary GRF data was already freed */
					if (ReadSpriteLayout(buf, num_building_sprites, true, feature, false, type == 0, &group->dts)) return;
					break;
				}

				case GSF_INDUSTRIES: {
					if (type > 2) {
						GrfMsg(1, "NewSpriteGroup: Unsupported industry production version {}, skipping", type);
						break;
					}

					assert(IndustryProductionSpriteGroup::CanAllocateItem());
					IndustryProductionSpriteGroup *group = new IndustryProductionSpriteGroup();
					group->nfo_line = _cur.nfo_line;
					if (_action6_override_active) group->sg_flags |= SGF_ACTION6;
					act_group = group;
					group->version = type;
					if (type == 0) {
						group->num_input = INDUSTRY_ORIGINAL_NUM_INPUTS;
						for (uint i = 0; i < INDUSTRY_ORIGINAL_NUM_INPUTS; i++) {
							group->subtract_input[i] = (int16_t)buf.ReadWord(); // signed
						}
						group->num_output = INDUSTRY_ORIGINAL_NUM_OUTPUTS;
						for (uint i = 0; i < INDUSTRY_ORIGINAL_NUM_OUTPUTS; i++) {
							group->add_output[i] = buf.ReadWord(); // unsigned
						}
						group->again = buf.ReadByte();
					} else if (type == 1) {
						group->num_input = INDUSTRY_ORIGINAL_NUM_INPUTS;
						for (uint i = 0; i < INDUSTRY_ORIGINAL_NUM_INPUTS; i++) {
							group->subtract_input[i] = buf.ReadByte();
						}
						group->num_output = INDUSTRY_ORIGINAL_NUM_OUTPUTS;
						for (uint i = 0; i < INDUSTRY_ORIGINAL_NUM_OUTPUTS; i++) {
							group->add_output[i] = buf.ReadByte();
						}
						group->again = buf.ReadByte();
					} else if (type == 2) {
						group->num_input = buf.ReadByte();
						if (group->num_input > lengthof(group->subtract_input)) {
							GRFError *error = DisableGrf(STR_NEWGRF_ERROR_INDPROD_CALLBACK);
							error->data = "too many inputs (max 16)";
							return;
						}
						for (uint i = 0; i < group->num_input; i++) {
							uint8_t rawcargo = buf.ReadByte();
							CargoType cargo = GetCargoTranslation(rawcargo, _cur.grffile);
							if (!IsValidCargoType(cargo)) {
								/* The mapped cargo is invalid. This is permitted at this point,
								 * as long as the result is not used. Mark it invalid so this
								 * can be tested later. */
								group->version = 0xFF;
							} else if (std::find(group->cargo_input, group->cargo_input + i, cargo) != group->cargo_input + i) {
								GRFError *error = DisableGrf(STR_NEWGRF_ERROR_INDPROD_CALLBACK);
								error->data = "duplicate input cargo";
								return;
							}
							group->cargo_input[i] = cargo;
							group->subtract_input[i] = buf.ReadByte();
						}
						group->num_output = buf.ReadByte();
						if (group->num_output > lengthof(group->add_output)) {
							GRFError *error = DisableGrf(STR_NEWGRF_ERROR_INDPROD_CALLBACK);
							error->data = "too many outputs (max 16)";
							return;
						}
						for (uint i = 0; i < group->num_output; i++) {
							uint8_t rawcargo = buf.ReadByte();
							CargoType cargo = GetCargoTranslation(rawcargo, _cur.grffile);
							if (!IsValidCargoType(cargo)) {
								/* Mark this result as invalid to use */
								group->version = 0xFF;
							} else if (std::find(group->cargo_output, group->cargo_output + i, cargo) != group->cargo_output + i) {
								GRFError *error = DisableGrf(STR_NEWGRF_ERROR_INDPROD_CALLBACK);
								error->data = "duplicate output cargo";
								return;
							}
							group->cargo_output[i] = cargo;
							group->add_output[i] = buf.ReadByte();
						}
						group->again = buf.ReadByte();
					} else {
						NOT_REACHED();
					}
					break;
				}

				case GSF_FAKE_TOWNS:
					act_group = NewCallbackResultSpriteGroupNoTransform(CALLBACK_FAILED);
					break;

				/* Loading of Tile Layout and Production Callback groups would happen here */
				default: GrfMsg(1, "NewSpriteGroup: Unsupported feature {}, skipping", GetFeatureString(feature));
			}
		}
	}

	if ((size_t)setid >= _cur.spritegroups.size()) _cur.spritegroups.resize(setid + 1);
	_cur.spritegroups[setid] = act_group;
}

/**
 * Get the cargo translation table to use for the given GRF file.
 * @param grffile GRF file.
 * @returns Readonly cargo translation table to use.
 */
std::span<const CargoLabel> GetCargoTranslationTable(const GRFFile &grffile)
{
	/* Always use the translation table if it's installed. */
	if (!grffile.cargo_list.empty()) return grffile.cargo_list;

	/* Pre-v7 use climate-dependent "slot" table. */
	if (grffile.grf_version < 7) return GetClimateDependentCargoTranslationTable();

	/* Otherwise use climate-independent "bitnum" table. */
	return GetClimateIndependentCargoTranslationTable();
}

static CargoType TranslateCargo(uint8_t feature, uint8_t ctype)
{
	/* Special cargo types for purchase list and stations */
	if ((feature == GSF_STATIONS || feature == GSF_ROADSTOPS) && ctype == 0xFE) return SpriteGroupCargo::SG_DEFAULT_NA;
	if (ctype == 0xFF) return SpriteGroupCargo::SG_PURCHASE;

	auto cargo_list = GetCargoTranslationTable(*_cur.grffile);

	/* Check if the cargo type is out of bounds of the cargo translation table */
	if (ctype >= cargo_list.size()) {
		GrfMsg(1, "TranslateCargo: Cargo type {} out of range (max {}), skipping.", ctype, (unsigned int)_cur.grffile->cargo_list.size() - 1);
		return INVALID_CARGO;
	}

	/* Look up the cargo label from the translation table */
	CargoLabel cl = cargo_list[ctype];
	if (cl == CT_INVALID) {
		GrfMsg(5, "TranslateCargo: Cargo type {} not available in this climate, skipping.", ctype);
		return INVALID_CARGO;
	}

	CargoType cargo_type = GetCargoTypeByLabel(cl);
	if (!IsValidCargoType(cargo_type)) {
		GrfMsg(5, "TranslateCargo: Cargo '{:c}{:c}{:c}{:c}' unsupported, skipping.", GB(cl.base(), 24, 8), GB(cl.base(), 16, 8), GB(cl.base(), 8, 8), GB(cl.base(), 0, 8));
		return INVALID_CARGO;
	}

	GrfMsg(6, "TranslateCargo: Cargo '{:c}{:c}{:c}{:c}' mapped to cargo type {}.", GB(cl.base(), 24, 8), GB(cl.base(), 16, 8), GB(cl.base(), 8, 8), GB(cl.base(), 0, 8), cargo_type);
	return cargo_type;
}


static bool IsValidGroupID(uint16_t groupid, const char *function)
{
	if ((size_t)groupid >= _cur.spritegroups.size() || _cur.spritegroups[groupid] == nullptr) {
		GrfMsg(1, "{}: Spritegroup 0x{:04X} out of range or empty, skipping.", function, groupid);
		return false;
	}

	return true;
}

static void VehicleMapSpriteGroup(ByteReader &buf, uint8_t feature, uint8_t idcount)
{
	static std::vector<EngineID> last_engines; // Engine IDs are remembered in case the next action is a wagon override.
	bool wagover = false;

	/* Test for 'wagon override' flag */
	if (HasBit(idcount, 7)) {
		wagover = true;
		/* Strip off the flag */
		idcount = GB(idcount, 0, 7);

		if (last_engines.empty()) {
			GrfMsg(0, "VehicleMapSpriteGroup: WagonOverride: No engine to do override with");
			return;
		}

		GrfMsg(6, "VehicleMapSpriteGroup: WagonOverride: {} engines, {} wagons", last_engines.size(), idcount);
	} else {
		last_engines.resize(idcount);
	}

	TempBufferST<EngineID> engines(idcount);
	for (uint i = 0; i < idcount; i++) {
		Engine *e = GetNewEngine(_cur.grffile, (VehicleType)feature, buf.ReadExtendedByte());
		if (e == nullptr) {
			/* No engine could be allocated?!? Deal with it. Okay,
			 * this might look bad. Also make sure this NewGRF
			 * gets disabled, as a half loaded one is bad. */
			HandleChangeInfoResult("VehicleMapSpriteGroup", CIR_INVALID_ID, (GrfSpecFeature)0, 0);
			return;
		}

		engines[i] = e->index;
		if (!wagover) last_engines[i] = engines[i];
	}

	uint8_t cidcount = buf.ReadByte();
	for (uint c = 0; c < cidcount; c++) {
		uint8_t ctype = buf.ReadByte();
		uint16_t groupid = buf.ReadWord();
		if (!IsValidGroupID(groupid, "VehicleMapSpriteGroup")) continue;

		GrfMsg(8, "VehicleMapSpriteGroup: * [{}] Cargo type 0x{:X}, group id 0x{:02X}", c, ctype, groupid);

		CargoType cargo_type = TranslateCargo(feature, ctype);
		if (!IsValidCargoType(cargo_type)) continue;

		for (uint i = 0; i < idcount; i++) {
			EngineID engine = engines[i];

			GrfMsg(7, "VehicleMapSpriteGroup: [{}] Engine {}...", i, engine);

			if (wagover) {
				SetWagonOverrideSprites(engine, cargo_type, GetGroupByID(groupid), last_engines);
			} else {
				SetCustomEngineSprites(engine, cargo_type, GetGroupByID(groupid));
			}
		}
	}

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "VehicleMapSpriteGroup")) return;

	GrfMsg(8, "-- Default group id 0x{:04X}", groupid);

	for (uint i = 0; i < idcount; i++) {
		EngineID engine = engines[i];

		if (wagover) {
			SetWagonOverrideSprites(engine, SpriteGroupCargo::SG_DEFAULT, GetGroupByID(groupid), last_engines);
		} else {
			SetCustomEngineSprites(engine, SpriteGroupCargo::SG_DEFAULT, GetGroupByID(groupid));
			SetEngineGRF(engine, _cur.grffile);
		}
	}
}


static void CanalMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	TempBufferST<uint16_t> cfs(idcount);
	for (uint i = 0; i < idcount; i++) {
		cfs[i] = buf.ReadExtendedByte();
	}

	uint8_t cidcount = buf.ReadByte();
	buf.Skip(cidcount * 3);

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "CanalMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		uint16_t cf = cfs[i];

		if (cf >= CF_END) {
			GrfMsg(1, "CanalMapSpriteGroup: Canal subset {} out of range, skipping", cf);
			continue;
		}

		_water_feature[cf].grffile = _cur.grffile;
		_water_feature[cf].group = GetGroupByID(groupid);
	}
}


static void StationMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	if (_cur.grffile->stations.empty()) {
		GrfMsg(1, "StationMapSpriteGroup: No stations defined, skipping");
		return;
	}

	TempBufferST<uint16_t> stations(idcount);
	for (uint i = 0; i < idcount; i++) {
		stations[i] = buf.ReadExtendedByte();
	}

	uint8_t cidcount = buf.ReadByte();
	for (uint c = 0; c < cidcount; c++) {
		uint8_t ctype = buf.ReadByte();
		uint16_t groupid = buf.ReadWord();
		if (!IsValidGroupID(groupid, "StationMapSpriteGroup")) continue;

		ctype = TranslateCargo(GSF_STATIONS, ctype);
		if (ctype == INVALID_CARGO) continue;

		for (uint i = 0; i < idcount; i++) {
			StationSpec *statspec = stations[i] >= _cur.grffile->stations.size() ? nullptr : _cur.grffile->stations[stations[i]].get();

			if (statspec == nullptr) {
				GrfMsg(1, "StationMapSpriteGroup: Station with ID 0x{:X} undefined, skipping", stations[i]);
				continue;
			}

			statspec->grf_prop.SetSpriteGroup(ctype, GetGroupByID(groupid));
		}
	}

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "StationMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		StationSpec *statspec = stations[i] >= _cur.grffile->stations.size() ? nullptr : _cur.grffile->stations[stations[i]].get();

		if (statspec == nullptr) {
			GrfMsg(1, "StationMapSpriteGroup: Station with ID 0x{:X} undefined, skipping", stations[i]);
			continue;
		}

		if (statspec->grf_prop.HasGrfFile()) {
			GrfMsg(1, "StationMapSpriteGroup: Station with ID 0x{:X} mapped multiple times, skipping", stations[i]);
			continue;
		}

		statspec->grf_prop.SetSpriteGroup(SpriteGroupCargo::SG_DEFAULT, GetGroupByID(groupid));
		statspec->grf_prop.grfid = _cur.grffile->grfid;
		statspec->grf_prop.grffile = _cur.grffile;
		statspec->grf_prop.local_id = stations[i];
		StationClass::Assign(statspec);
	}
}


static void TownHouseMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	if (_cur.grffile->housespec.empty()) {
		GrfMsg(1, "TownHouseMapSpriteGroup: No houses defined, skipping");
		return;
	}

	TempBufferST<uint16_t> houses(idcount);
	for (uint i = 0; i < idcount; i++) {
		houses[i] = buf.ReadExtendedByte();
	}

	/* Skip the cargo type section, we only care about the default group */
	uint8_t cidcount = buf.ReadByte();
	buf.Skip(cidcount * 3);

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "TownHouseMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		HouseSpec *hs = houses[i] >= _cur.grffile->housespec.size() ? nullptr : _cur.grffile->housespec[houses[i]].get();

		if (hs == nullptr) {
			GrfMsg(1, "TownHouseMapSpriteGroup: House {} undefined, skipping.", houses[i]);
			continue;
		}

		hs->grf_prop.SetSpriteGroup(0, GetGroupByID(groupid));
	}
}

static void IndustryMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	if (_cur.grffile->industryspec.empty()) {
		GrfMsg(1, "IndustryMapSpriteGroup: No industries defined, skipping");
		return;
	}

	TempBufferST<uint16_t> industries(idcount);
	for (uint i = 0; i < idcount; i++) {
		industries[i] = buf.ReadExtendedByte();
	}

	/* Skip the cargo type section, we only care about the default group */
	uint8_t cidcount = buf.ReadByte();
	buf.Skip(cidcount * 3);

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "IndustryMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		IndustrySpec *indsp = industries[i] >= _cur.grffile->industryspec.size() ? nullptr : _cur.grffile->industryspec[industries[i]].get();

		if (indsp == nullptr) {
			GrfMsg(1, "IndustryMapSpriteGroup: Industry {} undefined, skipping", industries[i]);
			continue;
		}

		indsp->grf_prop.SetSpriteGroup(0, GetGroupByID(groupid));
	}
}

static void IndustrytileMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	if (_cur.grffile->indtspec.empty()) {
		GrfMsg(1, "IndustrytileMapSpriteGroup: No industry tiles defined, skipping");
		return;
	}

	TempBufferST<uint16_t> indtiles(idcount);
	for (uint i = 0; i < idcount; i++) {
		indtiles[i] = buf.ReadExtendedByte();
	}

	/* Skip the cargo type section, we only care about the default group */
	uint8_t cidcount = buf.ReadByte();
	buf.Skip(cidcount * 3);

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "IndustrytileMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		IndustryTileSpec *indtsp = indtiles[i] >= _cur.grffile->indtspec.size() ? nullptr : _cur.grffile->indtspec[indtiles[i]].get();

		if (indtsp == nullptr) {
			GrfMsg(1, "IndustrytileMapSpriteGroup: Industry tile {} undefined, skipping", indtiles[i]);
			continue;
		}

		indtsp->grf_prop.SetSpriteGroup(0, GetGroupByID(groupid));
	}
}

static void CargoMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	TempBufferST<uint16_t> cargoes(idcount);
	for (uint i = 0; i < idcount; i++) {
		cargoes[i] = buf.ReadExtendedByte();
	}

	/* Skip the cargo type section, we only care about the default group */
	uint8_t cidcount = buf.ReadByte();
	buf.Skip(cidcount * 3);

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "CargoMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		uint16_t cargo_type = cargoes[i];

		if (cargo_type >= NUM_CARGO) {
			GrfMsg(1, "CargoMapSpriteGroup: Cargo ID {} out of range, skipping", cargo_type);
			continue;
		}

		CargoSpec *cs = CargoSpec::Get(cargo_type);
		cs->grffile = _cur.grffile;
		cs->group = GetGroupByID(groupid);
	}
}

static void SignalsMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	TempBufferST<uint16_t> ids(idcount);
	for (uint i = 0; i < idcount; i++) {
		ids[i] = buf.ReadExtendedByte();
	}

	/* Skip the cargo type section, we only care about the default group */
	uint8_t cidcount = buf.ReadByte();
	buf.Skip(cidcount * 3);

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "SignalsMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		uint16_t id = ids[i];

		switch (id) {
			case NSA3ID_CUSTOM_SIGNALS:
				_cur.grffile->new_signals_group = GetGroupByID(groupid);
				if (!HasBit(_cur.grffile->new_signal_ctrl_flags, NSCF_GROUPSET)) {
					SetBit(_cur.grffile->new_signal_ctrl_flags, NSCF_GROUPSET);
					_new_signals_grfs.push_back(_cur.grffile);
				}
				break;

			default:
				GrfMsg(1, "SignalsMapSpriteGroup: ID not implemented: {}", id);
			break;
		}
	}
}

static void ObjectMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	if (_cur.grffile->objectspec.empty()) {
		GrfMsg(1, "ObjectMapSpriteGroup: No object tiles defined, skipping");
		return;
	}

	TempBufferST<uint16_t> objects(idcount);
	for (uint i = 0; i < idcount; i++) {
		objects[i] = buf.ReadExtendedByte();
	}

	uint8_t cidcount = buf.ReadByte();
	for (uint c = 0; c < cidcount; c++) {
		uint8_t ctype = buf.ReadByte();
		uint16_t groupid = buf.ReadWord();
		if (!IsValidGroupID(groupid, "ObjectMapSpriteGroup")) continue;

		/* The only valid option here is purchase list sprite groups. */
		if (ctype != 0xFF) {
			GrfMsg(1, "ObjectMapSpriteGroup: Invalid cargo bitnum {} for objects, skipping.", ctype);
			continue;
		}

		for (uint i = 0; i < idcount; i++) {
			ObjectSpec *spec = (objects[i] >= _cur.grffile->objectspec.size()) ? nullptr : _cur.grffile->objectspec[objects[i]].get();

			if (spec == nullptr) {
				GrfMsg(1, "ObjectMapSpriteGroup: Object with ID 0x{:X} undefined, skipping", objects[i]);
				continue;
			}

			spec->grf_prop.SetSpriteGroup(OBJECT_SPRITE_GROUP_PURCHASE, GetGroupByID(groupid));
		}
	}

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "ObjectMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		ObjectSpec *spec = (objects[i] >= _cur.grffile->objectspec.size()) ? nullptr : _cur.grffile->objectspec[objects[i]].get();

		if (spec == nullptr) {
			GrfMsg(1, "ObjectMapSpriteGroup: Object with ID 0x{:X} undefined, skipping", objects[i]);
			continue;
		}

		if (spec->grf_prop.HasGrfFile()) {
			GrfMsg(1, "ObjectMapSpriteGroup: Object with ID 0x{:X} mapped multiple times, skipping", objects[i]);
			continue;
		}

		spec->grf_prop.SetSpriteGroup(OBJECT_SPRITE_GROUP_DEFAULT, GetGroupByID(groupid));
		spec->grf_prop.grfid = _cur.grffile->grfid;
		spec->grf_prop.grffile = _cur.grffile;
		spec->grf_prop.local_id = objects[i];
	}
}

static void RailTypeMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	TempBufferST<uint8_t> railtypes(idcount);
	for (uint i = 0; i < idcount; i++) {
		uint16_t id = buf.ReadExtendedByte();
		railtypes[i] = id < RAILTYPE_END ? _cur.grffile->railtype_map[id] : INVALID_RAILTYPE;
	}

	uint8_t cidcount = buf.ReadByte();
	for (uint c = 0; c < cidcount; c++) {
		uint8_t ctype = buf.ReadByte();
		uint16_t groupid = buf.ReadWord();
		if (!IsValidGroupID(groupid, "RailTypeMapSpriteGroup")) continue;

		if (ctype >= RTSG_END) continue;

		extern RailTypeInfo _railtypes[RAILTYPE_END];
		for (uint i = 0; i < idcount; i++) {
			if (railtypes[i] != INVALID_RAILTYPE) {
				RailTypeInfo *rti = &_railtypes[railtypes[i]];

				rti->grffile[ctype] = _cur.grffile;
				rti->group[ctype] = GetGroupByID(groupid);
			}
		}
	}

	/* Railtypes do not use the default group. */
	buf.ReadWord();
}

static void RoadTypeMapSpriteGroup(ByteReader &buf, uint8_t idcount, RoadTramType rtt)
{
	std::array<RoadType, ROADTYPE_END> &type_map = (rtt == RTT_TRAM) ? _cur.grffile->tramtype_map : _cur.grffile->roadtype_map;

	TempBufferST<uint8_t> roadtypes(idcount);
	for (uint i = 0; i < idcount; i++) {
		uint16_t id = buf.ReadExtendedByte();
		roadtypes[i] = id < ROADTYPE_END ? type_map[id] : INVALID_ROADTYPE;
	}

	uint8_t cidcount = buf.ReadByte();
	for (uint c = 0; c < cidcount; c++) {
		uint8_t ctype = buf.ReadByte();
		uint16_t groupid = buf.ReadWord();
		if (!IsValidGroupID(groupid, "RoadTypeMapSpriteGroup")) continue;

		if (ctype >= ROTSG_END) continue;

		extern RoadTypeInfo _roadtypes[ROADTYPE_END];
		for (uint i = 0; i < idcount; i++) {
			if (roadtypes[i] != INVALID_ROADTYPE) {
				RoadTypeInfo *rti = &_roadtypes[roadtypes[i]];

				rti->grffile[ctype] = _cur.grffile;
				rti->group[ctype] = GetGroupByID(groupid);
			}
		}
	}

	/* Roadtypes do not use the default group. */
	buf.ReadWord();
}

static void AirportMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	if (_cur.grffile->airportspec.empty()) {
		GrfMsg(1, "AirportMapSpriteGroup: No airports defined, skipping");
		return;
	}

	TempBufferST<uint16_t> airports(idcount);
	for (uint i = 0; i < idcount; i++) {
		airports[i] = buf.ReadExtendedByte();
	}

	/* Skip the cargo type section, we only care about the default group */
	uint8_t cidcount = buf.ReadByte();
	buf.Skip(cidcount * 3);

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "AirportMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		AirportSpec *as = airports[i] >= _cur.grffile->airportspec.size() ? nullptr : _cur.grffile->airportspec[airports[i]].get();

		if (as == nullptr) {
			GrfMsg(1, "AirportMapSpriteGroup: Airport {} undefined, skipping", airports[i]);
			continue;
		}

		as->grf_prop.SetSpriteGroup(0, GetGroupByID(groupid));
	}
}

static void AirportTileMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	if (_cur.grffile->airtspec.empty()) {
		GrfMsg(1, "AirportTileMapSpriteGroup: No airport tiles defined, skipping");
		return;
	}

	TempBufferST<uint16_t> airptiles(idcount);
	for (uint i = 0; i < idcount; i++) {
		airptiles[i] = buf.ReadExtendedByte();
	}

	/* Skip the cargo type section, we only care about the default group */
	uint8_t cidcount = buf.ReadByte();
	buf.Skip(cidcount * 3);

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "AirportTileMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		AirportTileSpec *airtsp = airptiles[i] >= _cur.grffile->airtspec.size() ? nullptr : _cur.grffile->airtspec[airptiles[i]].get();

		if (airtsp == nullptr) {
			GrfMsg(1, "AirportTileMapSpriteGroup: Airport tile {} undefined, skipping", airptiles[i]);
			continue;
		}

		airtsp->grf_prop.SetSpriteGroup(0, GetGroupByID(groupid));
	}
}

static void RoadStopMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	TempBufferST<uint16_t> roadstops(idcount);
	for (uint i = 0; i < idcount; i++) {
		roadstops[i] = buf.ReadExtendedByte();
	}

	uint8_t cidcount = buf.ReadByte();
	for (uint c = 0; c < cidcount; c++) {
		uint8_t ctype = buf.ReadByte();
		uint16_t groupid = buf.ReadWord();
		if (!IsValidGroupID(groupid, "RoadStopMapSpriteGroup")) continue;

		ctype = TranslateCargo(GSF_ROADSTOPS, ctype);
		if (ctype == INVALID_CARGO) continue;

		for (uint i = 0; i < idcount; i++) {
			RoadStopSpec *roadstopspec = (roadstops[i] >= _cur.grffile->roadstops.size()) ? nullptr : _cur.grffile->roadstops[roadstops[i]].get();

			if (roadstopspec == nullptr) {
				GrfMsg(1, "RoadStopMapSpriteGroup: Road stop with ID 0x{:X} does not exist, skipping", roadstops[i]);
				continue;
			}

			roadstopspec->grf_prop.SetSpriteGroup(ctype, GetGroupByID(groupid));
		}
	}

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "RoadStopMapSpriteGroup")) return;

	if (_cur.grffile->roadstops.empty()) {
		GrfMsg(0, "RoadStopMapSpriteGroup: No roadstops defined, skipping.");
		return;
	}

	for (uint i = 0; i < idcount; i++) {
		RoadStopSpec *roadstopspec = (roadstops[i] >= _cur.grffile->roadstops.size()) ? nullptr : _cur.grffile->roadstops[roadstops[i]].get();

		if (roadstopspec == nullptr) {
			GrfMsg(1, "RoadStopMapSpriteGroup: Road stop with ID 0x{:X} does not exist, skipping.", roadstops[i]);
			continue;
		}

		if (roadstopspec->grf_prop.HasGrfFile()) {
			GrfMsg(1, "RoadStopMapSpriteGroup: Road stop with ID 0x{:X} mapped multiple times, skipping", roadstops[i]);
			continue;
		}

		roadstopspec->grf_prop.SetSpriteGroup(SpriteGroupCargo::SG_DEFAULT, GetGroupByID(groupid));
		roadstopspec->grf_prop.grfid = _cur.grffile->grfid;
		roadstopspec->grf_prop.grffile = _cur.grffile;
		roadstopspec->grf_prop.local_id = roadstops[i];
		RoadStopClass::Assign(roadstopspec);
	}
}

static void BadgeMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	if (_cur.grffile->badge_map.empty()) {
		GrfMsg(1, "BadgeMapSpriteGroup: No badges defined, skipping");
		return;
	}

	std::vector<uint16_t> local_ids;
	local_ids.reserve(idcount);
	for (uint i = 0; i < idcount; i++) {
		local_ids.push_back(buf.ReadExtendedByte());
	}

	uint8_t cidcount = buf.ReadByte();
	for (uint c = 0; c < cidcount; c++) {
		uint8_t ctype = buf.ReadByte();
		uint16_t groupid = buf.ReadWord();
		if (!IsValidGroupID(groupid, "BadgeMapSpriteGroup")) continue;

		if (ctype >= GSF_END) continue;

		for (const auto &local_id : local_ids) {
			auto found = _cur.grffile->badge_map.find(local_id);
			if (found == std::end(_cur.grffile->badge_map)) {
				GrfMsg(1, "BadgeMapSpriteGroup: Badge {} undefined, skipping", local_id);
				continue;
			}

			auto &badge = *GetBadge(found->second);
			badge.grf_prop.SetSpriteGroup(ctype, _cur.spritegroups[groupid]);
		}
	}

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "BadgeMapSpriteGroup")) return;

	for (auto &local_id : local_ids) {
		auto found = _cur.grffile->badge_map.find(local_id);
		if (found == std::end(_cur.grffile->badge_map)) {
			GrfMsg(1, "BadgeMapSpriteGroup: Badge {} undefined, skipping", local_id);
			continue;
		}

		auto &badge = *GetBadge(found->second);
		badge.grf_prop.SetSpriteGroup(GSF_END, _cur.spritegroups[groupid]);
		badge.grf_prop.grffile = _cur.grffile;
		badge.grf_prop.local_id = local_id;
	}
}

static void NewLandscapeMapSpriteGroup(ByteReader &buf, uint8_t idcount)
{
	TempBufferST<uint16_t> ids(idcount);
	for (uint i = 0; i < idcount; i++) {
		ids[i] = buf.ReadExtendedByte();
	}

	/* Skip the cargo type section, we only care about the default group */
	uint8_t cidcount = buf.ReadByte();
	buf.Skip(cidcount * 3);

	uint16_t groupid = buf.ReadWord();
	if (!IsValidGroupID(groupid, "NewLandscapeMapSpriteGroup")) return;

	for (uint i = 0; i < idcount; i++) {
		uint16_t id = ids[i];

		switch (id) {
			case NLA3ID_CUSTOM_ROCKS:
				_cur.grffile->new_rocks_group = GetGroupByID(groupid);
				if (!HasBit(_cur.grffile->new_landscape_ctrl_flags, NLCF_ROCKS_SET)) {
					SetBit(_cur.grffile->new_landscape_ctrl_flags, NLCF_ROCKS_SET);
					_new_landscape_rocks_grfs.push_back(_cur.grffile);
				}
				break;

			default:
				GrfMsg(1, "NewLandscapeMapSpriteGroup: ID not implemented: {}", id);
			break;
		}
	}
}

/* Action 0x03 */
static void FeatureMapSpriteGroup(ByteReader &buf)
{
	/* <03> <feature> <n-id> <ids>... <num-cid> [<cargo-type> <cid>]... <def-cid>
	 * id-list    := [<id>] [id-list]
	 * cargo-list := <cargo-type> <cid> [cargo-list]
	 *
	 * B feature       see action 0
	 * B n-id          bits 0-6: how many IDs this definition applies to
	 *                 bit 7: if set, this is a wagon override definition (see below)
	 * E ids           the IDs for which this definition applies
	 * B num-cid       number of cargo IDs (sprite group IDs) in this definition
	 *                 can be zero, in that case the def-cid is used always
	 * B cargo-type    type of this cargo type (e.g. mail=2, wood=7, see below)
	 * W cid           cargo ID (sprite group ID) for this type of cargo
	 * W def-cid       default cargo ID (sprite group ID) */

	GrfSpecFeatureRef feature_ref = ReadFeature(buf.ReadByte());
	GrfSpecFeature feature = feature_ref.id;
	uint8_t idcount = buf.ReadByte();

	if (feature >= GSF_END) {
		GrfMsg(1, "FeatureMapSpriteGroup: Unsupported feature {}, skipping", GetFeatureString(feature_ref));
		return;
	}

	/* If idcount is zero, this is a feature callback */
	if (idcount == 0) {
		/* Skip number of cargo ids? */
		buf.ReadByte();
		uint16_t groupid = buf.ReadWord();
		if (!IsValidGroupID(groupid, "FeatureMapSpriteGroup")) return;

		GrfMsg(6, "FeatureMapSpriteGroup: Adding generic feature callback for feature {}", GetFeatureString(feature_ref));

		AddGenericCallback(feature, _cur.grffile, GetGroupByID(groupid));
		return;
	}

	/* Mark the feature as used by the grf (generic callbacks do not count) */
	SetBit(_cur.grffile->grf_features, feature);

	GrfMsg(6, "FeatureMapSpriteGroup: Feature {}, {} ids", GetFeatureString(feature_ref), idcount);

	switch (feature) {
		case GSF_TRAINS:
		case GSF_ROADVEHICLES:
		case GSF_SHIPS:
		case GSF_AIRCRAFT:
			VehicleMapSpriteGroup(buf, feature, idcount);
			return;

		case GSF_CANALS:
			CanalMapSpriteGroup(buf, idcount);
			return;

		case GSF_STATIONS:
			StationMapSpriteGroup(buf, idcount);
			return;

		case GSF_HOUSES:
			TownHouseMapSpriteGroup(buf, idcount);
			return;

		case GSF_INDUSTRIES:
			IndustryMapSpriteGroup(buf, idcount);
			return;

		case GSF_INDUSTRYTILES:
			IndustrytileMapSpriteGroup(buf, idcount);
			return;

		case GSF_CARGOES:
			CargoMapSpriteGroup(buf, idcount);
			return;

		case GSF_AIRPORTS:
			AirportMapSpriteGroup(buf, idcount);
			return;

		case GSF_SIGNALS:
			SignalsMapSpriteGroup(buf, idcount);
			break;

		case GSF_OBJECTS:
			ObjectMapSpriteGroup(buf, idcount);
			break;

		case GSF_RAILTYPES:
			RailTypeMapSpriteGroup(buf, idcount);
			break;

		case GSF_ROADTYPES:
			RoadTypeMapSpriteGroup(buf, idcount, RTT_ROAD);
			break;

		case GSF_TRAMTYPES:
			RoadTypeMapSpriteGroup(buf, idcount, RTT_TRAM);
			break;

		case GSF_AIRPORTTILES:
			AirportTileMapSpriteGroup(buf, idcount);
			return;

		case GSF_ROADSTOPS:
			RoadStopMapSpriteGroup(buf, idcount);
			return;

		case GSF_BADGES:
			BadgeMapSpriteGroup(buf, idcount);
			break;

		case GSF_NEWLANDSCAPE:
			NewLandscapeMapSpriteGroup(buf, idcount);
			return;

		default:
			GrfMsg(1, "FeatureMapSpriteGroup: Unsupported feature {}, skipping", GetFeatureString(feature_ref));
			return;
	}
}

/* Action 0x04 */
static void FeatureNewName(ByteReader &buf)
{
	/* <04> <veh-type> <language-id> <num-veh> <offset> <data...>
	 *
	 * B veh-type      see action 0 (as 00..07, + 0A
	 *                 But IF veh-type = 48, then generic text
	 * B language-id   If bit 6 is set, This is the extended language scheme,
	 *                 with up to 64 language.
	 *                 Otherwise, it is a mapping where set bits have meaning
	 *                 0 = american, 1 = english, 2 = german, 3 = french, 4 = spanish
	 *                 Bit 7 set means this is a generic text, not a vehicle one (or else)
	 * B num-veh       number of vehicles which are getting a new name
	 * B/W offset      number of the first vehicle that gets a new name
	 *                 Byte : ID of vehicle to change
	 *                 Word : ID of string to change/add
	 * S data          new texts, each of them zero-terminated, after
	 *                 which the next name begins. */

	bool new_scheme = _cur.grffile->grf_version >= 7;

	GrfSpecFeatureRef feature_ref = ReadFeature(buf.ReadByte(), true);
	GrfSpecFeature feature = feature_ref.id;
	if (feature >= GSF_END && feature != 0x48) {
		GrfMsg(1, "FeatureNewName: Unsupported feature {}, skipping", GetFeatureString(feature_ref));
		return;
	}

	uint8_t lang   = buf.ReadByte();
	uint8_t num    = buf.ReadByte();
	bool generic   = HasBit(lang, 7);
	uint16_t id;
	if (generic) {
		id = buf.ReadWord();
	} else if (feature <= GSF_AIRCRAFT || feature == GSF_BADGES) {
		id = buf.ReadExtendedByte();
	} else {
		id = buf.ReadByte();
	}

	ClrBit(lang, 7);

	uint16_t endid = id + num;

	GrfMsg(6, "FeatureNewName: About to rename engines {}..{} (feature {}) in language 0x{:02X}",
	               id, endid, GetFeatureString(feature), lang);

	/* Feature overlay to make non-generic strings unique in their feature. We use feature + 1 so that generic strings stay as they are. */
	uint32_t feature_overlay = generic ? 0 : ((feature + 1) << 16);

	for (; id < endid && buf.HasData(); id++) {
		const std::string_view name = buf.ReadString();
		GrfMsg(8, "FeatureNewName: 0x{:04X} <- {}", id, StrMakeValid(name));

		switch (feature) {
			case GSF_TRAINS:
			case GSF_ROADVEHICLES:
			case GSF_SHIPS:
			case GSF_AIRCRAFT:
				if (!generic) {
					Engine *e = GetNewEngine(_cur.grffile, (VehicleType)feature, id, _cur.grfconfig->flags.Test(GRFConfigFlag::Static));
					if (e == nullptr) break;
					StringID string = AddGRFString(_cur.grffile->grfid, GRFStringID{feature_overlay | e->index}, lang, new_scheme, false, name, e->info.string_id);
					e->info.string_id = string;
				} else {
					AddGRFString(_cur.grffile->grfid, GRFStringID{id}, lang, new_scheme, true, name, STR_UNDEFINED);
				}
				break;

			case GSF_BADGES: {
				if (!generic) {
					auto found = _cur.grffile->badge_map.find(id);
					if (found == std::end(_cur.grffile->badge_map)) {
						GrfMsg(1, "FeatureNewName: Attempt to name undefined badge 0x{:X}, ignoring", id);
					} else {
						Badge &badge = *GetBadge(found->second);
						badge.name = AddGRFString(_cur.grffile->grfid, GRFStringID{feature_overlay | id}, lang, true, false, name, STR_UNDEFINED);
					}
				} else {
					AddGRFString(_cur.grffile->grfid, GRFStringID{id}, lang, new_scheme, true, name, STR_UNDEFINED);
				}
				break;
			}

			default:
				if (IsInsideMM(id, 0xD000, 0xD400) || IsInsideMM(id, 0xD800, 0x10000)) {
					AddGRFString(_cur.grffile->grfid, GRFStringID{id}, lang, new_scheme, true, name, STR_UNDEFINED);
					break;
				}

				switch (GB(id, 8, 8)) {
					case 0xC4: // Station class name
						if (GB(id, 0, 8) >= _cur.grffile->stations.size() || _cur.grffile->stations[GB(id, 0, 8)] == nullptr) {
							GrfMsg(1, "FeatureNewName: Attempt to name undefined station 0x{:X}, ignoring", GB(id, 0, 8));
						} else {
							StationClassID class_index = _cur.grffile->stations[GB(id, 0, 8)]->class_index;
							StationClass::Get(class_index)->name = AddGRFString(_cur.grffile->grfid, GRFStringID{id}, lang, new_scheme, false, name, STR_UNDEFINED);
						}
						break;

					case 0xC5: // Station name
						if (GB(id, 0, 8) >= _cur.grffile->stations.size() || _cur.grffile->stations[GB(id, 0, 8)] == nullptr) {
							GrfMsg(1, "FeatureNewName: Attempt to name undefined station 0x{:X}, ignoring", GB(id, 0, 8));
						} else {
							_cur.grffile->stations[GB(id, 0, 8)]->name = AddGRFString(_cur.grffile->grfid, GRFStringID{id}, lang, new_scheme, false, name, STR_UNDEFINED);
						}
						break;

					case 0xC7: // Airporttile name
						if (GB(id, 0, 8) >= _cur.grffile->airtspec.size() || _cur.grffile->airtspec[GB(id, 0, 8)] == nullptr) {
							GrfMsg(1, "FeatureNewName: Attempt to name undefined airport tile 0x{:X}, ignoring", GB(id, 0, 8));
						} else {
							_cur.grffile->airtspec[GB(id, 0, 8)]->name = AddGRFString(_cur.grffile->grfid, GRFStringID{id}, lang, new_scheme, false, name, STR_UNDEFINED);
						}
						break;

					case 0xC9: // House name
						if (GB(id, 0, 8) >= _cur.grffile->housespec.size() || _cur.grffile->housespec[GB(id, 0, 8)] == nullptr) {
							GrfMsg(1, "FeatureNewName: Attempt to name undefined house 0x{:X}, ignoring.", GB(id, 0, 8));
						} else {
							_cur.grffile->housespec[GB(id, 0, 8)]->building_name = AddGRFString(_cur.grffile->grfid, GRFStringID{id}, lang, new_scheme, false, name, STR_UNDEFINED);
						}
						break;

					default:
						GrfMsg(7, "FeatureNewName: Unsupported ID (0x{:04X})", id);
						break;
				}
				break;
		}
	}
}

/**
 * Sanitize incoming sprite offsets for Action 5 graphics replacements.
 * @param num         The number of sprites to load.
 * @param offset      Offset from the base.
 * @param max_sprites The maximum number of sprites that can be loaded in this action 5.
 * @param name        Used for error warnings.
 * @return The number of sprites that is going to be skipped.
 */
static uint16_t SanitizeSpriteOffset(uint16_t &num, uint16_t offset, int max_sprites, const std::string_view name)
{

	if (offset >= max_sprites) {
		GrfMsg(1, "GraphicsNew: {} sprite offset must be less than {}, skipping", name, max_sprites);
		uint orig_num = num;
		num = 0;
		return orig_num;
	}

	if (offset + num > max_sprites) {
		GrfMsg(4, "GraphicsNew: {} sprite overflow, truncating...", name);
		uint orig_num = num;
		num = std::max(max_sprites - offset, 0);
		return orig_num - num;
	}

	return 0;
}


/** The information about action 5 types. */
static constexpr auto _action5_types = std::to_array<Action5Type>({
	/* Note: min_sprites should not be changed. Therefore these constants are directly here and not in sprites.h */
	/* 0x00 */ { A5BLOCK_INVALID,      0,                            0, 0,                                           "Type 0x00"                },
	/* 0x01 */ { A5BLOCK_INVALID,      0,                            0, 0,                                           "Type 0x01"                },
	/* 0x02 */ { A5BLOCK_INVALID,      0,                            0, 0,                                           "Type 0x02"                },
	/* 0x03 */ { A5BLOCK_INVALID,      0,                            0, 0,                                           "Type 0x03"                },
	/* 0x04 */ { A5BLOCK_ALLOW_OFFSET, SPR_SIGNALS_BASE,             1, PRESIGNAL_SEMAPHORE_AND_PBS_SPRITE_COUNT,    "Signal graphics"          },
	/* 0x05 */ { A5BLOCK_ALLOW_OFFSET, SPR_ELRAIL_BASE,              1, ELRAIL_SPRITE_COUNT,                         "Rail catenary graphics"   },
	/* 0x06 */ { A5BLOCK_ALLOW_OFFSET, SPR_SLOPES_BASE,              1, NORMAL_AND_HALFTILE_FOUNDATION_SPRITE_COUNT, "Foundation graphics"      },
	/* 0x07 */ { A5BLOCK_INVALID,      0,                           75, 0,                                           "TTDP GUI graphics"        }, // Not used by OTTD.
	/* 0x08 */ { A5BLOCK_ALLOW_OFFSET, SPR_CANALS_BASE,              1, CANALS_SPRITE_COUNT,                         "Canal graphics"           },
	/* 0x09 */ { A5BLOCK_ALLOW_OFFSET, SPR_ONEWAY_BASE,              1, ONEWAY_SPRITE_COUNT,                         "One way road graphics"    },
	/* 0x0A */ { A5BLOCK_ALLOW_OFFSET, SPR_2CCMAP_BASE,              1, TWOCCMAP_SPRITE_COUNT,                       "2CC colour maps"          },
	/* 0x0B */ { A5BLOCK_ALLOW_OFFSET, SPR_TRAMWAY_BASE,             1, TRAMWAY_SPRITE_COUNT,                        "Tramway graphics"         },
	/* 0x0C */ { A5BLOCK_INVALID,      0,                          133, 0,                                           "Snowy temperate tree"     }, // Not yet used by OTTD.
	/* 0x0D */ { A5BLOCK_FIXED,        SPR_SHORE_BASE,              16, SHORE_SPRITE_COUNT,                          "Shore graphics"           },
	/* 0x0E */ { A5BLOCK_INVALID,      0,                            0, 0,                                           "New Signals graphics"     }, // Not yet used by OTTD.
	/* 0x0F */ { A5BLOCK_ALLOW_OFFSET, SPR_TRACKS_FOR_SLOPES_BASE,   1, TRACKS_FOR_SLOPES_SPRITE_COUNT,              "Sloped rail track"        },
	/* 0x10 */ { A5BLOCK_ALLOW_OFFSET, SPR_AIRPORTX_BASE,            1, AIRPORTX_SPRITE_COUNT,                       "Airport graphics"         },
	/* 0x11 */ { A5BLOCK_ALLOW_OFFSET, SPR_ROADSTOP_BASE,            1, ROADSTOP_SPRITE_COUNT,                       "Road stop graphics"       },
	/* 0x12 */ { A5BLOCK_ALLOW_OFFSET, SPR_AQUEDUCT_BASE,            1, AQUEDUCT_SPRITE_COUNT,                       "Aqueduct graphics"        },
	/* 0x13 */ { A5BLOCK_ALLOW_OFFSET, SPR_AUTORAIL_BASE,            1, AUTORAIL_SPRITE_COUNT,                       "Autorail graphics"        },
	/* 0x14 */ { A5BLOCK_INVALID,      0,                            1, 0,                                           "Flag graphics"            }, // deprecated, no longer used.
	/* 0x15 */ { A5BLOCK_ALLOW_OFFSET, SPR_OPENTTD_BASE,             1, OPENTTD_SPRITE_COUNT,                        "OpenTTD GUI graphics"     },
	/* 0x16 */ { A5BLOCK_ALLOW_OFFSET, SPR_AIRPORT_PREVIEW_BASE,     1, AIRPORT_PREVIEW_SPRITE_COUNT,                "Airport preview graphics" },
	/* 0x17 */ { A5BLOCK_ALLOW_OFFSET, SPR_RAILTYPE_TUNNEL_BASE,     1, RAILTYPE_TUNNEL_BASE_COUNT,                  "Railtype tunnel base"     },
	/* 0x18 */ { A5BLOCK_ALLOW_OFFSET, SPR_PALETTE_BASE,             1, PALETTE_SPRITE_COUNT,                        "Palette"                  },
	/* 0x19 */ { A5BLOCK_ALLOW_OFFSET, SPR_ROAD_WAYPOINTS_BASE,      1, ROAD_WAYPOINTS_SPRITE_COUNT,                 "Road waypoints"           },
	/* 0x1A */ { A5BLOCK_ALLOW_OFFSET, SPR_OVERLAY_ROCKS_BASE,       1, OVERLAY_ROCKS_SPRITE_COUNT,                  "Overlay rocks"            },
});

/**
 * Get list of all action 5 types
 * @return Read-only span of action 5 type information.
 */
std::span<const Action5Type> GetAction5Types()
{
	return _action5_types;
}

/* Action 0x05 */
static void GraphicsNew(ByteReader &buf)
{
	/* <05> <graphics-type> <num-sprites> <other data...>
	 *
	 * B graphics-type What set of graphics the sprites define.
	 * E num-sprites   How many sprites are in this set?
	 * V other data    Graphics type specific data.  Currently unused. */

	uint8_t type = buf.ReadByte();
	uint16_t num = buf.ReadExtendedByte();
	uint16_t offset = HasBit(type, 7) ? buf.ReadExtendedByte() : 0;
	ClrBit(type, 7); // Clear the high bit as that only indicates whether there is an offset.

	const Action5Type *action5_type;
	const Action5TypeRemapSet &remap = _cur.grffile->action5_type_remaps;
	if (remap.remapped_ids[type]) {
		auto iter = remap.mapping.find(type);
		assert(iter != remap.mapping.end());
		const Action5TypeRemapEntry &def = iter->second;
		if (def.info == nullptr) {
			if (def.fallback_mode == GPMFM_ERROR_ON_USE) {
				GrfMsg(0, "Error: Unimplemented action 5 type: {}, mapped to: {:X}", def.name, type);
				GRFError *error = DisableGrf(STR_NEWGRF_ERROR_UNIMPLEMETED_MAPPED_ACTION5_TYPE);
				error->data = stredup(def.name);
				error->param_value[1] = type;
			} else if (def.fallback_mode == GPMFM_IGNORE) {
				GrfMsg(2, "Ignoring unimplemented action 5 type: {}, mapped to: {:X}", def.name, type);
			}
			_cur.skip_sprites = num;
			return;
		} else {
			action5_type = def.info;
		}
	} else {
		if ((type == 0x0D) && (num == 10) && _cur.grfconfig->flags.Test(GRFConfigFlag::System)) {
			/* Special not-TTDP-compatible case used in openttd.grf
			 * Missing shore sprites and initialisation of SPR_SHORE_BASE */
			GrfMsg(2, "GraphicsNew: Loading 10 missing shore sprites from extra grf.");
			LoadNextSprite(SPR_SHORE_BASE +  0, *_cur.file, _cur.nfo_line++); // SLOPE_STEEP_S
			LoadNextSprite(SPR_SHORE_BASE +  5, *_cur.file, _cur.nfo_line++); // SLOPE_STEEP_W
			LoadNextSprite(SPR_SHORE_BASE +  7, *_cur.file, _cur.nfo_line++); // SLOPE_WSE
			LoadNextSprite(SPR_SHORE_BASE + 10, *_cur.file, _cur.nfo_line++); // SLOPE_STEEP_N
			LoadNextSprite(SPR_SHORE_BASE + 11, *_cur.file, _cur.nfo_line++); // SLOPE_NWS
			LoadNextSprite(SPR_SHORE_BASE + 13, *_cur.file, _cur.nfo_line++); // SLOPE_ENW
			LoadNextSprite(SPR_SHORE_BASE + 14, *_cur.file, _cur.nfo_line++); // SLOPE_SEN
			LoadNextSprite(SPR_SHORE_BASE + 15, *_cur.file, _cur.nfo_line++); // SLOPE_STEEP_E
			LoadNextSprite(SPR_SHORE_BASE + 16, *_cur.file, _cur.nfo_line++); // SLOPE_EW
			LoadNextSprite(SPR_SHORE_BASE + 17, *_cur.file, _cur.nfo_line++); // SLOPE_NS
			if (_loaded_newgrf_features.shore == SHORE_REPLACE_NONE) _loaded_newgrf_features.shore = SHORE_REPLACE_ONLY_NEW;
			return;
		}

		/* Supported type? */
		if ((type >= std::size(_action5_types)) || (_action5_types[type].block_type == A5BLOCK_INVALID)) {
			GrfMsg(2, "GraphicsNew: Custom graphics (type 0x{:02X}) sprite block of length {} (unimplemented, ignoring)", type, num);
			_cur.skip_sprites = num;
			return;
		}

		action5_type = &_action5_types[type];
	}

	/* Contrary to TTDP we allow always to specify too few sprites as we allow always an offset,
	 * except for the long version of the shore type:
	 * Ignore offset if not allowed */
	if ((action5_type->block_type != A5BLOCK_ALLOW_OFFSET) && (offset != 0)) {
		GrfMsg(1, "GraphicsNew: {} (type 0x{:02X}) do not allow an <offset> field. Ignoring offset.", action5_type->name, type);
		offset = 0;
	}

	/* Ignore action5 if too few sprites are specified. (for TTDP compatibility)
	 * This does not make sense, if <offset> is allowed */
	if ((action5_type->block_type == A5BLOCK_FIXED) && (num < action5_type->min_sprites)) {
		GrfMsg(1, "GraphicsNew: {} (type 0x{:02X}) count must be at least {}. Only {} were specified. Skipping.", action5_type->name, type, action5_type->min_sprites, num);
		_cur.skip_sprites = num;
		return;
	}

	/* Load at most max_sprites sprites. Skip remaining sprites. (for compatibility with TTDP and future extensions) */
	uint16_t skip_num = SanitizeSpriteOffset(num, offset, action5_type->max_sprites, action5_type->name);
	SpriteID replace = action5_type->sprite_base + offset;

	/* Load <num> sprites starting from <replace>, then skip <skip_num> sprites. */
	GrfMsg(2, "GraphicsNew: Replacing sprites {} to {} of {} (type 0x{:02X}) at SpriteID 0x{:04X}", offset, offset + num - 1, action5_type->name, type, replace);

	if (type == 0x0D) _loaded_newgrf_features.shore = SHORE_REPLACE_ACTION_5;

	if (type == 0x0B) {
		static const SpriteID depot_with_track_offset = SPR_TRAMWAY_DEPOT_WITH_TRACK - SPR_TRAMWAY_BASE;
		static const SpriteID depot_no_track_offset = SPR_TRAMWAY_DEPOT_NO_TRACK - SPR_TRAMWAY_BASE;
		if (offset <= depot_with_track_offset && offset + num > depot_with_track_offset) _loaded_newgrf_features.tram = TRAMWAY_REPLACE_DEPOT_WITH_TRACK;
		if (offset <= depot_no_track_offset && offset + num > depot_no_track_offset) _loaded_newgrf_features.tram = TRAMWAY_REPLACE_DEPOT_NO_TRACK;
	}

	/* If the baseset or grf only provides sprites for flat tiles (pre #10282), duplicate those for use on slopes. */
	bool dup_oneway_sprites = ((type == 0x09) && (offset + num <= ONEWAY_SLOPE_N_OFFSET));

	for (uint16_t n = num; n > 0; n--) {
		_cur.nfo_line++;
		SpriteID load_index = (replace == 0 ? _cur.spriteid++ : replace++);
		LoadNextSprite(load_index, *_cur.file, _cur.nfo_line);
		if (dup_oneway_sprites) {
			DupSprite(load_index, load_index + ONEWAY_SLOPE_N_OFFSET);
			DupSprite(load_index, load_index + ONEWAY_SLOPE_S_OFFSET);
		}
	}

	if (type == 0x04 && ((_cur.grfconfig->ident.grfid & 0x00FFFFFF) == OPENTTD_GRAPHICS_BASE_GRF_ID ||
			_cur.grfconfig->ident.grfid == std::byteswap<uint32_t>(0xFF4F4701) || _cur.grfconfig->ident.grfid == std::byteswap<uint32_t>(0xFFFFFFFE))) {
		/* Signal graphics action 5: Fill duplicate signal sprite block if this is a baseset GRF or OpenGFX */
		const SpriteID end = offset + num;
		for (SpriteID i = offset; i < end; i++) {
			DupSprite(SPR_SIGNALS_BASE + i, SPR_DUP_SIGNALS_BASE + i);
		}
	}

	_cur.skip_sprites = skip_num;
}

/* Action 0x05 (SKIP) */
static void SkipAct5(ByteReader &buf)
{
	/* Ignore type byte */
	buf.ReadByte();

	/* Skip the sprites of this action */
	_cur.skip_sprites = buf.ReadExtendedByte();

	GrfMsg(3, "SkipAct5: Skipping {} sprites", _cur.skip_sprites);
}

/**
 * Reads a variable common to VarAction2 and Action7/9/D.
 *
 * Returns VarAction2 variable 'param' resp. Action7/9/D variable '0x80 + param'.
 * If a variable is not accessible from all four actions, it is handled in the action specific functions.
 *
 * @param param variable number (as for VarAction2, for Action7/9/D you have to subtract 0x80 first).
 * @param value returns the value of the variable.
 * @param grffile NewGRF querying the variable
 * @return true iff the variable is known and the value is returned in 'value'.
 */
bool GetGlobalVariable(uint8_t param, uint32_t *value, const GRFFile *grffile)
{
	if (_sprite_group_resolve_check_veh_check) {
		switch (param) {
			case 0x00:
			case 0x02:
			case 0x09:
			case 0x0A:
			case 0x20:
			case 0x23:
				_sprite_group_resolve_check_veh_check = false;
				break;
		}
	}

	switch (param) {
		case 0x00: // current date
			*value = std::max<CalTime::DateDelta>(CalTime::CurDate() - CalTime::DAYS_TILL_ORIGINAL_BASE_YEAR, CalTime::DateDelta{0}).base();
			return true;

		case 0x01: // current year
			*value = (Clamp(CalTime::CurYear(), CalTime::ORIGINAL_BASE_YEAR, CalTime::ORIGINAL_MAX_YEAR) - CalTime::ORIGINAL_BASE_YEAR).base();
			return true;

		case 0x02: { // detailed date information: month of year (bit 0-7), day of month (bit 8-12), leap year (bit 15), day of year (bit 16-24)
			CalTime::Date start_of_year = CalTime::ConvertYMDToDate(CalTime::CurYear(), 0, 1);
			*value = CalTime::CurMonth() | (CalTime::CurDay() - 1) << 8 | (CalTime::IsLeapYear(CalTime::CurYear()) ? 1 << 15 : 0) | (CalTime::CurDate() - start_of_year).base() << 16;
			return true;
		}

		case 0x03: // current climate, 0=temp, 1=arctic, 2=trop, 3=toyland
			*value = to_underlying(_settings_game.game_creation.landscape);
			return true;

		case 0x06: // road traffic side, bit 4 clear=left, set=right
			*value = _settings_game.vehicle.road_side << 4;
			return true;

		case 0x09: // date fraction
			*value = CalTime::CurDateFract() * 885;
			return true;

		case 0x0A: // animation counter
			*value = GB(_scaled_tick_counter, 0, 16);
			return true;

		case 0x0B: { // TTDPatch version
			uint major    = 2;
			uint minor    = 6;
			uint revision = 1; // special case: 2.0.1 is 2.0.10
			uint build    = 1382;
			*value = (major << 24) | (minor << 20) | (revision << 16) | build;
			return true;
		}

		case 0x0D: // TTD Version, 00=DOS, 01=Windows
			*value = (_cur.grfconfig->palette & GRFP_USE_MASK) | grffile->var8D_overlay;
			return true;

		case 0x0E: // Y-offset for train sprites
			*value = _cur.grffile->traininfo_vehicle_pitch;
			return true;

		case 0x0F: // Rail track type cost factors
			*value = 0;
			SB(*value, 0, 8, GetRailTypeInfo(RAILTYPE_RAIL)->cost_multiplier); // normal rail
			if (_settings_game.vehicle.disable_elrails) {
				/* skip elrail multiplier - disabled */
				SB(*value, 8, 8, GetRailTypeInfo(RAILTYPE_MONO)->cost_multiplier); // monorail
			} else {
				SB(*value, 8, 8, GetRailTypeInfo(RAILTYPE_ELECTRIC)->cost_multiplier); // electified railway
				/* Skip monorail multiplier - no space in result */
			}
			SB(*value, 16, 8, GetRailTypeInfo(RAILTYPE_MAGLEV)->cost_multiplier); // maglev
			return true;

		case 0x11: // current rail tool type
			*value = 0; // constant fake value to avoid desync
			return true;

		case 0x12: // Game mode
			*value = _game_mode;
			return true;

		/* case 0x13: // Tile refresh offset to left    not implemented */
		/* case 0x14: // Tile refresh offset to right   not implemented */
		/* case 0x15: // Tile refresh offset upwards    not implemented */
		/* case 0x16: // Tile refresh offset downwards  not implemented */
		/* case 0x17: // temperate snow line            not implemented */

		case 0x1A: // Always -1
			*value = UINT_MAX;
			return true;

		case 0x1B: // Display options
			*value = 0x3F; // constant fake value to avoid desync
			return true;

		case 0x1D: // TTD Platform, 00=TTDPatch, 01=OpenTTD, also used for feature tests (bits 31..4)
			*value = 1 | grffile->var9D_overlay;
			return true;

		case 0x1E: // Miscellaneous GRF features
			*value = _misc_grf_features;

			/* Add the local flags */
			assert(!HasBit(*value, GMB_TRAIN_WIDTH_32_PIXELS));
			if (_cur.grffile->traininfo_vehicle_width == VEHICLEINFO_FULL_VEHICLE_WIDTH) SetBit(*value, GMB_TRAIN_WIDTH_32_PIXELS);
			return true;

		/* case 0x1F: // locale dependent settings not implemented to avoid desync */

		case 0x20: { // snow line height
			uint8_t snowline = GetSnowLine();
			if (_settings_game.game_creation.landscape == LandscapeType::Arctic && snowline <= _settings_game.construction.map_height_limit) {
				*value = Clamp(snowline * (grffile->grf_version >= 8 ? 1 : TILE_HEIGHT), 0, 0xFE);
			} else {
				/* No snow */
				*value = 0xFF;
			}
			return true;
		}

		case 0x21: // OpenTTD version
			*value = _openttd_newgrf_version;
			return true;

		case 0x22: // difficulty level
			*value = SP_CUSTOM;
			return true;

		case 0x23: // long format date
			*value = CalTime::CurDate().base();
			return true;

		case 0x24: // long format year
			*value = CalTime::CurYear().base();
			return true;

		default: return false;
	}
}

static uint32_t GetParamVal(uint8_t param, uint32_t *cond_val)
{
	/* First handle variable common with VarAction2 */
	uint32_t value;
	if (GetGlobalVariable(param - 0x80, &value, _cur.grffile)) return value;

	/* Non-common variable */
	switch (param) {
		case 0x84: { // GRF loading stage
			uint32_t res = 0;

			if (_cur.stage > GLS_INIT) SetBit(res, 0);
			if (_cur.stage == GLS_RESERVE) SetBit(res, 8);
			if (_cur.stage == GLS_ACTIVATION) SetBit(res, 9);
			return res;
		}

		case 0x85: // TTDPatch flags, only for bit tests
			if (cond_val == nullptr) {
				/* Supported in Action 0x07 and 0x09, not 0x0D */
				return 0;
			} else {
				uint32_t index = *cond_val / 0x20;
				*cond_val %= 0x20;
				uint32_t param_val = 0;
				if (index < lengthof(_ttdpatch_flags)) {
					param_val = _ttdpatch_flags[index];
					if (!_cur.grfconfig->flags.Any({GRFConfigFlag::Static, GRFConfigFlag::System})) {
						SetBit(_observed_ttdpatch_flags[index], *cond_val);
					}
				}
				return param_val;
			}

		case 0x88: // GRF ID check
			return 0;

		/* case 0x99: Global ID offset not implemented */

		default:
			/* GRF Parameter */
			if (param < 0x80) return _cur.grffile->GetParam(param);

			/* In-game variable. */
			GrfMsg(1, "Unsupported in-game variable 0x{:02X}", param);
			return UINT_MAX;
	}
}

/* Action 0x06 */
static void CfgApply(ByteReader &buf)
{
	/* <06> <param-num> <param-size> <offset> ... <FF>
	 *
	 * B param-num     Number of parameter to substitute (First = "zero")
	 *                 Ignored if that parameter was not specified in newgrf.cfg
	 * B param-size    How many bytes to replace.  If larger than 4, the
	 *                 bytes of the following parameter are used.  In that
	 *                 case, nothing is applied unless *all* parameters
	 *                 were specified.
	 * B offset        Offset into data from beginning of next sprite
	 *                 to place where parameter is to be stored. */

	/* Preload the next sprite */
	SpriteFile &file = *_cur.file;
	size_t pos = file.GetPos();
	uint32_t num = file.GetContainerVersion() >= 2 ? file.ReadDword() : file.ReadWord();
	uint8_t type = file.ReadByte();

	/* Check if the sprite is a pseudo sprite. We can't operate on real sprites. */
	if (type != 0xFF) {
		GrfMsg(2, "CfgApply: Ignoring (next sprite is real, unsupported)");

		/* Reset the file position to the start of the next sprite */
		file.SeekTo(pos, SEEK_SET);
		return;
	}

	/* Get (or create) the override for the next sprite. */
	GRFLocation location(_cur.grfconfig->ident.grfid, _cur.nfo_line + 1);
	std::unique_ptr<uint8_t[]> &preload_sprite = _grf_line_to_action6_sprite_override[location];

	/* Load new sprite data if it hasn't already been loaded. */
	if (preload_sprite == nullptr) {
		preload_sprite = std::make_unique<uint8_t[]>(num);
		file.ReadBlock(preload_sprite.get(), num);
	}

	/* Reset the file position to the start of the next sprite */
	file.SeekTo(pos, SEEK_SET);

	/* Now perform the Action 0x06 on our data. */
	for (;;) {
		uint i;
		uint param_num;
		uint param_size;
		uint offset;
		bool add_value;

		/* Read the parameter to apply. 0xFF indicates no more data to change. */
		param_num = buf.ReadByte();
		if (param_num == 0xFF) break;

		/* Get the size of the parameter to use. If the size covers multiple
		 * double words, sequential parameter values are used. */
		param_size = buf.ReadByte();

		/* Bit 7 of param_size indicates we should add to the original value
		 * instead of replacing it. */
		add_value  = HasBit(param_size, 7);
		param_size = GB(param_size, 0, 7);

		/* Where to apply the data to within the pseudo sprite data. */
		offset     = buf.ReadExtendedByte();

		/* If the parameter is a GRF parameter (not an internal variable) check
		 * if it (and all further sequential parameters) has been defined. */
		if (param_num < 0x80 && (param_num + (param_size - 1) / 4) >= std::size(_cur.grffile->param)) {
			GrfMsg(2, "CfgApply: Ignoring (param {} not set)", (param_num + (param_size - 1) / 4));
			break;
		}

		GrfMsg(8, "CfgApply: Applying {} bytes from parameter 0x{:02X} at offset 0x{:04X}", param_size, param_num, offset);

		bool carry = false;
		for (i = 0; i < param_size && offset + i < num; i++) {
			uint32_t value = GetParamVal(param_num + i / 4, nullptr);
			/* Reset carry flag for each iteration of the variable (only really
			 * matters if param_size is greater than 4) */
			if (i % 4 == 0) carry = false;

			if (add_value) {
				uint new_value = preload_sprite[offset + i] + GB(value, (i % 4) * 8, 8) + (carry ? 1 : 0);
				preload_sprite[offset + i] = GB(new_value, 0, 8);
				/* Check if the addition overflowed */
				carry = new_value >= 256;
			} else {
				preload_sprite[offset + i] = GB(value, (i % 4) * 8, 8);
			}
		}
	}
}

/**
 * Disable a static NewGRF when it is influencing another (non-static)
 * NewGRF as this could cause desyncs.
 *
 * We could just tell the NewGRF querying that the file doesn't exist,
 * but that might give unwanted results. Disabling the NewGRF gives the
 * best result as no NewGRF author can complain about that.
 * @param c The NewGRF to disable.
 */
static void DisableStaticNewGRFInfluencingNonStaticNewGRFs(GRFConfig &c)
{
	GRFError *error = DisableGrf(STR_NEWGRF_ERROR_STATIC_GRF_CAUSES_DESYNC, &c);
	error->data = _cur.grfconfig->GetName();
}

/* Action 0x07
 * Action 0x09 */
static void SkipIf(ByteReader &buf)
{
	/* <07/09> <param-num> <param-size> <condition-type> <value> <num-sprites>
	 *
	 * B param-num
	 * B param-size
	 * B condition-type
	 * V value
	 * B num-sprites */
	uint32_t cond_val = 0;
	uint32_t mask = 0;
	bool result;

	uint8_t param     = buf.ReadByte();
	uint8_t paramsize = buf.ReadByte();
	uint8_t condtype  = buf.ReadByte();

	if (condtype < 2) {
		/* Always 1 for bit tests, the given value should be ignored. */
		paramsize = 1;
	}

	switch (paramsize) {
		case 8: cond_val = buf.ReadDWord(); mask = buf.ReadDWord(); break;
		case 4: cond_val = buf.ReadDWord(); mask = 0xFFFFFFFF; break;
		case 2: cond_val = buf.ReadWord();  mask = 0x0000FFFF; break;
		case 1: cond_val = buf.ReadByte();  mask = 0x000000FF; break;
		default: break;
	}

	if (param < 0x80 && std::size(_cur.grffile->param) <= param) {
		GrfMsg(7, "SkipIf: Param {} undefined, skipping test", param);
		return;
	}

	GrfMsg(7, "SkipIf: Test condtype {}, param 0x{:02X}, condval 0x{:08X}", condtype, param, cond_val);

	/* condtypes that do not use 'param' are always valid.
	 * condtypes that use 'param' are either not valid for param 0x88, or they are only valid for param 0x88.
	 */
	if (condtype >= 0x0B) {
		/* Tests that ignore 'param' */
		switch (condtype) {
			case 0x0B: result = !IsValidCargoType(GetCargoTypeByLabel(CargoLabel(std::byteswap(cond_val))));
				break;
			case 0x0C: result = IsValidCargoType(GetCargoTypeByLabel(CargoLabel(std::byteswap(cond_val))));
				break;
			case 0x0D: result = GetRailTypeByLabel(std::byteswap(cond_val)) == INVALID_RAILTYPE;
				break;
			case 0x0E: result = GetRailTypeByLabel(std::byteswap(cond_val)) != INVALID_RAILTYPE;
				break;
			case 0x0F: {
				RoadType rt = GetRoadTypeByLabel(std::byteswap(cond_val));
				result = rt == INVALID_ROADTYPE || !RoadTypeIsRoad(rt);
				break;
			}
			case 0x10: {
				RoadType rt = GetRoadTypeByLabel(std::byteswap(cond_val));
				result = rt != INVALID_ROADTYPE && RoadTypeIsRoad(rt);
				break;
			}
			case 0x11: {
				RoadType rt = GetRoadTypeByLabel(std::byteswap(cond_val));
				result = rt == INVALID_ROADTYPE || !RoadTypeIsTram(rt);
				break;
			}
			case 0x12: {
				RoadType rt = GetRoadTypeByLabel(std::byteswap(cond_val));
				result = rt != INVALID_ROADTYPE && RoadTypeIsTram(rt);
				break;
			}
			default: GrfMsg(1, "SkipIf: Unsupported condition type {:02X}. Ignoring", condtype); return;
		}
	} else if (param == 0x88) {
		/* GRF ID checks */

		GRFConfig *c = GetGRFConfig(cond_val, mask);

		if (c != nullptr && c->flags.Test(GRFConfigFlag::Static) && !_cur.grfconfig->flags.Test(GRFConfigFlag::Static) && _networking) {
			DisableStaticNewGRFInfluencingNonStaticNewGRFs(*c);
			c = nullptr;
		}

		if (condtype != 10 && c == nullptr) {
			GrfMsg(7, "SkipIf: GRFID 0x{:08X} unknown, skipping test", std::byteswap(cond_val));
			return;
		}

		switch (condtype) {
			/* Tests 0x06 to 0x0A are only for param 0x88, GRFID checks */
			case 0x06: // Is GRFID active?
				result = c->status == GCS_ACTIVATED;
				break;

			case 0x07: // Is GRFID non-active?
				result = c->status != GCS_ACTIVATED;
				break;

			case 0x08: // GRFID is not but will be active?
				result = c->status == GCS_INITIALISED;
				break;

			case 0x09: // GRFID is or will be active?
				result = c->status == GCS_ACTIVATED || c->status == GCS_INITIALISED;
				break;

			case 0x0A: // GRFID is not nor will be active
				/* This is the only condtype that doesn't get ignored if the GRFID is not found */
				result = c == nullptr || c->status == GCS_DISABLED || c->status == GCS_NOT_FOUND;
				break;

			default: GrfMsg(1, "SkipIf: Unsupported GRF condition type {:02X}. Ignoring", condtype); return;
		}
	} else if (param == 0x91 && (condtype == 0x02 || condtype == 0x03) && cond_val > 0) {
		const std::vector<uint32_t> &values = _cur.grffile->var91_values;
		/* condtype 0x02: skip if test result found
		 * condtype 0x03: skip if test result not found
		 */
		bool found = std::find(values.begin(), values.end(), cond_val) != values.end();
		result = (found == (condtype == 0x02));
	} else {
		/* Tests that use 'param' and are not GRF ID checks.  */
		uint32_t param_val = GetParamVal(param, &cond_val); // cond_val is modified for param == 0x85
		switch (condtype) {
			case 0x00: result = !!(param_val & (1 << cond_val));
				break;
			case 0x01: result = !(param_val & (1 << cond_val));
				break;
			case 0x02: result = (param_val & mask) == cond_val;
				break;
			case 0x03: result = (param_val & mask) != cond_val;
				break;
			case 0x04: result = (param_val & mask) < cond_val;
				break;
			case 0x05: result = (param_val & mask) > cond_val;
				break;
			default: GrfMsg(1, "SkipIf: Unsupported condition type {:02X}. Ignoring", condtype); return;
		}
	}

	if (!result) {
		GrfMsg(2, "SkipIf: Not skipping sprites, test was false");
		return;
	}

	uint8_t numsprites = buf.ReadByte();

	/* numsprites can be a GOTO label if it has been defined in the GRF
	 * file. The jump will always be the first matching label that follows
	 * the current nfo_line. If no matching label is found, the first matching
	 * label in the file is used. */
	const GRFLabel *choice = nullptr;
	for (const auto &label : _cur.grffile->labels) {
		if (label.label != numsprites) continue;

		/* Remember a goto before the current line */
		if (choice == nullptr) choice = &label;
		/* If we find a label here, this is definitely good */
		if (label.nfo_line > _cur.nfo_line) {
			choice = &label;
			break;
		}
	}

	if (choice != nullptr) {
		GrfMsg(2, "SkipIf: Jumping to label 0x{:X} at line {}, test was true", choice->label, choice->nfo_line);
		_cur.file->SeekTo(choice->pos, SEEK_SET);
		_cur.nfo_line = choice->nfo_line;
		return;
	}

	GrfMsg(2, "SkipIf: Skipping {} sprites, test was true", numsprites);
	_cur.skip_sprites = numsprites;
	if (_cur.skip_sprites == 0) {
		/* Zero means there are no sprites to skip, so
		 * we use -1 to indicate that all further
		 * sprites should be skipped. */
		_cur.skip_sprites = -1;

		/* If an action 8 hasn't been encountered yet, disable the grf. */
		if (_cur.grfconfig->status != (_cur.stage < GLS_RESERVE ? GCS_INITIALISED : GCS_ACTIVATED)) {
			DisableGrf();
		}
	}
}


/* Action 0x08 (GLS_FILESCAN) */
static void ScanInfo(ByteReader &buf)
{
	uint8_t grf_version = buf.ReadByte();
	uint32_t grfid      = buf.ReadDWord();
	std::string_view name = buf.ReadString();

	_cur.grfconfig->ident.grfid = grfid;

	if (grf_version < 2 || grf_version > 8) {
		_cur.grfconfig->flags.Set(GRFConfigFlag::Invalid);
		Debug(grf, 0, "{}: NewGRF \"{}\" (GRFID {:08X}) uses GRF version {}, which is incompatible with this version of OpenTTD.", _cur.grfconfig->GetDisplayPath(), StrMakeValid(name), std::byteswap(grfid), grf_version);
	}

	/* GRF IDs starting with 0xFF are reserved for internal TTDPatch use */
	if (GB(grfid, 0, 8) == 0xFF) _cur.grfconfig->flags.Set(GRFConfigFlag::System);

	AddGRFTextToList(_cur.grfconfig->name, 0x7F, grfid, false, name);

	if (buf.HasData()) {
		std::string_view info = buf.ReadString();
		AddGRFTextToList(_cur.grfconfig->info, 0x7F, grfid, true, info);
	}

	/* GLS_INFOSCAN only looks for the action 8, so we can skip the rest of the file */
	_cur.skip_sprites = -1;
}

/* Action 0x08 */
static void GRFInfo(ByteReader &buf)
{
	/* <08> <version> <grf-id> <name> <info>
	 *
	 * B version       newgrf version, currently 06
	 * 4*B grf-id      globally unique ID of this .grf file
	 * S name          name of this .grf set
	 * S info          string describing the set, and e.g. author and copyright */

	uint8_t version    = buf.ReadByte();
	uint32_t grfid     = buf.ReadDWord();
	std::string_view name = buf.ReadString();

	if (_cur.stage < GLS_RESERVE && _cur.grfconfig->status != GCS_UNKNOWN) {
		DisableGrf(STR_NEWGRF_ERROR_MULTIPLE_ACTION_8);
		return;
	}

	if (_cur.grffile->grfid != grfid) {
		Debug(grf, 0, "GRFInfo: GRFID {:08X} in FILESCAN stage does not match GRFID {:08X} in INIT/RESERVE/ACTIVATION stage", std::byteswap(_cur.grffile->grfid), std::byteswap(grfid));
		_cur.grffile->grfid = grfid;
	}

	_cur.grffile->grf_version = version;
	_cur.grfconfig->status = _cur.stage < GLS_RESERVE ? GCS_INITIALISED : GCS_ACTIVATED;

	/* Do swap the GRFID for displaying purposes since people expect that */
	Debug(grf, 1, "GRFInfo: Loaded GRFv{} set {:08X} - {} (palette: {}, version: {})", version, std::byteswap(grfid), StrMakeValid(name), (_cur.grfconfig->palette & GRFP_USE_MASK) ? "Windows" : "DOS", _cur.grfconfig->version);
}

/**
 * Check if a sprite ID range is within the GRM reversed range for the currently loading NewGRF.
 * @param first_sprite First sprite of range.
 * @param num_sprites Number of sprites in the range.
 * @return True iff the NewGRF has reserved a range equal to or greater than the provided range.
 */
static bool IsGRMReservedSprite(SpriteID first_sprite, uint16_t num_sprites)
{
	for (const auto &grm_sprite : _grm_sprites) {
		if (grm_sprite.first.grfid != _cur.grffile->grfid) continue;
		if (grm_sprite.second.first <= first_sprite && grm_sprite.second.first + grm_sprite.second.second >= first_sprite + num_sprites) return true;
	}
	return false;
}

/* Action 0x0A */
static void SpriteReplace(ByteReader &buf)
{
	/* <0A> <num-sets> <set1> [<set2> ...]
	 * <set>: <num-sprites> <first-sprite>
	 *
	 * B num-sets      How many sets of sprites to replace.
	 * Each set:
	 * B num-sprites   How many sprites are in this set
	 * W first-sprite  First sprite number to replace */

	uint8_t num_sets = buf.ReadByte();

	for (uint i = 0; i < num_sets; i++) {
		uint8_t num_sprites = buf.ReadByte();
		uint16_t first_sprite = buf.ReadWord();

		GrfMsg(2, "SpriteReplace: [Set {}] Changing {} sprites, beginning with {}",
			i, num_sprites, first_sprite
		);

		if (first_sprite + num_sprites >= SPR_OPENTTD_BASE) {
			/* Outside allowed range, check for GRM sprite reservations. */
			if (!IsGRMReservedSprite(first_sprite, num_sprites)) {
				GrfMsg(0, "SpriteReplace: [Set {}] Changing {} sprites, beginning with {}, above limit of {} and not within reserved range, ignoring.",
					i, num_sprites, first_sprite, SPR_OPENTTD_BASE);

				for (uint j = 0; j < num_sprites; j++) {
					_cur.nfo_line++;
					LoadNextSprite(INVALID_SPRITE_ID, *_cur.file, _cur.nfo_line);
				}
				return;
			}
		}

		for (uint j = 0; j < num_sprites; j++) {
			SpriteID load_index = first_sprite + j;
			_cur.nfo_line++;
			if (load_index < (int)SPR_PROGSIGNAL_BASE || load_index >= (int)SPR_NEWGRFS_BASE) {
				LoadNextSprite(load_index, *_cur.file, _cur.nfo_line); // XXX
			} else {
				/* Skip sprite */
				GrfMsg(0, "SpriteReplace: Ignoring attempt to replace protected sprite ID: {}", load_index);
				LoadNextSprite(INVALID_SPRITE_ID, *_cur.file, _cur.nfo_line);
			}

			/* Shore sprites now located at different addresses.
			 * So detect when the old ones get replaced. */
			if (IsInsideMM(load_index, SPR_ORIGINALSHORE_START, SPR_ORIGINALSHORE_END + 1)) {
				if (_loaded_newgrf_features.shore != SHORE_REPLACE_ACTION_5) _loaded_newgrf_features.shore = SHORE_REPLACE_ACTION_A;
			}
		}
	}
}

/* Action 0x0A (SKIP) */
static void SkipActA(ByteReader &buf)
{
	uint8_t num_sets = buf.ReadByte();

	for (uint i = 0; i < num_sets; i++) {
		/* Skip the sprites this replaces */
		_cur.skip_sprites += buf.ReadByte();
		/* But ignore where they go */
		buf.ReadWord();
	}

	GrfMsg(3, "SkipActA: Skipping {} sprites", _cur.skip_sprites);
}

/* Action 0x0B */
static void GRFLoadError(ByteReader &buf)
{
	/* <0B> <severity> <language-id> <message-id> [<message...> 00] [<data...>] 00 [<parnum>]
	 *
	 * B severity      00: notice, continue loading grf file
	 *                 01: warning, continue loading grf file
	 *                 02: error, but continue loading grf file, and attempt
	 *                     loading grf again when loading or starting next game
	 *                 03: error, abort loading and prevent loading again in
	 *                     the future (only when restarting the patch)
	 * B language-id   see action 4, use 1F for built-in error messages
	 * B message-id    message to show, see below
	 * S message       for custom messages (message-id FF), text of the message
	 *                 not present for built-in messages.
	 * V data          additional data for built-in (or custom) messages
	 * B parnum        parameter numbers to be shown in the message (maximum of 2) */

	static const StringID msgstr[] = {
		STR_NEWGRF_ERROR_VERSION_NUMBER,
		STR_NEWGRF_ERROR_DOS_OR_WINDOWS,
		STR_NEWGRF_ERROR_UNSET_SWITCH,
		STR_NEWGRF_ERROR_INVALID_PARAMETER,
		STR_NEWGRF_ERROR_LOAD_BEFORE,
		STR_NEWGRF_ERROR_LOAD_AFTER,
		STR_NEWGRF_ERROR_OTTD_VERSION_NUMBER,
	};

	static const StringID sevstr[] = {
		STR_NEWGRF_ERROR_MSG_INFO,
		STR_NEWGRF_ERROR_MSG_WARNING,
		STR_NEWGRF_ERROR_MSG_ERROR,
		STR_NEWGRF_ERROR_MSG_FATAL
	};

	uint8_t severity   = buf.ReadByte();
	uint8_t lang       = buf.ReadByte();
	uint8_t message_id = buf.ReadByte();

	/* Skip the error if it isn't valid for the current language. */
	if (!CheckGrfLangID(lang, _cur.grffile->grf_version)) return;

	/* Skip the error until the activation stage unless bit 7 of the severity
	 * is set. */
	if (!HasBit(severity, 7) && _cur.stage == GLS_INIT) {
		GrfMsg(7, "GRFLoadError: Skipping non-fatal GRFLoadError in stage {}", _cur.stage);
		return;
	}
	ClrBit(severity, 7);

	if (severity >= lengthof(sevstr)) {
		GrfMsg(7, "GRFLoadError: Invalid severity id {}. Setting to 2 (non-fatal error).", severity);
		severity = 2;
	} else if (severity == 3) {
		/* This is a fatal error, so make sure the GRF is deactivated and no
		 * more of it gets loaded. */
		DisableGrf();

		/* Make sure we show fatal errors, instead of silly infos from before */
		_cur.grfconfig->error.reset();
	}

	if (message_id >= lengthof(msgstr) && message_id != 0xFF) {
		GrfMsg(7, "GRFLoadError: Invalid message id.");
		return;
	}

	if (buf.Remaining() <= 1) {
		GrfMsg(7, "GRFLoadError: No message data supplied.");
		return;
	}

	/* For now we can only show one message per newgrf file. */
	if (_cur.grfconfig->error.has_value()) return;

	_cur.grfconfig->error = {sevstr[severity]};
	GRFError *error = &_cur.grfconfig->error.value();

	if (message_id == 0xFF) {
		/* This is a custom error message. */
		if (buf.HasData()) {
			std::string_view message = buf.ReadString();

			error->custom_message = TranslateTTDPatchCodes(_cur.grffile->grfid, lang, true, message, SCC_RAW_STRING_POINTER);
		} else {
			GrfMsg(7, "GRFLoadError: No custom message supplied.");
			error->custom_message.clear();
		}
	} else {
		error->message = msgstr[message_id];
	}

	if (buf.HasData()) {
		std::string_view data = buf.ReadString();

		error->data = TranslateTTDPatchCodes(_cur.grffile->grfid, lang, true, data);
	} else {
		GrfMsg(7, "GRFLoadError: No message data supplied.");
		error->data.clear();
	}

	/* Only two parameter numbers can be used in the string. */
	for (uint i = 0; i < error->param_value.size() && buf.HasData(); i++) {
		uint param_number = buf.ReadByte();
		error->param_value[i] = _cur.grffile->GetParam(param_number);
	}
}

/* Action 0x0C */
static void GRFComment(ByteReader &buf)
{
	/* <0C> [<ignored...>]
	 *
	 * V ignored       Anything following the 0C is ignored */

	if (!buf.HasData()) return;

	std::string_view text = buf.ReadString();
	GrfMsg(2, "GRFComment: {}", StrMakeValid(text));
}

/* Action 0x0D (GLS_SAFETYSCAN) */
static void SafeParamSet(ByteReader &buf)
{
	uint8_t target = buf.ReadByte();

	/* Writing GRF parameters and some bits of 'misc GRF features' are safe. */
	if (target < 0x80 || target == 0x9E) return;

	/* GRM could be unsafe, but as here it can only happen after other GRFs
	 * are loaded, it should be okay. If the GRF tried to use the slots it
	 * reserved, it would be marked unsafe anyway. GRM for (e.g. bridge)
	 * sprites  is considered safe. */

	_cur.grfconfig->flags.Set(GRFConfigFlag::Unsafe);

	/* Skip remainder of GRF */
	_cur.skip_sprites = -1;
}


static uint32_t GetPatchVariable(uint8_t param)
{
	switch (param) {
		/* start year - 1920 */
		case 0x0B: return (std::max(_settings_game.game_creation.starting_year, CalTime::ORIGINAL_BASE_YEAR) - CalTime::ORIGINAL_BASE_YEAR).base();

		/* freight trains weight factor */
		case 0x0E: return _settings_game.vehicle.freight_trains;

		/* empty wagon speed increase */
		case 0x0F: return 0;

		/* plane speed factor; our patch option is reversed from TTDPatch's,
		 * the following is good for 1x, 2x and 4x (most common?) and...
		 * well not really for 3x. */
		case 0x10:
			switch (_settings_game.vehicle.plane_speed) {
				default:
				case 4: return 1;
				case 3: return 2;
				case 2: return 2;
				case 1: return 4;
			}


		/* 2CC colourmap base sprite */
		case 0x11: return SPR_2CCMAP_BASE;

		/* map size: format = -MABXYSS
		 * M  : the type of map
		 *       bit 0 : set   : squared map. Bit 1 is now not relevant
		 *               clear : rectangle map. Bit 1 will indicate the bigger edge of the map
		 *       bit 1 : set   : Y is the bigger edge. Bit 0 is clear
		 *               clear : X is the bigger edge.
		 * A  : minimum edge(log2) of the map
		 * B  : maximum edge(log2) of the map
		 * XY : edges(log2) of each side of the map.
		 * SS : combination of both X and Y, thus giving the size(log2) of the map
		 */
		case 0x13: {
			uint8_t map_bits = 0;
			uint8_t log_X = Map::LogX() - 6; // subtraction is required to make the minimal size (64) zero based
			uint8_t log_Y = Map::LogY() - 6;
			uint8_t max_edge = std::max(log_X, log_Y);

			if (log_X == log_Y) { // we have a squared map, since both edges are identical
				SetBit(map_bits, 0);
			} else {
				if (max_edge == log_Y) SetBit(map_bits, 1); // edge Y been the biggest, mark it
			}

			return (map_bits << 24) | (std::min(log_X, log_Y) << 20) | (max_edge << 16) |
				(log_X << 12) | (log_Y << 8) | (log_X + log_Y);
		}

		/* The maximum height of the map. */
		case 0x14:
			return _settings_game.construction.map_height_limit;

		/* Extra foundations base sprite */
		case 0x15:
			return SPR_SLOPES_BASE;

		/* Shore base sprite */
		case 0x16:
			return SPR_SHORE_BASE;

		/* Game map seed */
		case 0x17:
			return _settings_game.game_creation.generation_seed;

		default:
			GrfMsg(2, "ParamSet: Unknown Patch variable 0x{:02X}.", param);
			return 0;
	}
}


static uint32_t PerformGRM(uint32_t *grm, uint16_t num_ids, uint16_t count, uint8_t op, uint8_t target, const char *type)
{
	uint start = 0;
	uint size  = 0;

	if (op == 6) {
		/* Return GRFID of set that reserved ID */
		return grm[_cur.grffile->GetParam(target)];
	}

	/* With an operation of 2 or 3, we want to reserve a specific block of IDs */
	if (op == 2 || op == 3) start = _cur.grffile->GetParam(target);

	for (uint i = start; i < num_ids; i++) {
		if (grm[i] == 0) {
			size++;
		} else {
			if (op == 2 || op == 3) break;
			start = i + 1;
			size = 0;
		}

		if (size == count) break;
	}

	if (size == count) {
		/* Got the slot... */
		if (op == 0 || op == 3) {
			GrfMsg(2, "ParamSet: GRM: Reserving {} {} at {}", count, type, start);
			for (uint i = 0; i < count; i++) grm[start + i] = _cur.grffile->grfid;
		}
		return start;
	}

	/* Unable to allocate */
	if (op != 4 && op != 5) {
		/* Deactivate GRF */
		GrfMsg(0, "ParamSet: GRM: Unable to allocate {} {}, deactivating", count, type);
		DisableGrf(STR_NEWGRF_ERROR_GRM_FAILED);
		return UINT_MAX;
	}

	GrfMsg(1, "ParamSet: GRM: Unable to allocate {} {}", count, type);
	return UINT_MAX;
}


/** Action 0x0D: Set parameter */
static void ParamSet(ByteReader &buf)
{
	/* <0D> <target> <operation> <source1> <source2> [<data>]
	 *
	 * B target        parameter number where result is stored
	 * B operation     operation to perform, see below
	 * B source1       first source operand
	 * B source2       second source operand
	 * D data          data to use in the calculation, not necessary
	 *                 if both source1 and source2 refer to actual parameters
	 *
	 * Operations
	 * 00      Set parameter equal to source1
	 * 01      Addition, source1 + source2
	 * 02      Subtraction, source1 - source2
	 * 03      Unsigned multiplication, source1 * source2 (both unsigned)
	 * 04      Signed multiplication, source1 * source2 (both signed)
	 * 05      Unsigned bit shift, source1 by source2 (source2 taken to be a
	 *         signed quantity; left shift if positive and right shift if
	 *         negative, source1 is unsigned)
	 * 06      Signed bit shift, source1 by source2
	 *         (source2 like in 05, and source1 as well)
	 */

	uint8_t target = buf.ReadByte();
	uint8_t oper   = buf.ReadByte();
	uint32_t src1  = buf.ReadByte();
	uint32_t src2  = buf.ReadByte();

	uint32_t data = 0;
	if (buf.Remaining() >= 4) data = buf.ReadDWord();

	/* You can add 80 to the operation to make it apply only if the target
	 * is not defined yet.  In this respect, a parameter is taken to be
	 * defined if any of the following applies:
	 * - it has been set to any value in the newgrf(w).cfg parameter list
	 * - it OR A PARAMETER WITH HIGHER NUMBER has been set to any value by
	 *   an earlier action D */
	if (HasBit(oper, 7)) {
		if (target < 0x80 && target < std::size(_cur.grffile->param)) {
			GrfMsg(7, "ParamSet: Param {} already defined, skipping", target);
			return;
		}

		oper = GB(oper, 0, 7);
	}

	if (src2 == 0xFE) {
		if (GB(data, 0, 8) == 0xFF) {
			if (data == 0x0000FFFF) {
				/* Patch variables */
				src1 = GetPatchVariable(src1);
			} else {
				/* GRF Resource Management */
				uint8_t  op      = src1;
				GrfSpecFeatureRef feature_ref = ReadFeature(GB(data, 8, 8));
				GrfSpecFeature feature = feature_ref.id;
				uint16_t count   = GB(data, 16, 16);

				if (_cur.stage == GLS_RESERVE) {
					if (feature == 0x08) {
						/* General sprites */
						if (op == 0) {
							/* Check if the allocated sprites will fit below the original sprite limit */
							if (_cur.spriteid + count >= 16384) {
								GrfMsg(0, "ParamSet: GRM: Unable to allocate {} sprites; try changing NewGRF order", count);
								DisableGrf(STR_NEWGRF_ERROR_GRM_FAILED);
								return;
							}

							/* Reserve space at the current sprite ID */
							GrfMsg(4, "ParamSet: GRM: Allocated {} sprites at {}", count, _cur.spriteid);
							_grm_sprites[GRFLocation(_cur.grffile->grfid, _cur.nfo_line)] = std::make_pair(_cur.spriteid, count);
							_cur.spriteid += count;
						}
					}
					/* Ignore GRM result during reservation */
					src1 = 0;
				} else if (_cur.stage == GLS_ACTIVATION) {
					switch (feature) {
						case 0x00: // Trains
						case 0x01: // Road Vehicles
						case 0x02: // Ships
						case 0x03: // Aircraft
							if (!_settings_game.vehicle.dynamic_engines) {
								src1 = PerformGRM(&_grm_engines[_engine_offsets[feature]], _engine_counts[feature], count, op, target, "vehicles");
								if (_cur.skip_sprites == -1) return;
							} else {
								/* GRM does not apply for dynamic engine allocation. */
								switch (op) {
									case 2:
									case 3:
										src1 = _cur.grffile->GetParam(target);
										break;

									default:
										src1 = 0;
										break;
								}
							}
							break;

						case 0x08: // General sprites
							switch (op) {
								case 0: {
									/* Return space reserved during reservation stage */
									const auto &grm_alloc = _grm_sprites[GRFLocation(_cur.grffile->grfid, _cur.nfo_line)];
									src1 = grm_alloc.first;
									GrfMsg(4, "ParamSet: GRM: Using pre-allocated sprites at {} (count: {})", src1, grm_alloc.second);
									break;
								}

								case 1:
									src1 = _cur.spriteid;
									break;

								default:
									GrfMsg(1, "ParamSet: GRM: Unsupported operation {} for general sprites", op);
									return;
							}
							break;

						case 0x0B: // Cargo
							/* There are two ranges: one for cargo IDs and one for cargo bitmasks */
							src1 = PerformGRM(_grm_cargoes, NUM_CARGO * 2, count, op, target, "cargoes");
							if (_cur.skip_sprites == -1) return;
							break;

						default: GrfMsg(1, "ParamSet: GRM: Unsupported feature {}", GetFeatureString(feature_ref)); return;
					}
				} else {
					/* Ignore GRM during initialization */
					src1 = 0;
				}
			}
		} else {
			/* Read another GRF File's parameter */
			const GRFFile *file = GetFileByGRFID(data);
			GRFConfig *c = GetGRFConfig(data);
			if (c != nullptr && c->flags.Test(GRFConfigFlag::Static) && !_cur.grfconfig->flags.Test(GRFConfigFlag::Static) && _networking) {
				/* Disable the read GRF if it is a static NewGRF. */
				DisableStaticNewGRFInfluencingNonStaticNewGRFs(*c);
				src1 = 0;
			} else if (file == nullptr || c == nullptr || c->status == GCS_DISABLED) {
				src1 = 0;
			} else if (src1 == 0xFE) {
				src1 = c->version;
			} else {
				src1 = file->GetParam(src1);
			}
		}
	} else {
		/* The source1 and source2 operands refer to the grf parameter number
		 * like in action 6 and 7.  In addition, they can refer to the special
		 * variables available in action 7, or they can be FF to use the value
		 * of <data>.  If referring to parameters that are undefined, a value
		 * of 0 is used instead.  */
		src1 = (src1 == 0xFF) ? data : GetParamVal(src1, nullptr);
		src2 = (src2 == 0xFF) ? data : GetParamVal(src2, nullptr);
	}

	uint32_t res;
	switch (oper) {
		case 0x00:
			res = src1;
			break;

		case 0x01:
			res = src1 + src2;
			break;

		case 0x02:
			res = src1 - src2;
			break;

		case 0x03:
			res = src1 * src2;
			break;

		case 0x04:
			res = (int32_t)src1 * (int32_t)src2;
			break;

		case 0x05:
			if ((int32_t)src2 < 0) {
				res = src1 >> -(int32_t)src2;
			} else {
				res = src1 << (src2 & 0x1F); // Same behaviour as in EvalAdjustT, mask 'value' to 5 bits, which should behave the same on all architectures.
			}
			break;

		case 0x06:
			if ((int32_t)src2 < 0) {
				res = (int32_t)src1 >> -(int32_t)src2;
			} else {
				res = (int32_t)src1 << (src2 & 0x1F); // Same behaviour as in EvalAdjustT, mask 'value' to 5 bits, which should behave the same on all architectures.
			}
			break;

		case 0x07: // Bitwise AND
			res = src1 & src2;
			break;

		case 0x08: // Bitwise OR
			res = src1 | src2;
			break;

		case 0x09: // Unsigned division
			if (src2 == 0) {
				res = src1;
			} else {
				res = src1 / src2;
			}
			break;

		case 0x0A: // Signed division
			if (src2 == 0) {
				res = src1;
			} else {
				res = (int32_t)src1 / (int32_t)src2;
			}
			break;

		case 0x0B: // Unsigned modulo
			if (src2 == 0) {
				res = src1;
			} else {
				res = src1 % src2;
			}
			break;

		case 0x0C: // Signed modulo
			if (src2 == 0) {
				res = src1;
			} else {
				res = (int32_t)src1 % (int32_t)src2;
			}
			break;

		default: GrfMsg(0, "ParamSet: Unknown operation {}, skipping", oper); return;
	}

	switch (target) {
		case 0x8E: // Y-Offset for train sprites
			_cur.grffile->traininfo_vehicle_pitch = res;
			break;

		case 0x8F: { // Rail track type cost factors
			extern RailTypeInfo _railtypes[RAILTYPE_END];
			_railtypes[RAILTYPE_RAIL].cost_multiplier = GB(res, 0, 8);
			if (_settings_game.vehicle.disable_elrails) {
				_railtypes[RAILTYPE_ELECTRIC].cost_multiplier = GB(res, 0, 8);
				_railtypes[RAILTYPE_MONO].cost_multiplier = GB(res, 8, 8);
			} else {
				_railtypes[RAILTYPE_ELECTRIC].cost_multiplier = GB(res, 8, 8);
				_railtypes[RAILTYPE_MONO].cost_multiplier = GB(res, 16, 8);
			}
			_railtypes[RAILTYPE_MAGLEV].cost_multiplier = GB(res, 16, 8);
			break;
		}

		/* not implemented */
		case 0x93: // Tile refresh offset to left -- Intended to allow support for larger sprites, not necessary for OTTD
		case 0x94: // Tile refresh offset to right
		case 0x95: // Tile refresh offset upwards
		case 0x96: // Tile refresh offset downwards
		case 0x97: // Snow line height -- Better supported by feature 8 property 10h (snow line table) TODO: implement by filling the entire snow line table with the given value
		case 0x99: // Global ID offset -- Not necessary since IDs are remapped automatically
			GrfMsg(7, "ParamSet: Skipping unimplemented target 0x{:02X}", target);
			break;

		case 0x9E: // Miscellaneous GRF features
			/* Set train list engine width */
			_cur.grffile->traininfo_vehicle_width = HasBit(res, GMB_TRAIN_WIDTH_32_PIXELS) ? VEHICLEINFO_FULL_VEHICLE_WIDTH : TRAININFO_DEFAULT_VEHICLE_WIDTH;
			/* Remove the local flags from the global flags */
			ClrBit(res, GMB_TRAIN_WIDTH_32_PIXELS);

			/* Only copy safe bits for static grfs */
			if (_cur.grfconfig->flags.Test(GRFConfigFlag::Static)) {
				uint32_t safe_bits = 0;
				SetBit(safe_bits, GMB_SECOND_ROCKY_TILE_SET);

				_misc_grf_features = (_misc_grf_features & ~safe_bits) | (res & safe_bits);
			} else {
				_misc_grf_features = res;
			}
			break;

		case 0x9F: // locale-dependent settings
			GrfMsg(7, "ParamSet: Skipping unimplemented target 0x{:02X}", target);
			break;

		default:
			if (target < 0x80) {
				/* Resize (and fill with zeroes) if needed. */
				if (target >= std::size(_cur.grffile->param)) _cur.grffile->param.resize(target + 1);
				_cur.grffile->param[target] = res;
			} else {
				GrfMsg(7, "ParamSet: Skipping unknown target 0x{:02X}", target);
			}
			break;
	}
}

/* Action 0x0E (GLS_SAFETYSCAN) */
static void SafeGRFInhibit(ByteReader &buf)
{
	/* <0E> <num> <grfids...>
	 *
	 * B num           Number of GRFIDs that follow
	 * D grfids        GRFIDs of the files to deactivate */

	uint8_t num = buf.ReadByte();

	for (uint i = 0; i < num; i++) {
		uint32_t grfid = buf.ReadDWord();

		/* GRF is unsafe it if tries to deactivate other GRFs */
		if (grfid != _cur.grfconfig->ident.grfid) {
			_cur.grfconfig->flags.Set(GRFConfigFlag::Unsafe);

			/* Skip remainder of GRF */
			_cur.skip_sprites = -1;

			return;
		}
	}
}

/* Action 0x0E */
static void GRFInhibit(ByteReader &buf)
{
	/* <0E> <num> <grfids...>
	 *
	 * B num           Number of GRFIDs that follow
	 * D grfids        GRFIDs of the files to deactivate */

	uint8_t num = buf.ReadByte();

	for (uint i = 0; i < num; i++) {
		uint32_t grfid = buf.ReadDWord();
		GRFConfig *file = GetGRFConfig(grfid);

		/* Unset activation flag */
		if (file != nullptr && file != _cur.grfconfig) {
			GrfMsg(2, "GRFInhibit: Deactivating file '{}'", file->GetDisplayPath());
			GRFError *error = DisableGrf(STR_NEWGRF_ERROR_FORCEFULLY_DISABLED, file);
			error->data = _cur.grfconfig->GetName();
		}
	}
}

/** Action 0x0F - Define Town names */
static void FeatureTownName(ByteReader &buf)
{
	/* <0F> <id> <style-name> <num-parts> <parts>
	 *
	 * B id          ID of this definition in bottom 7 bits (final definition if bit 7 set)
	 * V style-name  Name of the style (only for final definition)
	 * B num-parts   Number of parts in this definition
	 * V parts       The parts */

	uint32_t grfid = _cur.grffile->grfid;

	GRFTownName *townname = AddGRFTownName(grfid);

	uint8_t id = buf.ReadByte();
	GrfMsg(6, "FeatureTownName: definition 0x{:02X}", id & 0x7F);

	if (HasBit(id, 7)) {
		/* Final definition */
		ClrBit(id, 7);
		bool new_scheme = _cur.grffile->grf_version >= 7;

		uint8_t lang = buf.ReadByte();
		StringID style = STR_UNDEFINED;

		do {
			ClrBit(lang, 7);

			std::string_view name = buf.ReadString();

			std::string lang_name = TranslateTTDPatchCodes(grfid, lang, false, name);
			GrfMsg(6, "FeatureTownName: lang 0x{:X} -> '{}'", lang, lang_name);

			style = AddGRFString(grfid, GRFStringID{id}, lang, new_scheme, false, name, STR_UNDEFINED);

			lang = buf.ReadByte();
		} while (lang != 0);
		townname->styles.emplace_back(style, id);
	}

	uint8_t parts = buf.ReadByte();
	GrfMsg(6, "FeatureTownName: {} parts", parts);

	townname->partlists[id].reserve(parts);
	for (uint partnum = 0; partnum < parts; partnum++) {
		NamePartList &partlist = townname->partlists[id].emplace_back();
		uint8_t texts = buf.ReadByte();
		partlist.bitstart = buf.ReadByte();
		partlist.bitcount = buf.ReadByte();
		partlist.maxprob  = 0;
		GrfMsg(6, "FeatureTownName: part {} contains {} texts and will use GB(seed, {}, {})", partnum, texts, partlist.bitstart, partlist.bitcount);

		partlist.parts.reserve(texts);
		for (uint textnum = 0; textnum < texts; textnum++) {
			NamePart &part = partlist.parts.emplace_back();
			part.prob = buf.ReadByte();

			if (HasBit(part.prob, 7)) {
				uint8_t ref_id = buf.ReadByte();
				if (ref_id >= GRFTownName::MAX_LISTS || townname->partlists[ref_id].empty()) {
					GrfMsg(0, "FeatureTownName: definition 0x{:02X} doesn't exist, deactivating", ref_id);
					DelGRFTownName(grfid);
					DisableGrf(STR_NEWGRF_ERROR_INVALID_ID);
					return;
				}
				part.id = ref_id;
				GrfMsg(6, "FeatureTownName: part {}, text {}, uses intermediate definition 0x{:02X} (with probability {})", partnum, textnum, ref_id, part.prob & 0x7F);
			} else {
				std::string_view text = buf.ReadString();
				part.text = TranslateTTDPatchCodes(grfid, 0, false, text);
				GrfMsg(6, "FeatureTownName: part {}, text {}, '{}' (with probability {})", partnum, textnum, part.text, part.prob);
			}
			partlist.maxprob += GB(part.prob, 0, 7);
		}
		GrfMsg(6, "FeatureTownName: part {}, total probability {}", partnum, partlist.maxprob);
	}
}

/** Action 0x10 - Define goto label */
static void DefineGotoLabel(ByteReader &buf)
{
	/* <10> <label> [<comment>]
	 *
	 * B label      The label to define
	 * V comment    Optional comment - ignored */

	uint8_t nfo_label = buf.ReadByte();

	_cur.grffile->labels.emplace_back(nfo_label, _cur.nfo_line, _cur.file->GetPos());

	GrfMsg(2, "DefineGotoLabel: GOTO target with label 0x{:02X}", nfo_label);
}

/**
 * Process a sound import from another GRF file.
 * @param sound Destination for sound.
 */
static void ImportGRFSound(SoundEntry *sound)
{
	const GRFFile *file;
	uint32_t grfid = _cur.file->ReadDword();
	SoundID sound_id = _cur.file->ReadWord();

	file = GetFileByGRFID(grfid);
	if (file == nullptr || file->sound_offset == 0) {
		GrfMsg(1, "ImportGRFSound: Source file not available");
		return;
	}

	if (sound_id >= file->num_sounds) {
		GrfMsg(1, "ImportGRFSound: Sound effect {} is invalid", sound_id);
		return;
	}

	GrfMsg(2, "ImportGRFSound: Copying sound {} ({}) from file {:x}", sound_id, file->sound_offset + sound_id, grfid);

	*sound = *GetSound(file->sound_offset + sound_id);

	/* Reset volume and priority, which TTDPatch doesn't copy */
	sound->volume = SOUND_EFFECT_MAX_VOLUME;
	sound->priority = 0;
}

/**
 * Load a sound from a file.
 * @param offs File offset to read sound from.
 * @param sound Destination for sound.
 */
static void LoadGRFSound(size_t offs, SoundEntry *sound)
{
	/* Set default volume and priority */
	sound->volume = SOUND_EFFECT_MAX_VOLUME;
	sound->priority = 0;

	if (offs != SIZE_MAX) {
		/* Sound is present in the NewGRF. */
		sound->file = _cur.file;
		sound->file_offset = offs;
		sound->source = SoundSource::NewGRF;
		sound->grf_container_ver = _cur.file->GetContainerVersion();
	}
}

/* Action 0x11 */
static void GRFSound(ByteReader &buf)
{
	/* <11> <num>
	 *
	 * W num      Number of sound files that follow */

	uint16_t num = buf.ReadWord();
	if (num == 0) return;

	SoundEntry *sound;
	if (_cur.grffile->sound_offset == 0) {
		_cur.grffile->sound_offset = GetNumSounds();
		_cur.grffile->num_sounds = num;
		sound = AllocateSound(num);
	} else {
		sound = GetSound(_cur.grffile->sound_offset);
	}

	SpriteFile &file = *_cur.file;
	uint8_t grf_container_version = file.GetContainerVersion();
	for (int i = 0; i < num; i++) {
		_cur.nfo_line++;

		/* Check whether the index is in range. This might happen if multiple action 11 are present.
		 * While this is invalid, we do not check for this. But we should prevent it from causing bigger trouble */
		bool invalid = i >= _cur.grffile->num_sounds;

		size_t offs = file.GetPos();

		uint32_t len = grf_container_version >= 2 ? file.ReadDword() : file.ReadWord();
		uint8_t type = file.ReadByte();

		if (grf_container_version >= 2 && type == 0xFD) {
			/* Reference to sprite section. */
			if (invalid) {
				GrfMsg(1, "GRFSound: Sound index out of range (multiple Action 11?)");
				file.SkipBytes(len);
			} else if (len != 4) {
				GrfMsg(1, "GRFSound: Invalid sprite section import");
				file.SkipBytes(len);
			} else {
				uint32_t id = file.ReadDword();
				if (_cur.stage == GLS_INIT) LoadGRFSound(GetGRFSpriteOffset(id), sound + i);
			}
			continue;
		}

		if (type != 0xFF) {
			GrfMsg(1, "GRFSound: Unexpected RealSprite found, skipping");
			file.SkipBytes(7);
			SkipSpriteData(*_cur.file, type, len - 8);
			continue;
		}

		if (invalid) {
			GrfMsg(1, "GRFSound: Sound index out of range (multiple Action 11?)");
			file.SkipBytes(len);
		}

		uint8_t action = file.ReadByte();
		switch (action) {
			case 0xFF:
				/* Allocate sound only in init stage. */
				if (_cur.stage == GLS_INIT) {
					if (grf_container_version >= 2) {
						GrfMsg(1, "GRFSound: Inline sounds are not supported for container version >= 2");
					} else {
						LoadGRFSound(offs, sound + i);
					}
				}
				file.SkipBytes(len - 1); // already read <action>
				break;

			case 0xFE:
				if (_cur.stage == GLS_ACTIVATION) {
					/* XXX 'Action 0xFE' isn't really specified. It is only mentioned for
					 * importing sounds, so this is probably all wrong... */
					if (file.ReadByte() != 0) GrfMsg(1, "GRFSound: Import type mismatch");
					ImportGRFSound(sound + i);
				} else {
					file.SkipBytes(len - 1); // already read <action>
				}
				break;

			default:
				GrfMsg(1, "GRFSound: Unexpected Action {:x} found, skipping", action);
				file.SkipBytes(len - 1); // already read <action>
				break;
		}
	}
}

/* Action 0x11 (SKIP) */
static void SkipAct11(ByteReader &buf)
{
	/* <11> <num>
	 *
	 * W num      Number of sound files that follow */

	_cur.skip_sprites = buf.ReadWord();

	GrfMsg(3, "SkipAct11: Skipping {} sprites", _cur.skip_sprites);
}

/** Action 0x12 */
static void LoadFontGlyph(ByteReader &buf)
{
	/* <12> <num_def> <font_size> <num_char> <base_char>
	 *
	 * B num_def      Number of definitions
	 * B font_size    Size of font (0 = normal, 1 = small, 2 = large, 3 = mono)
	 * B num_char     Number of consecutive glyphs
	 * W base_char    First character index */

	uint8_t num_def = buf.ReadByte();

	for (uint i = 0; i < num_def; i++) {
		FontSize size    = (FontSize)buf.ReadByte();
		uint8_t  num_char  = buf.ReadByte();
		uint16_t base_char = buf.ReadWord();

		if (size >= FS_END) {
			GrfMsg(1, "LoadFontGlyph: Size {} is not supported, ignoring", size);
		}

		GrfMsg(7, "LoadFontGlyph: Loading {} glyph(s) at 0x{:04X} for size {}", num_char, base_char, size);

		for (uint c = 0; c < num_char; c++) {
			if (size < FS_END) SetUnicodeGlyph(size, base_char + c, _cur.spriteid);
			_cur.nfo_line++;
			LoadNextSprite(_cur.spriteid++, *_cur.file, _cur.nfo_line);
		}
	}
}

/** Action 0x12 (SKIP) */
static void SkipAct12(ByteReader &buf)
{
	/* <12> <num_def> <font_size> <num_char> <base_char>
	 *
	 * B num_def      Number of definitions
	 * B font_size    Size of font (0 = normal, 1 = small, 2 = large)
	 * B num_char     Number of consecutive glyphs
	 * W base_char    First character index */

	uint8_t num_def = buf.ReadByte();

	for (uint i = 0; i < num_def; i++) {
		/* Ignore 'size' byte */
		buf.ReadByte();

		/* Sum up number of characters */
		_cur.skip_sprites += buf.ReadByte();

		/* Ignore 'base_char' word */
		buf.ReadWord();
	}

	GrfMsg(3, "SkipAct12: Skipping {} sprites", _cur.skip_sprites);
}

/** Action 0x13 */
static void TranslateGRFStrings(ByteReader &buf)
{
	/* <13> <grfid> <num-ent> <offset> <text...>
	 *
	 * 4*B grfid     The GRFID of the file whose texts are to be translated
	 * B   num-ent   Number of strings
	 * W   offset    First text ID
	 * S   text...   Zero-terminated strings */

	uint32_t grfid = buf.ReadDWord();
	const GRFConfig *c = GetGRFConfig(grfid);
	if (c == nullptr || (c->status != GCS_INITIALISED && c->status != GCS_ACTIVATED)) {
		GrfMsg(7, "TranslateGRFStrings: GRFID 0x{:08X} unknown, skipping action 13", std::byteswap(grfid));
		return;
	}

	if (c->status == GCS_INITIALISED) {
		/* If the file is not active but will be activated later, give an error
		 * and disable this file. */
		GRFError *error = DisableGrf(STR_NEWGRF_ERROR_LOAD_AFTER);

		error->data = GetString(STR_NEWGRF_ERROR_AFTER_TRANSLATED_FILE);

		return;
	}

	/* Since no language id is supplied for with version 7 and lower NewGRFs, this string has
	 * to be added as a generic string, thus the language id of 0x7F. For this to work
	 * new_scheme has to be true as well, which will also be implicitly the case for version 8
	 * and higher. A language id of 0x7F will be overridden by a non-generic id, so this will
	 * not change anything if a string has been provided specifically for this language. */
	uint8_t language = _cur.grffile->grf_version >= 8 ? buf.ReadByte() : 0x7F;
	uint8_t num_strings = buf.ReadByte();
	uint16_t first_id  = buf.ReadWord();

	if (!((first_id >= 0xD000 && first_id + num_strings <= 0xD400) || (first_id >= 0xD800 && first_id + num_strings <= 0xE000))) {
		GrfMsg(7, "TranslateGRFStrings: Attempting to set out-of-range string IDs in action 13 (first: 0x{:04X}, number: 0x{:02X})", first_id, num_strings);
		return;
	}

	for (uint i = 0; i < num_strings && buf.HasData(); i++) {
		std::string_view string = buf.ReadString();

		if (string.empty()) {
			GrfMsg(7, "TranslateGRFString: Ignoring empty string.");
			continue;
		}

		AddGRFString(grfid, GRFStringID(first_id + i), language, true, true, string, STR_UNDEFINED);
	}
}

/** Callback function for 'INFO'->'NAME' to add a translation to the newgrf name. */
static bool ChangeGRFName(uint8_t langid, std::string_view str)
{
	AddGRFTextToList(_cur.grfconfig->name, langid, _cur.grfconfig->ident.grfid, false, str);
	return true;
}

/** Callback function for 'INFO'->'DESC' to add a translation to the newgrf description. */
static bool ChangeGRFDescription(uint8_t langid, std::string_view str)
{
	AddGRFTextToList(_cur.grfconfig->info, langid, _cur.grfconfig->ident.grfid, true, str);
	return true;
}

/** Callback function for 'INFO'->'URL_' to set the newgrf url. */
static bool ChangeGRFURL(uint8_t langid, std::string_view str)
{
	AddGRFTextToList(_cur.grfconfig->url, langid, _cur.grfconfig->ident.grfid, false, str);
	return true;
}

/** Callback function for 'INFO'->'NPAR' to set the number of valid parameters. */
static bool ChangeGRFNumUsedParams(size_t len, ByteReader &buf)
{
	if (len != 1) {
		GrfMsg(2, "StaticGRFInfo: expected only 1 byte for 'INFO'->'NPAR' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		_cur.grfconfig->num_valid_params = std::min(buf.ReadByte(), GRFConfig::MAX_NUM_PARAMS);
	}
	return true;
}

/** Callback function for 'INFO'->'PALS' to set the number of valid parameters. */
static bool ChangeGRFPalette(size_t len, ByteReader &buf)
{
	if (len != 1) {
		GrfMsg(2, "StaticGRFInfo: expected only 1 byte for 'INFO'->'PALS' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		char data = buf.ReadByte();
		GRFPalette pal = GRFP_GRF_UNSET;
		switch (data) {
			case '*':
			case 'A': pal = GRFP_GRF_ANY;     break;
			case 'W': pal = GRFP_GRF_WINDOWS; break;
			case 'D': pal = GRFP_GRF_DOS;     break;
			default:
				GrfMsg(2, "StaticGRFInfo: unexpected value '{:02X}' for 'INFO'->'PALS', ignoring this field", data);
				break;
		}
		if (pal != GRFP_GRF_UNSET) {
			_cur.grfconfig->palette &= ~GRFP_GRF_MASK;
			_cur.grfconfig->palette |= pal;
		}
	}
	return true;
}

/** Callback function for 'INFO'->'BLTR' to set the blitter info. */
static bool ChangeGRFBlitter(size_t len, ByteReader &buf)
{
	if (len != 1) {
		GrfMsg(2, "StaticGRFInfo: expected only 1 byte for 'INFO'->'BLTR' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		char data = buf.ReadByte();
		GRFPalette pal = GRFP_BLT_UNSET;
		switch (data) {
			case '8': pal = GRFP_BLT_UNSET; break;
			case '3': pal = GRFP_BLT_32BPP;  break;
			default:
				GrfMsg(2, "StaticGRFInfo: unexpected value '{:02X}' for 'INFO'->'BLTR', ignoring this field", data);
				return true;
		}
		_cur.grfconfig->palette &= ~GRFP_BLT_MASK;
		_cur.grfconfig->palette |= pal;
	}
	return true;
}

/** Callback function for 'INFO'->'VRSN' to the version of the NewGRF. */
static bool ChangeGRFVersion(size_t len, ByteReader &buf)
{
	if (len != 4) {
		GrfMsg(2, "StaticGRFInfo: expected 4 bytes for 'INFO'->'VRSN' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		/* Set min_loadable_version as well (default to minimal compatibility) */
		_cur.grfconfig->version = _cur.grfconfig->min_loadable_version = buf.ReadDWord();
	}
	return true;
}

/** Callback function for 'INFO'->'MINV' to the minimum compatible version of the NewGRF. */
static bool ChangeGRFMinVersion(size_t len, ByteReader &buf)
{
	if (len != 4) {
		GrfMsg(2, "StaticGRFInfo: expected 4 bytes for 'INFO'->'MINV' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		_cur.grfconfig->min_loadable_version = buf.ReadDWord();
		if (_cur.grfconfig->version == 0) {
			GrfMsg(2, "StaticGRFInfo: 'MINV' defined before 'VRSN' or 'VRSN' set to 0, ignoring this field");
			_cur.grfconfig->min_loadable_version = 0;
		}
		if (_cur.grfconfig->version < _cur.grfconfig->min_loadable_version) {
			GrfMsg(2, "StaticGRFInfo: 'MINV' defined as {}, limiting it to 'VRSN'", _cur.grfconfig->min_loadable_version);
			_cur.grfconfig->min_loadable_version = _cur.grfconfig->version;
		}
	}
	return true;
}

static GRFParameterInfo *_cur_parameter; ///< The parameter which info is currently changed by the newgrf.

/** Callback function for 'INFO'->'PARAM'->param_num->'NAME' to set the name of a parameter. */
static bool ChangeGRFParamName(uint8_t langid, std::string_view str)
{
	AddGRFTextToList(_cur_parameter->name, langid, _cur.grfconfig->ident.grfid, false, str);
	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'DESC' to set the description of a parameter. */
static bool ChangeGRFParamDescription(uint8_t langid, std::string_view str)
{
	AddGRFTextToList(_cur_parameter->desc, langid, _cur.grfconfig->ident.grfid, true, str);
	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'TYPE' to set the typeof a parameter. */
static bool ChangeGRFParamType(size_t len, ByteReader &buf)
{
	if (len != 1) {
		GrfMsg(2, "StaticGRFInfo: expected 1 byte for 'INFO'->'PARA'->'TYPE' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		uint8_t type = buf.ReadByte();
		if (type < PTYPE_END) {
			_cur_parameter->type = (GRFParameterType)type;
		} else {
			GrfMsg(3, "StaticGRFInfo: unknown parameter type {}, ignoring this field", type);
		}
	}
	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'LIMI' to set the min/max value of a parameter. */
static bool ChangeGRFParamLimits(size_t len, ByteReader &buf)
{
	if (_cur_parameter->type != PTYPE_UINT_ENUM) {
		GrfMsg(2, "StaticGRFInfo: 'INFO'->'PARA'->'LIMI' is only valid for parameters with type uint/enum, ignoring this field");
		buf.Skip(len);
	} else if (len != 8) {
		GrfMsg(2, "StaticGRFInfo: expected 8 bytes for 'INFO'->'PARA'->'LIMI' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		uint32_t min_value = buf.ReadDWord();
		uint32_t max_value = buf.ReadDWord();
		if (min_value <= max_value) {
			_cur_parameter->min_value = min_value;
			_cur_parameter->max_value = max_value;
		} else {
			GrfMsg(2, "StaticGRFInfo: 'INFO'->'PARA'->'LIMI' values are incoherent, ignoring this field");
		}
	}
	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'MASK' to set the parameter and bits to use. */
static bool ChangeGRFParamMask(size_t len, ByteReader &buf)
{
	if (len < 1 || len > 3) {
		GrfMsg(2, "StaticGRFInfo: expected 1 to 3 bytes for 'INFO'->'PARA'->'MASK' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		uint8_t param_nr = buf.ReadByte();
		if (param_nr >= GRFConfig::MAX_NUM_PARAMS) {
			GrfMsg(2, "StaticGRFInfo: invalid parameter number in 'INFO'->'PARA'->'MASK', param {}, ignoring this field", param_nr);
			buf.Skip(len - 1);
		} else {
			_cur_parameter->param_nr = param_nr;
			if (len >= 2) _cur_parameter->first_bit = std::min<uint8_t>(buf.ReadByte(), 31);
			if (len >= 3) _cur_parameter->num_bit = std::min<uint8_t>(buf.ReadByte(), 32 - _cur_parameter->first_bit);
		}
	}

	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'DFLT' to set the default value. */
static bool ChangeGRFParamDefault(size_t len, ByteReader &buf)
{
	if (len != 4) {
		GrfMsg(2, "StaticGRFInfo: expected 4 bytes for 'INFO'->'PARA'->'DEFA' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		_cur_parameter->def_value = buf.ReadDWord();
	}
	_cur.grfconfig->has_param_defaults = true;
	return true;
}

typedef bool (*DataHandler)(size_t, ByteReader &);          ///< Type of callback function for binary nodes
typedef bool (*TextHandler)(uint8_t, std::string_view str); ///< Type of callback function for text nodes
typedef bool (*BranchHandler)(ByteReader &);                ///< Type of callback function for branch nodes

/**
 * Data structure to store the allowed id/type combinations for action 14. The
 * data can be represented as a tree with 3 types of nodes:
 * 1. Branch nodes (identified by 'C' for choice).
 * 2. Binary leaf nodes (identified by 'B').
 * 3. Text leaf nodes (identified by 'T').
 */
struct AllowedSubtags {
	/** Custom 'span' of subtags. Required because std::span with an incomplete type is UB. */
	using Span = std::pair<const AllowedSubtags *, const AllowedSubtags *>;

	uint32_t id; ///< The identifier for this node.
	std::variant<DataHandler, TextHandler, BranchHandler, Span> handler; ///< The handler for this node.
};

static bool SkipUnknownInfo(ByteReader &buf, uint8_t type);
static bool HandleNodes(ByteReader &buf, std::span<const AllowedSubtags> tags);

/**
 * Try to skip the current branch node and all subnodes.
 * This is suitable for use with AllowedSubtags.
 * @param buf Buffer.
 * @return True if we could skip the node, false if an error occurred.
 */
static bool SkipInfoChunk(ByteReader &buf)
{
	uint8_t type = buf.ReadByte();
	while (type != 0) {
		buf.ReadDWord(); // chunk ID
		if (!SkipUnknownInfo(buf, type)) return false;
		type = buf.ReadByte();
	}
	return true;
}

/**
 * Callback function for 'INFO'->'PARA'->param_num->'VALU' to set the names
 * of some parameter values (type uint/enum) or the names of some bits
 * (type bitmask). In both cases the format is the same:
 * Each subnode should be a text node with the value/bit number as id.
 */
static bool ChangeGRFParamValueNames(ByteReader &buf)
{
	uint8_t type = buf.ReadByte();
	while (type != 0) {
		uint32_t id = buf.ReadDWord();
		if (type != 'T' || id > _cur_parameter->max_value) {
			GrfMsg(2, "StaticGRFInfo: all child nodes of 'INFO'->'PARA'->param_num->'VALU' should have type 't' and the value/bit number as id");
			if (!SkipUnknownInfo(buf, type)) return false;
			type = buf.ReadByte();
			continue;
		}

		uint8_t langid = buf.ReadByte();
		std::string_view name_string = buf.ReadString();

		auto it = std::ranges::lower_bound(_cur_parameter->value_names, id, std::less{}, &GRFParameterInfo::ValueName::first);
		if (it == std::end(_cur_parameter->value_names) || it->first != id) {
			it = _cur_parameter->value_names.emplace(it, id, GRFTextList{});
		}
		AddGRFTextToList(it->second, langid, _cur.grfconfig->ident.grfid, false, name_string);

		type = buf.ReadByte();
	}
	return true;
}

/** Action14 parameter tags */
static constexpr AllowedSubtags _tags_parameters[] = {
	AllowedSubtags{'NAME', ChangeGRFParamName},
	AllowedSubtags{'DESC', ChangeGRFParamDescription},
	AllowedSubtags{'TYPE', ChangeGRFParamType},
	AllowedSubtags{'LIMI', ChangeGRFParamLimits},
	AllowedSubtags{'MASK', ChangeGRFParamMask},
	AllowedSubtags{'VALU', ChangeGRFParamValueNames},
	AllowedSubtags{'DFLT', ChangeGRFParamDefault},
};

/**
 * Callback function for 'INFO'->'PARA' to set extra information about the
 * parameters. Each subnode of 'INFO'->'PARA' should be a branch node with
 * the parameter number as id. The first parameter has id 0. The maximum
 * parameter that can be changed is set by 'INFO'->'NPAR' which defaults to 80.
 */
static bool HandleParameterInfo(ByteReader &buf)
{
	uint8_t type = buf.ReadByte();
	while (type != 0) {
		uint32_t id = buf.ReadDWord();
		if (type != 'C' || id >= _cur.grfconfig->num_valid_params) {
			GrfMsg(2, "StaticGRFInfo: all child nodes of 'INFO'->'PARA' should have type 'C' and their parameter number as id");
			if (!SkipUnknownInfo(buf, type)) return false;
			type = buf.ReadByte();
			continue;
		}

		if (id >= _cur.grfconfig->param_info.size()) {
			_cur.grfconfig->param_info.resize(id + 1);
		}
		if (!_cur.grfconfig->param_info[id].has_value()) {
			_cur.grfconfig->param_info[id] = GRFParameterInfo(id);
		}
		_cur_parameter = &_cur.grfconfig->param_info[id].value();
		/* Read all parameter-data and process each node. */
		if (!HandleNodes(buf, _tags_parameters)) return false;
		type = buf.ReadByte();
	}
	return true;
}

/** Action14 tags for the INFO node */
static constexpr AllowedSubtags _tags_info[] = {
	AllowedSubtags{'NAME', ChangeGRFName},
	AllowedSubtags{'DESC', ChangeGRFDescription},
	AllowedSubtags{'URL_', ChangeGRFURL},
	AllowedSubtags{'NPAR', ChangeGRFNumUsedParams},
	AllowedSubtags{'PALS', ChangeGRFPalette},
	AllowedSubtags{'BLTR', ChangeGRFBlitter},
	AllowedSubtags{'VRSN', ChangeGRFVersion},
	AllowedSubtags{'MINV', ChangeGRFMinVersion},
	AllowedSubtags{'PARA', HandleParameterInfo},
};


/** Action14 feature test instance */
struct GRFFeatureTest {
	const GRFFeatureInfo *feature;
	uint16_t min_version;
	uint16_t max_version;
	uint8_t platform_var_bit;
	uint32_t test_91_value;

	void Reset()
	{
		this->feature = nullptr;
		this->min_version = 1;
		this->max_version = UINT16_MAX;
		this->platform_var_bit = 0;
		this->test_91_value = 0;
	}

	void ExecuteTest()
	{
		uint16_t version = (this->feature != nullptr) ? this->feature->version : 0;
		bool has_feature = (version >= this->min_version && version <= this->max_version);
		if (this->platform_var_bit > 0) {
			AssignBit(_cur.grffile->var9D_overlay, this->platform_var_bit, has_feature);
			GrfMsg(2, "Action 14 feature test: feature test: setting bit {} of var 0x9D to {}, {}", platform_var_bit, has_feature ? 1 : 0, _cur.grffile->var9D_overlay);
		}
		if (this->test_91_value > 0) {
			if (has_feature) {
				GrfMsg(2, "Action 14 feature test: feature test: adding test value 0x{:X} to var 0x91", this->test_91_value);
				include(_cur.grffile->var91_values, this->test_91_value);
			} else {
				GrfMsg(2, "Action 14 feature test: feature test: not adding test value 0x{:X} to var 0x91", this->test_91_value);
			}
		}
		if (this->platform_var_bit == 0 && this->test_91_value == 0) {
			GrfMsg(2, "Action 14 feature test: feature test: doing nothing: {}", has_feature ? 1 : 0);
		}
		if (this->feature != nullptr && this->feature->observation_flag != GFTOF_INVALID) {
			SetBit(_cur.grffile->observed_feature_tests, this->feature->observation_flag);
		}
	}
};

static GRFFeatureTest _current_grf_feature_test;

/** Callback function for 'FTST'->'NAME' to set the name of the feature being tested. */
static bool ChangeGRFFeatureTestName(uint8_t langid, std::string_view str)
{
	extern const GRFFeatureInfo _grf_feature_list[];
	for (const GRFFeatureInfo *info = _grf_feature_list; info->name != nullptr; info++) {
		if (str == info->name) {
			_current_grf_feature_test.feature = info;
			GrfMsg(2, "Action 14 feature test: found feature named: '{}' (version: {}) in 'FTST'->'NAME'", StrMakeValid(str), info->version);
			return true;
		}
	}
	GrfMsg(2, "Action 14 feature test: could not find feature named: '{}' in 'FTST'->'NAME'", StrMakeValid(str));
	_current_grf_feature_test.feature = nullptr;
	return true;
}

/** Callback function for 'FTST'->'MINV' to set the minimum version of the feature being tested. */
static bool ChangeGRFFeatureMinVersion(size_t len, ByteReader &buf)
{
	if (len != 2) {
		GrfMsg(2, "Action 14 feature test: expected 2 bytes for 'FTST'->'MINV' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		_current_grf_feature_test.min_version = buf.ReadWord();
	}
	return true;
}

/** Callback function for 'FTST'->'MAXV' to set the maximum version of the feature being tested. */
static bool ChangeGRFFeatureMaxVersion(size_t len, ByteReader &buf)
{
	if (len != 2) {
		GrfMsg(2, "Action 14 feature test: expected 2 bytes for 'FTST'->'MAXV' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		_current_grf_feature_test.max_version = buf.ReadWord();
	}
	return true;
}

/** Callback function for 'FTST'->'SETP' to set the bit number of global variable 9D (platform version) to set/unset with the result of the feature test. */
static bool ChangeGRFFeatureSetPlatformVarBit(size_t len, ByteReader &buf)
{
	if (len != 1) {
		GrfMsg(2, "Action 14 feature test: expected 1 byte for 'FTST'->'SETP' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		uint8_t bit_number = buf.ReadByte();
		if (bit_number >= 4 && bit_number <= 31) {
			_current_grf_feature_test.platform_var_bit = bit_number;
		} else {
			GrfMsg(2, "Action 14 feature test: expected a bit number >= 4 and <= 32 for 'FTST'->'SETP' but got {}, ignoring this field", bit_number);
		}
	}
	return true;
}

/** Callback function for 'FTST'->'SVAL' to add a test success result value for checking using global variable 91. */
static bool ChangeGRFFeatureTestSuccessResultValue(size_t len, ByteReader &buf)
{
	if (len != 4) {
		GrfMsg(2, "Action 14 feature test: expected 4 bytes for 'FTST'->'SVAL' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		_current_grf_feature_test.test_91_value = buf.ReadDWord();
	}
	return true;
}

/** Action14 tags for the FTST node */
static constexpr AllowedSubtags _tags_ftst[] = {
	AllowedSubtags{'NAME', ChangeGRFFeatureTestName},
	AllowedSubtags{'MINV', ChangeGRFFeatureMinVersion},
	AllowedSubtags{'MAXV', ChangeGRFFeatureMaxVersion},
	AllowedSubtags{'SETP', ChangeGRFFeatureSetPlatformVarBit},
	AllowedSubtags{'SVAL', ChangeGRFFeatureTestSuccessResultValue},
};

/**
 * Callback function for 'FTST' (feature test)
 */
static bool HandleFeatureTestInfo(ByteReader &buf)
{
	_current_grf_feature_test.Reset();
	HandleNodes(buf, _tags_ftst);
	_current_grf_feature_test.ExecuteTest();
	return true;
}

/** Action14 Action0 property map action instance */
struct GRFPropertyMapAction {
	const char *tag_name = nullptr;
	const char *descriptor = nullptr;

	GrfSpecFeature feature;
	int prop_id;
	int ext_prop_id;
	std::string name;
	GRFPropertyMapFallbackMode fallback_mode;
	uint8_t ttd_ver_var_bit;
	uint32_t test_91_value;
	uint8_t input_shift;
	uint8_t output_shift;
	uint input_mask;
	uint output_mask;
	uint output_param;

	void Reset(const char *tag, const char *desc)
	{
		this->tag_name = tag;
		this->descriptor = desc;

		this->feature = GSF_INVALID;
		this->prop_id = -1;
		this->ext_prop_id = -1;
		this->name.clear();
		this->fallback_mode = GPMFM_IGNORE;
		this->ttd_ver_var_bit = 0;
		this->test_91_value = 0;
		this->input_shift = 0;
		this->output_shift = 0;
		this->input_mask = 0;
		this->output_mask = 0;
		this->output_param = 0;
	}

	void ExecuteFeatureIDRemapping()
	{
		if (this->prop_id < 0) {
			GrfMsg(2, "Action 14 {} remapping: no feature ID defined, doing nothing", this->descriptor);
			return;
		}
		if (this->name.empty()) {
			GrfMsg(2, "Action 14 {} remapping: no name defined, doing nothing", this->descriptor);
			return;
		}
		SetBit(_cur.grffile->ctrl_flags, GFCF_HAVE_FEATURE_ID_REMAP);
		bool success = false;
		const char *str = this->name.c_str();
		extern const GRFFeatureMapDefinition _grf_remappable_features[];
		for (const GRFFeatureMapDefinition *info = _grf_remappable_features; info->name != nullptr; info++) {
			if (strcmp(info->name, str) == 0) {
				GRFFeatureMapRemapEntry &entry = _cur.grffile->feature_id_remaps.Entry(this->prop_id);
				entry.name = info->name;
				entry.feature = info->feature;
				entry.raw_id = this->prop_id;
				success = true;
				break;
			}
		}
		if (this->ttd_ver_var_bit > 0) {
			AssignBit(_cur.grffile->var8D_overlay, this->ttd_ver_var_bit, success);
		}
		if (this->test_91_value > 0 && success) {
			include(_cur.grffile->var91_values, this->test_91_value);
		}
		if (!success) {
			if (this->fallback_mode == GPMFM_ERROR_ON_DEFINITION) {
				GrfMsg(0, "Error: Unimplemented mapped {}: {}, mapped to: 0x{:02X}", this->descriptor, str, this->prop_id);
				GRFError *error = DisableGrf(STR_NEWGRF_ERROR_UNIMPLEMETED_MAPPED_FEATURE_ID);
				error->data = stredup(str);
				error->param_value[1] = GSF_INVALID;
				error->param_value[2] = this->prop_id;
			} else {
				const char *str_store = stredup(str);
				GrfMsg(2, "Unimplemented mapped {}: {}, mapped to: {:X}, {} on use",
						this->descriptor, str, this->prop_id, (this->fallback_mode == GPMFM_IGNORE) ? "ignoring" : "error");
				_cur.grffile->remap_unknown_property_names.emplace_back(str_store);
				GRFFeatureMapRemapEntry &entry = _cur.grffile->feature_id_remaps.Entry(this->prop_id);
				entry.name = str_store;
				entry.feature = (this->fallback_mode == GPMFM_IGNORE) ? GSF_INVALID : GSF_ERROR_ON_USE;
				entry.raw_id = this->prop_id;
			}
		}
	}

	void ExecutePropertyRemapping()
	{
		if (this->feature == GSF_INVALID) {
			GrfMsg(2, "Action 14 {} remapping: no feature defined, doing nothing", this->descriptor);
			return;
		}
		if (this->prop_id < 0 && this->ext_prop_id < 0) {
			GrfMsg(2, "Action 14 {} remapping: no property ID defined, doing nothing", this->descriptor);
			return;
		}
		if (this->name.empty()) {
			GrfMsg(2, "Action 14 {} remapping: no name defined, doing nothing", this->descriptor);
			return;
		}
		bool success = false;
		const char *str = this->name.c_str();
		extern const GRFPropertyMapDefinition _grf_action0_remappable_properties[];
		for (const GRFPropertyMapDefinition *info = _grf_action0_remappable_properties; info->name != nullptr; info++) {
			if ((info->feature == GSF_INVALID || info->feature == this->feature) && strcmp(info->name, str) == 0) {
				if (this->prop_id > 0) {
					GRFFilePropertyRemapEntry &entry = _cur.grffile->action0_property_remaps[this->feature].Entry(this->prop_id);
					entry.name = info->name;
					entry.id = info->id;
					entry.feature = this->feature;
					entry.property_id = this->prop_id;
				}
				if (this->ext_prop_id > 0) {
					GRFFilePropertyRemapEntry &entry = _cur.grffile->action0_extended_property_remaps[(((uint32_t)this->feature) << 16) | this->ext_prop_id];
					entry.name = info->name;
					entry.id = info->id;
					entry.feature = this->feature;
					entry.extended = true;
					entry.property_id = this->ext_prop_id;
				}
				success = true;
				break;
			}
		}
		if (this->ttd_ver_var_bit > 0) {
			AssignBit(_cur.grffile->var8D_overlay, this->ttd_ver_var_bit, success);
		}
		if (this->test_91_value > 0 && success) {
			include(_cur.grffile->var91_values, this->test_91_value);
		}
		if (!success) {
			uint mapped_to = (this->prop_id > 0) ? this->prop_id : this->ext_prop_id;
			const char *extended = (this->prop_id > 0) ? "" : " (extended)";
			if (this->fallback_mode == GPMFM_ERROR_ON_DEFINITION) {
				GrfMsg(0, "Error: Unimplemented mapped {}: {}, feature: {}, mapped to: {:X}{}", this->descriptor, str, GetFeatureString(this->feature), mapped_to, extended);
				GRFError *error = DisableGrf(STR_NEWGRF_ERROR_UNIMPLEMETED_MAPPED_PROPERTY);
				error->data = stredup(str);
				error->param_value[1] = this->feature;
				error->param_value[2] = ((this->prop_id > 0) ? 0 : 0xE0000) | mapped_to;
			} else {
				const char *str_store = stredup(str);
				GrfMsg(2, "Unimplemented mapped {}: {}, feature: {}, mapped to: {:X}{}, {} on use",
						this->descriptor, str, GetFeatureString(this->feature), mapped_to, extended, (this->fallback_mode == GPMFM_IGNORE) ? "ignoring" : "error");
				_cur.grffile->remap_unknown_property_names.emplace_back(str_store);
				if (this->prop_id > 0) {
					GRFFilePropertyRemapEntry &entry = _cur.grffile->action0_property_remaps[this->feature].Entry(this->prop_id);
					entry.name = str_store;
					entry.id = (this->fallback_mode == GPMFM_IGNORE) ? A0RPI_UNKNOWN_IGNORE : A0RPI_UNKNOWN_ERROR;
					entry.feature = this->feature;
					entry.property_id = this->prop_id;
				}
				if (this->ext_prop_id > 0) {
					GRFFilePropertyRemapEntry &entry = _cur.grffile->action0_extended_property_remaps[(((uint32_t)this->feature) << 16) | this->ext_prop_id];
					entry.name = str_store;
					entry.id = (this->fallback_mode == GPMFM_IGNORE) ? A0RPI_UNKNOWN_IGNORE : A0RPI_UNKNOWN_ERROR;;
					entry.feature = this->feature;
					entry.extended = true;
					entry.property_id = this->ext_prop_id;
				}
			}
		}
	}

	void ExecuteVariableRemapping()
	{
		if (this->feature == GSF_INVALID) {
			GrfMsg(2, "Action 14 {} remapping: no feature defined, doing nothing", this->descriptor);
			return;
		}
		if (this->name.empty()) {
			GrfMsg(2, "Action 14 {} remapping: no name defined, doing nothing", this->descriptor);
			return;
		}
		bool success = false;
		const char *str = this->name.c_str();
		extern const GRFVariableMapDefinition _grf_action2_remappable_variables[];
		for (const GRFVariableMapDefinition *info = _grf_action2_remappable_variables; info->name != nullptr; info++) {
			if (info->feature == this->feature && strcmp(info->name, str) == 0) {
				_cur.grffile->grf_variable_remaps.push_back({ (uint16_t)info->id, (uint8_t)this->feature, this->input_shift, this->output_shift, this->input_mask, this->output_mask, this->output_param });
				success = true;
				break;
			}
		}
		if (this->ttd_ver_var_bit > 0) {
			AssignBit(_cur.grffile->var8D_overlay, this->ttd_ver_var_bit, success);
		}
		if (this->test_91_value > 0 && success) {
			include(_cur.grffile->var91_values, this->test_91_value);
		}
		if (!success) {
			GrfMsg(2, "Unimplemented mapped {}: {}, feature: {}, mapped to 0", this->descriptor, str, GetFeatureString(this->feature));
		}
	}

	void ExecuteAction5TypeRemapping()
	{
		if (this->prop_id < 0) {
			GrfMsg(2, "Action 14 {} remapping: no type ID defined, doing nothing", this->descriptor);
			return;
		}
		if (this->name.empty()) {
			GrfMsg(2, "Action 14 {} remapping: no name defined, doing nothing", this->descriptor);
			return;
		}
		bool success = false;
		const char *str = this->name.c_str();
		extern const Action5TypeRemapDefinition _grf_action5_remappable_types[];
		for (const Action5TypeRemapDefinition *info = _grf_action5_remappable_types; info->name != nullptr; info++) {
			if (strcmp(info->name, str) == 0) {
				Action5TypeRemapEntry &entry = _cur.grffile->action5_type_remaps.Entry(this->prop_id);
				entry.name = info->name;
				entry.info = &(info->info);
				entry.type_id = this->prop_id;
				success = true;
				break;
			}
		}
		if (this->ttd_ver_var_bit > 0) {
			AssignBit(_cur.grffile->var8D_overlay, this->ttd_ver_var_bit, success);
		}
		if (this->test_91_value > 0 && success) {
			include(_cur.grffile->var91_values, this->test_91_value);
		}
		if (!success) {
			if (this->fallback_mode == GPMFM_ERROR_ON_DEFINITION) {
				GrfMsg(0, "Error: Unimplemented mapped {}: {}, mapped to: {:X}", this->descriptor, str, this->prop_id);
				GRFError *error = DisableGrf(STR_NEWGRF_ERROR_UNIMPLEMETED_MAPPED_ACTION5_TYPE);
				error->data = stredup(str);
				error->param_value[1] = this->prop_id;
			} else {
				const char *str_store = stredup(str);
				GrfMsg(2, "Unimplemented mapped {}: {}, mapped to: {:X}, {} on use",
						this->descriptor, str, this->prop_id, (this->fallback_mode == GPMFM_IGNORE) ? "ignoring" : "error");
				_cur.grffile->remap_unknown_property_names.emplace_back(str_store);
				Action5TypeRemapEntry &entry = _cur.grffile->action5_type_remaps.Entry(this->prop_id);
				entry.name = str_store;
				entry.info = nullptr;
				entry.type_id = this->prop_id;
				entry.fallback_mode = this->fallback_mode;
			}
		}
	}
};

static GRFPropertyMapAction _current_grf_property_map_action;

/** Callback function for ->'NAME' to set the name of the item to be mapped. */
static bool ChangePropertyRemapName(uint8_t langid, std::string_view str)
{
	_current_grf_property_map_action.name = str;
	return true;
}

/** Callback function for ->'FEAT' to set which feature this mapping applies to. */
static bool ChangePropertyRemapFeature(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 1) {
		GrfMsg(2, "Action 14 {} mapping: expected 1 byte for '{}'->'FEAT' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		GrfSpecFeatureRef feature = ReadFeature(buf.ReadByte());
		if (feature.id >= GSF_END) {
			GrfMsg(2, "Action 14 {} mapping: invalid feature ID: {}, in '{}'->'FEAT', ignoring this field", action.descriptor, GetFeatureString(feature), action.tag_name);
		} else {
			action.feature = feature.id;
		}
	}
	return true;
}

/** Callback function for ->'PROP' to set the property ID to which this item is being mapped. */
static bool ChangePropertyRemapPropertyId(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 1) {
		GrfMsg(2, "Action 14 {} mapping: expected 1 byte for '{}'->'PROP' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		action.prop_id = buf.ReadByte();
	}
	return true;
}

/** Callback function for ->'XPRP' to set the extended property ID to which this item is being mapped. */
static bool ChangePropertyRemapExtendedPropertyId(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 2) {
		GrfMsg(2, "Action 14 {} mapping: expected 2 bytes for '{}'->'XPRP' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		action.ext_prop_id = buf.ReadWord();
	}
	return true;
}

/** Callback function for ->'FTID' to set the feature ID to which this feature is being mapped. */
static bool ChangePropertyRemapFeatureId(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 1) {
		GrfMsg(2, "Action 14 {} mapping: expected 1 byte for '{}'->'FTID' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		action.prop_id = buf.ReadByte();
	}
	return true;
}

/** Callback function for ->'TYPE' to set the property ID to which this item is being mapped. */
static bool ChangePropertyRemapTypeId(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 1) {
		GrfMsg(2, "Action 14 {} mapping: expected 1 byte for '{}'->'TYPE' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		uint8_t prop = buf.ReadByte();
		if (prop < 128) {
			action.prop_id = prop;
		} else {
			GrfMsg(2, "Action 14 {} mapping: expected a type < 128 for '{}'->'TYPE' but got {}, ignoring this field", action.descriptor, action.tag_name, prop);
		}
	}
	return true;
}

/** Callback function for ->'FLBK' to set the fallback mode. */
static bool ChangePropertyRemapSetFallbackMode(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 1) {
		GrfMsg(2, "Action 14 {} mapping: expected 1 byte for '{}'->'FLBK' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		GRFPropertyMapFallbackMode mode = (GRFPropertyMapFallbackMode) buf.ReadByte();
		if (mode < GPMFM_END) action.fallback_mode = mode;
	}
	return true;
}
/** Callback function for ->'SETT' to set the bit number of global variable 8D (TTD version) to set/unset with whether the remapping was successful. */
static bool ChangePropertyRemapSetTTDVerVarBit(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 1) {
		GrfMsg(2, "Action 14 {} mapping: expected 1 byte for '{}'->'SETT' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		uint8_t bit_number = buf.ReadByte();
		if (bit_number >= 4 && bit_number <= 31) {
			action.ttd_ver_var_bit = bit_number;
		} else {
			GrfMsg(2, "Action 14 {} mapping: expected a bit number >= 4 and <= 32 for '{}'->'SETT' but got {}, ignoring this field", action.descriptor, action.tag_name, bit_number);
		}
	}
	return true;
}

/** Callback function for >'SVAL' to add a success result value for checking using global variable 91. */
static bool ChangePropertyRemapSuccessResultValue(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 4) {
		GrfMsg(2, "Action 14 {} mapping: expected 4 bytes for '{}'->'SVAL' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		action.test_91_value = buf.ReadDWord();
	}
	return true;
}

/** Callback function for ->'RSFT' to set the input shift value for variable remapping. */
static bool ChangePropertyRemapSetInputShift(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 1) {
		GrfMsg(2, "Action 14 {} mapping: expected 1 byte for '{}'->'RSFT' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		uint8_t input_shift = buf.ReadByte();
		if (input_shift < 0x20) {
			action.input_shift = input_shift;
		} else {
			GrfMsg(2, "Action 14 {} mapping: expected a shift value < 0x20 for '{}'->'RSFT' but got {}, ignoring this field", action.descriptor, action.tag_name, input_shift);
		}
	}
	return true;
}

/** Callback function for ->'VSFT' to set the output shift value for variable remapping. */
static bool ChangePropertyRemapSetOutputShift(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 1) {
		GrfMsg(2, "Action 14 {} mapping: expected 1 byte for '{}'->'VSFT' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		uint8_t output_shift = buf.ReadByte();
		if (output_shift < 0x20) {
			action.output_shift = output_shift;
		} else {
			GrfMsg(2, "Action 14 {} mapping: expected a shift value < 0x20 for '{}'->'VSFT' but got {}, ignoring this field", action.descriptor, action.tag_name, output_shift);
		}
	}
	return true;
}

/** Callback function for ->'RMSK' to set the input mask value for variable remapping. */
static bool ChangePropertyRemapSetInputMask(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 4) {
		GrfMsg(2, "Action 14 {} mapping: expected 4 bytes for '{}'->'RMSK' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		action.input_mask = buf.ReadDWord();
	}
	return true;
}

/** Callback function for ->'VMSK' to set the output mask value for variable remapping. */
static bool ChangePropertyRemapSetOutputMask(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 4) {
		GrfMsg(2, "Action 14 {} mapping: expected 4 bytes for '{}'->'VMSK' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		action.output_mask = buf.ReadDWord();
	}
	return true;
}

/** Callback function for ->'VPRM' to set the output parameter value for variable remapping. */
static bool ChangePropertyRemapSetOutputParam(size_t len, ByteReader &buf)
{
	GRFPropertyMapAction &action = _current_grf_property_map_action;
	if (len != 4) {
		GrfMsg(2, "Action 14 {} mapping: expected 4 bytes for '{}'->'VPRM' but got {}, ignoring this field", action.descriptor, action.tag_name, len);
		buf.Skip(len);
	} else {
		action.output_param = buf.ReadDWord();
	}
	return true;
}

/** Action14 tags for the FIDM node */
static constexpr AllowedSubtags _tags_fidm[] = {
	AllowedSubtags{'NAME', ChangePropertyRemapName},
	AllowedSubtags{'FTID', ChangePropertyRemapFeatureId},
	AllowedSubtags{'FLBK', ChangePropertyRemapSetFallbackMode},
	AllowedSubtags{'SETT', ChangePropertyRemapSetTTDVerVarBit},
	AllowedSubtags{'SVAL', ChangePropertyRemapSuccessResultValue},
};

/**
 * Callback function for 'FIDM' (feature ID mapping)
 */
static bool HandleFeatureIDMap(ByteReader &buf)
{
	_current_grf_property_map_action.Reset("FIDM", "feature");
	HandleNodes(buf, _tags_fidm);
	_current_grf_property_map_action.ExecuteFeatureIDRemapping();
	return true;
}

/** Action14 tags for the A0PM node */
static constexpr AllowedSubtags _tags_a0pm[] = {
	AllowedSubtags{'NAME', ChangePropertyRemapName},
	AllowedSubtags{'FEAT', ChangePropertyRemapFeature},
	AllowedSubtags{'PROP', ChangePropertyRemapPropertyId},
	AllowedSubtags{'XPRP', ChangePropertyRemapExtendedPropertyId},
	AllowedSubtags{'FLBK', ChangePropertyRemapSetFallbackMode},
	AllowedSubtags{'SETT', ChangePropertyRemapSetTTDVerVarBit},
	AllowedSubtags{'SVAL', ChangePropertyRemapSuccessResultValue},
};

/**
 * Callback function for 'A0PM' (action 0 property mapping)
 */
static bool HandleAction0PropertyMap(ByteReader &buf)
{
	_current_grf_property_map_action.Reset("A0PM", "property");
	HandleNodes(buf, _tags_a0pm);
	_current_grf_property_map_action.ExecutePropertyRemapping();
	return true;
}

/** Action14 tags for the A2VM node */
static constexpr AllowedSubtags _tags_a2vm[] = {
	AllowedSubtags{'NAME', ChangePropertyRemapName},
	AllowedSubtags{'FEAT', ChangePropertyRemapFeature},
	AllowedSubtags{'RSFT', ChangePropertyRemapSetInputShift},
	AllowedSubtags{'RMSK', ChangePropertyRemapSetInputMask},
	AllowedSubtags{'VSFT', ChangePropertyRemapSetOutputShift},
	AllowedSubtags{'VMSK', ChangePropertyRemapSetOutputMask},
	AllowedSubtags{'VPRM', ChangePropertyRemapSetOutputParam},
	AllowedSubtags{'SETT', ChangePropertyRemapSetTTDVerVarBit},
	AllowedSubtags{'SVAL', ChangePropertyRemapSuccessResultValue},
};

/**
 * Callback function for 'A2VM' (action 2 variable mapping)
 */
static bool HandleAction2VariableMap(ByteReader &buf)
{
	_current_grf_property_map_action.Reset("A2VM", "variable");
	HandleNodes(buf, _tags_a2vm);
	_current_grf_property_map_action.ExecuteVariableRemapping();
	return true;
}

/** Action14 tags for the A5TM node */
static constexpr AllowedSubtags _tags_a5tm[] = {
	AllowedSubtags{'NAME', ChangePropertyRemapName},
	AllowedSubtags{'TYPE', ChangePropertyRemapTypeId},
	AllowedSubtags{'FLBK', ChangePropertyRemapSetFallbackMode},
	AllowedSubtags{'SETT', ChangePropertyRemapSetTTDVerVarBit},
	AllowedSubtags{'SVAL', ChangePropertyRemapSuccessResultValue},
};

/**
 * Callback function for 'A5TM' (action 5 type mapping)
 */
static bool HandleAction5TypeMap(ByteReader &buf)
{
	_current_grf_property_map_action.Reset("A5TM", "Action 5 type");
	HandleNodes(buf, _tags_a5tm);
	_current_grf_property_map_action.ExecuteAction5TypeRemapping();
	return true;
}

/** Action14 root tags */
static constexpr AllowedSubtags _tags_root_static[] = {
	AllowedSubtags{'INFO', std::make_pair(std::begin(_tags_info), std::end(_tags_info))},
	AllowedSubtags{'FTST', SkipInfoChunk},
	AllowedSubtags{'FIDM', SkipInfoChunk},
	AllowedSubtags{'A0PM', SkipInfoChunk},
	AllowedSubtags{'A2VM', SkipInfoChunk},
	AllowedSubtags{'A5TM', SkipInfoChunk},
};

/** Action14 root tags */
static constexpr AllowedSubtags _tags_root_feature_tests[] = {
	AllowedSubtags{'INFO', SkipInfoChunk},
	AllowedSubtags{'FTST', HandleFeatureTestInfo},
	AllowedSubtags{'FIDM', HandleFeatureIDMap},
	AllowedSubtags{'A0PM', HandleAction0PropertyMap},
	AllowedSubtags{'A2VM', HandleAction2VariableMap},
	AllowedSubtags{'A5TM', HandleAction5TypeMap},
};


/**
 * Try to skip the current node and all subnodes (if it's a branch node).
 * @param buf Buffer.
 * @param type The node type to skip.
 * @return True if we could skip the node, false if an error occurred.
 */
static bool SkipUnknownInfo(ByteReader &buf, uint8_t type)
{
	/* type and id are already read */
	switch (type) {
		case 'C': {
			uint8_t new_type = buf.ReadByte();
			while (new_type != 0) {
				buf.ReadDWord(); // skip the id
				if (!SkipUnknownInfo(buf, new_type)) return false;
				new_type = buf.ReadByte();
			}
			break;
		}

		case 'T':
			buf.ReadByte(); // lang
			buf.ReadString(); // actual text
			break;

		case 'B': {
			uint16_t size = buf.ReadWord();
			buf.Skip(size);
			break;
		}

		default:
			return false;
	}

	return true;
}

/**
 * Handle the nodes of an Action14
 * @param type Type of node.
 * @param id ID.
 * @param buf Buffer.
 * @param subtags Allowed subtags.
 * @return Whether all tags could be handled.
 */
static bool HandleNode(uint8_t type, uint32_t id, ByteReader &buf, std::span<const AllowedSubtags> subtags)
{
	/* Visitor to get a subtag handler's type. */
	struct type_visitor {
		char operator()(const DataHandler &) { return 'B'; }
		char operator()(const TextHandler &) { return 'T'; }
		char operator()(const BranchHandler &) { return 'C'; }
		char operator()(const AllowedSubtags::Span &) { return 'C'; }
	};

	/* Visitor to evaluate a subtag handler. */
	struct evaluate_visitor {
		ByteReader &buf;

		bool operator()(const DataHandler &handler)
		{
			size_t len = buf.ReadWord();
			if (buf.Remaining() < len) return false;
			return handler(len, buf);
		}

		bool operator()(const TextHandler &handler)
		{
			uint8_t langid = buf.ReadByte();
			return handler(langid, buf.ReadString());
		}

		bool operator()(const BranchHandler &handler)
		{
			return handler(buf);
		}

		bool operator()(const AllowedSubtags::Span &subtags)
		{
			return HandleNodes(buf, {subtags.first, subtags.second});
		}
	};

	for (const auto &tag : subtags) {
		if (tag.id != std::byteswap(id) || std::visit(type_visitor{}, tag.handler) != type) continue;
		return std::visit(evaluate_visitor{buf}, tag.handler);
	}

	GrfMsg(2, "StaticGRFInfo: unknown type/id combination found, type={:c}, id={:x}", type, id);
	return SkipUnknownInfo(buf, type);
}

/**
 * Handle the contents of a 'C' choice of an Action14
 * @param buf Buffer.
 * @param subtags List of subtags.
 * @return Whether the nodes could all be handled.
 */
static bool HandleNodes(ByteReader &buf, std::span<const AllowedSubtags> subtags)
{
	uint8_t type = buf.ReadByte();
	while (type != 0) {
		uint32_t id = buf.ReadDWord();
		if (!HandleNode(type, id, buf, subtags)) return false;
		type = buf.ReadByte();
	}
	return true;
}

/**
 * Handle Action 0x14 (static info)
 * @param buf Buffer.
 */
static void StaticGRFInfo(ByteReader &buf)
{
	/* <14> <type> <id> <text/data...> */
	HandleNodes(buf, _tags_root_static);
}

/**
 * Handle Action 0x14 (feature tests)
 * @param buf Buffer.
 */
static void Act14FeatureTest(ByteReader &buf)
{
	/* <14> <type> <id> <text/data...> */
	HandleNodes(buf, _tags_root_feature_tests);
}

/**
 * Set the current NewGRF as unsafe for static use
 * @note Used during safety scan on unsafe actions.
 */
static void GRFUnsafe(ByteReader &)
{
	_cur.grfconfig->flags.Set(GRFConfigFlag::Unsafe);

	/* Skip remainder of GRF */
	_cur.skip_sprites = -1;
}


/** Initialize the TTDPatch flags */
static void InitializeGRFSpecial()
{
	_ttdpatch_flags[0] = ((_settings_game.station.never_expire_airports ? 1U : 0U) << 0x0C)  // keepsmallairport
	                   |                                                       (1U << 0x0D)  // newairports
	                   |                                                       (1U << 0x0E)  // largestations
	                   | ((_settings_game.construction.max_bridge_length > 16 ? 1U : 0U) << 0x0F)  // longbridges
	                   |                                                       (0U << 0x10)  // loadtime
	                   |                                                       (1U << 0x12)  // presignals
	                   |                                                       (1U << 0x13)  // extpresignals
	                   | ((_settings_game.vehicle.never_expire_vehicles ? 1U : 0U) << 0x16)  // enginespersist
	                   |                                                       (1U << 0x1B)  // multihead
	                   |                                                       (1U << 0x1D)  // lowmemory
	                   |                                                       (1U << 0x1E); // generalfixes

	_ttdpatch_flags[1] =   ((_settings_game.economy.station_noise_level ? 1U : 0U) << 0x07)  // moreairports - based on units of noise
	                   |                                                       (1U << 0x08)  // mammothtrains
	                   |                                                       (1U << 0x09)  // trainrefit
	                   |                                                       (0U << 0x0B)  // subsidiaries
	                   |         ((_settings_game.order.gradual_loading ? 1U : 0U) << 0x0C)  // gradualloading
	                   |                                                       (1U << 0x12)  // unifiedmaglevmode - set bit 0 mode. Not revelant to OTTD
	                   |                                                       (1U << 0x13)  // unifiedmaglevmode - set bit 1 mode
	                   |                                                       (1U << 0x14)  // bridgespeedlimits
	                   |                                                       (1U << 0x16)  // eternalgame
	                   |                                                       (1U << 0x17)  // newtrains
	                   |                                                       (1U << 0x18)  // newrvs
	                   |                                                       (1U << 0x19)  // newships
	                   |                                                       (1U << 0x1A)  // newplanes
	                   | ((_settings_game.construction.train_signal_side == 1 ? 1U : 0U) << 0x1B)  // signalsontrafficside
	                   |       ((_settings_game.vehicle.disable_elrails ? 0U : 1U) << 0x1C); // electrifiedrailway

	_ttdpatch_flags[2] =                                                       (1U << 0x01)  // loadallgraphics - obsolote
	                   |                                                       (1U << 0x03)  // semaphores
	                   |                                                       (1U << 0x0A)  // newobjects
	                   |                                                       (0U << 0x0B)  // enhancedgui
	                   |                                                       (0U << 0x0C)  // newagerating
	                   |  ((_settings_game.construction.build_on_slopes ? 1U : 0U) << 0x0D)  // buildonslopes
	                   |                                                       (1U << 0x0E)  // fullloadany
	                   |                                                       (1U << 0x0F)  // planespeed
	                   |                                                       (0U << 0x10)  // moreindustriesperclimate - obsolete
	                   |                                                       (0U << 0x11)  // moretoylandfeatures
	                   |                                                       (1U << 0x12)  // newstations
	                   |                                                       (1U << 0x13)  // tracktypecostdiff
	                   |                                                       (1U << 0x14)  // manualconvert
	                   |  ((_settings_game.construction.build_on_slopes ? 1U : 0U) << 0x15)  // buildoncoasts
	                   |                                                       (1U << 0x16)  // canals
	                   |                                                       (1U << 0x17)  // newstartyear
	                   |    ((_settings_game.vehicle.freight_trains > 1 ? 1U : 0U) << 0x18)  // freighttrains
	                   |                                                       (1U << 0x19)  // newhouses
	                   |                                                       (1U << 0x1A)  // newbridges
	                   |                                                       (1U << 0x1B)  // newtownnames
	                   |                                                       (1U << 0x1C)  // moreanimation
	                   |    ((_settings_game.vehicle.wagon_speed_limits ? 1U : 0U) << 0x1D)  // wagonspeedlimits
	                   |                                                       (1U << 0x1E)  // newshistory
	                   |                                                       (0U << 0x1F); // custombridgeheads

	_ttdpatch_flags[3] =                                                       (0U << 0x00)  // newcargodistribution
	                   |                                                       (1U << 0x01)  // windowsnap
	                   | ((_settings_game.economy.allow_town_roads || _generating_world ? 0U : 1U) << 0x02)  // townbuildnoroad
	                   |                                                       (1U << 0x03)  // pathbasedsignalling
	                   |                                                       (0U << 0x04)  // aichoosechance
	                   |                                                       (1U << 0x05)  // resolutionwidth
	                   |                                                       (1U << 0x06)  // resolutionheight
	                   |                                                       (1U << 0x07)  // newindustries
	                   |           ((_settings_game.order.improved_load ? 1U : 0U) << 0x08)  // fifoloading
	                   |                                                       (0U << 0x09)  // townroadbranchprob
	                   |                                                       (0U << 0x0A)  // tempsnowline
	                   |                                                       (1U << 0x0B)  // newcargo
	                   |                                                       (1U << 0x0C)  // enhancemultiplayer
	                   |                                                       (1U << 0x0D)  // onewayroads
	                   |                                                       (1U << 0x0E)  // irregularstations
	                   |                                                       (1U << 0x0F)  // statistics
	                   |                                                       (1U << 0x10)  // newsounds
	                   |                                                       (1U << 0x11)  // autoreplace
	                   |                                                       (1U << 0x12)  // autoslope
	                   |                                                       (0U << 0x13)  // followvehicle
	                   |                                                       (1U << 0x14)  // trams
	                   |                                                       (0U << 0x15)  // enhancetunnels
	                   |                                                       (1U << 0x16)  // shortrvs
	                   |                                                       (1U << 0x17)  // articulatedrvs
	                   |       ((_settings_game.vehicle.dynamic_engines ? 1U : 0U) << 0x18)  // dynamic engines
	                   |                                                       (1U << 0x1E)  // variablerunningcosts
	                   |                                                       (1U << 0x1F); // any switch is on

	_ttdpatch_flags[4] =                                                       (1U << 0x00)  // larger persistent storage
	                   | ((_settings_game.economy.inflation && !_settings_game.economy.disable_inflation_newgrf_flag ? 1U : 0U) << 0x01) // inflation is on
	                   |                                                       (1U << 0x02); // extended string range
	MemSetT(_observed_ttdpatch_flags, 0, lengthof(_observed_ttdpatch_flags));
}

bool HasTTDPatchFlagBeenObserved(uint flag)
{
	uint index = flag / 0x20;
	flag %= 0x20;
	if (index >= lengthof(_ttdpatch_flags)) return false;
	return HasBit(_observed_ttdpatch_flags[index], flag);
}

/** Reset and clear all NewGRF stations */
static void ResetCustomStations()
{
	for (GRFFile * const file : _grf_files) {
		file->stations.clear();
	}
}

/** Reset and clear all NewGRF houses */
static void ResetCustomHouses()
{
	for (GRFFile * const file : _grf_files) {
		file->housespec.clear();
	}
}

/** Reset and clear all NewGRF airports */
static void ResetCustomAirports()
{
	for (GRFFile * const file : _grf_files) {
		file->airportspec.clear();
		file->airtspec.clear();
	}
}

/** Reset and clear all NewGRF industries */
static void ResetCustomIndustries()
{
	for (GRFFile * const file : _grf_files) {
		file->industryspec.clear();
		file->indtspec.clear();
	}
}

/** Reset and clear all NewObjects */
static void ResetCustomObjects()
{
	for (GRFFile * const file : _grf_files) {
		file->objectspec.clear();
	}
}

static void ResetCustomRoadStops()
{
	for (auto file : _grf_files) {
		file->roadstops.clear();
	}
}

/** Reset and clear all NewGRFs */
static void ResetNewGRF()
{
	for (GRFFile * const file : _grf_files) {
		delete file;
	}

	_grf_files.clear();
	_grf_file_map.clear();
	_cur.grffile   = nullptr;
	_new_signals_grfs.clear();
	MemSetT<NewSignalStyle>(_new_signal_styles.data(), 0, MAX_NEW_SIGNAL_STYLES);
	_num_new_signal_styles = 0;
	_new_landscape_rocks_grfs.clear();
}

/** Clear all NewGRF errors */
static void ResetNewGRFErrors()
{
	for (const auto &c : _grfconfig) {
		c->error.reset();
	}
}

/**
 * Reset all NewGRF loaded data
 */
void ResetNewGRFData()
{
	CleanUpStrings();
	CleanUpGRFTownNames();

	ResetBadges();

	/* Copy/reset original engine info data */
	SetupEngines();

	/* Copy/reset original bridge info data */
	ResetBridges();

	/* Reset rail type information */
	ResetRailTypes();

	/* Copy/reset original road type info data */
	ResetRoadTypes();

	/* Allocate temporary refit/cargo class data */
	_gted.resize(Engine::GetPoolSize());

	/* Fill rail type label temporary data for default trains */
	for (const Engine *e : Engine::IterateType(VEH_TRAIN)) {
		_gted[e->index].railtypelabel = GetRailTypeInfo(e->u.rail.railtype)->label;
	}

	/* Reset GRM reservations */
	memset(&_grm_engines, 0, sizeof(_grm_engines));
	memset(&_grm_cargoes, 0, sizeof(_grm_cargoes));

	/* Reset generic feature callback lists */
	ResetGenericCallbacks();

	/* Reset price base data */
	ResetPriceBaseMultipliers();

	/* Reset the curencies array */
	ResetCurrencies();

	/* Reset the house array */
	ResetCustomHouses();
	ResetHouses();

	/* Reset the industries structures*/
	ResetCustomIndustries();
	ResetIndustries();

	/* Reset the objects. */
	ObjectClass::Reset();
	ResetCustomObjects();
	ResetObjects();

	/* Reset station classes */
	StationClass::Reset();
	ResetCustomStations();

	/* Reset airport-related structures */
	AirportClass::Reset();
	ResetCustomAirports();
	AirportSpec::ResetAirports();
	AirportTileSpec::ResetAirportTiles();

	/* Reset road stop classes */
	RoadStopClass::Reset();
	ResetCustomRoadStops();

	/* Reset canal sprite groups and flags */
	_water_feature.fill({});

	/* Reset the snowline table. */
	ClearSnowLine();

	/* Reset NewGRF files */
	ResetNewGRF();

	/* Reset NewGRF errors. */
	ResetNewGRFErrors();

	/* Set up the default cargo types */
	SetupCargoForClimate(_settings_game.game_creation.landscape);

	/* Reset misc GRF features and train list display variables */
	_misc_grf_features = 0;

	_loaded_newgrf_features.has_2CC           = false;
	_loaded_newgrf_features.used_liveries     = 1 << LS_DEFAULT;
	_loaded_newgrf_features.shore             = SHORE_REPLACE_NONE;
	_loaded_newgrf_features.tram              = TRAMWAY_REPLACE_DEPOT_NONE;

	/* Clear all GRF overrides */
	_grf_id_overrides.clear();

	InitializeSoundPool();
	_spritegroup_pool.CleanPool();
	_callback_result_cache.clear();
	_deterministic_sg_shadows.clear();
	_randomized_sg_shadows.clear();
	_grfs_loaded_with_sg_shadow_enable = HasBit(_misc_debug_flags, MDF_NEWGRF_SG_SAVE_RAW);
}

/**
 * Reset NewGRF data which is stored persistently in savegames.
 */
void ResetPersistentNewGRFData()
{
	/* Reset override managers */
	_engine_mngr.ResetToDefaultMapping();
	_house_mngr.ResetMapping();
	_industry_mngr.ResetMapping();
	_industile_mngr.ResetMapping();
	_airport_mngr.ResetMapping();
	_airporttile_mngr.ResetMapping();
}

/**
 * Construct the Cargo Mapping
 * @note This is the reverse of a cargo translation table
 */
static void BuildCargoTranslationMap()
{
	_cur.grffile->cargo_map.fill(UINT8_MAX);

	auto cargo_list = GetCargoTranslationTable(*_cur.grffile);

	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		if (!cs->IsValid()) continue;

		/* Check the translation table for this cargo's label */
		int idx = find_index(cargo_list, cs->label);
		if (idx >= 0) _cur.grffile->cargo_map[cs->Index()] = idx;
	}
}

/**
 * Prepare loading a NewGRF file with its config
 * @param config The NewGRF configuration struct with name, id, parameters and alike.
 */
static void InitNewGRFFile(const GRFConfig &config)
{
	GRFFile *newfile = GetFileByFilename(config.filename);
	if (newfile != nullptr) {
		/* We already loaded it once. */
		_cur.grffile = newfile;
		return;
	}

	newfile = new GRFFile(config);
	_cur.grffile = newfile;
	_grf_files.push_back(newfile);
	_grf_file_map[newfile->grfid] = newfile;
}

/**
 * Constructor for GRFFile
 * @param config GRFConfig to copy name, grfid and parameters from.
 */
GRFFile::GRFFile(const GRFConfig &config)
{
	this->filename = config.filename;
	this->grfid = config.ident.grfid;

	/* Initialise local settings to defaults */
	this->traininfo_vehicle_pitch = 0;
	this->traininfo_vehicle_width = TRAININFO_DEFAULT_VEHICLE_WIDTH;

	this->new_signals_group = nullptr;
	this->new_signal_ctrl_flags = 0;
	this->new_signal_extra_aspects = 0;
	this->new_signal_style_mask = 1;
	this->current_new_signal_style = nullptr;

	this->new_rocks_group = nullptr;
	this->new_landscape_ctrl_flags = 0;

	/* Mark price_base_multipliers as 'not set' */
	for (Price i = PR_BEGIN; i < PR_END; i++) {
		this->price_base_multipliers[i] = INVALID_PRICE_MODIFIER;
	}

	/* Initialise rail type map with default rail types */
	this->railtype_map.fill(INVALID_RAILTYPE);
	this->railtype_map[0] = RAILTYPE_RAIL;
	this->railtype_map[1] = RAILTYPE_ELECTRIC;
	this->railtype_map[2] = RAILTYPE_MONO;
	this->railtype_map[3] = RAILTYPE_MAGLEV;

	/* Initialise road type map with default road types */
	this->roadtype_map.fill(INVALID_ROADTYPE);
	this->roadtype_map[0] = ROADTYPE_ROAD;

	/* Initialise tram type map with default tram types */
	this->tramtype_map.fill(INVALID_ROADTYPE);
	this->tramtype_map[0] = ROADTYPE_TRAM;

	/* Copy the initial parameter list */
	this->param = config.param;
}

/**
 * Find first cargo label that exists and is active from a list of cargo labels.
 * @param labels List of cargo labels.
 * @returns First cargo label in list that exists, or CT_INVALID if none exist.
 */
static CargoLabel GetActiveCargoLabel(const std::initializer_list<CargoLabel> &labels)
{
	for (const CargoLabel &label : labels) {
		CargoType cargo_type = GetCargoTypeByLabel(label);
		if (cargo_type != INVALID_CARGO) return label;
	}
	return CT_INVALID;
}

/**
 * Get active cargo label from either a cargo label or climate-dependent mixed cargo type.
 * @param label Cargo label or climate-dependent mixed cargo type.
 * @returns Active cargo label, or CT_INVALID if cargo label is not active.
 */
static CargoLabel GetActiveCargoLabel(const std::variant<CargoLabel, MixedCargoType> &label)
{
	struct visitor {
		CargoLabel operator()(const CargoLabel &label) { return label; }
		CargoLabel operator()(const MixedCargoType &mixed)
		{
			switch (mixed) {
				case MCT_LIVESTOCK_FRUIT: return GetActiveCargoLabel({CT_LIVESTOCK, CT_FRUIT});
				case MCT_GRAIN_WHEAT_MAIZE: return GetActiveCargoLabel({CT_GRAIN, CT_WHEAT, CT_MAIZE});
				case MCT_VALUABLES_GOLD_DIAMONDS: return GetActiveCargoLabel({CT_VALUABLES, CT_GOLD, CT_DIAMONDS});
				default: NOT_REACHED();
			}
		}
	};

	return std::visit(visitor{}, label);
}

/**
 * Precalculate refit masks from cargo classes for all vehicles.
 */
static void CalculateRefitMasks()
{
	CargoTypes original_known_cargoes = 0;
	for (CargoType cargo_type = 0; cargo_type != NUM_CARGO; ++cargo_type) {
		if (IsDefaultCargo(cargo_type)) SetBit(original_known_cargoes, cargo_type);
	}

	for (Engine *e : Engine::Iterate()) {
		EngineID engine = e->index;
		EngineInfo *ei = &e->info;
		bool only_defaultcargo; ///< Set if the vehicle shall carry only the default cargo

		/* Apply default cargo translation map if cargo type hasn't been set, either explicitly or by aircraft cargo handling. */
		if (!IsValidCargoType(e->info.cargo_type)) {
			e->info.cargo_type = GetCargoTypeByLabel(GetActiveCargoLabel(e->info.cargo_label));
		}

		/* If the NewGRF did not set any cargo properties, we apply default values. */
		if (_gted[engine].defaultcargo_grf == nullptr) {
			/* If the vehicle has any capacity, apply the default refit masks */
			if (e->type != VEH_TRAIN || e->u.rail.capacity != 0) {
				static constexpr LandscapeType T = LandscapeType::Temperate;
				static constexpr LandscapeType A = LandscapeType::Arctic;
				static constexpr LandscapeType S = LandscapeType::Tropic;
				static constexpr LandscapeType Y = LandscapeType::Toyland;
				static const struct DefaultRefitMasks {
					LandscapeTypes climate;
					CargoLabel cargo_label;
					CargoClasses cargo_allowed;
					CargoClasses cargo_disallowed;
				} _default_refit_masks[] = {
					{{T, A, S, Y}, CT_PASSENGERS, CC_PASSENGERS,               0},
					{{T, A, S   }, CT_MAIL,       CC_MAIL,                     0},
					{{T, A, S   }, CT_VALUABLES,  CC_ARMOURED,                 CC_LIQUID},
					{{         Y}, CT_MAIL,       CC_MAIL | CC_ARMOURED,       CC_LIQUID},
					{{T, A      }, CT_COAL,       CC_BULK,                     0},
					{{      S   }, CT_COPPER_ORE, CC_BULK,                     0},
					{{         Y}, CT_SUGAR,      CC_BULK,                     0},
					{{T, A, S   }, CT_OIL,        CC_LIQUID,                   0},
					{{         Y}, CT_COLA,       CC_LIQUID,                   0},
					{{T         }, CT_GOODS,      CC_PIECE_GOODS | CC_EXPRESS, CC_LIQUID | CC_PASSENGERS},
					{{   A, S   }, CT_GOODS,      CC_PIECE_GOODS | CC_EXPRESS, CC_LIQUID | CC_PASSENGERS | CC_REFRIGERATED},
					{{   A, S   }, CT_FOOD,       CC_REFRIGERATED,             0},
					{{         Y}, CT_CANDY,      CC_PIECE_GOODS | CC_EXPRESS, CC_LIQUID | CC_PASSENGERS},
				};

				if (e->type == VEH_AIRCRAFT) {
					/* Aircraft default to "light" cargoes */
					_gted[engine].cargo_allowed = CC_PASSENGERS | CC_MAIL | CC_ARMOURED | CC_EXPRESS;
					_gted[engine].cargo_disallowed = CC_LIQUID;
				} else if (e->type == VEH_SHIP) {
					CargoLabel label = GetActiveCargoLabel(ei->cargo_label);
					switch (label.base()) {
						case CT_PASSENGERS.base():
							/* Ferries */
							_gted[engine].cargo_allowed = CC_PASSENGERS;
							_gted[engine].cargo_disallowed = 0;
							break;
						case CT_OIL.base():
							/* Tankers */
							_gted[engine].cargo_allowed = CC_LIQUID;
							_gted[engine].cargo_disallowed = 0;
							break;
						default:
							/* Cargo ships */
							if (_settings_game.game_creation.landscape == LandscapeType::Toyland) {
								/* No tanker in toyland :( */
								_gted[engine].cargo_allowed = CC_MAIL | CC_ARMOURED | CC_EXPRESS | CC_BULK | CC_PIECE_GOODS | CC_LIQUID;
								_gted[engine].cargo_disallowed = CC_PASSENGERS;
							} else {
								_gted[engine].cargo_allowed = CC_MAIL | CC_ARMOURED | CC_EXPRESS | CC_BULK | CC_PIECE_GOODS;
								_gted[engine].cargo_disallowed = CC_LIQUID | CC_PASSENGERS;
							}
							break;
					}
					e->u.ship.old_refittable = true;
				} else if (e->type == VEH_TRAIN && e->u.rail.railveh_type != RAILVEH_WAGON) {
					/* Train engines default to all cargoes, so you can build single-cargo consists with fast engines.
					 * Trains loading multiple cargoes may start stations accepting unwanted cargoes. */
					_gted[engine].cargo_allowed = CC_PASSENGERS | CC_MAIL | CC_ARMOURED | CC_EXPRESS | CC_BULK | CC_PIECE_GOODS | CC_LIQUID;
					_gted[engine].cargo_disallowed = 0;
				} else {
					/* Train wagons and road vehicles are classified by their default cargo type */
					CargoLabel label = GetActiveCargoLabel(ei->cargo_label);
					for (const auto &drm : _default_refit_masks) {
						if (!drm.climate.Test(_settings_game.game_creation.landscape)) continue;
						if (drm.cargo_label != label) continue;

						_gted[engine].cargo_allowed = drm.cargo_allowed;
						_gted[engine].cargo_disallowed = drm.cargo_disallowed;
						break;
					}

					/* All original cargoes have specialised vehicles, so exclude them */
					_gted[engine].ctt_exclude_mask = original_known_cargoes;
				}
			}
			_gted[engine].UpdateRefittability(_gted[engine].cargo_allowed != 0);

			if (IsValidCargoType(ei->cargo_type)) ClrBit(_gted[engine].ctt_exclude_mask, ei->cargo_type);
		}

		/* Compute refittability */
		{
			CargoTypes mask = 0;
			CargoTypes not_mask = 0;
			CargoTypes xor_mask = ei->refit_mask;

			/* If the original masks set by the grf are zero, the vehicle shall only carry the default cargo.
			 * Note: After applying the translations, the vehicle may end up carrying no defined cargo. It becomes unavailable in that case. */
			only_defaultcargo = _gted[engine].refittability != GRFTempEngineData::NONEMPTY;

			if (_gted[engine].cargo_allowed != 0) {
				/* Build up the list of cargo types from the set cargo classes. */
				for (const CargoSpec *cs : CargoSpec::Iterate()) {
					if ((_gted[engine].cargo_allowed & cs->classes) != 0 && (_gted[engine].cargo_allowed_required & cs->classes) == _gted[engine].cargo_allowed_required) SetBit(mask, cs->Index());
					if (_gted[engine].cargo_disallowed & cs->classes) SetBit(not_mask, cs->Index());
				}
			}

			ei->refit_mask = ((mask & ~not_mask) ^ xor_mask) & _cargo_mask;

			/* Apply explicit refit includes/excludes. */
			ei->refit_mask |= _gted[engine].ctt_include_mask;
			ei->refit_mask &= ~_gted[engine].ctt_exclude_mask;

			/* Custom refit mask callback. */
			const GRFFile *file = _gted[e->index].defaultcargo_grf;
			if (file == nullptr) file = e->GetGRF();
			if (file != nullptr && e->info.callback_mask.Test(VehicleCallbackMask::CustomRefit)) {
				for (const CargoSpec *cs : CargoSpec::Iterate()) {
					uint8_t local_slot = file->cargo_map[cs->Index()];
					uint16_t callback = GetVehicleCallback(CBID_VEHICLE_CUSTOM_REFIT, cs->classes, local_slot, engine, nullptr);
					switch (callback) {
						case CALLBACK_FAILED:
						case 0:
							break; // Do nothing.
						case 1: SetBit(ei->refit_mask, cs->Index()); break;
						case 2: ClrBit(ei->refit_mask, cs->Index()); break;

						default: ErrorUnknownCallbackResult(file->grfid, CBID_VEHICLE_CUSTOM_REFIT, callback);
					}
				}
			}
		}

		/* Clear invalid cargoslots (from default vehicles or pre-NewCargo GRFs) */
		if (IsValidCargoType(ei->cargo_type) && !HasBit(_cargo_mask, ei->cargo_type)) ei->cargo_type = INVALID_CARGO;

		/* Ensure that the vehicle is either not refittable, or that the default cargo is one of the refittable cargoes.
		 * Note: Vehicles refittable to no cargo are handle differently to vehicle refittable to a single cargo. The latter might have subtypes. */
		if (!only_defaultcargo && (e->type != VEH_SHIP || e->u.ship.old_refittable) && IsValidCargoType(ei->cargo_type) && !HasBit(ei->refit_mask, ei->cargo_type)) {
			ei->cargo_type = INVALID_CARGO;
		}

		/* Check if this engine's cargo type is valid. If not, set to the first refittable
		 * cargo type. Finally disable the vehicle, if there is still no cargo. */
		if (!IsValidCargoType(ei->cargo_type) && ei->refit_mask != 0) {
			/* Figure out which CTT to use for the default cargo, if it is 'first refittable'. */
			const GRFFile *file = _gted[engine].defaultcargo_grf;
			if (file == nullptr) file = e->GetGRF();
			if (file != nullptr && file->grf_version >= 8 && !file->cargo_list.empty()) {
				/* Use first refittable cargo from cargo translation table */
				uint8_t best_local_slot = UINT8_MAX;
				for (CargoType cargo_type : SetCargoBitIterator(ei->refit_mask)) {
					uint8_t local_slot = file->cargo_map[cargo_type];
					if (local_slot < best_local_slot) {
						best_local_slot = local_slot;
						ei->cargo_type = cargo_type;
					}
				}
			}

			if (!IsValidCargoType(ei->cargo_type)) {
				/* Use first refittable cargo slot */
				ei->cargo_type = (CargoType)FindFirstBit(ei->refit_mask);
			}
		}
		if (!IsValidCargoType(ei->cargo_type) && e->type == VEH_TRAIN && e->u.rail.railveh_type != RAILVEH_WAGON && e->u.rail.capacity == 0) {
			/* For train engines which do not carry cargo it does not matter if their cargo type is invalid.
			 * Fallback to the first available instead, if the cargo type has not been changed (as indicated by
			 * cargo_label not being CT_INVALID). */
			if (GetActiveCargoLabel(ei->cargo_label) != CT_INVALID) {
				ei->cargo_type = static_cast<CargoType>(FindFirstBit(_standard_cargo_mask));
			}
		}
		if (!IsValidCargoType(ei->cargo_type)) ei->climates = {};

		/* Clear refit_mask for not refittable ships */
		if (e->type == VEH_SHIP && !e->u.ship.old_refittable) {
			ei->refit_mask = 0;
		}
	}
}

/** Set to use the correct action0 properties for each canal feature */
static void FinaliseCanals()
{
	for (uint i = 0; i < CF_END; i++) {
		if (_water_feature[i].grffile != nullptr) {
			_water_feature[i].callback_mask = _water_feature[i].grffile->canal_local_properties[i].callback_mask;
			_water_feature[i].flags = _water_feature[i].grffile->canal_local_properties[i].flags;
		}
	}
}

/** Check for invalid engines */
static void FinaliseEngineArray()
{
	for (Engine *e : Engine::Iterate()) {
		if (e->GetGRF() == nullptr) {
			const EngineIDMapping &eid = _engine_mngr.mappings[e->index];
			if (eid.grfid != INVALID_GRFID || eid.internal_id != eid.substitute_id) {
				e->info.string_id = STR_NEWGRF_INVALID_ENGINE;
			}
		}

		/* Do final mapping on variant engine ID. */
		if (e->info.variant_id != INVALID_ENGINE) {
			e->info.variant_id = GetNewEngineID(e->grf_prop.grffile, e->type, e->info.variant_id);
		}

		if (!e->info.climates.Test(_settings_game.game_creation.landscape)) continue;

		switch (e->type) {
			case VEH_TRAIN: AppendCopyableBadgeList(e->badges, GetRailTypeInfo(e->u.rail.railtype)->badges, GSF_TRAINS); break;
			case VEH_ROAD: AppendCopyableBadgeList(e->badges, GetRoadTypeInfo(e->u.road.roadtype)->badges, GSF_ROADVEHICLES); break;
			default: break;
		}

		/* Skip wagons, there livery is defined via the engine */
		if (e->type != VEH_TRAIN || e->u.rail.railveh_type != RAILVEH_WAGON) {
			LiveryScheme ls = GetEngineLiveryScheme(e->index, INVALID_ENGINE, nullptr);
			SetBit(_loaded_newgrf_features.used_liveries, ls);
			/* Note: For ships and roadvehicles we assume that they cannot be refitted between passenger and freight */

			if (e->type == VEH_TRAIN) {
				SetBit(_loaded_newgrf_features.used_liveries, LS_FREIGHT_WAGON);
				switch (ls) {
					case LS_STEAM:
					case LS_DIESEL:
					case LS_ELECTRIC:
					case LS_MONORAIL:
					case LS_MAGLEV:
						SetBit(_loaded_newgrf_features.used_liveries, LS_PASSENGER_WAGON_STEAM + ls - LS_STEAM);
						break;

					case LS_DMU:
					case LS_EMU:
						SetBit(_loaded_newgrf_features.used_liveries, LS_PASSENGER_WAGON_DIESEL + ls - LS_DMU);
						break;

					default: NOT_REACHED();
				}
			}
		}
	}

	/* Check engine variants don't point back on themselves (either directly or via a loop) then set appropriate flags
	 * on variant engine. This is performed separately as all variant engines need to have been resolved. */
	for (Engine *e : Engine::Iterate()) {
		EngineID parent = e->info.variant_id;
		while (parent != INVALID_ENGINE) {
			parent = Engine::Get(parent)->info.variant_id;
			if (parent != e->index) continue;

			/* Engine looped back on itself, so clear the variant. */
			e->info.variant_id = INVALID_ENGINE;

			GrfMsg(1, "FinaliseEngineArray: Variant of engine {:x} in '{}' loops back on itself", _engine_mngr.mappings[e->index].internal_id, e->GetGRF()->filename);
			break;
		}

		if (e->info.variant_id != INVALID_ENGINE) {
			Engine::Get(e->info.variant_id)->display_flags.Set(EngineDisplayFlag::HasVariants).Set(EngineDisplayFlag::IsFolded);
		}
	}
}

/** Check for invalid cargoes */
void FinaliseCargoArray()
{
	for (CargoSpec &cs : CargoSpec::array) {
		if (cs.town_production_effect == INVALID_TPE) {
			/* Set default town production effect by cargo label. */
			switch (cs.label.base()) {
				case CT_PASSENGERS.base(): cs.town_production_effect = TPE_PASSENGERS; break;
				case CT_MAIL.base():       cs.town_production_effect = TPE_MAIL; break;
				default:                   cs.town_production_effect = TPE_NONE; break;
			}
		}
		if (!cs.IsValid()) {
			cs.name = cs.name_single = cs.units_volume = STR_NEWGRF_INVALID_CARGO;
			cs.quantifier = STR_NEWGRF_INVALID_CARGO_QUANTITY;
			cs.abbrev = STR_NEWGRF_INVALID_CARGO_ABBREV;
		}
	}
}

/**
 * Check if a given housespec is valid and disable it if it's not.
 * The housespecs that follow it are used to check the validity of
 * multitile houses.
 * @param hs The housespec to check.
 * @param next1 The housespec that follows \c hs.
 * @param next2 The housespec that follows \c next1.
 * @param next3 The housespec that follows \c next2.
 * @param filename The filename of the newgrf this house was defined in.
 * @return Whether the given housespec is valid.
 */
static bool IsHouseSpecValid(HouseSpec *hs, const HouseSpec *next1, const HouseSpec *next2, const HouseSpec *next3, const std::string &filename)
{
	if ((hs->building_flags.Any(BUILDING_HAS_2_TILES) &&
				(next1 == nullptr || !next1->enabled || next1->building_flags.Any(BUILDING_HAS_1_TILE))) ||
			(hs->building_flags.Any(BUILDING_HAS_4_TILES) &&
				(next2 == nullptr || !next2->enabled || next2->building_flags.Any(BUILDING_HAS_1_TILE) ||
				next3 == nullptr || !next3->enabled || next3->building_flags.Any(BUILDING_HAS_1_TILE)))) {
		hs->enabled = false;
		if (!filename.empty()) Debug(grf, 1, "FinaliseHouseArray: {} defines house {} as multitile, but no suitable tiles follow. Disabling house.", filename, hs->grf_prop.local_id);
		return false;
	}

	/* Some places sum population by only counting north tiles. Other places use all tiles causing desyncs.
	 * As the newgrf specs define population to be zero for non-north tiles, we just disable the offending house.
	 * If you want to allow non-zero populations somewhen, make sure to sum the population of all tiles in all places. */
	if ((hs->building_flags.Any(BUILDING_HAS_2_TILES) && next1->population != 0) ||
			(hs->building_flags.Any(BUILDING_HAS_4_TILES) && (next2->population != 0 || next3->population != 0))) {
		hs->enabled = false;
		if (!filename.empty()) Debug(grf, 1, "FinaliseHouseArray: {} defines multitile house {} with non-zero population on additional tiles. Disabling house.", filename, hs->grf_prop.local_id);
		return false;
	}

	/* Substitute type is also used for override, and having an override with a different size causes crashes.
	 * This check should only be done for NewGRF houses because grf_prop.subst_id is not set for original houses.*/
	if (!filename.empty() && (hs->building_flags & BUILDING_HAS_1_TILE) != (HouseSpec::Get(hs->grf_prop.subst_id)->building_flags & BUILDING_HAS_1_TILE)) {
		hs->enabled = false;
		Debug(grf, 1, "FinaliseHouseArray: {} defines house {} with different house size then it's substitute type. Disabling house.", filename, hs->grf_prop.local_id);
		return false;
	}

	/* Make sure that additional parts of multitile houses are not available. */
	if (!hs->building_flags.Any(BUILDING_HAS_1_TILE) && (hs->building_availability & HZ_ZONALL) != 0 && (hs->building_availability & HZ_CLIMALL) != 0) {
		hs->enabled = false;
		if (!filename.empty()) Debug(grf, 1, "FinaliseHouseArray: {} defines house {} without a size but marked it as available. Disabling house.", filename, hs->grf_prop.local_id);
		return false;
	}

	return true;
}

/**
 * Make sure there is at least one house available in the year 0 for the given
 * climate / housezone combination.
 * @param bitmask The climate and housezone to check for. Exactly one climate
 *   bit and one housezone bit should be set.
 */
static void EnsureEarlyHouse(HouseZones bitmask)
{
	CalTime::Year min_year = CalTime::MAX_YEAR;

	for (const auto &hs : HouseSpec::Specs()) {
		if (!hs.enabled) continue;
		if ((hs.building_availability & bitmask) != bitmask) continue;
		if (hs.min_year < min_year) min_year = hs.min_year;
	}

	if (min_year == 0) return;

	for (auto &hs : HouseSpec::Specs()) {
		if (!hs.enabled) continue;
		if ((hs.building_availability & bitmask) != bitmask) continue;
		if (hs.min_year == min_year) hs.min_year = CalTime::MIN_YEAR;
	}
}

/**
 * Add all new houses to the house array. House properties can be set at any
 * time in the GRF file, so we can only add a house spec to the house array
 * after the file has finished loading. We also need to check the dates, due to
 * the TTDPatch behaviour described below that we need to emulate.
 */
static void FinaliseHouseArray()
{
	/* If there are no houses with start dates before 1930, then all houses
	 * with start dates of 1930 have them reset to 0. This is in order to be
	 * compatible with TTDPatch, where if no houses have start dates before
	 * 1930 and the date is before 1930, the game pretends that this is 1930.
	 * If there have been any houses defined with start dates before 1930 then
	 * the dates are left alone.
	 * On the other hand, why 1930? Just 'fix' the houses with the lowest
	 * minimum introduction date to 0.
	 */
	for (GRFFile * const file : _grf_files) {
		if (file->housespec.empty()) continue;

		size_t num_houses = file->housespec.size();
		for (size_t i = 0; i < num_houses; i++) {
			HouseSpec *hs = file->housespec[i].get();

			if (hs == nullptr) continue;

			const HouseSpec *next1 = (i + 1 < num_houses ? file->housespec[i + 1].get() : nullptr);
			const HouseSpec *next2 = (i + 2 < num_houses ? file->housespec[i + 2].get() : nullptr);
			const HouseSpec *next3 = (i + 3 < num_houses ? file->housespec[i + 3].get() : nullptr);

			if (!IsHouseSpecValid(hs, next1, next2, next3, file->filename)) continue;

			_house_mngr.SetEntitySpec(hs);
		}
	}

	for (size_t i = 0; i < HouseSpec::Specs().size(); i++) {
		HouseSpec *hs = HouseSpec::Get(i);
		const HouseSpec *next1 = (i + 1 < NUM_HOUSES ? HouseSpec::Get(i + 1) : nullptr);
		const HouseSpec *next2 = (i + 2 < NUM_HOUSES ? HouseSpec::Get(i + 2) : nullptr);
		const HouseSpec *next3 = (i + 3 < NUM_HOUSES ? HouseSpec::Get(i + 3) : nullptr);

		/* We need to check all houses again to we are sure that multitile houses
		 * did get consecutive IDs and none of the parts are missing. */
		if (!IsHouseSpecValid(hs, next1, next2, next3, std::string{})) {
			/* GetHouseNorthPart checks 3 houses that are directly before
			 * it in the house pool. If any of those houses have multi-tile
			 * flags set it assumes it's part of a multitile house. Since
			 * we can have invalid houses in the pool marked as disabled, we
			 * don't want to have them influencing valid tiles. As such set
			 * building_flags to zero here to make sure any house following
			 * this one in the pool is properly handled as 1x1 house. */
			hs->building_flags = {};
		}

		/* Apply default cargo translation map for unset cargo slots */
		for (uint i = 0; i < lengthof(hs->accepts_cargo_label); ++i) {
			if (!IsValidCargoType(hs->accepts_cargo[i])) hs->accepts_cargo[i] = GetCargoTypeByLabel(hs->accepts_cargo_label[i]);
			/* Disable acceptance if cargo type is invalid. */
			if (!IsValidCargoType(hs->accepts_cargo[i])) hs->cargo_acceptance[i] = 0;
		}
	}

	HouseZones climate_mask = (HouseZones)(1 << (to_underlying(_settings_game.game_creation.landscape) + 12));
	EnsureEarlyHouse(HZ_ZON1 | climate_mask);
	EnsureEarlyHouse(HZ_ZON2 | climate_mask);
	EnsureEarlyHouse(HZ_ZON3 | climate_mask);
	EnsureEarlyHouse(HZ_ZON4 | climate_mask);
	EnsureEarlyHouse(HZ_ZON5 | climate_mask);

	if (_settings_game.game_creation.landscape == LandscapeType::Arctic) {
		EnsureEarlyHouse(HZ_ZON1 | HZ_SUBARTC_ABOVE);
		EnsureEarlyHouse(HZ_ZON2 | HZ_SUBARTC_ABOVE);
		EnsureEarlyHouse(HZ_ZON3 | HZ_SUBARTC_ABOVE);
		EnsureEarlyHouse(HZ_ZON4 | HZ_SUBARTC_ABOVE);
		EnsureEarlyHouse(HZ_ZON5 | HZ_SUBARTC_ABOVE);
	}
}

/**
 * Add all new industries to the industry array. Industry properties can be set at any
 * time in the GRF file, so we can only add a industry spec to the industry array
 * after the file has finished loading.
 */
static void FinaliseIndustriesArray()
{
	for (GRFFile * const file : _grf_files) {
		for (const auto &indsp : file->industryspec) {
			if (indsp == nullptr || !indsp->enabled) continue;

			_industry_mngr.SetEntitySpec(indsp.get());
		}

		for (const auto &indtsp : file->indtspec) {
			if (indtsp != nullptr) {
				_industile_mngr.SetEntitySpec(indtsp.get());
			}
		}
	}

	for (auto &indsp : _industry_specs) {
		if (indsp.enabled && indsp.grf_prop.HasGrfFile()) {
			for (auto &conflicting : indsp.conflicting) {
				conflicting = MapNewGRFIndustryType(conflicting, indsp.grf_prop.grfid);
			}
		}
		if (!indsp.enabled) {
			indsp.name = STR_NEWGRF_INVALID_INDUSTRYTYPE;
		}

		/* Apply default cargo translation map for unset cargo slots */
		for (size_t i = 0; i < std::size(indsp.produced_cargo_label); ++i) {
			if (!IsValidCargoType(indsp.produced_cargo[i])) indsp.produced_cargo[i] = GetCargoTypeByLabel(GetActiveCargoLabel(indsp.produced_cargo_label[i]));
		}
		for (size_t i = 0; i < std::size(indsp.accepts_cargo_label); ++i) {
			if (!IsValidCargoType(indsp.accepts_cargo[i])) indsp.accepts_cargo[i] = GetCargoTypeByLabel(GetActiveCargoLabel(indsp.accepts_cargo_label[i]));
		}
	}

	for (auto &indtsp : _industry_tile_specs) {
		/* Apply default cargo translation map for unset cargo slots */
		for (size_t i = 0; i < std::size(indtsp.accepts_cargo_label); ++i) {
			if (!IsValidCargoType(indtsp.accepts_cargo[i])) indtsp.accepts_cargo[i] = GetCargoTypeByLabel(GetActiveCargoLabel(indtsp.accepts_cargo_label[i]));
		}
	}
}

/**
 * Add all new objects to the object array. Object properties can be set at any
 * time in the GRF file, so we can only add an object spec to the object array
 * after the file has finished loading.
 */
static void FinaliseObjectsArray()
{
	for (GRFFile * const file : _grf_files) {
		for (auto &objectspec : file->objectspec) {
			if (objectspec != nullptr && objectspec->grf_prop.HasGrfFile() && objectspec->IsEnabled()) {
				_object_mngr.SetEntitySpec(objectspec.get());
			}
		}
	}

	ObjectSpec::BindToClasses();
}

/**
 * Add all new airports to the airport array. Airport properties can be set at any
 * time in the GRF file, so we can only add a airport spec to the airport array
 * after the file has finished loading.
 */
static void FinaliseAirportsArray()
{
	for (GRFFile * const file : _grf_files) {
		for (auto &as : file->airportspec) {
			if (as != nullptr && as->enabled) {
				_airport_mngr.SetEntitySpec(as.get());
			}
		}

		for (auto &ats : file->airtspec) {
			if (ats != nullptr && ats->enabled) {
				_airporttile_mngr.SetEntitySpec(ats.get());
			}
		}
	}
}

/* Here we perform initial decoding of some special sprites (as are they
 * described at http://www.ttdpatch.net/src/newgrf.txt, but this is only a very
 * partial implementation yet).
 * XXX: We consider GRF files trusted. It would be trivial to exploit OTTD by
 * a crafted invalid GRF file. We should tell that to the user somehow, or
 * better make this more robust in the future. */
static void DecodeSpecialSprite(uint8_t *buf, uint num, GrfLoadingStage stage)
{
	/* XXX: There is a difference between staged loading in TTDPatch and
	 * here.  In TTDPatch, for some reason actions 1 and 2 are carried out
	 * during stage 1, whilst action 3 is carried out during stage 2 (to
	 * "resolve" cargo IDs... wtf). This is a little problem, because cargo
	 * IDs are valid only within a given set (action 1) block, and may be
	 * overwritten after action 3 associates them. But overwriting happens
	 * in an earlier stage than associating, so...  We just process actions
	 * 1 and 2 in stage 2 now, let's hope that won't get us into problems.
	 * --pasky
	 * We need a pre-stage to set up GOTO labels of Action 0x10 because the grf
	 * is not in memory and scanning the file every time would be too expensive.
	 * In other stages we skip action 0x10 since it's already dealt with. */
	static const SpecialSpriteHandler handlers[][GLS_END] = {
		/* 0x00 */ { nullptr,       SafeChangeInfo, nullptr,         nullptr,         ReserveChangeInfo, FeatureChangeInfo, },
		/* 0x01 */ { SkipAct1,      SkipAct1,       SkipAct1,        SkipAct1,        SkipAct1,          NewSpriteSet, },
		/* 0x02 */ { nullptr,       nullptr,        nullptr,         nullptr,         nullptr,           NewSpriteGroup, },
		/* 0x03 */ { nullptr,       GRFUnsafe,      nullptr,         nullptr,         nullptr,           FeatureMapSpriteGroup, },
		/* 0x04 */ { nullptr,       nullptr,        nullptr,         nullptr,         nullptr,           FeatureNewName, },
		/* 0x05 */ { SkipAct5,      SkipAct5,       SkipAct5,        SkipAct5,        SkipAct5,          GraphicsNew, },
		/* 0x06 */ { nullptr,       nullptr,        nullptr,         CfgApply,        CfgApply,          CfgApply, },
		/* 0x07 */ { nullptr,       nullptr,        nullptr,         nullptr,         SkipIf,            SkipIf, },
		/* 0x08 */ { ScanInfo,      nullptr,        nullptr,         GRFInfo,         GRFInfo,           GRFInfo, },
		/* 0x09 */ { nullptr,       nullptr,        nullptr,         SkipIf,          SkipIf,            SkipIf, },
		/* 0x0A */ { SkipActA,      SkipActA,       SkipActA,        SkipActA,        SkipActA,          SpriteReplace, },
		/* 0x0B */ { nullptr,       nullptr,        nullptr,         GRFLoadError,    GRFLoadError,      GRFLoadError, },
		/* 0x0C */ { nullptr,       nullptr,        nullptr,         GRFComment,      nullptr,           GRFComment, },
		/* 0x0D */ { nullptr,       SafeParamSet,   nullptr,         ParamSet,        ParamSet,          ParamSet, },
		/* 0x0E */ { nullptr,       SafeGRFInhibit, nullptr,         GRFInhibit,      GRFInhibit,        GRFInhibit, },
		/* 0x0F */ { nullptr,       GRFUnsafe,      nullptr,         FeatureTownName, nullptr,           nullptr, },
		/* 0x10 */ { nullptr,       nullptr,        DefineGotoLabel, nullptr,         nullptr,           nullptr, },
		/* 0x11 */ { SkipAct11,     GRFUnsafe,      SkipAct11,       GRFSound,        SkipAct11,         GRFSound, },
		/* 0x12 */ { SkipAct12,     SkipAct12,      SkipAct12,       SkipAct12,       SkipAct12,         LoadFontGlyph, },
		/* 0x13 */ { nullptr,       nullptr,        nullptr,         nullptr,         nullptr,           TranslateGRFStrings, },
		/* 0x14 */ { StaticGRFInfo, nullptr,        nullptr,         Act14FeatureTest,nullptr,           nullptr, },
	};

	GRFLocation location(_cur.grfconfig->ident.grfid, _cur.nfo_line);

	GRFLineToSpriteOverride::iterator it = _grf_line_to_action6_sprite_override.find(location);
	_action6_override_active = (it != _grf_line_to_action6_sprite_override.end());
	if (it == _grf_line_to_action6_sprite_override.end()) {
		/* No preloaded sprite to work with; read the
		 * pseudo sprite content. */
		_cur.file->ReadBlock(buf, num);
	} else {
		/* Use the preloaded sprite data. */
		buf = it->second.get();
		GrfMsg(7, "DecodeSpecialSprite: Using preloaded pseudo sprite data");

		/* Skip the real (original) content of this action. */
		_cur.file->SeekTo(num, SEEK_CUR);
	}

	ByteReader br(buf, buf + num);

	try {
		uint8_t action = br.ReadByte();

		if (action == 0xFF) {
			GrfMsg(2, "DecodeSpecialSprite: Unexpected data block, skipping");
		} else if (action == 0xFE) {
			GrfMsg(2, "DecodeSpecialSprite: Unexpected import block, skipping");
		} else if (action >= lengthof(handlers)) {
			GrfMsg(7, "DecodeSpecialSprite: Skipping unknown action 0x{:02X}", action);
		} else if (handlers[action][stage] == nullptr) {
			GrfMsg(7, "DecodeSpecialSprite: Skipping action 0x{:02X} in stage {}", action, stage);
		} else {
			GrfMsg(7, "DecodeSpecialSprite: Handling action 0x{:02X} in stage {}", action, stage);
			handlers[action][stage](br);
		}
	} catch (...) {
		GrfMsg(1, "DecodeSpecialSprite: Tried to read past end of pseudo-sprite data");
		DisableGrf(STR_NEWGRF_ERROR_READ_BOUNDS);
	}
}

/**
 * Load a particular NewGRF from a SpriteFile.
 * @param config The configuration of the to be loaded NewGRF.
 * @param stage  The loading stage of the NewGRF.
 * @param file   The file to load the GRF data from.
 */
static void LoadNewGRFFileFromFile(GRFConfig &config, GrfLoadingStage stage, SpriteFile &file)
{
	_cur.file = &file;
	_cur.grfconfig = &config;

	Debug(grf, 2, "LoadNewGRFFile: Reading NewGRF-file '{}'", config.GetDisplayPath());

	uint8_t grf_container_version = file.GetContainerVersion();
	if (grf_container_version == 0) {
		Debug(grf, 7, "LoadNewGRFFile: Custom .grf has invalid format");
		return;
	}

	if (stage == GLS_INIT || stage == GLS_ACTIVATION) {
		/* We need the sprite offsets in the init stage for NewGRF sounds
		 * and in the activation stage for real sprites. */
		ReadGRFSpriteOffsets(file);
	} else {
		/* Skip sprite section offset if present. */
		if (grf_container_version >= 2) file.ReadDword();
	}

	if (grf_container_version >= 2) {
		/* Read compression value. */
		uint8_t compression = file.ReadByte();
		if (compression != 0) {
			Debug(grf, 7, "LoadNewGRFFile: Unsupported compression format");
			return;
		}
	}

	/* Skip the first sprite; we don't care about how many sprites this
	 * does contain; newest TTDPatches and George's longvehicles don't
	 * neither, apparently. */
	uint32_t num = grf_container_version >= 2 ? file.ReadDword() : file.ReadWord();
	if (num == 4 && file.ReadByte() == 0xFF) {
		file.ReadDword();
	} else {
		Debug(grf, 7, "LoadNewGRFFile: Custom .grf has invalid format");
		return;
	}

	_cur.ClearDataForNextFile();

	ReusableBuffer<uint8_t> buf;

	while ((num = (grf_container_version >= 2 ? file.ReadDword() : file.ReadWord())) != 0) {
		uint8_t type = file.ReadByte();
		_cur.nfo_line++;

		if (type == 0xFF) {
			if (_cur.skip_sprites == 0) {
				DecodeSpecialSprite(buf.Allocate(num), num, stage);

				/* Stop all processing if we are to skip the remaining sprites */
				if (_cur.skip_sprites == -1) break;

				continue;
			} else {
				file.SkipBytes(num);
			}
		} else {
			if (_cur.skip_sprites == 0) {
				GrfMsg(0, "LoadNewGRFFile: Unexpected sprite, disabling");
				DisableGrf(STR_NEWGRF_ERROR_UNEXPECTED_SPRITE);
				break;
			}

			if (grf_container_version >= 2 && type == 0xFD) {
				/* Reference to data section. Container version >= 2 only. */
				file.SkipBytes(num);
			} else {
				file.SkipBytes(7);
				SkipSpriteData(file, type, num - 8);
			}
		}

		if (_cur.skip_sprites > 0) _cur.skip_sprites--;
	}
}

/**
 * Load a particular NewGRF.
 * @param config     The configuration of the to be loaded NewGRF.
 * @param stage      The loading stage of the NewGRF.
 * @param subdir     The sub directory to find the NewGRF in.
 * @param temporary  The NewGRF/sprite file is to be loaded temporarily and should be closed immediately,
 *                   contrary to loading the SpriteFile and having it cached by the SpriteCache.
 */
void LoadNewGRFFile(GRFConfig &config, GrfLoadingStage stage, Subdirectory subdir, bool temporary)
{
	const std::string &filename = config.filename;

	/* A .grf file is activated only if it was active when the game was
	 * started.  If a game is loaded, only its active .grfs will be
	 * reactivated, unless "loadallgraphics on" is used.  A .grf file is
	 * considered active if its action 8 has been processed, i.e. its
	 * action 8 hasn't been skipped using an action 7.
	 *
	 * During activation, only actions 0, 1, 2, 3, 4, 5, 7, 8, 9, 0A and 0B are
	 * carried out.  All others are ignored, because they only need to be
	 * processed once at initialization.  */
	if (stage != GLS_FILESCAN && stage != GLS_SAFETYSCAN && stage != GLS_LABELSCAN) {
		_cur.grffile = GetFileByFilename(filename);
		if (_cur.grffile == nullptr) UserError("File '{}' lost in cache.\n", filename);
		if (stage == GLS_RESERVE && config.status != GCS_INITIALISED) return;
		if (stage == GLS_ACTIVATION && !config.flags.Test(GRFConfigFlag::Reserved)) return;
	}

	bool needs_palette_remap = config.palette & GRFP_USE_MASK;
	if (temporary) {
		SpriteFile temporarySpriteFile(filename, subdir, needs_palette_remap);
		LoadNewGRFFileFromFile(config, stage, temporarySpriteFile);
	} else {
		SpriteFile &file = OpenCachedSpriteFile(filename, subdir, needs_palette_remap);
		LoadNewGRFFileFromFile(config, stage, file);
		if (!config.flags.Test(GRFConfigFlag::System)) file.flags |= SFF_USERGRF;
		if (config.ident.grfid == std::byteswap<uint32_t>(0xFFFFFFFE)) file.flags |= SFF_OPENTTDGRF;
	}
}

/**
 * Relocates the old shore sprites at new positions.
 *
 * 1. If shore sprites are neither loaded by Action5 nor ActionA, the extra sprites from openttd(w/d).grf are used. (SHORE_REPLACE_ONLY_NEW)
 * 2. If a newgrf replaces some shore sprites by ActionA. The (maybe also replaced) grass tiles are used for corner shores. (SHORE_REPLACE_ACTION_A)
 * 3. If a newgrf replaces shore sprites by Action5 any shore replacement by ActionA has no effect. (SHORE_REPLACE_ACTION_5)
 */
static void ActivateOldShore()
{
	/* Use default graphics, if no shore sprites were loaded.
	 * Should not happen, as the base set's extra grf should include some. */
	if (_loaded_newgrf_features.shore == SHORE_REPLACE_NONE) _loaded_newgrf_features.shore = SHORE_REPLACE_ACTION_A;

	if (_loaded_newgrf_features.shore != SHORE_REPLACE_ACTION_5) {
		DupSprite(SPR_ORIGINALSHORE_START +  1, SPR_SHORE_BASE +  1); // SLOPE_W
		DupSprite(SPR_ORIGINALSHORE_START +  2, SPR_SHORE_BASE +  2); // SLOPE_S
		DupSprite(SPR_ORIGINALSHORE_START +  6, SPR_SHORE_BASE +  3); // SLOPE_SW
		DupSprite(SPR_ORIGINALSHORE_START +  0, SPR_SHORE_BASE +  4); // SLOPE_E
		DupSprite(SPR_ORIGINALSHORE_START +  4, SPR_SHORE_BASE +  6); // SLOPE_SE
		DupSprite(SPR_ORIGINALSHORE_START +  3, SPR_SHORE_BASE +  8); // SLOPE_N
		DupSprite(SPR_ORIGINALSHORE_START +  7, SPR_SHORE_BASE +  9); // SLOPE_NW
		DupSprite(SPR_ORIGINALSHORE_START +  5, SPR_SHORE_BASE + 12); // SLOPE_NE
	}

	if (_loaded_newgrf_features.shore == SHORE_REPLACE_ACTION_A) {
		DupSprite(SPR_FLAT_GRASS_TILE + 16, SPR_SHORE_BASE +  0); // SLOPE_STEEP_S
		DupSprite(SPR_FLAT_GRASS_TILE + 17, SPR_SHORE_BASE +  5); // SLOPE_STEEP_W
		DupSprite(SPR_FLAT_GRASS_TILE +  7, SPR_SHORE_BASE +  7); // SLOPE_WSE
		DupSprite(SPR_FLAT_GRASS_TILE + 15, SPR_SHORE_BASE + 10); // SLOPE_STEEP_N
		DupSprite(SPR_FLAT_GRASS_TILE + 11, SPR_SHORE_BASE + 11); // SLOPE_NWS
		DupSprite(SPR_FLAT_GRASS_TILE + 13, SPR_SHORE_BASE + 13); // SLOPE_ENW
		DupSprite(SPR_FLAT_GRASS_TILE + 14, SPR_SHORE_BASE + 14); // SLOPE_SEN
		DupSprite(SPR_FLAT_GRASS_TILE + 18, SPR_SHORE_BASE + 15); // SLOPE_STEEP_E

		/* XXX - SLOPE_EW, SLOPE_NS are currently not used.
		 *       If they would be used somewhen, then these grass tiles will most like not look as needed */
		DupSprite(SPR_FLAT_GRASS_TILE +  5, SPR_SHORE_BASE + 16); // SLOPE_EW
		DupSprite(SPR_FLAT_GRASS_TILE + 10, SPR_SHORE_BASE + 17); // SLOPE_NS
	}
}

/**
 * Replocate the old tram depot sprites to the new position, if no new ones were loaded.
 */
static void ActivateOldTramDepot()
{
	if (_loaded_newgrf_features.tram == TRAMWAY_REPLACE_DEPOT_WITH_TRACK) {
		DupSprite(SPR_ROAD_DEPOT               + 0, SPR_TRAMWAY_DEPOT_NO_TRACK + 0); // use road depot graphics for "no tracks"
		DupSprite(SPR_TRAMWAY_DEPOT_WITH_TRACK + 1, SPR_TRAMWAY_DEPOT_NO_TRACK + 1);
		DupSprite(SPR_ROAD_DEPOT               + 2, SPR_TRAMWAY_DEPOT_NO_TRACK + 2); // use road depot graphics for "no tracks"
		DupSprite(SPR_TRAMWAY_DEPOT_WITH_TRACK + 3, SPR_TRAMWAY_DEPOT_NO_TRACK + 3);
		DupSprite(SPR_TRAMWAY_DEPOT_WITH_TRACK + 4, SPR_TRAMWAY_DEPOT_NO_TRACK + 4);
		DupSprite(SPR_TRAMWAY_DEPOT_WITH_TRACK + 5, SPR_TRAMWAY_DEPOT_NO_TRACK + 5);
	}
}

/**
 * Decide whether price base multipliers of grfs shall apply globally or only to the grf specifying them
 */
static void FinalisePriceBaseMultipliers()
{
	extern const PriceBaseSpec _price_base_specs[];
	/** Features, to which '_grf_id_overrides' applies. Currently vehicle features only. */
	static const uint32_t override_features = (1 << GSF_TRAINS) | (1 << GSF_ROADVEHICLES) | (1 << GSF_SHIPS) | (1 << GSF_AIRCRAFT);

	/* Evaluate grf overrides */
	int num_grfs = (uint)_grf_files.size();
	TempBufferST<int> grf_overrides(num_grfs);
	for (int i = 0; i < num_grfs; i++) {
		grf_overrides[i] = -1;

		GRFFile *source = _grf_files[i];
		auto it = _grf_id_overrides.find(source->grfid);
		if (it == std::end(_grf_id_overrides)) continue;
		uint32_t override = it->second;

		GRFFile *dest = GetFileByGRFID(override);
		if (dest == nullptr) continue;

		grf_overrides[i] = find_index(_grf_files, dest);
		assert(grf_overrides[i] >= 0);
	}

	/* Override features and price base multipliers of earlier loaded grfs */
	for (int i = 0; i < num_grfs; i++) {
		if (grf_overrides[i] < 0 || grf_overrides[i] >= i) continue;
		GRFFile *source = _grf_files[i];
		GRFFile *dest = _grf_files[grf_overrides[i]];

		uint32_t features = (source->grf_features | dest->grf_features) & override_features;
		source->grf_features |= features;
		dest->grf_features |= features;

		for (Price p = PR_BEGIN; p < PR_END; p++) {
			/* No price defined -> nothing to do */
			if (!HasBit(features, _price_base_specs[p].grf_feature) || source->price_base_multipliers[p] == INVALID_PRICE_MODIFIER) continue;
			Debug(grf, 3, "'{}' overrides price base multiplier {} of '{}'", source->filename, p, dest->filename);
			dest->price_base_multipliers[p] = source->price_base_multipliers[p];
		}
	}

	/* Propagate features and price base multipliers of afterwards loaded grfs, if none is present yet */
	for (int i = num_grfs - 1; i >= 0; i--) {
		if (grf_overrides[i] < 0 || grf_overrides[i] <= i) continue;
		GRFFile *source = _grf_files[i];
		GRFFile *dest = _grf_files[grf_overrides[i]];

		uint32_t features = (source->grf_features | dest->grf_features) & override_features;
		source->grf_features |= features;
		dest->grf_features |= features;

		for (Price p = PR_BEGIN; p < PR_END; p++) {
			/* Already a price defined -> nothing to do */
			if (!HasBit(features, _price_base_specs[p].grf_feature) || dest->price_base_multipliers[p] != INVALID_PRICE_MODIFIER) continue;
			Debug(grf, 3, "Price base multiplier {} from '{}' propagated to '{}'", p, source->filename, dest->filename);
			dest->price_base_multipliers[p] = source->price_base_multipliers[p];
		}
	}

	/* The 'master grf' now have the correct multipliers. Assign them to the 'addon grfs' to make everything consistent. */
	for (int i = 0; i < num_grfs; i++) {
		if (grf_overrides[i] < 0) continue;
		GRFFile *source = _grf_files[i];
		GRFFile *dest = _grf_files[grf_overrides[i]];

		uint32_t features = (source->grf_features | dest->grf_features) & override_features;
		source->grf_features |= features;
		dest->grf_features |= features;

		for (Price p = PR_BEGIN; p < PR_END; p++) {
			if (!HasBit(features, _price_base_specs[p].grf_feature)) continue;
			if (source->price_base_multipliers[p] != dest->price_base_multipliers[p]) {
				Debug(grf, 3, "Price base multiplier {} from '{}' propagated to '{}'", p, dest->filename, source->filename);
			}
			source->price_base_multipliers[p] = dest->price_base_multipliers[p];
		}
	}

	/* Apply fallback prices for grf version < 8 */
	for (GRFFile * const file : _grf_files) {
		if (file->grf_version >= 8) continue;
		PriceMultipliers &price_base_multipliers = file->price_base_multipliers;
		for (Price p = PR_BEGIN; p < PR_END; p++) {
			Price fallback_price = _price_base_specs[p].fallback_price;
			if (fallback_price != INVALID_PRICE && price_base_multipliers[p] == INVALID_PRICE_MODIFIER) {
				/* No price multiplier has been set.
				 * So copy the multiplier from the fallback price, maybe a multiplier was set there. */
				price_base_multipliers[p] = price_base_multipliers[fallback_price];
			}
		}
	}

	/* Decide local/global scope of price base multipliers */
	for (GRFFile * const file : _grf_files) {
		PriceMultipliers &price_base_multipliers = file->price_base_multipliers;
		for (Price p = PR_BEGIN; p < PR_END; p++) {
			if (price_base_multipliers[p] == INVALID_PRICE_MODIFIER) {
				/* No multiplier was set; set it to a neutral value */
				price_base_multipliers[p] = 0;
			} else {
				if (!HasBit(file->grf_features, _price_base_specs[p].grf_feature)) {
					/* The grf does not define any objects of the feature,
					 * so it must be a difficulty setting. Apply it globally */
					Debug(grf, 3, "'{}' sets global price base multiplier {} to {}", file->filename, p, price_base_multipliers[p]);
					SetPriceBaseMultiplier(p, price_base_multipliers[p]);
					price_base_multipliers[p] = 0;
				} else {
					Debug(grf, 3, "'{}' sets local price base multiplier {} to {}", file->filename, p, price_base_multipliers[p]);
				}
			}
		}
	}
}

template <typename T>
void AddBadgeToSpecs(T &specs, GrfSpecFeature feature, Badge &badge)
{
	for (auto &spec : specs) {
		if (spec == nullptr) continue;
		spec->badges.push_back(badge.index);
		badge.features.Set(feature);
	}
}

/** Finish up applying badges to things */
static void FinaliseBadges()
{
	for (GRFFile * const file : _grf_files) {
		Badge *badge = GetBadgeByLabel(fmt::format("newgrf/{:08x}", std::byteswap(file->grfid)));
		if (badge == nullptr) continue;

		for (Engine *e : Engine::Iterate()) {
			if (e->grf_prop.grffile != file) continue;
			e->badges.push_back(badge->index);
			badge->features.Set(static_cast<GrfSpecFeature>(GSF_TRAINS + e->type));
		}

		AddBadgeToSpecs(file->stations, GSF_STATIONS, *badge);
		AddBadgeToSpecs(file->housespec, GSF_HOUSES, *badge);
		AddBadgeToSpecs(file->industryspec, GSF_INDUSTRIES, *badge);
		AddBadgeToSpecs(file->indtspec, GSF_INDUSTRYTILES, *badge);
		AddBadgeToSpecs(file->objectspec, GSF_OBJECTS, *badge);
		AddBadgeToSpecs(file->airportspec, GSF_AIRPORTS, *badge);
		AddBadgeToSpecs(file->airtspec, GSF_AIRPORTTILES, *badge);
		AddBadgeToSpecs(file->roadstops, GSF_ROADSTOPS, *badge);
	}

	ApplyBadgeFeaturesToClassBadges();
}

extern void InitGRFTownGeneratorNames();

/** Finish loading NewGRFs and execute needed post-processing */
static void AfterLoadGRFs()
{
	ReleaseVarAction2OptimisationCaches();

	for (StringIDMapping &it : _string_to_grf_mapping) {
		StringID str = MapGRFStringID(it.grf, it.source);
		if (it.func == nullptr) {
			*reinterpret_cast<StringID *>(it.func_data) = str;
		} else {
			it.func(str, it.func_data);
		}
	}
	_string_to_grf_mapping.clear();

	/* Clear the action 6 override sprites. */
	_grf_line_to_action6_sprite_override.clear();

	FinaliseBadges();

	/* Polish cargoes */
	FinaliseCargoArray();

	/* Pre-calculate all refit masks after loading GRF files. */
	CalculateRefitMasks();

	/* Polish engines */
	FinaliseEngineArray();

	/* Set the actually used Canal properties */
	FinaliseCanals();

	/* Add all new houses to the house array. */
	FinaliseHouseArray();

	/* Add all new industries to the industry array. */
	FinaliseIndustriesArray();

	/* Add all new objects to the object array. */
	FinaliseObjectsArray();

	InitializeSortedCargoSpecs();

	/* Sort the list of industry types. */
	SortIndustryTypes();

	/* Create dynamic list of industry legends for smallmap_gui.cpp */
	BuildIndustriesLegend();

	/* Build the routemap legend, based on the available cargos */
	BuildLinkStatsLegend();

	/* Add all new airports to the airports array. */
	FinaliseAirportsArray();
	BindAirportSpecs();

	/* Update the townname generators list */
	InitGRFTownGeneratorNames();

	/* Run all queued vehicle list order changes */
	CommitVehicleListOrderChanges();

	/* Load old shore sprites in new position, if they were replaced by ActionA */
	ActivateOldShore();

	/* Load old tram depot sprites in new position, if no new ones are present */
	ActivateOldTramDepot();

	/* Set up custom rail types */
	InitRailTypes();
	InitRoadTypes();
	InitRoadTypesCaches();

	for (Engine *e : Engine::IterateType(VEH_ROAD)) {
		if (_gted[e->index].rv_max_speed != 0) {
			/* Set RV maximum speed from the mph/0.8 unit value */
			e->u.road.max_speed = _gted[e->index].rv_max_speed * 4;
		}

		RoadTramType rtt = e->info.misc_flags.Test(EngineMiscFlag::RoadIsTram) ? RTT_TRAM : RTT_ROAD;

		const GRFFile *file = e->GetGRF();
		if (file == nullptr || _gted[e->index].roadtramtype == 0) {
			e->u.road.roadtype = (rtt == RTT_TRAM) ? ROADTYPE_TRAM : ROADTYPE_ROAD;
			continue;
		}

		/* Remove +1 offset. */
		_gted[e->index].roadtramtype--;

		const std::vector<RoadTypeLabel> *list = (rtt == RTT_TRAM) ? &file->tramtype_list : &file->roadtype_list;
		if (_gted[e->index].roadtramtype < list->size())
		{
			RoadTypeLabel rtl = (*list)[_gted[e->index].roadtramtype];
			RoadType rt = GetRoadTypeByLabel(rtl);
			if (rt != INVALID_ROADTYPE && GetRoadTramType(rt) == rtt) {
				e->u.road.roadtype = rt;
				continue;
			}
		}

		/* Road type is not available, so disable this engine */
		e->info.climates = {};
	}

	for (Engine *e : Engine::IterateType(VEH_TRAIN)) {
		RailType railtype = GetRailTypeByLabel(_gted[e->index].railtypelabel);
		if (railtype == INVALID_RAILTYPE) {
			/* Rail type is not available, so disable this engine */
			e->info.climates = {};
		} else {
			e->u.rail.railtype = railtype;
			e->u.rail.intended_railtype = railtype;
		}
	}

	SetYearEngineAgingStops();

	FinalisePriceBaseMultipliers();

	/* Deallocate temporary loading data */
	_gted.clear();
	_grm_sprites.clear();

	ObjectClass::PrepareIndices();
	StationClass::PrepareIndices();
	AirportClass::PrepareIndices();
	RoadStopClass::PrepareIndices();
}

/**
 * Load all the NewGRFs.
 * @param load_index The offset for the first sprite to add.
 * @param num_baseset Number of NewGRFs at the front of the list to look up in the baseset dir instead of the newgrf dir.
 */
void LoadNewGRF(SpriteID load_index, uint num_baseset)
{
	/* In case of networking we need to "sync" the start values
	 * so all NewGRFs are loaded equally. For this we use the
	 * start date of the game and we set the counters, etc. to
	 * 0 so they're the same too. */
	CalTime::State cal_state = CalTime::Detail::now;
	EconTime::State econ_state = EconTime::Detail::now;
	uint8_t tick_skip_counter = DateDetail::_tick_skip_counter;
	uint64_t tick_counter = _tick_counter;
	uint64_t scaled_tick_counter = _scaled_tick_counter;
	StateTicks state_ticks = _state_ticks;
	StateTicksDelta state_ticks_offset = DateDetail::_state_ticks_offset;
	uint8_t display_opt = _display_opt;

	if (_networking) {
		CalTime::Detail::now = CalTime::Detail::NewState(_settings_game.game_creation.starting_year);
		EconTime::Detail::now = EconTime::Detail::NewState(ToEconTimeCast(_settings_game.game_creation.starting_year));
		_tick_counter = 0;
		_scaled_tick_counter = 0;
		_state_ticks = StateTicks{0};
		_display_opt  = 0;
		UpdateCachedSnowLine();
		RecalculateStateTicksOffset();
	}

	InitializeGRFSpecial();

	ResetNewGRFData();

	/*
	 * Reset the status of all files, so we can 'retry' to load them.
	 * This is needed when one for example rearranges the NewGRFs in-game
	 * and a previously disabled NewGRF becomes usable. If it would not
	 * be reset, the NewGRF would remain disabled even though it should
	 * have been enabled.
	 */
	for (const auto &c : _grfconfig) {
		if (c->status != GCS_NOT_FOUND) c->status = GCS_UNKNOWN;
		if (_settings_client.gui.newgrf_disable_big_gui && (c->ident.grfid == std::byteswap<uint32_t>(0x52577801) || c->ident.grfid == std::byteswap<uint32_t>(0x55464970))) {
			c->status = GCS_DISABLED;
		}
	}

	_cur.spriteid = load_index;

	/* Load newgrf sprites
	 * in each loading stage, (try to) open each file specified in the config
	 * and load information from it. */
	for (GrfLoadingStage stage = GLS_LABELSCAN; stage <= GLS_ACTIVATION; stage++) {
		/* Set activated grfs back to will-be-activated between reservation- and activation-stage.
		 * This ensures that action7/9 conditions 0x06 - 0x0A work correctly. */
		for (const auto &c : _grfconfig) {
			if (c->status == GCS_ACTIVATED) c->status = GCS_INITIALISED;
		}

		if (stage == GLS_RESERVE) {
			static const std::pair<uint32_t, uint32_t> default_grf_overrides[] = {
				{ std::byteswap<uint32_t>(0x44442202), std::byteswap<uint32_t>(0x44440111) }, // UKRS addons modifies UKRS
				{ std::byteswap<uint32_t>(0x6D620402), std::byteswap<uint32_t>(0x6D620401) }, // DBSetXL ECS extension modifies DBSetXL
				{ std::byteswap<uint32_t>(0x4D656f20), std::byteswap<uint32_t>(0x4D656F17) }, // LV4cut modifies LV4
			};
			for (const auto &grf_override : default_grf_overrides) {
				SetNewGRFOverride(grf_override.first, grf_override.second);
			}
		}

		uint num_grfs = 0;
		uint num_non_static = 0;

		_cur.stage = stage;
		for (const auto &c : _grfconfig) {
			if (c->status == GCS_DISABLED || c->status == GCS_NOT_FOUND) continue;
			if (stage > GLS_INIT && c->flags.Test(GRFConfigFlag::InitOnly)) continue;

			Subdirectory subdir = num_grfs < num_baseset ? BASESET_DIR : NEWGRF_DIR;
			if (!FioCheckFileExists(c->filename, subdir)) {
				Debug(grf, 0, "NewGRF file is missing '{}'; disabling", c->filename);
				c->status = GCS_NOT_FOUND;
				continue;
			}

			if (stage == GLS_LABELSCAN) InitNewGRFFile(*c);

			if (!c->flags.Test(GRFConfigFlag::Static) && !c->flags.Test(GRFConfigFlag::System)) {
				if (num_non_static == MAX_NON_STATIC_GRF_COUNT) {
					Debug(grf, 0, "'{}' is not loaded as the maximum number of non-static GRFs has been reached", c->filename);
					c->status = GCS_DISABLED;
					c->error  = {STR_NEWGRF_ERROR_MSG_FATAL, STR_NEWGRF_ERROR_TOO_MANY_NEWGRFS_LOADED};
					continue;
				}
				num_non_static++;
			}

			num_grfs++;

			LoadNewGRFFile(*c, stage, subdir, false);
			if (stage == GLS_RESERVE) {
				c->flags.Set(GRFConfigFlag::Reserved);
			} else if (stage == GLS_ACTIVATION) {
				c->flags.Reset(GRFConfigFlag::Reserved);
				assert_msg(GetFileByGRFID(c->ident.grfid) == _cur.grffile, "{:08X}", std::byteswap(c->ident.grfid));
				ClearTemporaryNewGRFData(_cur.grffile);
				BuildCargoTranslationMap();
				HandleVarAction2OptimisationPasses();
				Debug(sprite, 2, "LoadNewGRF: Currently {} sprites are loaded", _cur.spriteid);
			} else if (stage == GLS_INIT && c->flags.Test(GRFConfigFlag::InitOnly)) {
				/* We're not going to activate this, so free whatever data we allocated */
				ClearTemporaryNewGRFData(_cur.grffile);
			}
		}
	}

	/* Pseudo sprite processing is finished; free temporary stuff */
	_cur.ClearDataForNextFile();
	_callback_result_cache.clear();

	/* Call any functions that should be run after GRFs have been loaded. */
	AfterLoadGRFs();

	/* Now revert back to the original situation */
	CalTime::Detail::now = cal_state;
	EconTime::Detail::now = econ_state;
	DateDetail::_tick_skip_counter = tick_skip_counter;
	_tick_counter = tick_counter;
	_scaled_tick_counter = scaled_tick_counter;
	_state_ticks = state_ticks;
	DateDetail::_state_ticks_offset = state_ticks_offset;
	_display_opt  = display_opt;
	UpdateCachedSnowLine();
}

const char *GetExtendedVariableNameById(int id)
{
	extern const GRFVariableMapDefinition _grf_action2_remappable_variables[];
	for (const GRFVariableMapDefinition *info = _grf_action2_remappable_variables; info->name != nullptr; info++) {
		if (id == info->id) {
			return info->name;
		}
	}

	extern const GRFNameOnlyVariableMapDefinition _grf_action2_internal_variable_names[];
	for (const GRFNameOnlyVariableMapDefinition *info = _grf_action2_internal_variable_names; info->name != nullptr; info++) {
		if (id == info->id) {
			return info->name;
		}
	}

	return nullptr;
}

static bool IsLabelPrintable(uint32_t l)
{
	for (uint i = 0; i < 4; i++) {
		if ((l & 0xFF) < 0x20 || (l & 0xFF) > 0x7F) return false;
		l >>= 8;
	}
	return true;
}

const char *NewGRFLabelDumper::Label(uint32_t label)
{
	if (IsLabelPrintable(label)) {
		format_to_fixed_z::format_to(this->buffer, lastof(this->buffer), "{:c}{:c}{:c}{:c}", label >> 24, label >> 16, label >> 8, label);
	} else {
		format_to_fixed_z::format_to(this->buffer, lastof(this->buffer), "0x{:08X}", std::byteswap(label));
	}
	return this->buffer;
}
