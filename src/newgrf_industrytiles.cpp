/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file newgrf_industrytiles.cpp NewGRF handling of industry tiles. */

#include "stdafx.h"
#include "debug.h"
#include "landscape.h"
#include "newgrf_badge.h"
#include "newgrf_industrytiles.h"
#include "newgrf_sound.h"
#include "industry.h"
#include "town.h"
#include "command_func.h"
#include "water.h"
#include "bitmap_type.h"
#include "newgrf_animation_base.h"
#include "newgrf_analysis.h"
#include "newgrf_industrytiles_analysis.h"

#include "table/strings.h"

#include "safeguards.h"

/**
 * Based on newhouses equivalent, but adapted for newindustries
 * @param parameter from callback.  It's in fact a pair of coordinates
 * @param tile TileIndex from which the callback was initiated
 * @param index of the industry been queried for
 * @param signed_offsets Are the x and y offset encoded in parameter signed?
 * @param grf_version8 True, if we are dealing with a new NewGRF which uses GRF version >= 8.
 * @return a construction of bits obeying the newgrf format
 */
uint32_t GetNearbyIndustryTileInformation(uint8_t parameter, TileIndex tile, IndustryID index, bool signed_offsets, bool grf_version8, uint32_t mask)
{
	if (parameter != 0) tile = GetNearbyTile(parameter, tile, signed_offsets); // only perform if it is required
	bool is_same_industry = (IsTileType(tile, MP_INDUSTRY) && GetIndustryIndex(tile) == index);

	uint32_t result = (is_same_industry ? 1 : 0) << 8;
	if (mask & ~0x100) result |= GetNearbyTileInformation(tile, grf_version8, mask);
	return result;
}

/**
 * This is the position of the tile relative to the northernmost tile of the industry.
 * Format: 00yxYYXX
 * Variable  Content
 * x         the x offset from the northernmost tile
 * XX        same, but stored in a byte instead of a nibble
 * y         the y offset from the northernmost tile
 * YY        same, but stored in a byte instead of a nibble
 * @param tile TileIndex of the tile to evaluate
 * @param ind_tile northernmost tile of the industry
 */
uint32_t GetRelativePosition(TileIndex tile, TileIndex ind_tile)
{
	uint8_t x = TileX(tile) - TileX(ind_tile);
	uint8_t y = TileY(tile) - TileY(ind_tile);

	return ((y & 0xF) << 20) | ((x & 0xF) << 16) | (y << 8) | x;
}

/* virtual */ uint32_t IndustryTileScopeResolver::GetVariable(uint16_t variable, uint32_t parameter, GetVariableExtra &extra) const
{
	switch (variable) {
		/* Construction state of the tile: a value between 0 and 3 */
		case 0x40: return (IsTileType(this->tile, MP_INDUSTRY)) ? GetIndustryConstructionStage(this->tile) : 0;

		/* Terrain type */
		case 0x41: return GetTerrainType(this->tile);

		/* Current town zone of the tile in the nearest town */
		case 0x42: return GetTownRadiusGroup(ClosestTownFromTile(this->tile, UINT_MAX), this->tile);

		/* Relative position */
		case 0x43: return GetRelativePosition(this->tile, this->industry->location.tile);

		/* Animation frame. Like house variable 46 but can contain anything 0..FF. */
		case 0x44: return IsTileType(this->tile, MP_INDUSTRY) ? GetAnimationFrame(this->tile) : 0;

		/* Land info of nearby tiles */
		case 0x60: return GetNearbyIndustryTileInformation(parameter, this->tile,
				this->industry == nullptr ? (IndustryID)INVALID_INDUSTRY : this->industry->index, true, this->ro.grffile->grf_version >= 8, extra.mask);

		/* Animation stage of nearby tiles */
		case 0x61: {
			TileIndex tile = GetNearbyTile(parameter, this->tile);
			if (IsTileType(tile, MP_INDUSTRY) && Industry::GetByTile(tile) == this->industry) {
				return GetAnimationFrame(tile);
			}
			return UINT_MAX;
		}

		/* Get industry tile ID at offset */
		case 0x62: return GetIndustryIDAtOffset(GetNearbyTile(parameter, this->tile), this->industry, this->ro.grffile->grfid);

		case 0x7A: return GetBadgeVariableResult(*this->ro.grffile, GetIndustryTileSpec(GetIndustryGfx(this->tile))->badges, parameter);
	}

	Debug(grf, 1, "Unhandled industry tile variable 0x{:X}", variable);

	extra.available = false;
	return UINT_MAX;
}

/* virtual */ uint32_t IndustryTileScopeResolver::GetRandomBits() const
{
	assert_tile(this->industry != nullptr && IsValidTile(this->tile), this->tile);
	assert_tile(this->industry->index == INVALID_INDUSTRY || IsTileType(this->tile, MP_INDUSTRY), this->tile);

	return (this->industry->index != INVALID_INDUSTRY) ? GetIndustryRandomBits(this->tile) : 0;
}

/* virtual */ uint32_t IndustryTileScopeResolver::GetTriggers() const
{
	assert_tile(this->industry != nullptr && IsValidTile(this->tile), this->tile);
	assert_tile(this->industry->index == INVALID_INDUSTRY || IsTileType(this->tile, MP_INDUSTRY), this->tile);
	if (this->industry->index == INVALID_INDUSTRY) return 0;
	return GetIndustryTriggers(this->tile);
}

/**
 * Get the associated NewGRF file from the industry graphics.
 * @param gfx Graphics to query.
 * @return Grf file associated with the graphics, if any.
 */
static const GRFFile *GetIndTileGrffile(IndustryGfx gfx)
{
	const IndustryTileSpec *its = GetIndustryTileSpec(gfx);
	return (its != nullptr) ? its->grf_prop.grffile : nullptr;
}

/**
 * Constructor of the industry tiles scope resolver.
 * @param gfx Graphics of the industry.
 * @param tile %Tile of the industry.
 * @param indus %Industry owning the tile.
 * @param callback Callback ID.
 * @param callback_param1 First parameter (var 10) of the callback.
 * @param callback_param2 Second parameter (var 18) of the callback.
 */
IndustryTileResolverObject::IndustryTileResolverObject(IndustryGfx gfx, TileIndex tile, Industry *indus,
			CallbackID callback, uint32_t callback_param1, uint32_t callback_param2)
	: ResolverObject(GetIndTileGrffile(gfx), callback, callback_param1, callback_param2),
	indtile_scope(*this, indus, tile),
	ind_scope(*this, tile, indus, indus->type),
	gfx(gfx)
{
	this->root_spritegroup = GetIndustryTileSpec(gfx)->grf_prop.GetSpriteGroup();
}

GrfSpecFeature IndustryTileResolverObject::GetFeature() const
{
	return GSF_INDUSTRYTILES;
}

uint32_t IndustryTileResolverObject::GetDebugID() const
{
	return GetIndustryTileSpec(gfx)->grf_prop.local_id;
}

static void IndustryDrawTileLayout(const TileInfo *ti, const TileLayoutSpriteGroup *group, uint8_t rnd_colour, uint8_t stage)
{
	const DrawTileSprites *dts = group->ProcessRegisters(&stage);

	SpriteID image = dts->ground.sprite;
	PaletteID pal  = dts->ground.pal;

	if (HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE)) image += stage;
	if (HasBit(pal, SPRITE_MODIFIER_CUSTOM_SPRITE)) pal += stage;

	if (GB(image, 0, SPRITE_WIDTH) != 0) {
		/* If the ground sprite is the default flat water sprite, draw also canal/river borders
		 * Do not do this if the tile's WaterClass is 'land'. */
		if (image == SPR_FLAT_WATER_TILE && IsTileOnWater(ti->tile)) {
			DrawWaterClassGround(ti);
		} else {
			DrawGroundSprite(image, GroundSpritePaletteTransform(image, pal, GENERAL_SPRITE_COLOUR(rnd_colour)));
		}
	}

	DrawNewGRFTileSeq(ti, dts, TO_INDUSTRIES, stage, GENERAL_SPRITE_COLOUR(rnd_colour));
}

uint16_t GetIndustryTileCallback(CallbackID callback, uint32_t param1, uint32_t param2, IndustryGfx gfx_id, Industry *industry, TileIndex tile)
{
	assert_tile(industry != nullptr && IsValidTile(tile), tile);
	assert_tile(industry->index == INVALID_INDUSTRY || IsTileType(tile, MP_INDUSTRY), tile);

	IndustryTileResolverObject object(gfx_id, tile, industry, callback, param1, param2);
	return object.ResolveCallback();
}

bool DrawNewIndustryTile(TileInfo *ti, Industry *i, IndustryGfx gfx, const IndustryTileSpec *inds)
{
	if (ti->tileh != SLOPE_FLAT) {
		bool draw_old_one = true;
		if (inds->callback_mask.Test(IndustryTileCallbackMask::DrawFoundations)) {
			/* Called to determine the type (if any) of foundation to draw for industry tile */
			uint32_t callback_res = GetIndustryTileCallback(CBID_INDTILE_DRAW_FOUNDATIONS, 0, 0, gfx, i, ti->tile);
			if (callback_res != CALLBACK_FAILED) draw_old_one = ConvertBooleanCallback(inds->grf_prop.grffile, CBID_INDTILE_DRAW_FOUNDATIONS, callback_res);
		}

		if (draw_old_one) DrawFoundation(ti, FOUNDATION_LEVELED);
	}

	IndustryTileResolverObject object(gfx, ti->tile, i);

	const SpriteGroup *group = object.Resolve();
	if (group == nullptr || group->type != SGT_TILELAYOUT) return false;

	/* Limit the building stage to the number of stages supplied. */
	const TileLayoutSpriteGroup *tlgroup = (const TileLayoutSpriteGroup *)group;
	uint8_t stage = GetIndustryConstructionStage(ti->tile);
	IndustryDrawTileLayout(ti, tlgroup, i->random_colour, stage);
	return true;
}

extern bool IsSlopeRefused(Slope current, Slope refused);

/**
 * Check the slope of a tile of a new industry.
 * @param ind_base_tile Base tile of the industry.
 * @param ind_tile      Tile to check.
 * @param its           Tile specification.
 * @param type          Industry type.
 * @param gfx           Gfx of the tile.
 * @param layout_index  Layout.
 * @param initial_random_bits Random bits of industry after construction
 * @param founder       Industry founder
 * @param creation_type The circumstances the industry is created under.
 * @return Succeeded or failed command.
 */
CommandCost PerformIndustryTileSlopeCheck(TileIndex ind_base_tile, TileIndex ind_tile, const IndustryTileSpec *its, IndustryType type, IndustryGfx gfx, size_t layout_index, uint16_t initial_random_bits, Owner founder, IndustryAvailabilityCallType creation_type)
{
	Industry ind;
	ind.index = INVALID_INDUSTRY;
	ind.location.tile = ind_base_tile;
	ind.location.w = 0;
	ind.type = type;
	ind.random = initial_random_bits;
	ind.founder = founder;

	uint16_t callback_res = GetIndustryTileCallback(CBID_INDTILE_SHAPE_CHECK, 0, creation_type << 8 | (uint32_t)layout_index, gfx, &ind, ind_tile);
	if (callback_res == CALLBACK_FAILED) {
		if (!IsSlopeRefused(GetTileSlope(ind_tile), its->slopes_refused)) return CommandCost();
		return CommandCost(STR_ERROR_SITE_UNSUITABLE);
	}
	if (its->grf_prop.grffile->grf_version < 7) {
		if (callback_res != 0) return CommandCost();
		return CommandCost(STR_ERROR_SITE_UNSUITABLE);
	}

	return GetErrorMessageFromLocationCallbackResult(callback_res, its->grf_prop.grffile, STR_ERROR_SITE_UNSUITABLE);
}

/* Simple wrapper for GetHouseCallback to keep the animation unified. */
uint16_t GetSimpleIndustryCallback(CallbackID callback, uint32_t param1, uint32_t param2, const IndustryTileSpec *spec, Industry *ind, TileIndex tile, int extra_data)
{
	return GetIndustryTileCallback(callback, param1, param2, spec - GetIndustryTileSpec(0), ind, tile);
}

/** Helper class for animation control. */
struct IndustryAnimationBase : public AnimationBase<IndustryAnimationBase, IndustryTileSpec, Industry, int, GetSimpleIndustryCallback, TileAnimationFrameAnimationHelper<Industry> > {
	static constexpr CallbackID cb_animation_speed      = CBID_INDTILE_ANIMATION_SPEED;
	static constexpr CallbackID cb_animation_next_frame = CBID_INDTILE_ANIM_NEXT_FRAME;

	static constexpr IndustryTileCallbackMask cbm_animation_speed      = IndustryTileCallbackMask::AnimationSpeed;
	static constexpr IndustryTileCallbackMask cbm_animation_next_frame = IndustryTileCallbackMask::AnimationNextFrame;
};

void AnimateNewIndustryTile(TileIndex tile)
{
	const IndustryTileSpec *itspec = GetIndustryTileSpec(GetIndustryGfx(tile));
	if (itspec == nullptr) return;

	IndustryAnimationBase::AnimateTile(itspec, Industry::GetByTile(tile), tile, itspec->special_flags.Test(IndustryTileSpecialFlag::NextFrameRandomBits));
}

bool StartStopIndustryTileAnimation(TileIndex tile, IndustryAnimationTrigger iat, uint32_t random)
{
	const IndustryTileSpec *itspec = GetIndustryTileSpec(GetIndustryGfx(tile));

	if (!HasBit(itspec->animation.triggers, iat)) return false;

	bool inhibit_animation = false;
	if (iat == IAT_CONSTRUCTION_STATE_CHANGE) {
		/* Suppress animation changes according to layout anim inhibit mask */
		const Industry *ind = Industry::GetByTile(tile);
		const IndustrySpec *spec = GetIndustrySpec(ind->type);
		if (ind->selected_layout != 0 && ind->selected_layout <= spec->layouts.size()) {
			const TileIndexDiffC tile_delta = TileIndexToTileIndexDiffC(tile, ind->location.tile);
			const uint64_t mask = spec->layout_anim_masks[ind->selected_layout - 1];
			uint idx = 0;
			for (IndustryTileLayoutTile it : spec->layouts[ind->selected_layout - 1]) {
				if (it.gfx == 0xFF) continue;

				if (it.ti == tile_delta) {
					IndustryGfx gfx = GetTranslatedIndustryTileID(it.gfx);
					if (gfx != GetIndustryGfx(tile)) break;

					if (HasBit(mask, idx)) inhibit_animation = true;
					break;
				}

				idx++;
				if (idx == 64) break;
			}
		}
	}

	if (inhibit_animation) {
		IndustryAnimationBase::ChangeAnimationFrameSoundOnly(CBID_INDTILE_ANIM_START_STOP, itspec, Industry::GetByTile(tile), tile, random, iat);
	} else {
		IndustryAnimationBase::ChangeAnimationFrame(CBID_INDTILE_ANIM_START_STOP, itspec, Industry::GetByTile(tile), tile, random, iat);
	}
	return true;
}

bool StartStopIndustryTileAnimation(const Industry *ind, IndustryAnimationTrigger iat)
{
	bool ret = true;
	uint32_t random = Random();
	for (TileIndex tile : ind->location) {
		if (ind->TileBelongsToIndustry(tile)) {
			if (StartStopIndustryTileAnimation(tile, iat, random)) {
				SB(random, 0, 16, Random());
			} else {
				ret = false;
			}
		}
	}

	return ret;
}

uint8_t GetNewIndustryTileAnimationSpeed(TileIndex tile)
{
	const IndustryTileSpec *itspec = GetIndustryTileSpec(GetIndustryGfx(tile));
	if (itspec == nullptr) return 0;

	return IndustryAnimationBase::GetAnimationSpeed(itspec);
}

/**
 * Trigger random triggers for an industry tile and reseed its random bits.
 * @param tile Industry tile to trigger.
 * @param trigger Trigger to trigger.
 * @param ind Industry of the tile.
 * @param[in,out] reseed_industry Collects bits to reseed for the industry.
 */
static void DoTriggerIndustryTile(TileIndex tile, IndustryTileTrigger trigger, Industry *ind, uint32_t &reseed_industry)
{
	assert_tile(IsValidTile(tile) && IsTileType(tile, MP_INDUSTRY), tile);

	IndustryGfx gfx = GetIndustryGfx(tile);
	const IndustryTileSpec *itspec = GetIndustryTileSpec(gfx);

	if (itspec->grf_prop.GetSpriteGroup() == nullptr) return;

	IndustryTileResolverObject object(gfx, tile, ind, CBID_RANDOM_TRIGGER);
	object.waiting_triggers = GetIndustryTriggers(tile) | trigger;
	SetIndustryTriggers(tile, object.waiting_triggers); // store now for var 5F

	const SpriteGroup *group = object.Resolve();
	if (group == nullptr) return;

	/* Store remaining triggers. */
	SetIndustryTriggers(tile, object.GetRemainingTriggers());

	/* Rerandomise tile bits */
	uint8_t new_random_bits = Random();
	uint8_t random_bits = GetIndustryRandomBits(tile);
	random_bits &= ~object.reseed[VSG_SCOPE_SELF];
	random_bits |= new_random_bits & object.reseed[VSG_SCOPE_SELF];
	SetIndustryRandomBits(tile, random_bits);
	MarkTileDirtyByTile(tile, VMDF_NOT_MAP_MODE);

	reseed_industry |= object.reseed[VSG_SCOPE_PARENT];
}

/**
 * Reseeds the random bits of an industry.
 * @param ind Industry.
 * @param reseed Bits to reseed.
 */
static void DoReseedIndustry(Industry *ind, uint32_t reseed)
{
	if (reseed == 0 || ind == nullptr) return;

	uint16_t random_bits = Random();
	ind->random &= reseed;
	ind->random |= random_bits & reseed;
}

/**
 * Trigger a random trigger for a single industry tile.
 * @param tile Industry tile to trigger.
 * @param trigger Trigger to trigger.
 */
void TriggerIndustryTile(TileIndex tile, IndustryTileTrigger trigger)
{
	uint32_t reseed_industry = 0;
	Industry *ind = Industry::GetByTile(tile);
	DoTriggerIndustryTile(tile, trigger, ind, reseed_industry);
	DoReseedIndustry(ind, reseed_industry);
}

/**
 * Trigger a random trigger for all industry tiles.
 * @param ind Industry to trigger.
 * @param trigger Trigger to trigger.
 */
void TriggerIndustry(Industry *ind, IndustryTileTrigger trigger)
{
	uint32_t reseed_industry = 0;
	for (TileIndex tile : ind->location) {
		if (ind->TileBelongsToIndustry(tile)) {
			DoTriggerIndustryTile(tile, trigger, ind, reseed_industry);
		}
	}
	DoReseedIndustry(ind, reseed_industry);
}

void AnalyseIndustryTileSpriteGroups()
{
	for (IndustrySpec &spec : _industry_specs) {
		const uint layout_count = (uint)spec.layouts.size();
		spec.layout_anim_masks.clear();
		spec.layout_anim_masks.resize(layout_count);

		IndustryTileLayout layout;
		for (uint idx = 0; idx < layout_count; idx++) {
			btree::btree_set<IndustryGfx> seen_gfx;
			layout.clear();
			for (IndustryTileLayoutTile it : spec.layouts[idx]) {
				if (it.gfx == 0xFF) continue;

				IndustryGfx gfx = GetTranslatedIndustryTileID(it.gfx);
				layout.push_back({ it.ti, gfx });
				seen_gfx.insert(gfx);
				if (layout.size() == 64) break;
			}

			/* Layout now contains the translated tile layout with gaps removed, up to a maximum of 64 tiles */

			uint64_t anim_mask = 0;

			uint64_t to_check = UINT64_MAX >> (64 - layout.size());

			while (to_check != 0) {
				uint64_t current = 0;
				uint i = FindFirstBit(to_check);
				IndustryGfx gfx = layout[i].gfx;
				for (; i < layout.size(); i++) {
					if (gfx == layout[i].gfx) SetBit(current, i);
				}
				to_check &= ~current;

				const IndustryTileSpec &tilespec = _industry_tile_specs[gfx];
				if (tilespec.grf_prop.GetSpriteGroup() == nullptr) continue;

				anim_mask |= current;

				IndustryTileDataAnalyserConfig cfg;
				cfg.layout = &layout;
				cfg.result_mask = &anim_mask;
				cfg.layout_index = idx + 1;
				cfg.check_anim_next_frame_cb = tilespec.callback_mask.Test(IndustryTileCallbackMask::AnimationNextFrame);

				IndustryTileDataAnalyser analyser(cfg, current);
				analyser.AnalyseGroup(tilespec.grf_prop.GetSpriteGroup());

				if (analyser.anim_state_at_offset) {
					/* Give up: use of get anim state of offset tiles */
					anim_mask = 0;
					break;
				}
			}

			spec.layout_anim_masks[idx] = anim_mask;
		}
	}
}

void ApplyIndustryTileAnimMasking()
{
	for (Industry *ind : Industry::Iterate()) {
		const IndustrySpec *spec = GetIndustrySpec(ind->type);

		if (ind->selected_layout == 0 || ind->selected_layout > spec->layouts.size()) continue;

		uint64_t mask = spec->layout_anim_masks[ind->selected_layout - 1];

		uint idx = 0;
		for (IndustryTileLayoutTile it : spec->layouts[ind->selected_layout - 1]) {
			if (it.gfx == 0xFF) continue;

			TileIndex tile = AddTileIndexDiffCWrap(ind->location.tile, it.ti);
			if (!IsValidTile(tile) || !ind->TileBelongsToIndustry(tile)) break;

			IndustryGfx gfx = GetTranslatedIndustryTileID(it.gfx);
			if (gfx != GetIndustryGfx(tile)) break;

			if (HasBit(mask, idx)) DeleteAnimatedTile(tile);

			idx++;
			if (idx == 64) break;
		}
	}
}
