/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file industry_gui.cpp GUIs related to industries. */

#include "stdafx.h"
#include "error.h"
#include "gui.h"
#include "settings_gui.h"
#include "sound_func.h"
#include "window_func.h"
#include "textbuf_gui.h"
#include "command_func.h"
#include "viewport_func.h"
#include "industry.h"
#include "industry_cmd.h"
#include "town.h"
#include "cheat_type.h"
#include "newgrf_badge.h"
#include "newgrf_industries.h"
#include "newgrf_text.h"
#include "newgrf_debug.h"
#include "network/network.h"
#include "strings_func.h"
#include "company_func.h"
#include "tilehighlight_func.h"
#include "string_func.h"
#include "sortlist_type.h"
#include "dropdown_func.h"
#include "dropdown_common_type.h"
#include "company_base.h"
#include "core/geometry_func.hpp"
#include "core/random_func.hpp"
#include "core/backup_type.hpp"
#include "genworld.h"
#include "smallmap_gui.h"
#include "dropdown_type.h"
#include "clear_map.h"
#include "zoom_func.h"
#include "graph_gui.h"
#include "querystring_gui.h"
#include "stringfilter_type.h"
#include "hotkeys.h"

#include "widgets/industry_widget.h"

#include "table/strings.h"

#include <bitset>

#include "safeguards.h"

bool _ignore_restrictions;
static std::bitset<NUM_INDUSTRYTYPES> _displayed_industries; ///< Communication from the industry chain window to the smallmap window about what industries to display.
static std::bitset<NUM_INDUSTRYTYPES> _displayed_industries_in;
static std::bitset<NUM_INDUSTRYTYPES> _displayed_industries_out;

enum IndustryLinkMode {
	ILM_ALL,
	ILM_IN,
	ILM_OUT,
};
static IndustryLinkMode _link_mode = ILM_ALL;

const std::bitset<NUM_INDUSTRYTYPES> &GetIndustryLinkDisplayIndustries()
{
	switch (_link_mode) {
		case ILM_IN:
			return _displayed_industries_in;
		case ILM_OUT:
			return _displayed_industries_out;
		default:
			return _displayed_industries;
	}
}

/** Cargo suffix type (for which window is it requested) */
enum CargoSuffixType : uint8_t {
	CST_FUND,  ///< Fund-industry window
	CST_VIEW,  ///< View-industry window
	CST_DIR,   ///< Industry-directory window
};

/** Ways of displaying the cargo. */
enum CargoSuffixDisplay : uint8_t {
	CSD_CARGO,             ///< Display the cargo without sub-type (cb37 result 401).
	CSD_CARGO_AMOUNT,      ///< Display the cargo and amount (if useful), but no sub-type (cb37 result 400 or fail).
	CSD_CARGO_TEXT,        ///< Display then cargo and supplied string (cb37 result 800-BFF).
	CSD_CARGO_AMOUNT_TEXT, ///< Display then cargo, amount, and string (cb37 result 000-3FF).
};

/** Transfer storage of cargo suffix information. */
struct CargoSuffix {
	CargoSuffixDisplay display; ///< How to display the cargo and text.
	std::string text;           ///< Cargo suffix text.
};

extern void GenerateIndustries();
static void ShowIndustryCargoesWindow(IndustryType id);

/**
 * Gets the string to display after the cargo name (using callback 37)
 * @param cargo the cargo for which the suffix is requested, meaning depends on presence of flag 18 in prop 1A
 * @param cst the cargo suffix type (for which window is it requested). @see CargoSuffixType
 * @param ind the industry (nullptr if in fund window)
 * @param ind_type the industry type
 * @param indspec the industry spec
 * @param suffix is filled with the string to display
 */
static void GetCargoSuffix(uint cargo, CargoSuffixType cst, const Industry *ind, IndustryType ind_type, const IndustrySpec *indspec, CargoSuffix &suffix)
{
	suffix.text.clear();
	suffix.display = CSD_CARGO_AMOUNT;

	if (indspec->callback_mask.Test(IndustryCallbackMask::CargoSuffix)) {
		TileIndex t = (cst != CST_FUND) ? ind->location.tile : INVALID_TILE;
		uint16_t callback = GetIndustryCallback(CBID_INDUSTRY_CARGO_SUFFIX, 0, (cst << 8) | cargo, const_cast<Industry *>(ind), ind_type, t);
		if (callback == CALLBACK_FAILED) return;

		if (indspec->grf_prop.grffile->grf_version < 8) {
			if (GB(callback, 0, 8) == 0xFF) return;
			if (callback < 0x400) {
				StartTextRefStackUsage(indspec->grf_prop.grffile, 6);
				suffix.text = GetString(GetGRFStringID(indspec->grf_prop.grffile, GRFSTR_MISC_GRF_TEXT + callback));
				StopTextRefStackUsage();
				suffix.display = CSD_CARGO_AMOUNT_TEXT;
				return;
			}
			ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_CARGO_SUFFIX, callback);
			return;

		} else { // GRF version 8 or higher.
			if (callback == 0x400) return;
			if (callback == 0x401) {
				suffix.display = CSD_CARGO;
				return;
			}
			if (callback < 0x400) {
				StartTextRefStackUsage(indspec->grf_prop.grffile, 6);
				suffix.text = GetString(GetGRFStringID(indspec->grf_prop.grffile, GRFSTR_MISC_GRF_TEXT + callback));
				StopTextRefStackUsage();
				suffix.display = CSD_CARGO_AMOUNT_TEXT;
				return;
			}
			if (callback >= 0x800 && callback < 0xC00) {
				StartTextRefStackUsage(indspec->grf_prop.grffile, 6);
				suffix.text = GetString(GetGRFStringID(indspec->grf_prop.grffile, GRFSTR_MISC_GRF_TEXT - 0x800 + callback));
				StopTextRefStackUsage();
				suffix.display = CSD_CARGO_TEXT;
				return;
			}
			ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_CARGO_SUFFIX, callback);
			return;
		}
	}
}

enum CargoSuffixInOut : uint8_t {
	CARGOSUFFIX_OUT = 0,
	CARGOSUFFIX_IN  = 1,
};

/**
 * Gets all strings to display after the cargoes of industries (using callback 37)
 * @param use_input get suffixes for output cargoes or input cargoes?
 * @param cst the cargo suffix type (for which window is it requested). @see CargoSuffixType
 * @param ind the industry (nullptr if in fund window)
 * @param ind_type the industry type
 * @param indspec the industry spec
 * @param cargoes array with cargotypes. for INVALID_CARGO no suffix will be determined
 * @param suffixes is filled with the suffixes
 */
template <typename TC, typename TS>
static inline void GetAllCargoSuffixes(CargoSuffixInOut use_input, CargoSuffixType cst, const Industry *ind, IndustryType ind_type, const IndustrySpec *indspec, const TC &cargoes, TS &suffixes)
{
	static_assert(std::tuple_size_v<std::remove_reference_t<decltype(cargoes)>> <= std::tuple_size_v<std::remove_reference_t<decltype(suffixes)>>);

	if (indspec->behaviour.Test(IndustryBehaviour::CargoTypesUnlimited)) {
		/* Reworked behaviour with new many-in-many-out scheme */
		for (size_t j = 0; j < std::size(suffixes); j++) {
			if (cargoes[j] != INVALID_CARGO) {
				uint8_t local_id = indspec->grf_prop.grffile->cargo_map[cargoes[j]]; // should we check the value for valid?
				uint cargotype = local_id << 16 | use_input;
				GetCargoSuffix(cargotype, cst, ind, ind_type, indspec, suffixes[j]);
			} else {
				suffixes[j].text.clear();
				suffixes[j].display = CSD_CARGO;
			}
		}
	} else {
		/* Compatible behaviour with old 3-in-2-out scheme */
		for (size_t j = 0; j < std::size(suffixes); j++) {
			suffixes[j].text.clear();
			suffixes[j].display = CSD_CARGO;
		}
		switch (use_input) {
			case CARGOSUFFIX_OUT:
				// Handle INDUSTRY_ORIGINAL_NUM_OUTPUTS cargoes
				if (cargoes[0] != INVALID_CARGO) GetCargoSuffix(3, cst, ind, ind_type, indspec, suffixes[0]);
				if (cargoes[1] != INVALID_CARGO) GetCargoSuffix(4, cst, ind, ind_type, indspec, suffixes[1]);
				break;
			case CARGOSUFFIX_IN:
				// Handle INDUSTRY_ORIGINAL_NUM_INPUTS cargoes
				if (cargoes[0] != INVALID_CARGO) GetCargoSuffix(0, cst, ind, ind_type, indspec, suffixes[0]);
				if (cargoes[1] != INVALID_CARGO) GetCargoSuffix(1, cst, ind, ind_type, indspec, suffixes[1]);
				if (cargoes[2] != INVALID_CARGO) GetCargoSuffix(2, cst, ind, ind_type, indspec, suffixes[2]);
				break;
			default:
				NOT_REACHED();
		}
	}
}

/**
 * Gets the strings to display after the cargo of industries (using callback 37)
 * @param use_input get suffixes for output cargo or input cargo?
 * @param cst the cargo suffix type (for which window is it requested). @see CargoSuffixType
 * @param ind the industry (nullptr if in fund window)
 * @param ind_type the industry type
 * @param indspec the industry spec
 * @param cargo cargotype. for INVALID_CARGO no suffix will be determined
 * @param slot accepts/produced slot number, used for old-style 3-in/2-out industries.
 * @param suffix is filled with the suffix
 */
void GetCargoSuffix(CargoSuffixInOut use_input, CargoSuffixType cst, const Industry *ind, IndustryType ind_type, const IndustrySpec *indspec, CargoType cargo, uint8_t slot, CargoSuffix &suffix)
{
	suffix.text.clear();
	suffix.display = CSD_CARGO;
	if (!IsValidCargoType(cargo)) return;
	if (indspec->behaviour.Test(IndustryBehaviour::CargoTypesUnlimited)) {
		uint8_t local_id = indspec->grf_prop.grffile->cargo_map[cargo]; // should we check the value for valid?
		uint cargotype = local_id << 16 | use_input;
		GetCargoSuffix(cargotype, cst, ind, ind_type, indspec, suffix);
	} else if (use_input == CARGOSUFFIX_IN) {
		if (slot < INDUSTRY_ORIGINAL_NUM_INPUTS) GetCargoSuffix(slot, cst, ind, ind_type, indspec, suffix);
	} else if (use_input == CARGOSUFFIX_OUT) {
		if (slot < INDUSTRY_ORIGINAL_NUM_OUTPUTS) GetCargoSuffix(slot + INDUSTRY_ORIGINAL_NUM_INPUTS, cst, ind, ind_type, indspec, suffix);
	}
}

std::array<IndustryType, NUM_INDUSTRYTYPES> _sorted_industry_types; ///< Industry types sorted by name.

/**
 * Initialize the list of sorted industry types.
 */
void SortIndustryTypes()
{
	std::string industry_spec_names[NUM_INDUSTRYTYPES]{};

	/* Add each industry type to the list. */
	for (IndustryType i = 0; i < NUM_INDUSTRYTYPES; i++) {
		_sorted_industry_types[i] = i;
		industry_spec_names[i] = GetString(GetIndustrySpec(i)->name);
	}

	/* Sort industry types by name. */
	std::sort(_sorted_industry_types.begin(), _sorted_industry_types.end(), [&](const IndustryType &a, const IndustryType &b) {
		int r = StrNaturalCompare(industry_spec_names[a], industry_spec_names[b]); // Sort by name (natural sorting).

		/* If the names are equal, sort by industry type. */
		return (r != 0) ? r < 0 : (a < b);
	});
}

/**
 * Command callback. In case of failure to build an industry, show an error message.
 * @param result Result of the command.
 * @param tile   Tile where the industry is placed.
 * @param indtype Industry type.
 */
void CcBuildIndustry(const CommandCost &result, TileIndex tile, IndustryType indtype, uint32_t first_layout, bool fund, uint32_t seed)
{
	if (result.Succeeded()) return;

	if (indtype < NUM_INDUSTRYTYPES) {
		const IndustrySpec *indsp = GetIndustrySpec(indtype);
		if (indsp->enabled) {
			SetDParam(0, indsp->name);
			ShowErrorMessage(STR_ERROR_CAN_T_BUILD_HERE, result.GetErrorMessage(), WL_INFO, TileX(tile) * TILE_SIZE, TileY(tile) * TILE_SIZE);
		}
	}
}

static constexpr NWidgetPart _nested_build_industry_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN), SetStringTip(STR_FUND_INDUSTRY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_DEFSIZEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_STICKYBOX, COLOUR_DARK_GREEN),
	EndContainer(),
	NWidget(NWID_SELECTION, COLOUR_DARK_GREEN, WID_DPI_SCENARIO_EDITOR_PANE),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_DPI_CREATE_RANDOM_INDUSTRIES_WIDGET), SetMinimalSize(0, 12), SetFill(1, 0), SetResize(1, 0),
					SetStringTip(STR_FUND_INDUSTRY_MANY_RANDOM_INDUSTRIES, STR_FUND_INDUSTRY_MANY_RANDOM_INDUSTRIES_TOOLTIP),
			NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_DPI_REMOVE_ALL_INDUSTRIES_WIDGET), SetMinimalSize(0, 12), SetFill(1, 0), SetResize(1, 0),
					SetStringTip(STR_FUND_INDUSTRY_REMOVE_ALL_INDUSTRIES, STR_FUND_INDUSTRY_REMOVE_ALL_INDUSTRIES_TOOLTIP),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_MATRIX, COLOUR_DARK_GREEN, WID_DPI_MATRIX_WIDGET), SetMatrixDataTip(1, 0, STR_FUND_INDUSTRY_SELECTION_TOOLTIP), SetFill(1, 0), SetResize(1, 1), SetScrollbar(WID_DPI_SCROLLBAR),
		NWidget(NWID_VSCROLLBAR, COLOUR_DARK_GREEN, WID_DPI_SCROLLBAR),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_DARK_GREEN, WID_DPI_INFOPANEL), SetResize(1, 0),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_DPI_DISPLAY_WIDGET), SetFill(1, 0), SetResize(1, 0),
				SetStringTip(STR_INDUSTRY_DISPLAY_CHAIN, STR_INDUSTRY_DISPLAY_CHAIN_TOOLTIP),
		NWidget(WWT_TEXTBTN, COLOUR_DARK_GREEN, WID_DPI_FUND_WIDGET), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_JUST_STRING),
		NWidget(WWT_RESIZEBOX, COLOUR_DARK_GREEN),
	EndContainer(),
};

/** Window definition of the dynamic place industries gui */
static WindowDesc _build_industry_desc(__FILE__, __LINE__,
	WDP_AUTO, "build_industry", 170, 212,
	WC_BUILD_INDUSTRY, WC_NONE,
	WindowDefaultFlag::Construction,
	_nested_build_industry_widgets
);

/** Build (fund or prospect) a new industry, */
class BuildIndustryWindow : public Window {
	IndustryType selected_type;                 ///< industry corresponding to the above index
	std::vector<IndustryType> list;             ///< List of industries.
	bool enabled;                               ///< Availability state of the selected industry.
	Scrollbar *vscroll;
	Dimension legend;                           ///< Dimension of the legend 'blob'.
	GUIBadgeClasses badge_classes;

	/** The largest allowed minimum-width of the window, given in line heights */
	static const int MAX_MINWIDTH_LINEHEIGHTS = 20;

	void UpdateAvailability()
	{
		this->enabled = this->selected_type != IT_INVALID && (_game_mode == GM_EDITOR || GetIndustryProbabilityCallback(this->selected_type, IACT_USERCREATION, 1) > 0);
	}

	void SetupArrays()
	{
		this->list.clear();

		/* Fill the arrays with industries.
		 * The tests performed after the enabled allow to load the industries
		 * In the same way they are inserted by grf (if any)
		 */
		for (IndustryType ind : _sorted_industry_types) {
			const IndustrySpec *indsp = GetIndustrySpec(ind);
			if (indsp->enabled) {
				/* Rule is that editor mode loads all industries.
				 * In game mode, all non raw industries are loaded too
				 * and raw ones are loaded only when setting allows it */
				if (_game_mode != GM_EDITOR && indsp->IsRawIndustry() && _settings_game.construction.raw_industry_construction == 0) {
					/* Unselect if the industry is no longer in the list */
					if (this->selected_type == ind) this->selected_type = IT_INVALID;
					continue;
				}

				this->list.push_back(ind);
			}
		}

		/* First industry type is selected if the current selection is invalid. */
		if (this->selected_type == IT_INVALID && !this->list.empty()) this->selected_type = this->list[0];

		this->UpdateAvailability();

		this->vscroll->SetCount(this->list.size());
	}

	/** Update status of the fund and display-chain widgets. */
	void SetButtons()
	{
		this->SetWidgetDisabledState(WID_DPI_FUND_WIDGET, this->selected_type != IT_INVALID && !this->enabled);
		this->SetWidgetDisabledState(WID_DPI_DISPLAY_WIDGET, this->selected_type == IT_INVALID && this->enabled);
	}

	/**
	 * Build a string of cargo names with suffixes attached.
	 * This is distinct from the CARGO_LIST string formatting code in two ways:
	 *  - This cargo list uses the order defined by the industry, rather than alphabetic.
	 *  - NewGRF-supplied suffix strings can be attached to each cargo.
	 *
	 * @param cargolist    Array of CargoType to display
	 * @param cargo_suffix Array of suffixes to attach to each cargo
	 * @param cargolistlen Length of arrays
	 * @param prefixstr    String to use for the first item
	 * @return A formatted raw string
	 */
	std::string MakeCargoListString(const std::span<const CargoType> cargolist, const std::span<const CargoSuffix> cargo_suffix, StringID prefixstr) const
	{
		assert(cargolist.size() == cargo_suffix.size());

		std::string cargostring;
		size_t numcargo = 0;
		size_t firstcargo = 0;

		for (size_t j = 0; j < cargolist.size(); j++) {
			if (!IsValidCargoType(cargolist[j])) continue;
			numcargo++;
			if (numcargo == 1) {
				firstcargo = j;
				continue;
			}
			SetDParam(0, CargoSpec::Get(cargolist[j])->name);
			SetDParamStr(1, cargo_suffix[j].text);
			AppendStringInPlace(cargostring, STR_INDUSTRY_VIEW_CARGO_LIST_EXTENSION);
		}

		if (numcargo > 0) {
			SetDParam(0, CargoSpec::Get(cargolist[firstcargo])->name);
			SetDParamStr(1, cargo_suffix[firstcargo].text);
			cargostring = GetString(prefixstr) + cargostring;
		} else {
			SetDParam(0, STR_JUST_NOTHING);
			SetDParamStr(1, "");
			cargostring = GetString(prefixstr);
		}

		return cargostring;
	}

public:
	BuildIndustryWindow() : Window(_build_industry_desc)
	{
		this->selected_type = IT_INVALID;

		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_DPI_SCROLLBAR);
		/* Show scenario editor tools in editor. */
		if (_game_mode != GM_EDITOR) {
			this->GetWidget<NWidgetStacked>(WID_DPI_SCENARIO_EDITOR_PANE)->SetDisplayedPlane(SZSP_HORIZONTAL);
		}
		this->FinishInitNested(0);

		this->SetButtons();
	}

	void OnInit() override
	{
		this->badge_classes = GUIBadgeClasses{GSF_INDUSTRIES};

		/* Width of the legend blob -- slightly larger than the smallmap legend blob. */
		this->legend.height = GetCharacterHeight(FS_SMALL);
		this->legend.width = this->legend.height * 9 / 6;

		this->SetupArrays();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_DPI_MATRIX_WIDGET: {
				SetDParamMaxDigits(0, 4);
				Dimension count = GetStringBoundingBox(STR_JUST_COMMA, FS_SMALL);
				Dimension d{};
				for (const auto &indtype : this->list) {
					d = maxdim(d, GetStringBoundingBox(GetIndustrySpec(indtype)->name));
				}
				resize.height = std::max<uint>({this->legend.height, d.height, count.height}) + padding.height;
				d.width += this->badge_classes.GetTotalColumnsWidth() + this->legend.width + WidgetDimensions::scaled.hsep_wide + WidgetDimensions::scaled.hsep_normal + count.width + padding.width;
				d.height = 5 * resize.height;
				size = maxdim(size, d);
				break;
			}

			case WID_DPI_INFOPANEL: {
				/* Extra line for cost outside of editor. */
				int height = 2 + (_game_mode == GM_EDITOR ? 0 : 1);
				uint extra_lines_req = 0;
				uint extra_lines_prd = 0;
				uint extra_lines_newgrf = 0;
				uint max_minwidth = GetCharacterHeight(FS_NORMAL) * MAX_MINWIDTH_LINEHEIGHTS;
				Dimension d = {0, 0};
				for (const auto &indtype : this->list) {
					const IndustrySpec *indsp = GetIndustrySpec(indtype);
					std::array<CargoSuffix, std::tuple_size_v<decltype(indsp->accepts_cargo)>> cargo_suffix{};

					/* Measure the accepted cargoes, if any. */
					GetAllCargoSuffixes(CARGOSUFFIX_IN, CST_FUND, nullptr, indtype, indsp, indsp->accepts_cargo, cargo_suffix);
					std::string cargostring = this->MakeCargoListString(indsp->accepts_cargo, cargo_suffix, STR_INDUSTRY_VIEW_REQUIRES_N_CARGO);
					Dimension strdim = GetStringBoundingBox(cargostring);
					if (strdim.width > max_minwidth) {
						extra_lines_req = std::max(extra_lines_req, strdim.width / max_minwidth + 1);
						strdim.width = max_minwidth;
					}
					d = maxdim(d, strdim);

					/* Measure the produced cargoes, if any. */
					GetAllCargoSuffixes(CARGOSUFFIX_OUT, CST_FUND, nullptr, indtype, indsp, indsp->produced_cargo, cargo_suffix);
					cargostring = this->MakeCargoListString(indsp->produced_cargo, cargo_suffix, STR_INDUSTRY_VIEW_PRODUCES_N_CARGO);
					strdim = GetStringBoundingBox(cargostring);
					if (strdim.width > max_minwidth) {
						extra_lines_prd = std::max(extra_lines_prd, strdim.width / max_minwidth + 1);
						strdim.width = max_minwidth;
					}
					d = maxdim(d, strdim);

					if (indsp->grf_prop.HasGrfFile()) {
						/* Reserve a few extra lines for text from an industry NewGRF. */
						extra_lines_newgrf = 4;
					}
				}

				/* Set it to something more sane :) */
				height += extra_lines_prd + extra_lines_req + extra_lines_newgrf;
				size.height = height * GetCharacterHeight(FS_NORMAL) + padding.height;
				size.width = d.width + padding.width;
				break;
			}

			case WID_DPI_FUND_WIDGET: {
				Dimension d = GetStringBoundingBox(STR_FUND_INDUSTRY_BUILD_NEW_INDUSTRY);
				d = maxdim(d, GetStringBoundingBox(STR_FUND_INDUSTRY_PROSPECT_NEW_INDUSTRY));
				d = maxdim(d, GetStringBoundingBox(STR_FUND_INDUSTRY_FUND_NEW_INDUSTRY));
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}
		}
	}

	void SetStringParameters(WidgetID widget) const override
	{
		switch (widget) {
			case WID_DPI_FUND_WIDGET:
				/* Raw industries might be prospected. Show this fact by changing the string
				 * In Editor, you just build, while ingame, or you fund or you prospect */
				if (_game_mode == GM_EDITOR) {
					/* We've chosen many random industries but no industries have been specified */
					SetDParam(0, STR_FUND_INDUSTRY_BUILD_NEW_INDUSTRY);
				} else {
					if (this->selected_type != IT_INVALID) {
						const IndustrySpec *indsp = GetIndustrySpec(this->selected_type);
						SetDParam(0, (_settings_game.construction.raw_industry_construction == 2 && indsp->IsRawIndustry()) ? STR_FUND_INDUSTRY_PROSPECT_NEW_INDUSTRY : STR_FUND_INDUSTRY_FUND_NEW_INDUSTRY);
					} else {
						SetDParam(0, STR_FUND_INDUSTRY_FUND_NEW_INDUSTRY);
					}
				}
				break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_DPI_MATRIX_WIDGET: {
				bool rtl = _current_text_dir == TD_RTL;
				Rect text = r.WithHeight(this->resize.step_height).Shrink(WidgetDimensions::scaled.matrix);
				Rect icon = text.WithWidth(this->legend.width, rtl);
				text = text.Indent(this->legend.width + WidgetDimensions::scaled.hsep_wide, rtl);

				/* Vertical offset for legend icon. */
				icon.top    = r.top + (this->resize.step_height - this->legend.height + 1) / 2;
				icon.bottom = icon.top + this->legend.height - 1;

				auto badge_column_widths = badge_classes.GetColumnWidths();

				auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->list);
				for (auto it = first; it != last; ++it) {
					IndustryType type = *it;
					bool selected = this->selected_type == type;
					const IndustrySpec *indsp = GetIndustrySpec(type);

					Rect tr = text;
					if (badge_column_widths.size() >= 1 && badge_column_widths[0] > 0) {
						DrawBadgeColumn(tr.WithWidth(badge_column_widths[0], rtl), 0, this->badge_classes, indsp->badges, GSF_INDUSTRIES, std::nullopt, PAL_NONE);
						tr = tr.Indent(badge_column_widths[0], rtl);
					}
					if (badge_column_widths.size() >= 2 && badge_column_widths[1] > 0) {
						DrawBadgeColumn(tr.WithWidth(badge_column_widths[1], !rtl), 0, this->badge_classes, indsp->badges, GSF_INDUSTRIES, std::nullopt, PAL_NONE);
						tr = tr.Indent(badge_column_widths[1], !rtl);
					}

					/* Draw the name of the industry in white is selected, otherwise, in orange */
					DrawString(tr, indsp->name, selected ? TC_WHITE : TC_ORANGE);
					GfxFillRect(icon, selected ? PC_WHITE : PC_BLACK);
					GfxFillRect(icon.Shrink(WidgetDimensions::scaled.bevel), indsp->map_colour);
					SetDParam(0, Industry::GetIndustryTypeCount(type));
					DrawString(tr, STR_JUST_COMMA, TC_BLACK, SA_RIGHT, false, FS_SMALL);

					text = text.Translate(0, this->resize.step_height);
					icon = icon.Translate(0, this->resize.step_height);
				}
				break;
			}

			case WID_DPI_INFOPANEL: {
				Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);

				if (this->selected_type == IT_INVALID) {
					DrawStringMultiLine(ir, STR_FUND_INDUSTRY_MANY_RANDOM_INDUSTRIES_TOOLTIP);
					break;
				}

				const IndustrySpec *indsp = GetIndustrySpec(this->selected_type);

				if (_game_mode != GM_EDITOR) {
					SetDParam(0, indsp->GetConstructionCost());
					DrawString(ir, STR_FUND_INDUSTRY_INDUSTRY_BUILD_COST);
					ir.top += GetCharacterHeight(FS_NORMAL);
				}

				std::array<CargoSuffix, std::tuple_size_v<decltype(indsp->accepts_cargo)>> cargo_suffix{};

				/* Draw the accepted cargoes, if any. Otherwise, will print "Nothing". */
				GetAllCargoSuffixes(CARGOSUFFIX_IN, CST_FUND, nullptr, this->selected_type, indsp, indsp->accepts_cargo, cargo_suffix);
				std::string cargostring = this->MakeCargoListString(indsp->accepts_cargo, cargo_suffix, STR_INDUSTRY_VIEW_REQUIRES_N_CARGO);
				ir.top = DrawStringMultiLine(ir, cargostring);

				/* Draw the produced cargoes, if any. Otherwise, will print "Nothing". */
				GetAllCargoSuffixes(CARGOSUFFIX_OUT, CST_FUND, nullptr, this->selected_type, indsp, indsp->produced_cargo, cargo_suffix);
				cargostring = this->MakeCargoListString(indsp->produced_cargo, cargo_suffix, STR_INDUSTRY_VIEW_PRODUCES_N_CARGO);
				ir.top = DrawStringMultiLine(ir, cargostring);

				ir.top = DrawBadgeNameList(ir, indsp->badges, GSF_INDUSTRIES);

				/* Get the additional purchase info text, if it has not already been queried. */
				if (indsp->callback_mask.Test(IndustryCallbackMask::FundMoreText)) {
					uint16_t callback_res = GetIndustryCallback(CBID_INDUSTRY_FUND_MORE_TEXT, 0, 0, nullptr, this->selected_type, INVALID_TILE);
					if (callback_res != CALLBACK_FAILED && callback_res != 0x400) {
						if (callback_res > 0x400) {
							ErrorUnknownCallbackResult(indsp->grf_prop.grfid, CBID_INDUSTRY_FUND_MORE_TEXT, callback_res);
						} else {
							StringID str = GetGRFStringID(indsp->grf_prop.grfid, GRFSTR_MISC_GRF_TEXT + callback_res);  // No. here's the new string
							if (str != STR_UNDEFINED) {
								StartTextRefStackUsage(indsp->grf_prop.grffile, 6);
								DrawStringMultiLine(ir, str, TC_YELLOW);
								StopTextRefStackUsage();
							}
						}
					}
				}
				break;
			}
		}
	}

	static void AskManyRandomIndustriesCallback(Window *, bool confirmed)
	{
		if (!confirmed) return;

		if (Town::GetNumItems() == 0) {
			ShowErrorMessage(STR_ERROR_CAN_T_GENERATE_INDUSTRIES, STR_ERROR_MUST_FOUND_TOWN_FIRST, WL_INFO);
		} else {
			Backup<bool> old_generating_world(_generating_world, true, FILE_LINE);
			BasePersistentStorageArray::SwitchMode(PSM_ENTER_GAMELOOP);
			GenerateIndustries();
			BasePersistentStorageArray::SwitchMode(PSM_LEAVE_GAMELOOP);
			old_generating_world.Restore();
		}
	}

	static void AskRemoveAllIndustriesCallback(Window *, bool confirmed)
	{
		if (!confirmed) return;

		for (Industry *industry : Industry::Iterate()) delete industry;

		/* Clear farmland. */
		for (TileIndex tile(0); tile < Map::Size(); ++tile) {
			if (IsTileType(tile, MP_CLEAR) && GetClearGround(tile) == CLEAR_FIELDS) {
				MakeClear(tile, CLEAR_GRASS, 3);
			}
		}

		MarkWholeScreenDirty();
	}

	void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		switch (widget) {
			case WID_DPI_CREATE_RANDOM_INDUSTRIES_WIDGET: {
				assert(_game_mode == GM_EDITOR);
				this->HandleButtonClick(WID_DPI_CREATE_RANDOM_INDUSTRIES_WIDGET);
				ShowQuery(STR_FUND_INDUSTRY_MANY_RANDOM_INDUSTRIES_CAPTION, STR_FUND_INDUSTRY_MANY_RANDOM_INDUSTRIES_QUERY, nullptr, AskManyRandomIndustriesCallback);
				break;
			}

			case WID_DPI_REMOVE_ALL_INDUSTRIES_WIDGET: {
				assert(_game_mode == GM_EDITOR);
				this->HandleButtonClick(WID_DPI_REMOVE_ALL_INDUSTRIES_WIDGET);
				ShowQuery(STR_FUND_INDUSTRY_REMOVE_ALL_INDUSTRIES_CAPTION, STR_FUND_INDUSTRY_REMOVE_ALL_INDUSTRIES_QUERY, nullptr, AskRemoveAllIndustriesCallback);
				break;
			}

			case WID_DPI_MATRIX_WIDGET: {
				auto it = this->vscroll->GetScrolledItemFromWidget(this->list, pt.y, this, WID_DPI_MATRIX_WIDGET);
				if (it != this->list.end()) { // Is it within the boundaries of available data?
					this->SelectIndustryType(*it);
					if (this->enabled && click_count > 1) this->OnClick(pt, WID_DPI_FUND_WIDGET, 1);
				}
				break;
			}

			case WID_DPI_DISPLAY_WIDGET:
				if (this->selected_type != IT_INVALID) ShowIndustryCargoesWindow(this->selected_type);
				break;

			case WID_DPI_FUND_WIDGET: {
				if (this->selected_type != IT_INVALID) {
					if (_game_mode != GM_EDITOR && _settings_game.construction.raw_industry_construction == 2 && GetIndustrySpec(this->selected_type)->IsRawIndustry()) {
						Command<CMD_BUILD_INDUSTRY>::Post(STR_ERROR_CAN_T_CONSTRUCT_THIS_INDUSTRY, TileIndex{}, this->selected_type, 0, false, InteractiveRandom());
						this->HandleButtonClick(WID_DPI_FUND_WIDGET);
					} else {
						HandlePlacePushButton(this, WID_DPI_FUND_WIDGET, SPR_CURSOR_INDUSTRY, HT_RECT);
					}
				}
				break;
			}
		}
	}

	void ScrollToSelected()
	{
		auto iter = std::find(this->list.begin(), this->list.end(), this->selected_type);
		if (iter != this->list.end()) {
			this->vscroll->ScrollTowards(iter - this->list.begin());
		}
	}

	void SelectIndustryType(IndustryType type)
	{
		this->selected_type = type;
		this->UpdateAvailability();

		const IndustrySpec *indsp = GetIndustrySpec(this->selected_type);

		this->SetDirty();

		if (_thd.GetCallbackWnd() == this &&
				((_game_mode != GM_EDITOR && _settings_game.construction.raw_industry_construction == 2 && indsp != nullptr && indsp->IsRawIndustry()) || !this->enabled)) {
			/* Reset the button state if going to prospecting or "build many industries" */
			this->RaiseButtons();
			ResetObjectToPlace();
		}

		this->SetButtons();
	}

	void OnResize() override
	{
		/* Adjust the number of items in the matrix depending of the resize */
		this->vscroll->SetCapacityFromWidget(this, WID_DPI_MATRIX_WIDGET);
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		bool success = true;
		/* We do not need to protect ourselves against "Random Many Industries" in this mode */
		const IndustrySpec *indsp = GetIndustrySpec(this->selected_type);
		uint32_t seed = InteractiveRandom();
		uint32_t layout_index = InteractiveRandomRange((uint32_t)indsp->layouts.size());

		if (_game_mode == GM_EDITOR) {
			/* Show error if no town exists at all */
			if (Town::GetNumItems() == 0) {
				SetDParam(0, indsp->name);
				ShowErrorMessage(STR_ERROR_CAN_T_BUILD_HERE, STR_ERROR_MUST_FOUND_TOWN_FIRST, WL_INFO, pt.x, pt.y);
				return;
			}

			Backup<CompanyID> cur_company(_current_company, OWNER_NONE, FILE_LINE);
			Backup<bool> old_generating_world(_generating_world, true, FILE_LINE);
			_ignore_restrictions = true;

			Command<CMD_BUILD_INDUSTRY>::Post(STR_ERROR_CAN_T_CONSTRUCT_THIS_INDUSTRY, CommandCallback::BuildIndustry, tile, this->selected_type, layout_index, false, seed);

			cur_company.Restore();
			old_generating_world.Restore();
			_ignore_restrictions = false;
		} else {
			success = Command<CMD_BUILD_INDUSTRY>::Post(STR_ERROR_CAN_T_CONSTRUCT_THIS_INDUSTRY, tile, this->selected_type, layout_index, false, seed);
		}

		/* If an industry has been built, just reset the cursor and the system */
		if (success && !_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();
	}

	void OnHundredthTick() override
	{
		if (_game_mode == GM_EDITOR) return;
		if (this->selected_type == IT_INVALID) return;

		bool enabled = this->enabled;
		this->UpdateAvailability();
		if (enabled != this->enabled) {
			this->SetButtons();
			this->SetDirty();
		}
	}

	void OnTimeout() override
	{
		this->RaiseButtons();
	}

	void OnPlaceObjectAbort() override
	{
		this->RaiseButtons();
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		this->SetupArrays();
		this->SetButtons();
		this->SetDirty();
	}
};

static BuildIndustryWindow *CreateBuildIndustryWindow()
{
	if (_game_mode != GM_EDITOR && !Company::IsValidID(_local_company)) return nullptr;
	Window *existing = BringWindowToFrontById(WC_BUILD_INDUSTRY, 0);
	if (existing != nullptr) return static_cast<BuildIndustryWindow *>(existing);
	return new BuildIndustryWindow();
}

void ShowBuildIndustryWindow()
{
	CreateBuildIndustryWindow();
}

void ShowBuildIndustryWindowForIndustryType(IndustryType industry_type)
{
	const IndustrySpec *indsp = GetIndustrySpec(industry_type);
	if (!indsp->enabled) return;
	if (_game_mode != GM_EDITOR && indsp->IsRawIndustry() && _settings_game.construction.raw_industry_construction == 0) return;

	BuildIndustryWindow *w = CreateBuildIndustryWindow();
	if (w == nullptr) return;

	w->SelectIndustryType(industry_type);
	w->ScrollToSelected();
}

static void UpdateIndustryProduction(Industry *i);

static inline bool IsProductionAlterable(const Industry *i)
{
	const IndustrySpec *is = GetIndustrySpec(i->type);
	bool has_prod = std::any_of(std::begin(is->production_rate), std::end(is->production_rate), [](auto rate) { return rate != 0; });
	return ((_game_mode == GM_EDITOR || _cheats.setup_prod.value) &&
			(has_prod || is->IsRawIndustry()) &&
			!_networking);
}

class IndustryViewWindow : public Window
{
	/** Modes for changing production */
	enum Editability : uint8_t {
		EA_NONE,              ///< Not alterable
		EA_MULTIPLIER,        ///< Allow changing the production multiplier
		EA_RATE,              ///< Allow changing the production rates
	};

	/** Specific lines in the info panel */
	enum InfoLine : uint8_t {
		IL_NONE,              ///< No line
		IL_MULTIPLIER,        ///< Production multiplier
		IL_RATE1,             ///< Production rate of cargo 1
		IL_RATE2,             ///< Production rate of cargo 2
	};

	Dimension cargo_icon_size; ///< Largest cargo icon dimension.
	Editability editable;     ///< Mode for changing production
	InfoLine editbox_line;    ///< The line clicked to open the edit box
	InfoLine clicked_line;    ///< The line of the button that has been clicked
	uint8_t clicked_button;   ///< The button that has been clicked (to raise)
	int production_offset_y;  ///< The offset of the production texts/buttons
	int info_height;          ///< Height needed for the #WID_IV_INFO panel
	int cheat_line_height;    ///< Height of each line for the #WID_IV_INFO panel

public:
	IndustryViewWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->flags.Set(WindowFlag::DisableVpScroll);
		this->editbox_line = IL_NONE;
		this->clicked_line = IL_NONE;
		this->clicked_button = 0;
		this->info_height = WidgetDimensions::scaled.framerect.Vertical() + 2 * GetCharacterHeight(FS_NORMAL); // Info panel has at least two lines text.

		const Industry *i = Industry::Get(window_number);

		this->InitNested(window_number);
		NWidgetViewport *nvp = this->GetWidget<NWidgetViewport>(WID_IV_VIEWPORT);
		nvp->InitializeViewport(this, i->location.GetCenterTile().base(), ScaleZoomGUI(ZOOM_LVL_INDUSTRY));

		if (!i->IsCargoProduced()) this->DisableWidget(WID_IV_GRAPH);

		this->InvalidateData();
	}

	void Close(int data = 0) override
	{
		CloseWindowById(WC_INDUSTRY_PRODUCTION, this->window_number, false);
		this->Window::Close(data);
	}

	void OnInit() override
	{
		/* This only used when the cheat to alter industry production is enabled */
		this->cheat_line_height = std::max(SETTING_BUTTON_HEIGHT + WidgetDimensions::scaled.vsep_normal, GetCharacterHeight(FS_NORMAL));
		this->cargo_icon_size = GetLargestCargoIconSize();
	}

	void OnPaint() override
	{
		this->DrawWidgets();

		if (this->IsShaded()) return; // Don't draw anything when the window is shaded.

		const Rect r = this->GetWidget<NWidgetBase>(WID_IV_INFO)->GetCurrentRect();
		int expected = this->DrawInfo(r);
		if (expected != r.bottom) {
			this->info_height = expected - r.top + 1;
			this->ReInit();
			return;
		}
	}

	void DrawCargoIcon(const Rect &r, CargoType cargo_type) const
	{
		bool rtl = _current_text_dir == TD_RTL;
		SpriteID icon = CargoSpec::Get(cargo_type)->GetCargoIcon();
		Dimension d = GetSpriteSize(icon);
		Rect ir = r.WithWidth(this->cargo_icon_size.width, rtl).WithHeight(GetCharacterHeight(FS_NORMAL));
		DrawSprite(icon, PAL_NONE, CenterBounds(ir.left, ir.right, d.width), CenterBounds(ir.top, ir.bottom, this->cargo_icon_size.height));
	}

	/**
	 * Draw the text in the #WID_IV_INFO panel.
	 * @param r Rectangle of the panel.
	 * @return Expected position of the bottom edge of the panel.
	 */
	int DrawInfo(const Rect &r)
	{
		bool rtl = _current_text_dir == TD_RTL;
		Industry *i = Industry::Get(this->window_number);
		const IndustrySpec *ind = GetIndustrySpec(i->type);
		Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
		bool first = true;
		bool has_accept = false;

		if (i->prod_level == PRODLEVEL_CLOSURE) {
			DrawString(ir, STR_INDUSTRY_VIEW_INDUSTRY_ANNOUNCED_CLOSURE);
			ir.top += GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_wide;
		}

		const int label_indent = WidgetDimensions::scaled.hsep_normal + this->cargo_icon_size.width;
		bool stockpiling = ind->callback_mask.Any({IndustryCallbackMask::ProductionCargoArrival, IndustryCallbackMask::Production256Ticks});

		for (const auto &a : i->Accepted()) {
			if (a.cargo == INVALID_CARGO) continue;
			has_accept = true;
			if (first) {
				DrawString(ir, STR_INDUSTRY_VIEW_REQUIRES);
				ir.top += GetCharacterHeight(FS_NORMAL);
				first = false;
			}

			DrawCargoIcon(ir, a.cargo);

			CargoSuffix suffix;
			GetCargoSuffix(CARGOSUFFIX_IN, CST_VIEW, i, i->type, ind, a.cargo, &a - i->accepted.get(), suffix);

			SetDParam(0, CargoSpec::Get(a.cargo)->name);
			SetDParam(1, a.cargo);
			SetDParam(2, a.waiting);
			SetDParamStr(3, "");
			StringID str = STR_NULL;
			switch (suffix.display) {
				case CSD_CARGO_AMOUNT_TEXT:
					SetDParamStr(3, suffix.text);
					[[fallthrough]];
				case CSD_CARGO_AMOUNT:
					str = stockpiling ? STR_INDUSTRY_VIEW_ACCEPT_CARGO_AMOUNT : STR_INDUSTRY_VIEW_ACCEPT_CARGO;
					break;

				case CSD_CARGO_TEXT:
					SetDParamStr(3, suffix.text);
					[[fallthrough]];
				case CSD_CARGO:
					str = STR_INDUSTRY_VIEW_ACCEPT_CARGO;
					break;

				default:
					NOT_REACHED();
			}
			DrawString(ir.Indent(label_indent, rtl), str);
			ir.top += GetCharacterHeight(FS_NORMAL);
		}

		int line_height = this->editable == EA_RATE ? this->cheat_line_height : GetCharacterHeight(FS_NORMAL);
		int text_y_offset = (line_height - GetCharacterHeight(FS_NORMAL)) / 2;
		int button_y_offset = (line_height - SETTING_BUTTON_HEIGHT) / 2;
		first = true;
		for (const auto &p : i->Produced()) {
			if (p.cargo == INVALID_CARGO) continue;
			if (first) {
				if (has_accept) ir.top += WidgetDimensions::scaled.vsep_wide;
				if (EconTime::UsingWallclockUnits()) {
					DrawString(ir, ReplaceWallclockMinutesUnit() ? STR_INDUSTRY_VIEW_PRODUCTION_LAST_PRODUCTION_INTERVAL_TITLE : STR_INDUSTRY_VIEW_PRODUCTION_LAST_MINUTE_TITLE);
				} else {
					DrawString(ir, STR_INDUSTRY_VIEW_PRODUCTION_LAST_MONTH_TITLE);
				}
				ir.top += GetCharacterHeight(FS_NORMAL);
				if (this->editable == EA_RATE) this->production_offset_y = ir.top;
				first = false;
			}

			DrawCargoIcon(ir, p.cargo);

			CargoSuffix suffix;
			GetCargoSuffix(CARGOSUFFIX_OUT, CST_VIEW, i, i->type, ind, p.cargo, &p - i->produced.get(), suffix);

			SetDParam(0, p.cargo);
			SetDParam(1, p.history[LAST_MONTH].production);
			SetDParamStr(2, suffix.text);
			SetDParam(3, ToPercent8(p.history[LAST_MONTH].PctTransported()));
			DrawString(ir.Indent(label_indent + (this->editable == EA_RATE ? SETTING_BUTTON_WIDTH + WidgetDimensions::scaled.hsep_normal : 0), rtl).Translate(0, text_y_offset), STR_INDUSTRY_VIEW_TRANSPORTED);
			/* Let's put out those buttons.. */
			if (this->editable == EA_RATE) {
				DrawArrowButtons(ir.Indent(label_indent, rtl).WithWidth(SETTING_BUTTON_WIDTH, rtl).left, ir.top + button_y_offset, COLOUR_YELLOW, (this->clicked_line == IL_RATE1 + (&p - i->produced.get())) ? this->clicked_button : 0,
						p.rate > 0, p.rate < 255);
			}
			ir.top += line_height;
		}

		/* Display production multiplier if editable */
		if (this->editable == EA_MULTIPLIER) {
			line_height = this->cheat_line_height;
			text_y_offset = (line_height - GetCharacterHeight(FS_NORMAL)) / 2;
			button_y_offset = (line_height - SETTING_BUTTON_HEIGHT) / 2;
			ir.top += WidgetDimensions::scaled.vsep_wide;
			this->production_offset_y = ir.top;
			SetDParam(0, RoundDivSU(i->prod_level * 100, PRODLEVEL_DEFAULT));
			DrawString(ir.Indent(label_indent + SETTING_BUTTON_WIDTH + WidgetDimensions::scaled.hsep_normal, rtl).Translate(0, text_y_offset), STR_INDUSTRY_VIEW_PRODUCTION_LEVEL);
			DrawArrowButtons(ir.Indent(label_indent, rtl).WithWidth(SETTING_BUTTON_WIDTH, rtl).left, ir.top + button_y_offset, COLOUR_YELLOW, (this->clicked_line == IL_MULTIPLIER) ? this->clicked_button : 0,
					i->prod_level > PRODLEVEL_MINIMUM, i->prod_level < PRODLEVEL_MAXIMUM);
			ir.top += line_height;
		}

		/* Get the extra message for the GUI */
		if (ind->callback_mask.Test(IndustryCallbackMask::WindowMoreText)) {
			uint16_t callback_res = GetIndustryCallback(CBID_INDUSTRY_WINDOW_MORE_TEXT, 0, 0, i, i->type, i->location.tile);
			if (callback_res != CALLBACK_FAILED && callback_res != 0x400) {
				if (callback_res > 0x400) {
					ErrorUnknownCallbackResult(ind->grf_prop.grfid, CBID_INDUSTRY_WINDOW_MORE_TEXT, callback_res);
				} else {
					StringID message = GetGRFStringID(ind->grf_prop.grffile, GRFSTR_MISC_GRF_TEXT + callback_res);
					if (message != STR_NULL && message != STR_UNDEFINED) {
						ir.top += WidgetDimensions::scaled.vsep_wide;

						StartTextRefStackUsage(ind->grf_prop.grffile, 6);
						/* Use all the available space left from where we stand up to the
						 * end of the window. We ALSO enlarge the window if needed, so we
						 * can 'go' wild with the bottom of the window. */
						ir.top = DrawStringMultiLine(ir.left, ir.right, ir.top, UINT16_MAX, message, TC_BLACK);
						StopTextRefStackUsage();
					}
				}
			}
		}

		if (!i->text.empty()) {
			SetDParamStr(0, i->text);
			ir.top += WidgetDimensions::scaled.vsep_wide;
			ir.top = DrawStringMultiLine(ir.left, ir.right, ir.top, UINT16_MAX, STR_JUST_RAW_STRING, TC_BLACK);
		}

		/* Return required bottom position, the last pixel row plus some padding. */
		return ir.top - 1 + WidgetDimensions::scaled.framerect.bottom;
	}

	void SetStringParameters(WidgetID widget) const override
	{
		if (widget == WID_IV_CAPTION) SetDParam(0, this->window_number);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget == WID_IV_INFO) size.height = this->info_height;
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_IV_INFO: {
				Industry *i = Industry::Get(this->window_number);
				InfoLine line = IL_NONE;

				switch (this->editable) {
					case EA_NONE: break;

					case EA_MULTIPLIER:
						if (IsInsideBS(pt.y, this->production_offset_y, this->cheat_line_height)) line = IL_MULTIPLIER;
						break;

					case EA_RATE:
						if (pt.y >= this->production_offset_y) {
							int row = (pt.y - this->production_offset_y) / this->cheat_line_height;
							for (const auto &p : i->Produced()) {
								if (p.cargo == INVALID_CARGO) continue;
								row--;
								if (row < 0) {
									line = (InfoLine)(IL_RATE1 + (&p - i->produced.get()));
									break;
								}
							}
						}
						break;
				}
				if (line == IL_NONE) return;

				bool rtl = _current_text_dir == TD_RTL;
				Rect r = this->GetWidget<NWidgetBase>(widget)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect).Indent(this->cargo_icon_size.width + WidgetDimensions::scaled.hsep_normal, rtl);

				if (r.WithWidth(SETTING_BUTTON_WIDTH, rtl).Contains(pt)) {
					/* Clicked buttons, decrease or increase production */
					bool decrease = r.WithWidth(SETTING_BUTTON_WIDTH / 2, rtl).Contains(pt);
					switch (this->editable) {
						case EA_MULTIPLIER:
							if (decrease) {
								if (i->prod_level <= PRODLEVEL_MINIMUM) return;
								i->prod_level = static_cast<uint8_t>(std::max<uint>(i->prod_level / 2, PRODLEVEL_MINIMUM));
							} else {
								if (i->prod_level >= PRODLEVEL_MAXIMUM) return;
								i->prod_level = static_cast<uint8_t>(std::min<uint>(i->prod_level * 2, PRODLEVEL_MAXIMUM));
							}
							break;

						case EA_RATE:
							if (decrease) {
								if (i->produced[line - IL_RATE1].rate <= 0) return;
								i->produced[line - IL_RATE1].rate = std::max(i->produced[line - IL_RATE1].rate / 2, 0);
							} else {
								if (i->produced[line - IL_RATE1].rate >= 255) return;
								/* a zero production industry is unlikely to give anything but zero, so push it a little bit */
								int new_prod = i->produced[line - IL_RATE1].rate == 0 ? 1 : i->produced[line - IL_RATE1].rate * 2;
								i->produced[line - IL_RATE1].rate = ClampTo<uint8_t>(new_prod);
							}
							break;

						default: NOT_REACHED();
					}

					UpdateIndustryProduction(i);
					this->SetDirty();
					this->SetTimeout();
					this->clicked_line = line;
					this->clicked_button = (decrease ^ rtl) ? 1 : 2;
				} else if (r.Indent(SETTING_BUTTON_WIDTH + WidgetDimensions::scaled.hsep_normal, rtl).Contains(pt)) {
					/* clicked the text */
					this->editbox_line = line;
					switch (this->editable) {
						case EA_MULTIPLIER:
							ShowQueryString(GetString(STR_JUST_INT, RoundDivSU(i->prod_level * 100, PRODLEVEL_DEFAULT)), STR_CONFIG_GAME_PRODUCTION_LEVEL, 10, this, CS_ALPHANUMERAL, QSF_NONE);
							break;

						case EA_RATE:
							ShowQueryString(GetString(STR_JUST_INT, i->produced[line - IL_RATE1].rate * 8), STR_CONFIG_GAME_PRODUCTION, 10, this, CS_ALPHANUMERAL, QSF_NONE);
							break;

						default: NOT_REACHED();
					}
				}
				break;
			}

			case WID_IV_GOTO: {
				Industry *i = Industry::Get(this->window_number);
				if (_ctrl_pressed) {
					ShowExtraViewportWindow(i->location.GetCenterTile());
				} else {
					ScrollMainWindowToTile(i->location.GetCenterTile());
				}
				break;
			}

			case WID_IV_DISPLAY: {
				Industry *i = Industry::Get(this->window_number);
				ShowIndustryCargoesWindow(i->type);
				break;
			}

			case WID_IV_GRAPH:
				ShowIndustryProductionGraph(this->window_number);
				break;
		}
	}

	void OnTimeout() override
	{
		this->clicked_line = IL_NONE;
		this->clicked_button = 0;
		this->SetDirty();
	}

	void OnResize() override
	{
		if (this->viewport != nullptr) {
			NWidgetViewport *nvp = this->GetWidget<NWidgetViewport>(WID_IV_VIEWPORT);
			nvp->UpdateViewportCoordinates(this);

			ScrollWindowToTile(Industry::Get(this->window_number)->location.GetCenterTile(), this, true); // Re-center viewport.
		}
	}

	void OnMouseWheel(int wheel) override
	{
		if (_settings_client.gui.scrollwheel_scrolling != SWS_OFF) {
			DoZoomInOutWindow(wheel < 0 ? ZOOM_IN : ZOOM_OUT, this);
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value() || str->empty()) return;

		Industry *i = Industry::Get(this->window_number);
		uint value = atoi(str->c_str());
		switch (this->editbox_line) {
			case IL_NONE: NOT_REACHED();

			case IL_MULTIPLIER:
				i->prod_level = ClampU(RoundDivSU(value * PRODLEVEL_DEFAULT, 100), PRODLEVEL_MINIMUM, PRODLEVEL_MAXIMUM);
				break;

			default:
				i->produced[this->editbox_line - IL_RATE1].rate = ClampU(RoundDivSU(value, 8), 0, 255);
				break;
		}
		UpdateIndustryProduction(i);
		this->SetDirty();
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		const Industry *i = Industry::Get(this->window_number);
		if (IsProductionAlterable(i)) {
			const IndustrySpec *ind = GetIndustrySpec(i->type);
			this->editable = ind->UsesOriginalEconomy() ? EA_MULTIPLIER : EA_RATE;
		} else {
			this->editable = EA_NONE;
		}
	}

	bool IsNewGRFInspectable() const override
	{
		return ::IsNewGRFInspectable(GSF_INDUSTRIES, this->window_number);
	}

	void ShowNewGRFInspectWindow() const override
	{
		::ShowNewGRFInspectWindow(GSF_INDUSTRIES, this->window_number);
	}
};

static void UpdateIndustryProduction(Industry *i)
{
	const IndustrySpec *indspec = GetIndustrySpec(i->type);
	if (indspec->UsesOriginalEconomy()) i->RecomputeProductionMultipliers();

	for (auto &p : i->Produced()) {
		if (p.cargo != INVALID_CARGO) {
			p.history[LAST_MONTH].production = _industry_cargo_scaler.Scale(8 * p.rate);
		}
	}
}

/** Widget definition of the view industry gui */
static constexpr NWidgetPart _nested_industry_view_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_CREAM),
		NWidget(WWT_CAPTION, COLOUR_CREAM, WID_IV_CAPTION), SetStringTip(STR_INDUSTRY_VIEW_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_PUSHIMGBTN, COLOUR_CREAM, WID_IV_GOTO), SetAspect(WidgetDimensions::ASPECT_LOCATION), SetSpriteTip(SPR_GOTO_LOCATION, STR_INDUSTRY_VIEW_LOCATION_TOOLTIP),
		NWidget(WWT_DEBUGBOX, COLOUR_CREAM),
		NWidget(WWT_SHADEBOX, COLOUR_CREAM),
		NWidget(WWT_DEFSIZEBOX, COLOUR_CREAM),
		NWidget(WWT_STICKYBOX, COLOUR_CREAM),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_CREAM),
		NWidget(WWT_INSET, COLOUR_CREAM), SetPadding(2, 2, 2, 2),
			NWidget(NWID_VIEWPORT, INVALID_COLOUR, WID_IV_VIEWPORT), SetMinimalSize(254, 86), SetFill(1, 0), SetResize(1, 1),
		EndContainer(),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_CREAM, WID_IV_INFO), SetMinimalSize(260, 0), SetMinimalTextLines(2, WidgetDimensions::unscaled.framerect.Vertical()), SetResize(1, 0),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_CREAM, WID_IV_DISPLAY), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_INDUSTRY_DISPLAY_CHAIN, STR_INDUSTRY_DISPLAY_CHAIN_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, COLOUR_CREAM, WID_IV_GRAPH), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_INDUSTRY_VIEW_PRODUCTION_GRAPH, STR_INDUSTRY_VIEW_PRODUCTION_GRAPH_TOOLTIP),
		NWidget(WWT_RESIZEBOX, COLOUR_CREAM),
	EndContainer(),
};

/** Window definition of the view industry gui */
static WindowDesc _industry_view_desc(__FILE__, __LINE__,
	WDP_AUTO, "view_industry", 260, 120,
	WC_INDUSTRY_VIEW, WC_NONE,
	{},
	_nested_industry_view_widgets
);

void ShowIndustryViewWindow(IndustryID industry)
{
	AllocateWindowDescFront<IndustryViewWindow>(_industry_view_desc, industry);
}

/** Widget definition of the industry directory gui */
static constexpr NWidgetPart _nested_industry_directory_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_BROWN),
		NWidget(WWT_CAPTION, COLOUR_BROWN, WID_ID_CAPTION), SetStringTip(STR_INDUSTRY_DIRECTORY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_BROWN),
		NWidget(WWT_DEFSIZEBOX, COLOUR_BROWN),
		NWidget(WWT_STICKYBOX, COLOUR_BROWN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_TEXTBTN, COLOUR_BROWN, WID_ID_DROPDOWN_ORDER), SetStringTip(STR_BUTTON_SORT_BY, STR_TOOLTIP_SORT_ORDER),
				NWidget(WWT_DROPDOWN, COLOUR_BROWN, WID_ID_DROPDOWN_CRITERIA), SetStringTip(STR_JUST_STRING, STR_TOOLTIP_SORT_CRITERIA),
				NWidget(WWT_EDITBOX, COLOUR_BROWN, WID_ID_FILTER), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_LIST_FILTER_OSKTITLE, STR_LIST_FILTER_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_DROPDOWN, COLOUR_BROWN, WID_ID_FILTER_BY_ACC_CARGO), SetMinimalSize(225, 12), SetFill(0, 1), SetStringTip(STR_INDUSTRY_DIRECTORY_ACCEPTED_CARGO_FILTER, STR_TOOLTIP_FILTER_CRITERIA),
				NWidget(WWT_DROPDOWN, COLOUR_BROWN, WID_ID_FILTER_BY_PROD_CARGO), SetMinimalSize(225, 12), SetFill(0, 1), SetStringTip(STR_INDUSTRY_DIRECTORY_PRODUCED_CARGO_FILTER, STR_TOOLTIP_FILTER_CRITERIA),
				NWidget(WWT_PANEL, COLOUR_BROWN), SetResize(1, 0), EndContainer(),
			EndContainer(),
			NWidget(WWT_PANEL, COLOUR_BROWN, WID_ID_INDUSTRY_LIST), SetToolTip(STR_INDUSTRY_DIRECTORY_LIST_TOOLTIP), SetResize(1, 1), SetScrollbar(WID_ID_VSCROLLBAR),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_VSCROLLBAR, COLOUR_BROWN, WID_ID_VSCROLLBAR),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_HSCROLLBAR, COLOUR_BROWN, WID_ID_HSCROLLBAR),
		NWidget(WWT_RESIZEBOX, COLOUR_BROWN),
	EndContainer(),
};

typedef GUIList<const Industry *, const CargoType &, const std::pair<CargoType, CargoType> &> GUIIndustryList;

/** Cargo filter functions */
/**
 * Check whether an industry accepts and produces a certain cargo pair.
 * @param industry The industry whose cargoes will being checked.
 * @param cargoes The accepted and produced cargo pair to look for.
 * @return bool Whether the given cargoes accepted and produced by the industry.
 */
static bool CargoFilter(const Industry * const *industry, const std::pair<CargoType, CargoType> &cargoes)
{
	auto accepted_cargo = cargoes.first;
	auto produced_cargo = cargoes.second;

	bool accepted_cargo_matches;

	switch (accepted_cargo) {
		case CargoFilterCriteria::CF_ANY:
			accepted_cargo_matches = true;
			break;

		case CargoFilterCriteria::CF_NONE:
			accepted_cargo_matches = !(*industry)->IsCargoAccepted();
			break;

		default:
			accepted_cargo_matches = (*industry)->IsCargoAccepted(accepted_cargo);
			break;
	}

	bool produced_cargo_matches;

	switch (produced_cargo) {
		case CargoFilterCriteria::CF_ANY:
			produced_cargo_matches = true;
			break;

		case CargoFilterCriteria::CF_NONE:
			produced_cargo_matches = !(*industry)->IsCargoProduced();
			break;

		default:
			produced_cargo_matches = (*industry)->IsCargoProduced(produced_cargo);
			break;
	}

	return accepted_cargo_matches && produced_cargo_matches;
}

static GUIIndustryList::FilterFunction * const _industry_filter_funcs[] = { &CargoFilter };

/** Enum referring to the Hotkeys in the industry directory window */
enum IndustryDirectoryHotkeys : int32_t {
	IDHK_FOCUS_FILTER_BOX, ///< Focus the filter box
};
/**
 * The list of industries.
 */
class IndustryDirectoryWindow : public Window {
protected:
	/* Runtime saved values */
	static Listing last_sorting;

	/* Constants for sorting industries */
	static inline const StringID sorter_names[] = {
		STR_SORT_BY_NAME,
		STR_SORT_BY_TYPE,
		STR_SORT_BY_PRODUCTION,
		STR_SORT_BY_TRANSPORTED,
	};
	static const std::initializer_list<GUIIndustryList::SortFunction * const> sorter_funcs;

	GUIIndustryList industries{IndustryDirectoryWindow::produced_cargo_filter};
	Scrollbar *vscroll;
	Scrollbar *hscroll;

	CargoType produced_cargo_filter_criteria;   ///< Selected produced cargo filter index
	CargoType accepted_cargo_filter_criteria;   ///< Selected accepted cargo filter index
	static CargoType produced_cargo_filter;

	const int MAX_FILTER_LENGTH = 16;           ///< The max length of the filter, in chars
	StringFilter string_filter;                 ///< Filter for industries
	QueryString industry_editbox;               ///< Filter editbox

	enum class SorterType : uint8_t {
		ByName,        ///< Sorter type to sort by name
		ByType,        ///< Sorter type to sort by type
		ByProduction,  ///< Sorter type to sort by production amount
		ByTransported, ///< Sorter type to sort by transported percentage
	};

	/**
	 * Set produced cargo filter for the industry list.
	 * @param cargo_type The cargo to be set
	 */
	void SetProducedCargoFilter(CargoType cargo_type)
	{
		if (this->produced_cargo_filter_criteria != cargo_type) {
			this->produced_cargo_filter_criteria = cargo_type;
			/* deactivate filter if criteria is 'Show All', activate it otherwise */
			bool is_filtering_necessary = this->produced_cargo_filter_criteria != CargoFilterCriteria::CF_ANY || this->accepted_cargo_filter_criteria != CargoFilterCriteria::CF_ANY;

			this->industries.SetFilterState(is_filtering_necessary);
			this->industries.SetFilterType(0);
			this->industries.ForceRebuild();
		}
	}

	/**
	 * Set accepted cargo filter for the industry list.
	 * @param index The cargo to be set
	 */
	void SetAcceptedCargoFilter(CargoType cargo_type)
	{
		if (this->accepted_cargo_filter_criteria != cargo_type) {
			this->accepted_cargo_filter_criteria = cargo_type;
			/* deactivate filter if criteria is 'Show All', activate it otherwise */
			bool is_filtering_necessary = this->produced_cargo_filter_criteria != CargoFilterCriteria::CF_ANY || this->accepted_cargo_filter_criteria != CargoFilterCriteria::CF_ANY;

			this->industries.SetFilterState(is_filtering_necessary);
			this->industries.SetFilterType(0);
			this->industries.ForceRebuild();
		}
	}

	StringID GetCargoFilterLabel(CargoType cargo_type) const
	{
		switch (cargo_type) {
			case CargoFilterCriteria::CF_ANY: return STR_INDUSTRY_DIRECTORY_FILTER_ALL_TYPES;
			case CargoFilterCriteria::CF_NONE: return STR_INDUSTRY_DIRECTORY_FILTER_NONE;
			default: return CargoSpec::Get(cargo_type)->name;
		}
	}

	/**
	 * Populate the filter list and set the cargo filter criteria.
	 */
	void SetCargoFilterArray()
	{
		this->produced_cargo_filter_criteria = CargoFilterCriteria::CF_ANY;
		this->accepted_cargo_filter_criteria = CargoFilterCriteria::CF_ANY;

		this->industries.SetFilterFuncs(_industry_filter_funcs);

		bool is_filtering_necessary = this->produced_cargo_filter_criteria != CargoFilterCriteria::CF_ANY || this->accepted_cargo_filter_criteria != CargoFilterCriteria::CF_ANY;

		this->industries.SetFilterState(is_filtering_necessary);
	}

	/**
	 * Get the width needed to draw the longest industry line.
	 * @return Returns width of the longest industry line, including padding.
	 */
	uint GetIndustryListWidth() const
	{
		uint width = this->hscroll->GetCount();
		auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->industries);
		for (auto it = first; it != last; ++it) {
			width = std::max(width, GetStringBoundingBox(this->GetIndustryString(*it)).width);
		}
		return width;
	}

	/** (Re)Build industries list */
	void BuildSortIndustriesList()
	{
		if (this->industries.NeedRebuild()) {
			this->industries.clear();
			this->industries.reserve(Industry::GetNumItems());

			for (const Industry *i : Industry::Iterate()) {
				if (this->string_filter.IsEmpty()) {
					this->industries.push_back(i);
					continue;
				}
				this->string_filter.ResetState();
				this->string_filter.AddLine(i->GetCachedName());
				if (this->string_filter.GetState()) this->industries.push_back(i);
			}

			this->industries.RebuildDone();

			auto filter = std::make_pair(this->accepted_cargo_filter_criteria, this->produced_cargo_filter_criteria);

			this->industries.Filter(filter);

			this->vscroll->SetCount(this->industries.size()); // Update scrollbar as well.
		}

		IndustryDirectoryWindow::produced_cargo_filter = this->produced_cargo_filter_criteria;
		this->industries.Sort();

		this->SetDirty();
	}

	/**
	 * Returns percents of cargo transported if industry produces this cargo, else -1
	 *
	 * @param p Industry produced cargo
	 * @return percents of cargo transported, or -1 if industry doesn't use this cargo slot
	 */
	static inline int GetCargoTransportedPercentsIfValid(const Industry::ProducedCargo &p)
	{
		if (p.cargo == INVALID_CARGO) return -1;
		return ToPercent8(p.history[LAST_MONTH].PctTransported());
	}

	/**
	 * Returns value representing industry's transported cargo
	 *  percentage for industry sorting
	 *
	 * @param i industry to check
	 * @return value used for sorting
	 */
	static int GetCargoTransportedSortValue(const Industry *i)
	{
		CargoType filter = IndustryDirectoryWindow::produced_cargo_filter;
		if (filter == CargoFilterCriteria::CF_NONE) return 0;

		int percentage = 0, produced_cargo_count = 0;
		for (auto &p : i->Produced()) {
			if (filter == CargoFilterCriteria::CF_ANY) {
				int transported = GetCargoTransportedPercentsIfValid(p);
				if (transported != -1) {
					produced_cargo_count++;
					percentage += transported;
				}
				if (produced_cargo_count == 0 && &p == &i->produced[i->produced_cargo_count - 1] && percentage == 0) {
					return transported;
				}
			} else if (filter == p.cargo) {
				return GetCargoTransportedPercentsIfValid(p);
			}
		}

		if (produced_cargo_count == 0) return percentage;
		return percentage / produced_cargo_count;
	}

	/** Sort industries by name */
	static bool IndustryNameSorter(const Industry * const &a, const Industry * const &b, const CargoType &)
	{
		int r = StrNaturalCompare(a->GetCachedName(), b->GetCachedName()); // Sort by name (natural sorting).
		if (r == 0) return a->index < b->index;
		return r < 0;
	}

	/** Sort industries by type and name */
	static bool IndustryTypeSorter(const Industry * const &a, const Industry * const &b, const CargoType &filter)
	{
		int it_a = 0;
		while (it_a != NUM_INDUSTRYTYPES && a->type != _sorted_industry_types[it_a]) it_a++;
		int it_b = 0;
		while (it_b != NUM_INDUSTRYTYPES && b->type != _sorted_industry_types[it_b]) it_b++;
		int r = it_a - it_b;
		return (r == 0) ? IndustryNameSorter(a, b, filter) : r < 0;
	}

	/** Sort industries by production and name */
	static bool IndustryProductionSorter(const Industry * const &a, const Industry * const &b, const CargoType &filter)
	{
		if (filter == CargoFilterCriteria::CF_NONE) return IndustryTypeSorter(a, b, filter);

		uint prod_a = 0, prod_b = 0;
		if (filter == CargoFilterCriteria::CF_ANY) {
			for (const auto &pa : a->Produced()) {
				if (pa.cargo != INVALID_CARGO) prod_a += pa.history[LAST_MONTH].production;
			}
			for (const auto &pb : b->Produced()) {
				if (pb.cargo != INVALID_CARGO) prod_b += pb.history[LAST_MONTH].production;
			}
		} else {
			int index_a = a->GetCargoProducedIndex(filter);
			if (index_a >= 0) prod_a = a->produced[index_a].history[LAST_MONTH].production;
			int index_b = b->GetCargoProducedIndex(filter);
			if (index_b >= 0) prod_b = b->produced[index_b].history[LAST_MONTH].production;
		}
		int r = prod_a - prod_b;

		return (r == 0) ? IndustryTypeSorter(a, b, filter) : r < 0;
	}

	/** Sort industries by transported cargo and name */
	static bool IndustryTransportedCargoSorter(const Industry * const &a, const Industry * const &b, const CargoType &filter)
	{
		int r = GetCargoTransportedSortValue(a) - GetCargoTransportedSortValue(b);
		return (r == 0) ? IndustryNameSorter(a, b, filter) : r < 0;
	}

	/**
	 * Get the StringID to draw and set the appropriate DParams.
	 * @param i the industry to get the StringID of.
	 * @return the StringID.
	 */
	StringID GetIndustryString(const Industry *i) const
	{
		const IndustrySpec *indsp = GetIndustrySpec(i->type);
		uint8_t p = 0;

		/* Industry name */
		SetDParam(p++, i->index);

		/* Get industry productions (CargoType, production, suffix, transported) */
		struct CargoInfo {
			CargoType cargo_type; ///< Cargo type.
			uint16_t production;  ///< Production last month.
			uint transported;     ///< Percent transported last month.
			std::string suffix;   ///< Cargo suffix.

			CargoInfo(CargoType cargo_type, uint16_t production, uint transported, std::string &&suffix) : cargo_type(cargo_type), production(production), transported(transported), suffix(std::move(suffix)) {}
		};
		std::vector<CargoInfo> cargos;

		for (const auto &p : i->Produced()) {
			if (p.cargo == INVALID_CARGO) continue;
			CargoSuffix cargo_suffix;
			GetCargoSuffix(CARGOSUFFIX_OUT, CST_DIR, i, i->type, indsp, p.cargo, &p - i->produced.get(), cargo_suffix);
			cargos.emplace_back(p.cargo, p.history[LAST_MONTH].production, ToPercent8(p.history[LAST_MONTH].PctTransported()), std::move(cargo_suffix.text));
		}

		switch (static_cast<IndustryDirectoryWindow::SorterType>(this->industries.SortType())) {
			case IndustryDirectoryWindow::SorterType::ByName:
			case IndustryDirectoryWindow::SorterType::ByType:
			case IndustryDirectoryWindow::SorterType::ByProduction:
				/* Sort by descending production, then descending transported */
				std::sort(cargos.begin(), cargos.end(), [](const CargoInfo &a, const CargoInfo &b) {
					if (a.production != b.production) return a.production > b.production;
					return a.transported > b.transported;
				});
				break;

			case IndustryDirectoryWindow::SorterType::ByTransported:
				/* Sort by descending transported, then descending production */
				std::sort(cargos.begin(), cargos.end(), [](const CargoInfo &a, const CargoInfo &b) {
					if (a.transported != b.transported) return a.transported > b.transported;
					return a.production > b.production;
				});
				break;
		}

		/* If the produced cargo filter is active then move the filtered cargo to the beginning of the list,
		 * because this is the one the player interested in, and that way it is not hidden in the 'n' more cargos */
		const CargoType cargo_type = this->produced_cargo_filter_criteria;
		if (cargo_type != CargoFilterCriteria::CF_ANY && cargo_type != CargoFilterCriteria::CF_NONE) {
			auto filtered_ci = std::ranges::find(cargos, cargo_type, &CargoInfo::cargo_type);
			if (filtered_ci != cargos.end()) {
				std::rotate(cargos.begin(), filtered_ci, filtered_ci + 1);
			}
		}

		/* Display first 3 cargos */
		for (size_t j = 0; j < std::min<size_t>(3, cargos.size()); j++) {
			CargoInfo &ci = cargos[j];
			SetDParam(p++, STR_INDUSTRY_DIRECTORY_ITEM_INFO);
			SetDParam(p++, ci.cargo_type);
			SetDParam(p++, ci.production);
			SetDParamStr(p++, std::move(ci.suffix));
			SetDParam(p++, ci.transported);
		}

		/* Undisplayed cargos if any */
		SetDParam(p++, cargos.size() - 3);

		/* Drawing the right string */
		switch (cargos.size()) {
			case 0: return STR_INDUSTRY_DIRECTORY_ITEM_NOPROD;
			case 1: return STR_INDUSTRY_DIRECTORY_ITEM_PROD1;
			case 2: return STR_INDUSTRY_DIRECTORY_ITEM_PROD2;
			case 3: return STR_INDUSTRY_DIRECTORY_ITEM_PROD3;
			default: return STR_INDUSTRY_DIRECTORY_ITEM_PRODMORE;
		}
	}

public:
	IndustryDirectoryWindow(WindowDesc &desc, WindowNumber) : Window(desc), industry_editbox(MAX_FILTER_LENGTH * MAX_CHAR_LENGTH, MAX_FILTER_LENGTH)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_ID_VSCROLLBAR);
		this->hscroll = this->GetScrollbar(WID_ID_HSCROLLBAR);

		this->industries.SetListing(this->last_sorting);
		this->industries.SetSortFuncs(IndustryDirectoryWindow::sorter_funcs);
		this->industries.ForceRebuild();

		this->FinishInitNested(0);

		this->BuildSortIndustriesList();

		this->querystrings[WID_ID_FILTER] = &this->industry_editbox;
		this->industry_editbox.cancel_button = QueryString::ACTION_CLEAR;
	}

	void Close(int data = 0) override
	{
		this->last_sorting = this->industries.GetListing();
		this->Window::Close();
	}

	void OnInit() override
	{
		this->SetCargoFilterArray();
		this->hscroll->SetCount(0);
	}

	void SetStringParameters(WidgetID widget) const override
	{
		switch (widget) {
			case WID_ID_CAPTION:
				SetDParam(0, this->vscroll->GetCount());
				SetDParam(1, Industry::GetNumItems());
				break;

			case WID_ID_DROPDOWN_CRITERIA:
				SetDParam(0, IndustryDirectoryWindow::sorter_names[this->industries.SortType()]);
				break;

			case WID_ID_FILTER_BY_ACC_CARGO:
				SetDParam(0, this->GetCargoFilterLabel(this->accepted_cargo_filter_criteria));
				break;

			case WID_ID_FILTER_BY_PROD_CARGO:
				SetDParam(0, this->GetCargoFilterLabel(this->produced_cargo_filter_criteria));
				break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_ID_DROPDOWN_ORDER:
				this->DrawSortButtonState(widget, this->industries.IsDescSortOrder() ? SBS_DOWN : SBS_UP);
				break;

			case WID_ID_INDUSTRY_LIST: {
				Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);

				/* Setup a clipping rectangle... */
				DrawPixelInfo tmp_dpi;
				if (!FillDrawPixelInfo(&tmp_dpi, ir)) return;
				/* ...but keep coordinates relative to the window. */
				tmp_dpi.left += ir.left;
				tmp_dpi.top += ir.top;

				AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);

				ir = ScrollRect(ir, *this->hscroll, 1);

				if (this->industries.empty()) {
					DrawString(ir, STR_INDUSTRY_DIRECTORY_NONE);
					break;
				}
				const CargoType acf_cargo_type = this->accepted_cargo_filter_criteria;
				auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->industries);
				for (auto it = first; it != last; ++it) {
					TextColour tc = TC_FROMSTRING;
					if (acf_cargo_type != CargoFilterCriteria::CF_ANY && acf_cargo_type != CargoFilterCriteria::CF_NONE) {
						Industry *ind = const_cast<Industry *>(*it);
						if (IndustryTemporarilyRefusesCargo(ind, acf_cargo_type)) {
							tc = TC_GREY | TC_FORCED;
						}
					}
					DrawString(ir, this->GetIndustryString(*it), tc);

					ir.top += this->resize.step_height;
				}
				break;
			}
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_ID_DROPDOWN_ORDER: {
				Dimension d = GetStringBoundingBox(this->GetWidget<NWidgetCore>(widget)->GetString());
				d.width += padding.width + Window::SortButtonWidth() * 2; // Doubled since the string is centred and it also looks better.
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_ID_DROPDOWN_CRITERIA: {
				Dimension d = GetStringListBoundingBox(IndustryDirectoryWindow::sorter_names);
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_ID_INDUSTRY_LIST: {
				Dimension d = GetStringBoundingBox(STR_INDUSTRY_DIRECTORY_NONE);
				resize.height = d.height;
				d.height *= 5;
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}
		}
	}

	DropDownList BuildCargoDropDownList() const
	{
		DropDownList list;

		/* Add item for disabling filtering. */
		list.push_back(MakeDropDownListStringItem(this->GetCargoFilterLabel(CargoFilterCriteria::CF_ANY), CargoFilterCriteria::CF_ANY));
		/* Add item for industries not producing anything, e.g. power plants */
		list.push_back(MakeDropDownListStringItem(this->GetCargoFilterLabel(CargoFilterCriteria::CF_NONE), CargoFilterCriteria::CF_NONE));

		/* Add cargos */
		Dimension d = GetLargestCargoIconSize();
		for (const CargoSpec *cs : _sorted_standard_cargo_specs) {
			list.push_back(MakeDropDownListIconItem(d, cs->GetCargoIcon(), PAL_NONE, cs->name, cs->Index()));
		}

		return list;
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_ID_DROPDOWN_ORDER:
				this->industries.ToggleSortOrder();
				this->SetDirty();
				break;

			case WID_ID_DROPDOWN_CRITERIA:
				ShowDropDownMenu(this, IndustryDirectoryWindow::sorter_names, this->industries.SortType(), WID_ID_DROPDOWN_CRITERIA, 0, 0);
				break;

			case WID_ID_FILTER_BY_ACC_CARGO: // Cargo filter dropdown
				ShowDropDownList(this, this->BuildCargoDropDownList(), this->accepted_cargo_filter_criteria, widget);
				break;

			case WID_ID_FILTER_BY_PROD_CARGO: // Cargo filter dropdown
				ShowDropDownList(this, this->BuildCargoDropDownList(), this->produced_cargo_filter_criteria, widget);
				break;

			case WID_ID_INDUSTRY_LIST: {
				auto it = this->vscroll->GetScrolledItemFromWidget(this->industries, pt.y, this, WID_ID_INDUSTRY_LIST, WidgetDimensions::scaled.framerect.top);
				if (it != this->industries.end()) {
					if (_ctrl_pressed) {
						ShowExtraViewportWindow((*it)->location.tile);
					} else {
						ScrollMainWindowToTile((*it)->location.tile);
					}
				}
				break;
			}
		}
	}

	void OnDropdownSelect(WidgetID widget, int index) override
	{
		switch (widget) {
			case WID_ID_DROPDOWN_CRITERIA: {
				if (this->industries.SortType() != index) {
					this->industries.SetSortType(index);
					this->BuildSortIndustriesList();
				}
				break;
			}

			case WID_ID_FILTER_BY_ACC_CARGO: {
				this->SetAcceptedCargoFilter(index);
				this->BuildSortIndustriesList();
				break;
			}

			case WID_ID_FILTER_BY_PROD_CARGO: {
				this->SetProducedCargoFilter(index);
				this->BuildSortIndustriesList();
				break;
			}
		}
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_ID_INDUSTRY_LIST, WidgetDimensions::scaled.framerect.Vertical());
		this->hscroll->SetCapacityFromWidget(this, WID_ID_INDUSTRY_LIST, WidgetDimensions::scaled.framerect.Horizontal());
	}

	void OnEditboxChanged(WidgetID wid) override
	{
		if (wid == WID_ID_FILTER) {
			this->string_filter.SetFilterTerm(this->industry_editbox.text.GetText());
			this->InvalidateData(IDIWD_FORCE_REBUILD);
		}
	}

	void OnPaint() override
	{
		if (this->industries.NeedRebuild()) this->BuildSortIndustriesList();
		this->hscroll->SetCount(this->GetIndustryListWidth());
		this->DrawWidgets();
	}

	void OnHundredthTick() override
	{
		this->industries.ForceResort();
		this->BuildSortIndustriesList();
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		switch (data) {
			case IDIWD_FORCE_REBUILD:
				/* This needs to be done in command-scope to enforce rebuilding before resorting invalid data */
				this->industries.ForceRebuild();
				break;

			case IDIWD_PRODUCTION_CHANGE:
				if (this->industries.SortType() == 2) this->industries.ForceResort();
				break;

			default:
				this->industries.ForceResort();
				break;
		}
	}

	EventState OnHotkey(int hotkey) override
	{
		switch (hotkey) {
			case IDHK_FOCUS_FILTER_BOX:
				this->SetFocusedWidget(WID_ID_FILTER);
				SetFocusedWindow(this); // The user has asked to give focus to the text box, so make sure this window is focused.
				break;
			default:
				return ES_NOT_HANDLED;
		}
		return ES_HANDLED;
	}

	static HotkeyList hotkeys;
};

static Hotkey industrydirectory_hotkeys[] = {
	Hotkey('F', "focus_filter_box", IDHK_FOCUS_FILTER_BOX),
};
HotkeyList IndustryDirectoryWindow::hotkeys("industrydirectory", industrydirectory_hotkeys);

Listing IndustryDirectoryWindow::last_sorting = {false, 0};

/* Available station sorting functions. */
const std::initializer_list<GUIIndustryList::SortFunction * const> IndustryDirectoryWindow::sorter_funcs = {
	&IndustryNameSorter,
	&IndustryTypeSorter,
	&IndustryProductionSorter,
	&IndustryTransportedCargoSorter
};

CargoType IndustryDirectoryWindow::produced_cargo_filter = CargoFilterCriteria::CF_ANY;


/** Window definition of the industry directory gui */
static WindowDesc _industry_directory_desc(__FILE__, __LINE__,
	WDP_AUTO, "list_industries", 428, 190,
	WC_INDUSTRY_DIRECTORY, WC_NONE,
	{},
	_nested_industry_directory_widgets,
	&IndustryDirectoryWindow::hotkeys
);

void ShowIndustryDirectory()
{
	AllocateWindowDescFront<IndustryDirectoryWindow>(_industry_directory_desc, 0);
}

/** Widgets of the industry cargoes window. */
static constexpr NWidgetPart _nested_industry_cargoes_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_BROWN),
		NWidget(WWT_CAPTION, COLOUR_BROWN, WID_IC_CAPTION), SetStringTip(STR_INDUSTRY_CARGOES_INDUSTRY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_DEBUGBOX, COLOUR_BROWN),
		NWidget(WWT_SHADEBOX, COLOUR_BROWN),
		NWidget(WWT_DEFSIZEBOX, COLOUR_BROWN),
		NWidget(WWT_STICKYBOX, COLOUR_BROWN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PANEL, COLOUR_BROWN, WID_IC_PANEL), SetResize(1, 10), SetScrollbar(WID_IC_SCROLLBAR), EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(NWID_BUTTON_DROPDOWN, COLOUR_BROWN, WID_IC_NOTIFY),
					SetStringTip(STR_INDUSTRY_CARGOES_NOTIFY_SMALLMAP, STR_INDUSTRY_CARGOES_NOTIFY_SMALLMAP_TOOLTIP),
				NWidget(WWT_PANEL, COLOUR_BROWN), SetFill(1, 0), SetResize(0, 0), EndContainer(),
				NWidget(WWT_DROPDOWN, COLOUR_BROWN, WID_IC_IND_DROPDOWN), SetFill(0, 0), SetResize(0, 0),
						SetStringTip(STR_INDUSTRY_CARGOES_SELECT_INDUSTRY, STR_INDUSTRY_CARGOES_SELECT_INDUSTRY_TOOLTIP),
				NWidget(WWT_DROPDOWN, COLOUR_BROWN, WID_IC_CARGO_DROPDOWN), SetFill(0, 0), SetResize(0, 0),
						SetStringTip(STR_INDUSTRY_CARGOES_SELECT_CARGO, STR_INDUSTRY_CARGOES_SELECT_CARGO_TOOLTIP),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_VSCROLLBAR, COLOUR_BROWN, WID_IC_SCROLLBAR),
			NWidget(WWT_RESIZEBOX, COLOUR_BROWN),
		EndContainer(),
	EndContainer(),
};

/** Window description for the industry cargoes window. */
static WindowDesc _industry_cargoes_desc(__FILE__, __LINE__,
	WDP_AUTO, "industry_cargoes", 300, 210,
	WC_INDUSTRY_CARGOES, WC_NONE,
	{},
	_nested_industry_cargoes_widgets
);

/** Available types of field. */
enum CargoesFieldType : uint8_t {
	CFT_EMPTY,       ///< Empty field.
	CFT_SMALL_EMPTY, ///< Empty small field (for the header).
	CFT_INDUSTRY,    ///< Display industry.
	CFT_CARGO,       ///< Display cargo connections.
	CFT_CARGO_LABEL, ///< Display cargo labels.
	CFT_HEADER,      ///< Header text.
};

static const uint MAX_CARGOES = 16; ///< Maximum number of cargoes carried in a #CFT_CARGO field in #CargoesField.

/** Data about a single field in the #IndustryCargoesWindow panel. */
struct CargoesField {
	static int vert_inter_industry_space;
	static int blob_distance;

	static Dimension legend;
	static Dimension cargo_border;
	static Dimension cargo_line;
	static Dimension cargo_space;
	static Dimension cargo_stub;

	static const int INDUSTRY_LINE_COLOUR;
	static const int CARGO_LINE_COLOUR;

	static int small_height, normal_height;
	static int cargo_field_width;
	static int industry_width;
	static uint max_cargoes;

	using Cargoes = uint16_t;
	static_assert(std::numeric_limits<Cargoes>::digits >= MAX_CARGOES);

	CargoesFieldType type; ///< Type of field.
	union {
		struct {
			IndustryType ind_type;                   ///< Industry type (#NUM_INDUSTRYTYPES means 'houses').
			CargoType other_produced[MAX_CARGOES];   ///< Cargoes produced but not used in this figure.
			CargoType other_accepted[MAX_CARGOES];   ///< Cargoes accepted but not used in this figure.
		} industry; ///< Industry data (for #CFT_INDUSTRY).
		struct {
			CargoType vertical_cargoes[MAX_CARGOES]; ///< Cargoes running from top to bottom (cargo type or #INVALID_CARGO).
			Cargoes supp_cargoes;                    ///< Cargoes in \c vertical_cargoes entering from the left.
			Cargoes cust_cargoes;                    ///< Cargoes in \c vertical_cargoes leaving to the right.
			uint8_t num_cargoes;                     ///< Number of cargoes.
			uint8_t top_end;                         ///< Stop at the top of the vertical cargoes.
			uint8_t bottom_end;                      ///< Stop at the bottom of the vertical cargoes.
		} cargo; ///< Cargo data (for #CFT_CARGO).
		struct {
			CargoType cargoes[MAX_CARGOES];          ///< Cargoes to display (or #INVALID_CARGO).
			bool left_align;                         ///< Align all cargo texts to the left (else align to the right).
		} cargo_label;   ///< Label data (for #CFT_CARGO_LABEL).
		StringID header; ///< Header text (for #CFT_HEADER).
	} u; // Data for each type.

	/**
	 * Make one of the empty fields (#CFT_EMPTY or #CFT_SMALL_EMPTY).
	 * @param type Type of empty field.
	 */
	void MakeEmpty(CargoesFieldType type)
	{
		this->type = type;
	}

	/**
	 * Make an industry type field.
	 * @param ind_type Industry type (#NUM_INDUSTRYTYPES means 'houses').
	 * @note #other_accepted and #other_produced should be filled later.
	 */
	void MakeIndustry(IndustryType ind_type)
	{
		this->type = CFT_INDUSTRY;
		this->u.industry.ind_type = ind_type;
		std::fill(std::begin(this->u.industry.other_accepted), std::end(this->u.industry.other_accepted), INVALID_CARGO);
		std::fill(std::begin(this->u.industry.other_produced), std::end(this->u.industry.other_produced), INVALID_CARGO);
	}

	/**
	 * Connect a cargo from an industry to the #CFT_CARGO column.
	 * @param cargo Cargo to connect.
	 * @param producer Cargo is produced (if \c false, cargo is assumed to be accepted).
	 * @return Horizontal connection index, or \c -1 if not accepted at all.
	 */
	int ConnectCargo(CargoType cargo, bool producer)
	{
		assert(this->type == CFT_CARGO);
		if (cargo == INVALID_CARGO) return -1;

		/* Find the vertical cargo column carrying the cargo. */
		int column = -1;
		for (int i = 0; i < this->u.cargo.num_cargoes; i++) {
			if (cargo == this->u.cargo.vertical_cargoes[i]) {
				column = i;
				break;
			}
		}
		if (column < 0) return -1;

		if (producer) {
			assert(!HasBit(this->u.cargo.supp_cargoes, column));
			SetBit(this->u.cargo.supp_cargoes, column);
		} else {
			assert(!HasBit(this->u.cargo.cust_cargoes, column));
			SetBit(this->u.cargo.cust_cargoes, column);
		}
		return column;
	}

	/**
	 * Does this #CFT_CARGO field have a horizontal connection?
	 * @return \c true if a horizontal connection exists, \c false otherwise.
	 */
	bool HasConnection()
	{
		assert(this->type == CFT_CARGO);

		return this->u.cargo.supp_cargoes != 0 || this->u.cargo.cust_cargoes != 0;
	}

	/**
	 * Make a piece of cargo column.
	 * @param cargoes Span of #CargoType (may contain #INVALID_CARGO).
	 * @note #supp_cargoes and #cust_cargoes should be filled in later.
	 */
	void MakeCargo(const std::span<const CargoType> cargoes)
	{
		this->type = CFT_CARGO;
		assert(std::size(cargoes) <= std::size(this->u.cargo.vertical_cargoes));
		auto insert = std::copy_if(std::begin(cargoes), std::end(cargoes), std::begin(this->u.cargo.vertical_cargoes), IsValidCargoType);
		this->u.cargo.num_cargoes = static_cast<uint8_t>(std::distance(std::begin(this->u.cargo.vertical_cargoes), insert));
		CargoTypeComparator comparator;
		std::sort(std::begin(this->u.cargo.vertical_cargoes), insert, comparator);
		std::fill(insert, std::end(this->u.cargo.vertical_cargoes), INVALID_CARGO);
		this->u.cargo.top_end = false;
		this->u.cargo.bottom_end = false;
		this->u.cargo.supp_cargoes = 0;
		this->u.cargo.cust_cargoes = 0;
	}

	/**
	 * Make a field displaying cargo type names.
	 * @param cargoes    Span of #CargoType (may contain #INVALID_CARGO).
	 * @param left_align ALign texts to the left (else to the right).
	 */
	void MakeCargoLabel(const std::span<const CargoType> cargoes, bool left_align)
	{
		this->type = CFT_CARGO_LABEL;
		assert(std::size(cargoes) <= std::size(this->u.cargo_label.cargoes));
		auto insert = std::copy(std::begin(cargoes), std::end(cargoes), std::begin(this->u.cargo_label.cargoes));
		std::fill(insert, std::end(this->u.cargo_label.cargoes), INVALID_CARGO);
		this->u.cargo_label.left_align = left_align;
	}

	/**
	 * Make a header above an industry column.
	 * @param textid Text to display.
	 */
	void MakeHeader(StringID textid)
	{
		this->type = CFT_HEADER;
		this->u.header = textid;
	}

	/**
	 * For a #CFT_CARGO, compute the left position of the left-most vertical cargo connection.
	 * @param xpos Left position of the field.
	 * @return Left position of the left-most vertical cargo column.
	 */
	int GetCargoBase(int xpos) const
	{
		assert(this->type == CFT_CARGO);
		int n = this->u.cargo.num_cargoes;

		return xpos + cargo_field_width / 2 - (CargoesField::cargo_line.width * n + CargoesField::cargo_space.width * (n - 1)) / 2;
	}

	/**
	 * Draw the field.
	 * @param xpos Position of the left edge.
	 * @param ypos Position of the top edge.
	 */
	void Draw(int xpos, int ypos) const
	{
		switch (this->type) {
			case CFT_EMPTY:
			case CFT_SMALL_EMPTY:
				break;

			case CFT_HEADER:
				ypos += (small_height - GetCharacterHeight(FS_NORMAL)) / 2;
				DrawString(xpos, xpos + industry_width, ypos, this->u.header, TC_WHITE, SA_HOR_CENTER);
				break;

			case CFT_INDUSTRY: {
				int ypos1 = ypos + vert_inter_industry_space / 2;
				int ypos2 = ypos + normal_height - 1 - vert_inter_industry_space / 2;
				int xpos2 = xpos + industry_width - 1;
				DrawRectOutline({xpos, ypos1, xpos2, ypos2}, INDUSTRY_LINE_COLOUR);
				ypos += (normal_height - GetCharacterHeight(FS_NORMAL)) / 2;
				if (this->u.industry.ind_type < NUM_INDUSTRYTYPES) {
					const IndustrySpec *indsp = GetIndustrySpec(this->u.industry.ind_type);
					DrawString(xpos, xpos2, ypos, indsp->name, TC_WHITE, SA_HOR_CENTER);

					/* Draw the industry legend. */
					int blob_left, blob_right;
					if (_current_text_dir == TD_RTL) {
						blob_right = xpos2 - blob_distance;
						blob_left  = blob_right - CargoesField::legend.width;
					} else {
						blob_left  = xpos + blob_distance;
						blob_right = blob_left + CargoesField::legend.width;
					}
					GfxFillRect(blob_left,     ypos2 - blob_distance - CargoesField::legend.height,     blob_right,     ypos2 - blob_distance,     PC_BLACK); // Border
					GfxFillRect(blob_left + 1, ypos2 - blob_distance - CargoesField::legend.height + 1, blob_right - 1, ypos2 - blob_distance - 1, indsp->map_colour);
				} else {
					DrawString(xpos, xpos2, ypos, STR_INDUSTRY_CARGOES_HOUSES, TC_FROMSTRING, SA_HOR_CENTER);
				}

				/* Draw the other_produced/other_accepted cargoes. */
				std::span<const CargoType> other_right, other_left;
				if (_current_text_dir == TD_RTL) {
					other_right = this->u.industry.other_accepted;
					other_left  = this->u.industry.other_produced;
				} else {
					other_right = this->u.industry.other_produced;
					other_left  = this->u.industry.other_accepted;
				}
				ypos1 += CargoesField::cargo_border.height + (GetCharacterHeight(FS_NORMAL) - CargoesField::cargo_line.height) / 2;
				for (uint i = 0; i < CargoesField::max_cargoes; i++) {
					if (other_right[i] != INVALID_CARGO) {
						const CargoSpec *csp = CargoSpec::Get(other_right[i]);
						int xp = xpos + industry_width + CargoesField::cargo_stub.width;
						DrawHorConnection(xpos + industry_width, xp - 1, ypos1, csp);
						GfxDrawLine(xp, ypos1, xp, ypos1 + CargoesField::cargo_line.height - 1, CARGO_LINE_COLOUR);
					}
					if (other_left[i] != INVALID_CARGO) {
						const CargoSpec *csp = CargoSpec::Get(other_left[i]);
						int xp = xpos - CargoesField::cargo_stub.width;
						DrawHorConnection(xp + 1, xpos - 1, ypos1, csp);
						GfxDrawLine(xp, ypos1, xp, ypos1 + CargoesField::cargo_line.height - 1, CARGO_LINE_COLOUR);
					}
					ypos1 += GetCharacterHeight(FS_NORMAL) + CargoesField::cargo_space.height;
				}
				break;
			}

			case CFT_CARGO: {
				int cargo_base = this->GetCargoBase(xpos);
				int top = ypos + (this->u.cargo.top_end ? vert_inter_industry_space / 2 + 1 : 0);
				int bot = ypos - (this->u.cargo.bottom_end ? vert_inter_industry_space / 2 + 1 : 0) + normal_height - 1;
				int colpos = cargo_base;
				for (int i = 0; i < this->u.cargo.num_cargoes; i++) {
					if (this->u.cargo.top_end) GfxDrawLine(colpos, top - 1, colpos + CargoesField::cargo_line.width - 1, top - 1, CARGO_LINE_COLOUR);
					if (this->u.cargo.bottom_end) GfxDrawLine(colpos, bot + 1, colpos + CargoesField::cargo_line.width - 1, bot + 1, CARGO_LINE_COLOUR);
					GfxDrawLine(colpos, top, colpos, bot, CARGO_LINE_COLOUR);
					colpos++;
					const CargoSpec *csp = CargoSpec::Get(this->u.cargo.vertical_cargoes[i]);
					GfxFillRect(colpos, top, colpos + CargoesField::cargo_line.width - 2, bot, csp->legend_colour, FILLRECT_OPAQUE);
					colpos += CargoesField::cargo_line.width - 2;
					GfxDrawLine(colpos, top, colpos, bot, CARGO_LINE_COLOUR);
					colpos += 1 + CargoesField::cargo_space.width;
				}

				Cargoes hor_left, hor_right;
				if (_current_text_dir == TD_RTL) {
					hor_left  = this->u.cargo.cust_cargoes;
					hor_right = this->u.cargo.supp_cargoes;
				} else {
					hor_left  = this->u.cargo.supp_cargoes;
					hor_right = this->u.cargo.cust_cargoes;
				}
				ypos += CargoesField::cargo_border.height + vert_inter_industry_space / 2 + (GetCharacterHeight(FS_NORMAL) - CargoesField::cargo_line.height) / 2;
				for (uint i = 0; i < MAX_CARGOES; i++) {
					if (HasBit(hor_left, i)) {
						int col = i;
						int dx = 0;
						const CargoSpec *csp = CargoSpec::Get(this->u.cargo.vertical_cargoes[col]);
						for (; col > 0; col--) {
							int lf = cargo_base + col * CargoesField::cargo_line.width + (col - 1) * CargoesField::cargo_space.width;
							DrawHorConnection(lf, lf + CargoesField::cargo_space.width - dx, ypos, csp);
							dx = 1;
						}
						DrawHorConnection(xpos, cargo_base - dx, ypos, csp);
					}
					if (HasBit(hor_right, i)) {
						int col = i;
						int dx = 0;
						const CargoSpec *csp = CargoSpec::Get(this->u.cargo.vertical_cargoes[col]);
						for (; col < this->u.cargo.num_cargoes - 1; col++) {
							int lf = cargo_base + (col + 1) * CargoesField::cargo_line.width + col * CargoesField::cargo_space.width;
							DrawHorConnection(lf + dx - 1, lf + CargoesField::cargo_space.width - 1, ypos, csp);
							dx = 1;
						}
						DrawHorConnection(cargo_base + col * CargoesField::cargo_space.width + (col + 1) * CargoesField::cargo_line.width - 1 + dx, xpos + CargoesField::cargo_field_width - 1, ypos, csp);
					}
					ypos += GetCharacterHeight(FS_NORMAL) + CargoesField::cargo_space.height;
				}
				break;
			}

			case CFT_CARGO_LABEL:
				ypos += CargoesField::cargo_border.height + vert_inter_industry_space / 2;
				for (uint i = 0; i < MAX_CARGOES; i++) {
					if (this->u.cargo_label.cargoes[i] != INVALID_CARGO) {
						const CargoSpec *csp = CargoSpec::Get(this->u.cargo_label.cargoes[i]);
						DrawString(xpos + WidgetDimensions::scaled.framerect.left, xpos + industry_width - 1 - WidgetDimensions::scaled.framerect.right, ypos, csp->name, TC_WHITE,
								(this->u.cargo_label.left_align) ? SA_LEFT : SA_RIGHT);
					}
					ypos += GetCharacterHeight(FS_NORMAL) + CargoesField::cargo_space.height;
				}
				break;

			default:
				NOT_REACHED();
		}
	}

	/**
	 * Decide which cargo was clicked at in a #CFT_CARGO field.
	 * @param left  Left industry neighbour if available (else \c nullptr should be supplied).
	 * @param right Right industry neighbour if available (else \c nullptr should be supplied).
	 * @param pt    Click position in the cargo field.
	 * @return Cargo clicked at, or #INVALID_CARGO if none.
	 */
	CargoType CargoClickedAt(const CargoesField *left, const CargoesField *right, Point pt) const
	{
		assert(this->type == CFT_CARGO);

		/* Vertical matching. */
		int cpos = this->GetCargoBase(0);
		uint col;
		for (col = 0; col < this->u.cargo.num_cargoes; col++) {
			if (pt.x < cpos) break;
			if (pt.x < cpos + (int)CargoesField::cargo_line.width) return this->u.cargo.vertical_cargoes[col];
			cpos += CargoesField::cargo_line.width + CargoesField::cargo_space.width;
		}
		/* col = 0 -> left of first col, 1 -> left of 2nd col, ... this->u.cargo.num_cargoes right of last-col. */

		int vpos = (vert_inter_industry_space / 2) + (CargoesField::cargo_border.width / 2);
		uint row;
		for (row = 0; row < MAX_CARGOES; row++) {
			if (pt.y < vpos) return INVALID_CARGO;
			if (pt.y < vpos + (int)CargoesField::cargo_line.height) break;
			vpos += GetCharacterHeight(FS_NORMAL) + CargoesField::cargo_space.width;
		}
		if (row == MAX_CARGOES) return INVALID_CARGO;

		/* row = 0 -> at first horizontal row, row = 1 -> second horizontal row, 2 = 3rd horizontal row. */
		if (col == 0) {
			if (HasBit(this->u.cargo.supp_cargoes, row)) return this->u.cargo.vertical_cargoes[row];
			if (left != nullptr) {
				if (left->type == CFT_INDUSTRY) return left->u.industry.other_produced[row];
				if (left->type == CFT_CARGO_LABEL && !left->u.cargo_label.left_align) return left->u.cargo_label.cargoes[row];
			}
			return INVALID_CARGO;
		}
		if (col == this->u.cargo.num_cargoes) {
			if (HasBit(this->u.cargo.cust_cargoes, row)) return this->u.cargo.vertical_cargoes[row];
			if (right != nullptr) {
				if (right->type == CFT_INDUSTRY) return right->u.industry.other_accepted[row];
				if (right->type == CFT_CARGO_LABEL && right->u.cargo_label.left_align) return right->u.cargo_label.cargoes[row];
			}
			return INVALID_CARGO;
		}
		if (row >= col) {
			/* Clicked somewhere in-between vertical cargo connection.
			 * Since the horizontal connection is made in the same order as the vertical list, the above condition
			 * ensures we are left-below the main diagonal, thus at the supplying side.
			 */
			if (HasBit(this->u.cargo.supp_cargoes, row)) return this->u.cargo.vertical_cargoes[row];
			return INVALID_CARGO;
		}
		/* Clicked at a customer connection. */
		if (HasBit(this->u.cargo.cust_cargoes, row)) return this->u.cargo.vertical_cargoes[row];
		return INVALID_CARGO;
	}

	/**
	 * Decide what cargo the user clicked in the cargo label field.
	 * @param pt Click position in the cargo label field.
	 * @return Cargo clicked at, or #INVALID_CARGO if none.
	 */
	CargoType CargoLabelClickedAt(Point pt) const
	{
		assert(this->type == CFT_CARGO_LABEL);

		int vpos = vert_inter_industry_space / 2 + CargoesField::cargo_border.height;
		uint row;
		for (row = 0; row < MAX_CARGOES; row++) {
			if (pt.y < vpos) return INVALID_CARGO;
			if (pt.y < vpos + GetCharacterHeight(FS_NORMAL)) break;
			vpos += GetCharacterHeight(FS_NORMAL) + CargoesField::cargo_space.height;
		}
		if (row == MAX_CARGOES) return INVALID_CARGO;
		return this->u.cargo_label.cargoes[row];
	}

private:
	/**
	 * Draw a horizontal cargo connection.
	 * @param left  Left-most coordinate to draw.
	 * @param right Right-most coordinate to draw.
	 * @param top   Top coordinate of the cargo connection.
	 * @param csp   Cargo to draw.
	 */
	static void DrawHorConnection(int left, int right, int top, const CargoSpec *csp)
	{
		GfxDrawLine(left, top, right, top, CARGO_LINE_COLOUR);
		GfxFillRect(left, top + 1, right, top + CargoesField::cargo_line.height - 2, csp->legend_colour, FILLRECT_OPAQUE);
		GfxDrawLine(left, top + CargoesField::cargo_line.height - 1, right, top + CargoesField::cargo_line.height - 1, CARGO_LINE_COLOUR);
	}
};

static_assert(MAX_CARGOES >= std::tuple_size_v<decltype(IndustrySpec::produced_cargo)>);
static_assert(MAX_CARGOES >= std::tuple_size_v<decltype(IndustrySpec::accepts_cargo)>);

Dimension CargoesField::legend;       ///< Dimension of the legend blob.
Dimension CargoesField::cargo_border; ///< Dimensions of border between cargo lines and industry boxes.
Dimension CargoesField::cargo_line;   ///< Dimensions of cargo lines.
Dimension CargoesField::cargo_space;  ///< Dimensions of space between cargo lines.
Dimension CargoesField::cargo_stub;   ///< Dimensions of cargo stub (unconnected cargo line.)

int CargoesField::small_height;      ///< Height of the header row.
int CargoesField::normal_height;     ///< Height of the non-header rows.
int CargoesField::industry_width;    ///< Width of an industry field.
int CargoesField::cargo_field_width; ///< Width of a cargo field.
uint CargoesField::max_cargoes;      ///< Largest number of cargoes actually on any industry.
int CargoesField::vert_inter_industry_space; ///< Amount of space between two industries in a column.

int CargoesField::blob_distance; ///< Distance of the industry legend colour from the edge of the industry box.

const int CargoesField::INDUSTRY_LINE_COLOUR = PC_YELLOW; ///< Line colour of the industry type box.
const int CargoesField::CARGO_LINE_COLOUR    = PC_YELLOW; ///< Line colour around the cargo.

/** A single row of #CargoesField. */
struct CargoesRow {
	CargoesField columns[5]; ///< One row of fields.

	/**
	 * Connect industry production cargoes to the cargo column after it.
	 * @param column Column of the industry.
	 */
	void ConnectIndustryProduced(int column)
	{
		CargoesField *ind_fld   = this->columns + column;
		CargoesField *cargo_fld = this->columns + column + 1;
		assert(ind_fld->type == CFT_INDUSTRY && cargo_fld->type == CFT_CARGO);

		std::fill(std::begin(ind_fld->u.industry.other_produced), std::end(ind_fld->u.industry.other_produced), INVALID_CARGO);

		if (ind_fld->u.industry.ind_type < NUM_INDUSTRYTYPES) {
			CargoType others[MAX_CARGOES]; // Produced cargoes not carried in the cargo column.
			int other_count = 0;

			const IndustrySpec *indsp = GetIndustrySpec(ind_fld->u.industry.ind_type);
			assert(CargoesField::max_cargoes <= std::size(indsp->produced_cargo));
			for (uint i = 0; i < CargoesField::max_cargoes; i++) {
				int col = cargo_fld->ConnectCargo(indsp->produced_cargo[i], true);
				if (col < 0) others[other_count++] = indsp->produced_cargo[i];
			}

			/* Allocate other cargoes in the empty holes of the horizontal cargo connections. */
			for (uint i = 0; i < CargoesField::max_cargoes && other_count > 0; i++) {
				if (HasBit(cargo_fld->u.cargo.supp_cargoes, i)) ind_fld->u.industry.other_produced[i] = others[--other_count];
			}
		} else {
			/* Houses only display cargo that towns produce. */
			for (uint i = 0; i < cargo_fld->u.cargo.num_cargoes; i++) {
				CargoType cargo_type = cargo_fld->u.cargo.vertical_cargoes[i];
				TownProductionEffect tpe = CargoSpec::Get(cargo_type)->town_production_effect;
				if (tpe == TPE_PASSENGERS || tpe == TPE_MAIL) cargo_fld->ConnectCargo(cargo_type, true);
			}
		}
	}

	/**
	 * Construct a #CFT_CARGO_LABEL field.
	 * @param column    Column to create the new field.
	 * @param accepting Display accepted cargo (if \c false, display produced cargo).
	 */
	void MakeCargoLabel(int column, bool accepting)
	{
		CargoType cargoes[MAX_CARGOES];
		std::fill(std::begin(cargoes), std::end(cargoes), INVALID_CARGO);

		CargoesField *label_fld = this->columns + column;
		CargoesField *cargo_fld = this->columns + (accepting ? column - 1 : column + 1);

		assert(cargo_fld->type == CFT_CARGO && label_fld->type == CFT_EMPTY);
		for (uint i = 0; i < cargo_fld->u.cargo.num_cargoes; i++) {
			int col = cargo_fld->ConnectCargo(cargo_fld->u.cargo.vertical_cargoes[i], !accepting);
			if (col >= 0) cargoes[col] = cargo_fld->u.cargo.vertical_cargoes[i];
		}
		label_fld->MakeCargoLabel(cargoes, accepting);
	}


	/**
	 * Connect industry accepted cargoes to the cargo column before it.
	 * @param column Column of the industry.
	 */
	void ConnectIndustryAccepted(int column)
	{
		CargoesField *ind_fld   = this->columns + column;
		CargoesField *cargo_fld = this->columns + column - 1;
		assert(ind_fld->type == CFT_INDUSTRY && cargo_fld->type == CFT_CARGO);

		std::fill(std::begin(ind_fld->u.industry.other_accepted), std::end(ind_fld->u.industry.other_accepted), INVALID_CARGO);

		if (ind_fld->u.industry.ind_type < NUM_INDUSTRYTYPES) {
			CargoType others[MAX_CARGOES]; // Accepted cargoes not carried in the cargo column.
			int other_count = 0;

			const IndustrySpec *indsp = GetIndustrySpec(ind_fld->u.industry.ind_type);
			assert(CargoesField::max_cargoes <= std::size(indsp->accepts_cargo));
			for (uint i = 0; i < CargoesField::max_cargoes; i++) {
				int col = cargo_fld->ConnectCargo(indsp->accepts_cargo[i], false);
				if (col < 0) others[other_count++] = indsp->accepts_cargo[i];
			}

			/* Allocate other cargoes in the empty holes of the horizontal cargo connections. */
			for (uint i = 0; i < CargoesField::max_cargoes && other_count > 0; i++) {
				if (!HasBit(cargo_fld->u.cargo.cust_cargoes, i)) ind_fld->u.industry.other_accepted[i] = others[--other_count];
			}
		} else {
			/* Houses only display what is demanded. */
			for (uint i = 0; i < cargo_fld->u.cargo.num_cargoes; i++) {
				for (const auto &hs : HouseSpec::Specs()) {
					if (!hs.enabled) continue;

					for (size_t j = 0; j < std::size(hs.accepts_cargo); j++) {
						if (hs.cargo_acceptance[j] > 0 && cargo_fld->u.cargo.vertical_cargoes[i] == hs.accepts_cargo[j]) {
							cargo_fld->ConnectCargo(cargo_fld->u.cargo.vertical_cargoes[i], false);
							goto next_cargo;
						}
					}
				}
next_cargo: ;
			}
		}
	}
};


/**
 * Window displaying the cargo connections around an industry (or cargo).
 *
 * The main display is constructed from 'fields', rectangles that contain an industry, piece of the cargo connection, cargo labels, or headers.
 * For a nice display, the following should be kept in mind:
 * - A #CFT_HEADER is always at the top of an column of #CFT_INDUSTRY fields.
 * - A #CFT_CARGO_LABEL field is also always put in a column of #CFT_INDUSTRY fields.
 * - The top row contains #CFT_HEADER and #CFT_SMALL_EMPTY fields.
 * - Cargo connections have a column of their own (#CFT_CARGO fields).
 * - Cargo accepted or produced by an industry, but not carried in a cargo connection, is drawn in the space of a cargo column attached to the industry.
 *   The information however is part of the industry.
 *
 * This results in the following invariants:
 * - Width of a #CFT_INDUSTRY column is large enough to hold all industry type labels, all cargo labels, and all header texts.
 * - Height of a #CFT_INDUSTRY is large enough to hold a header line, or a industry type line, \c N cargo labels
 *   (where \c N is the maximum number of cargoes connected between industries), \c N connections of cargo types, and space
 *   between two industry types (1/2 above it, and 1/2 underneath it).
 * - Width of a cargo field (#CFT_CARGO) is large enough to hold \c N vertical columns (one for each type of cargo).
 *   Also, space is needed between an industry and the leftmost/rightmost column to draw the non-carried cargoes.
 * - Height of a #CFT_CARGO field is equally high as the height of the #CFT_INDUSTRY.
 * - A field at the top (#CFT_HEADER or #CFT_SMALL_EMPTY) match the width of the fields below them (#CFT_INDUSTRY respectively
 *   #CFT_CARGO), the height should be sufficient to display the header text.
 *
 * When displaying the cargoes around an industry type, five columns are needed (supplying industries, accepted cargoes, the industry,
 * produced cargoes, customer industries). Displaying the industries around a cargo needs three columns (supplying industries, the cargo,
 * customer industries). The remaining two columns are set to #CFT_EMPTY with a width equal to the average of a cargo and an industry column.
 */
struct IndustryCargoesWindow : public Window {
	typedef std::vector<CargoesRow> Fields;

	Fields fields;  ///< Fields to display in the #WID_IC_PANEL.
	uint ind_cargo; ///< If less than #NUM_INDUSTRYTYPES, an industry type, else a cargo type + NUM_INDUSTRYTYPES.
	Dimension cargo_textsize; ///< Size to hold any cargo text, as well as STR_INDUSTRY_CARGOES_SELECT_CARGO.
	Dimension ind_textsize;   ///< Size to hold any industry type text, as well as STR_INDUSTRY_CARGOES_SELECT_INDUSTRY.
	Scrollbar *vscroll;

	IndustryCargoesWindow(int id) : Window(_industry_cargoes_desc)
	{
		this->OnInit();
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_IC_SCROLLBAR);
		this->FinishInitNested(0);
		this->OnInvalidateData(id);
	}

	void OnInit() override
	{
		/* Initialize static CargoesField size variables. */
		Dimension d = GetStringBoundingBox(STR_INDUSTRY_CARGOES_PRODUCERS);
		d = maxdim(d, GetStringBoundingBox(STR_INDUSTRY_CARGOES_CUSTOMERS));
		d.width  += WidgetDimensions::scaled.frametext.Horizontal();
		d.height += WidgetDimensions::scaled.frametext.Vertical();
		CargoesField::small_height = d.height;

		/* Size of the legend blob -- slightly larger than the smallmap legend blob. */
		CargoesField::legend.height = GetCharacterHeight(FS_SMALL);
		CargoesField::legend.width = CargoesField::legend.height * 9 / 6;

		/* Size of cargo lines. */
		CargoesField::cargo_line.width = ScaleGUITrad(6);
		CargoesField::cargo_line.height = CargoesField::cargo_line.width;

		/* Size of border between cargo lines and industry boxes. */
		CargoesField::cargo_border.width = CargoesField::cargo_line.width * 3 / 2;
		CargoesField::cargo_border.height = CargoesField::cargo_line.width / 2;

		/* Size of space between cargo lines. */
		CargoesField::cargo_space.width = CargoesField::cargo_line.width / 2;
		CargoesField::cargo_space.height = CargoesField::cargo_line.height / 2;

		/* Size of cargo stub (unconnected cargo line.) */
		CargoesField::cargo_stub.width = CargoesField::cargo_line.width / 2;
		CargoesField::cargo_stub.height = CargoesField::cargo_line.height; /* Unused */

		CargoesField::vert_inter_industry_space = WidgetDimensions::scaled.vsep_wide;
		CargoesField::blob_distance = WidgetDimensions::scaled.hsep_normal;

		/* Decide about the size of the box holding the text of an industry type. */
		this->ind_textsize.width = 0;
		this->ind_textsize.height = 0;
		CargoesField::max_cargoes = 0;
		for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
			const IndustrySpec *indsp = GetIndustrySpec(it);
			if (!indsp->enabled) continue;
			this->ind_textsize = maxdim(this->ind_textsize, GetStringBoundingBox(indsp->name));
			CargoesField::max_cargoes = std::max<uint>(CargoesField::max_cargoes, std::ranges::count_if(indsp->accepts_cargo, IsValidCargoType));
			CargoesField::max_cargoes = std::max<uint>(CargoesField::max_cargoes, std::ranges::count_if(indsp->produced_cargo, IsValidCargoType));
		}
		d.width = std::max(d.width, this->ind_textsize.width);
		d.height = this->ind_textsize.height;
		this->ind_textsize = maxdim(this->ind_textsize, GetStringBoundingBox(STR_INDUSTRY_CARGOES_SELECT_INDUSTRY));

		/* Compute max size of the cargo texts. */
		this->cargo_textsize.width = 0;
		this->cargo_textsize.height = 0;
		for (const CargoSpec *csp : CargoSpec::Iterate()) {
			if (!csp->IsValid()) continue;
			this->cargo_textsize = maxdim(this->cargo_textsize, GetStringBoundingBox(csp->name));
		}
		d = maxdim(d, this->cargo_textsize); // Box must also be wide enough to hold any cargo label.
		this->cargo_textsize = maxdim(this->cargo_textsize, GetStringBoundingBox(STR_INDUSTRY_CARGOES_SELECT_CARGO));

		d.width += WidgetDimensions::scaled.frametext.Horizontal();
		/* Ensure the height is enough for the industry type text, for the horizontal connections, and for the cargo labels. */
		uint min_ind_height = CargoesField::cargo_border.height * 2 + CargoesField::max_cargoes * GetCharacterHeight(FS_NORMAL) + (CargoesField::max_cargoes - 1) * CargoesField::cargo_space.height;
		d.height = std::max(d.height + WidgetDimensions::scaled.frametext.Vertical(), min_ind_height);

		CargoesField::industry_width = d.width;
		CargoesField::normal_height = d.height + CargoesField::vert_inter_industry_space;

		/* Width of a #CFT_CARGO field. */
		CargoesField::cargo_field_width = CargoesField::cargo_border.width * 2 + CargoesField::cargo_line.width * CargoesField::max_cargoes + CargoesField::cargo_space.width * (CargoesField::max_cargoes - 1);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_IC_PANEL:
				resize.height = CargoesField::normal_height;
				size.width = CargoesField::industry_width * 3 + CargoesField::cargo_field_width * 2 + WidgetDimensions::scaled.frametext.Horizontal();
				size.height = CargoesField::small_height + 2 * resize.height + WidgetDimensions::scaled.frametext.Vertical();
				break;

			case WID_IC_IND_DROPDOWN:
				size.width = std::max(size.width, this->ind_textsize.width + padding.width);
				break;

			case WID_IC_CARGO_DROPDOWN:
				size.width = std::max(size.width, this->cargo_textsize.width + padding.width);
				break;
		}
	}

	void SetStringParameters(WidgetID widget) const override
	{
		if (widget != WID_IC_CAPTION) return;

		if (this->ind_cargo < NUM_INDUSTRYTYPES) {
			const IndustrySpec *indsp = GetIndustrySpec(this->ind_cargo);
			SetDParam(0, indsp->name);
		} else {
			const CargoSpec *csp = CargoSpec::Get(this->ind_cargo - NUM_INDUSTRYTYPES);
			SetDParam(0, csp->name);
		}
	}

	/**
	 * Do the two sets of cargoes have a valid cargo in common?
	 * @param cargoes1 Span of the first cargo list.
	 * @param cargoes2 Span of the second cargo list.
	 * @return Arrays have at least one valid cargo in common.
	 */
	static bool HasCommonValidCargo(const std::span<const CargoType> cargoes1, const std::span<const CargoType> cargoes2)
	{
		for (const CargoType cargo_type1 : cargoes1) {
			if (!IsValidCargoType(cargo_type1)) continue;
			for (const CargoType cargo_type2 : cargoes2) {
				if (cargo_type1 == cargo_type2) return true;
			}
		}
		return false;
	}

	/**
	 * Can houses be used to supply one of the cargoes?
	 * @param cargoes Span of cargo list.
	 * @return Houses can supply at least one of the cargoes.
	 */
	static bool HousesCanSupply(const std::span<const CargoType> cargoes)
	{
		for (const CargoType cargo_type : cargoes) {
			if (!IsValidCargoType(cargo_type)) continue;
			TownProductionEffect tpe = CargoSpec::Get(cargo_type)->town_production_effect;
			if (tpe == TPE_PASSENGERS || tpe == TPE_MAIL) return true;
		}
		return false;
	}

	/**
	 * Can houses be used as customers of the produced cargoes?
	 * @param cargoes Span of cargo list.
	 * @return Houses can accept at least one of the cargoes.
	 */
	static bool HousesCanAccept(const std::span<const CargoType> cargoes)
	{
		HouseZones climate_mask;
		switch (_settings_game.game_creation.landscape) {
			case LandscapeType::Temperate: climate_mask = HZ_TEMP; break;
			case LandscapeType::Arctic:    climate_mask = HZ_SUBARTC_ABOVE | HZ_SUBARTC_BELOW; break;
			case LandscapeType::Tropic:    climate_mask = HZ_SUBTROPIC; break;
			case LandscapeType::Toyland:   climate_mask = HZ_TOYLND; break;
			default: NOT_REACHED();
		}
		for (const CargoType cargo_type : cargoes) {
			if (!IsValidCargoType(cargo_type)) continue;

			for (const auto &hs : HouseSpec::Specs()) {
				if (!hs.enabled || !(hs.building_availability & climate_mask)) continue;

				for (size_t j = 0; j < std::size(hs.accepts_cargo); j++) {
					if (hs.cargo_acceptance[j] > 0 && cargo_type == hs.accepts_cargo[j]) return true;
				}
			}
		}
		return false;
	}

	/**
	 * Count how many industries have accepted cargoes in common with one of the supplied set.
	 * @param cargoes Cargoes to search.
	 * @return Number of industries that have an accepted cargo in common with the supplied set.
	 */
	static int CountMatchingAcceptingIndustries(const std::span<const CargoType> cargoes)
	{
		int count = 0;
		for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
			const IndustrySpec *indsp = GetIndustrySpec(it);
			if (!indsp->enabled) continue;

			if (HasCommonValidCargo(cargoes, indsp->accepts_cargo)) count++;
		}
		return count;
	}

	/**
	 * Count how many industries have produced cargoes in common with one of the supplied set.
	 * @param cargoes Cargoes to search.
	 * @return Number of industries that have a produced cargo in common with the supplied set.
	 */
	static int CountMatchingProducingIndustries(const std::span<const CargoType> cargoes)
	{
		int count = 0;
		for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
			const IndustrySpec *indsp = GetIndustrySpec(it);
			if (!indsp->enabled) continue;

			if (HasCommonValidCargo(cargoes, indsp->produced_cargo)) count++;
		}
		return count;
	}

	/**
	 * Shorten the cargo column to just the part between industries.
	 * @param column Column number of the cargo column.
	 * @param top    Current top row.
	 * @param bottom Current bottom row.
	 */
	void ShortenCargoColumn(int column, int top, int bottom)
	{
		while (top < bottom && !this->fields[top].columns[column].HasConnection()) {
			this->fields[top].columns[column].MakeEmpty(CFT_EMPTY);
			top++;
		}
		this->fields[top].columns[column].u.cargo.top_end = true;

		while (bottom > top && !this->fields[bottom].columns[column].HasConnection()) {
			this->fields[bottom].columns[column].MakeEmpty(CFT_EMPTY);
			bottom--;
		}
		this->fields[bottom].columns[column].u.cargo.bottom_end = true;
	}

	/**
	 * Place an industry in the fields.
	 * @param row Row of the new industry.
	 * @param col Column of the new industry.
	 * @param it  Industry to place.
	 */
	void PlaceIndustry(int row, int col, IndustryType it)
	{
		assert(this->fields[row].columns[col].type == CFT_EMPTY);
		this->fields[row].columns[col].MakeIndustry(it);
		if (col == 0) {
			this->fields[row].ConnectIndustryProduced(col);
		} else {
			this->fields[row].ConnectIndustryAccepted(col);
		}
	}

	/**
	 * Notify smallmap that new displayed industries have been selected (in #_displayed_industries).
	 */
	void NotifySmallmap()
	{
		if (!this->IsWidgetLowered(WID_IC_NOTIFY)) return;

		UpdateSmallMapSelectedIndustries();
	}

	/**
	 * Compute what and where to display for industry type \a it.
	 * @param displayed_it Industry type to display.
	 */
	void ComputeIndustryDisplay(IndustryType displayed_it)
	{
		this->GetWidget<NWidgetCore>(WID_IC_CAPTION)->SetString(STR_INDUSTRY_CARGOES_INDUSTRY_CAPTION);
		this->ind_cargo = displayed_it;
		_displayed_industries.reset();
		_displayed_industries_in.reset();
		_displayed_industries_out.reset();
		_displayed_industries.set(displayed_it);
		_displayed_industries_in.set(displayed_it);
		_displayed_industries_out.set(displayed_it);

		this->fields.clear();
		CargoesRow &first_row = this->fields.emplace_back();
		first_row.columns[0].MakeHeader(STR_INDUSTRY_CARGOES_PRODUCERS);
		first_row.columns[1].MakeEmpty(CFT_SMALL_EMPTY);
		first_row.columns[2].MakeEmpty(CFT_SMALL_EMPTY);
		first_row.columns[3].MakeEmpty(CFT_SMALL_EMPTY);
		first_row.columns[4].MakeHeader(STR_INDUSTRY_CARGOES_CUSTOMERS);

		const IndustrySpec *central_sp = GetIndustrySpec(displayed_it);
		bool houses_supply = HousesCanSupply(central_sp->accepts_cargo);
		bool houses_accept = HousesCanAccept(central_sp->produced_cargo);
		/* Make a field consisting of two cargo columns. */
		int num_supp = CountMatchingProducingIndustries(central_sp->accepts_cargo) + houses_supply;
		int num_cust = CountMatchingAcceptingIndustries(central_sp->produced_cargo) + houses_accept;
		int num_indrows = std::max(3, std::max(num_supp, num_cust)); // One is needed for the 'it' industry, and 2 for the cargo labels.
		for (int i = 0; i < num_indrows; i++) {
			CargoesRow &row = this->fields.emplace_back();
			row.columns[0].MakeEmpty(CFT_EMPTY);
			row.columns[1].MakeCargo(central_sp->accepts_cargo);
			row.columns[2].MakeEmpty(CFT_EMPTY);
			row.columns[3].MakeCargo(central_sp->produced_cargo);
			row.columns[4].MakeEmpty(CFT_EMPTY);
		}
		/* Add central industry. */
		int central_row = 1 + num_indrows / 2;
		this->fields[central_row].columns[2].MakeIndustry(displayed_it);
		this->fields[central_row].ConnectIndustryProduced(2);
		this->fields[central_row].ConnectIndustryAccepted(2);

		/* Add cargo labels. */
		this->fields[central_row - 1].MakeCargoLabel(2, true);
		this->fields[central_row + 1].MakeCargoLabel(2, false);

		/* Add suppliers and customers of the 'it' industry. */
		int supp_count = 0;
		int cust_count = 0;
		for (IndustryType it : _sorted_industry_types) {
			const IndustrySpec *indsp = GetIndustrySpec(it);
			if (!indsp->enabled) continue;

			if (HasCommonValidCargo(central_sp->accepts_cargo, indsp->produced_cargo)) {
				this->PlaceIndustry(1 + supp_count * num_indrows / num_supp, 0, it);
				_displayed_industries.set(it);
				_displayed_industries_in.set(it);
				supp_count++;
			}
			if (HasCommonValidCargo(central_sp->produced_cargo, indsp->accepts_cargo)) {
				this->PlaceIndustry(1 + cust_count * num_indrows / num_cust, 4, it);
				_displayed_industries.set(it);
				_displayed_industries_out.set(it);
				cust_count++;
			}
		}
		if (houses_supply) {
			this->PlaceIndustry(1 + supp_count * num_indrows / num_supp, 0, NUM_INDUSTRYTYPES);
			supp_count++;
		}
		if (houses_accept) {
			this->PlaceIndustry(1 + cust_count * num_indrows / num_cust, 4, NUM_INDUSTRYTYPES);
			cust_count++;
		}

		this->ShortenCargoColumn(1, 1, num_indrows);
		this->ShortenCargoColumn(3, 1, num_indrows);
		this->vscroll->SetCount(num_indrows);
		this->SetDirty();
		this->NotifySmallmap();
	}

	/**
	 * Compute what and where to display for cargo type \a cargo_type.
	 * @param cargo_type Cargo type to display.
	 */
	void ComputeCargoDisplay(CargoType cargo_type)
	{
		this->GetWidget<NWidgetCore>(WID_IC_CAPTION)->SetString(STR_INDUSTRY_CARGOES_CARGO_CAPTION);
		this->ind_cargo = cargo_type + NUM_INDUSTRYTYPES;
		_displayed_industries.reset();
		_displayed_industries_in.reset();
		_displayed_industries_out.reset();

		this->fields.clear();
		CargoesRow &first_row = this->fields.emplace_back();
		first_row.columns[0].MakeHeader(STR_INDUSTRY_CARGOES_PRODUCERS);
		first_row.columns[1].MakeEmpty(CFT_SMALL_EMPTY);
		first_row.columns[2].MakeHeader(STR_INDUSTRY_CARGOES_CUSTOMERS);
		first_row.columns[3].MakeEmpty(CFT_SMALL_EMPTY);
		first_row.columns[4].MakeEmpty(CFT_SMALL_EMPTY);

		auto cargoes = std::span(&cargo_type, 1);
		bool houses_supply = HousesCanSupply(cargoes);
		bool houses_accept = HousesCanAccept(cargoes);
		int num_supp = CountMatchingProducingIndustries(cargoes) + houses_supply + 1; // Ensure room for the cargo label.
		int num_cust = CountMatchingAcceptingIndustries(cargoes) + houses_accept;
		int num_indrows = std::max(num_supp, num_cust);
		for (int i = 0; i < num_indrows; i++) {
			CargoesRow &row = this->fields.emplace_back();
			row.columns[0].MakeEmpty(CFT_EMPTY);
			row.columns[1].MakeCargo(cargoes);
			row.columns[2].MakeEmpty(CFT_EMPTY);
			row.columns[3].MakeEmpty(CFT_EMPTY);
			row.columns[4].MakeEmpty(CFT_EMPTY);
		}

		this->fields[num_indrows].MakeCargoLabel(0, false); // Add cargo labels at the left bottom.

		/* Add suppliers and customers of the cargo. */
		int supp_count = 0;
		int cust_count = 0;
		for (IndustryType it : _sorted_industry_types) {
			const IndustrySpec *indsp = GetIndustrySpec(it);
			if (!indsp->enabled) continue;

			if (HasCommonValidCargo(cargoes, indsp->produced_cargo)) {
				this->PlaceIndustry(1 + supp_count * num_indrows / num_supp, 0, it);
				_displayed_industries.set(it);
				_displayed_industries_in.set(it);
				supp_count++;
			}
			if (HasCommonValidCargo(cargoes, indsp->accepts_cargo)) {
				this->PlaceIndustry(1 + cust_count * num_indrows / num_cust, 2, it);
				_displayed_industries.set(it);
				_displayed_industries_out.set(it);
				cust_count++;
			}
		}
		if (houses_supply) {
			this->PlaceIndustry(1 + supp_count * num_indrows / num_supp, 0, NUM_INDUSTRYTYPES);
			supp_count++;
		}
		if (houses_accept) {
			this->PlaceIndustry(1 + cust_count * num_indrows / num_cust, 2, NUM_INDUSTRYTYPES);
			cust_count++;
		}

		this->ShortenCargoColumn(1, 1, num_indrows);
		this->vscroll->SetCount(num_indrows);
		this->SetDirty();
		this->NotifySmallmap();
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * - data = 0 .. NUM_INDUSTRYTYPES - 1: Display the chain around the given industry.
	 * - data = NUM_INDUSTRYTYPES: Stop sending updates to the smallmap window.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		if (data == NUM_INDUSTRYTYPES) {
			this->RaiseWidgetWhenLowered(WID_IC_NOTIFY);
			return;
		}

		assert(data >= 0 && data < NUM_INDUSTRYTYPES);
		this->ComputeIndustryDisplay(data);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_IC_PANEL) return;

		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		DrawPixelInfo tmp_dpi;
		if (!FillDrawPixelInfo(&tmp_dpi, ir)) return;
		AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);

		int left_pos = WidgetDimensions::scaled.frametext.left - WidgetDimensions::scaled.bevel.left;
		if (this->ind_cargo >= NUM_INDUSTRYTYPES) left_pos += (CargoesField::industry_width + CargoesField::cargo_field_width) / 2;
		int last_column = (this->ind_cargo < NUM_INDUSTRYTYPES) ? 4 : 2;

		const NWidgetBase *nwp = this->GetWidget<NWidgetBase>(WID_IC_PANEL);
		int vpos = WidgetDimensions::scaled.frametext.top - WidgetDimensions::scaled.bevel.top - this->vscroll->GetPosition() * nwp->resize_y;
		int row_height = CargoesField::small_height;
		for (const auto &field : this->fields) {
			if (vpos + row_height >= 0) {
				int xpos = left_pos;
				int col, dir;
				if (_current_text_dir == TD_RTL) {
					col = last_column;
					dir = -1;
				} else {
					col = 0;
					dir = 1;
				}
				while (col >= 0 && col <= last_column) {
					field.columns[col].Draw(xpos, vpos);
					xpos += (col & 1) ? CargoesField::cargo_field_width : CargoesField::industry_width;
					col += dir;
				}
			}
			vpos += row_height;
			if (vpos >= height) break;
			row_height = CargoesField::normal_height;
		}
	}

	/**
	 * Calculate in which field was clicked, and within the field, at what position.
	 * @param pt Clicked position in the #WID_IC_PANEL widget.
	 * @param fieldxy If \c true is returned, field x/y coordinate of \a pt.
	 * @param xy      If \c true is returned, x/y coordinate with in the field.
	 * @return Clicked at a valid position.
	 */
	bool CalculatePositionInWidget(Point pt, Point *fieldxy, Point *xy)
	{
		const NWidgetBase *nw = this->GetWidget<NWidgetBase>(WID_IC_PANEL);
		pt.x -= nw->pos_x;
		pt.y -= nw->pos_y;

		int vpos = WidgetDimensions::scaled.frametext.top + CargoesField::small_height - this->vscroll->GetPosition() * nw->resize_y;
		if (pt.y < vpos) return false;

		int row = (pt.y - vpos) / CargoesField::normal_height; // row is relative to row 1.
		if (row + 1 >= (int)this->fields.size()) return false;
		vpos = pt.y - vpos - row * CargoesField::normal_height; // Position in the row + 1 field
		row++; // rebase row to match index of this->fields.

		int xpos = 2 * WidgetDimensions::scaled.frametext.left + ((this->ind_cargo < NUM_INDUSTRYTYPES) ? 0 :  (CargoesField::industry_width + CargoesField::cargo_field_width) / 2);
		if (pt.x < xpos) return false;
		int column;
		for (column = 0; column <= 5; column++) {
			int width = (column & 1) ? CargoesField::cargo_field_width : CargoesField::industry_width;
			if (pt.x < xpos + width) break;
			xpos += width;
		}
		int num_columns = (this->ind_cargo < NUM_INDUSTRYTYPES) ? 4 : 2;
		if (column > num_columns) return false;
		xpos = pt.x - xpos;

		/* Return both positions, compensating for RTL languages (which works due to the equal symmetry in both displays). */
		fieldxy->y = row;
		xy->y = vpos;
		if (_current_text_dir == TD_RTL) {
			fieldxy->x = num_columns - column;
			xy->x = ((column & 1) ? CargoesField::cargo_field_width : CargoesField::industry_width) - xpos;
		} else {
			fieldxy->x = column;
			xy->x = xpos;
		}
		return true;
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_IC_PANEL: {
				Point fieldxy, xy;
				if (!CalculatePositionInWidget(pt, &fieldxy, &xy)) return;

				const CargoesField *fld = this->fields[fieldxy.y].columns + fieldxy.x;
				switch (fld->type) {
					case CFT_INDUSTRY:
						if (fld->u.industry.ind_type < NUM_INDUSTRYTYPES) this->ComputeIndustryDisplay(fld->u.industry.ind_type);
						break;

					case CFT_CARGO: {
						CargoesField *lft = (fieldxy.x > 0) ? this->fields[fieldxy.y].columns + fieldxy.x - 1 : nullptr;
						CargoesField *rgt = (fieldxy.x < 4) ? this->fields[fieldxy.y].columns + fieldxy.x + 1 : nullptr;
						CargoType cid = fld->CargoClickedAt(lft, rgt, xy);
						if (cid != INVALID_CARGO) this->ComputeCargoDisplay(cid);
						break;
					}

					case CFT_CARGO_LABEL: {
						CargoType cid = fld->CargoLabelClickedAt(xy);
						if (cid != INVALID_CARGO) this->ComputeCargoDisplay(cid);
						break;
					}

					default:
						break;
				}
				break;
			}

			case WID_IC_NOTIFY:
				if (this->GetWidget<NWidgetLeaf>(widget)->ButtonHit(pt)) {
					this->ToggleWidgetLoweredState(WID_IC_NOTIFY);
					this->SetWidgetDirty(WID_IC_NOTIFY);
					if (_settings_client.sound.click_beep) SndPlayFx(SND_15_BEEP);

					if (this->IsWidgetLowered(WID_IC_NOTIFY)) {
						_link_mode = ILM_ALL;
						if (FindWindowByClass(WC_SMALLMAP) == nullptr) ShowSmallMap();
						this->NotifySmallmap();
					}
				} else {
					DropDownList list;
					auto add_item = [&](StringID string, int result) {
						std::unique_ptr<DropDownListStringItem> item = std::make_unique<DropDownListStringItem>(string, result, false);
						item->SetColourFlags(TC_FORCED);
						list.emplace_back(std::move(item));
					};
					add_item(STR_INDUSTRY_CARGOES_NOTIFY_SMALLMAP_ALL, ILM_ALL);
					add_item(STR_INDUSTRY_CARGOES_PRODUCERS, ILM_IN);
					add_item(STR_INDUSTRY_CARGOES_CUSTOMERS, ILM_OUT);
					int selected = (this->IsWidgetLowered(WID_IC_NOTIFY)) ? (int)_link_mode : -1;
					ShowDropDownList(this, std::move(list), selected, WID_IC_NOTIFY);
				}
				break;

			case WID_IC_CARGO_DROPDOWN: {
				DropDownList lst;
				Dimension d = GetLargestCargoIconSize();
				for (const CargoSpec *cs : _sorted_standard_cargo_specs) {
					lst.push_back(MakeDropDownListIconItem(d, cs->GetCargoIcon(), PAL_NONE, cs->name, cs->Index()));
				}
				if (!lst.empty()) {
					int selected = (this->ind_cargo >= NUM_INDUSTRYTYPES) ? (int)(this->ind_cargo - NUM_INDUSTRYTYPES) : -1;
					ShowDropDownList(this, std::move(lst), selected, WID_IC_CARGO_DROPDOWN);
				}
				break;
			}

			case WID_IC_IND_DROPDOWN: {
				DropDownList lst;
				for (IndustryType ind : _sorted_industry_types) {
					const IndustrySpec *indsp = GetIndustrySpec(ind);
					if (!indsp->enabled) continue;
					lst.push_back(MakeDropDownListStringItem(indsp->name, ind));
				}
				if (!lst.empty()) {
					int selected = (this->ind_cargo < NUM_INDUSTRYTYPES) ? (int)this->ind_cargo : -1;
					ShowDropDownList(this, std::move(lst), selected, WID_IC_IND_DROPDOWN);
				}
				break;
			}
		}
	}

	void OnDropdownSelect(WidgetID widget, int index) override
	{
		if (index < 0) return;

		switch (widget) {
			case WID_IC_CARGO_DROPDOWN:
				this->ComputeCargoDisplay(index);
				break;

			case WID_IC_IND_DROPDOWN:
				this->ComputeIndustryDisplay(index);
				break;

			case WID_IC_NOTIFY:
				_link_mode = (IndustryLinkMode)index;
				this->LowerWidget(WID_IC_NOTIFY);
				this->SetWidgetDirty(WID_IC_NOTIFY);
				if (FindWindowByClass(WC_SMALLMAP) == nullptr) ShowSmallMap();
				this->NotifySmallmap();
				break;
		}
	}

	bool OnTooltip([[maybe_unused]] Point pt, WidgetID widget, TooltipCloseCondition close_cond) override
	{
		if (widget != WID_IC_PANEL) return false;

		Point fieldxy, xy;
		if (!CalculatePositionInWidget(pt, &fieldxy, &xy)) return false;

		const CargoesField *fld = this->fields[fieldxy.y].columns + fieldxy.x;
		CargoType cargo_type = INVALID_CARGO;
		switch (fld->type) {
			case CFT_CARGO: {
				CargoesField *lft = (fieldxy.x > 0) ? this->fields[fieldxy.y].columns + fieldxy.x - 1 : nullptr;
				CargoesField *rgt = (fieldxy.x < 4) ? this->fields[fieldxy.y].columns + fieldxy.x + 1 : nullptr;
				cargo_type = fld->CargoClickedAt(lft, rgt, xy);
				break;
			}

			case CFT_CARGO_LABEL: {
				cargo_type = fld->CargoLabelClickedAt(xy);
				break;
			}

			case CFT_INDUSTRY:
				if (fld->u.industry.ind_type < NUM_INDUSTRYTYPES && (this->ind_cargo >= NUM_INDUSTRYTYPES || fieldxy.x != 2)) {
					GuiShowTooltips(this, STR_INDUSTRY_CARGOES_INDUSTRY_TOOLTIP, close_cond);
				}
				return true;

			default:
				break;
		}
		if (cargo_type != INVALID_CARGO && (this->ind_cargo < NUM_INDUSTRYTYPES || cargo_type != this->ind_cargo - NUM_INDUSTRYTYPES)) {
			const CargoSpec *csp = CargoSpec::Get(cargo_type);
			SetDParam(0, csp->name);
			GuiShowTooltips(this, STR_INDUSTRY_CARGOES_CARGO_TOOLTIP, close_cond, 1);
			return true;
		}

		return false;
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_IC_PANEL, WidgetDimensions::scaled.framerect.Vertical() + CargoesField::small_height);
	}

	bool IsNewGRFInspectable() const override
	{
		return true;
	}

	void ShowNewGRFInspectWindow() const override
	{
		if (this->ind_cargo < NUM_INDUSTRYTYPES) {
			::ShowNewGRFInspectWindow(GSF_INDUSTRIES, this->ind_cargo | (1 << 26));
		} else {
			::ShowNewGRFInspectWindow(GSF_CARGOES, this->ind_cargo - NUM_INDUSTRYTYPES);
		}
	}
};

/**
 * Open the industry and cargoes window.
 * @param id Industry type to display, \c NUM_INDUSTRYTYPES selects a default industry type.
 */
static void ShowIndustryCargoesWindow(IndustryType id)
{
	if (id >= NUM_INDUSTRYTYPES) {
		for (IndustryType ind : _sorted_industry_types) {
			const IndustrySpec *indsp = GetIndustrySpec(ind);
			if (indsp->enabled) {
				id = ind;
				break;
			}
		}
		if (id >= NUM_INDUSTRYTYPES) return;
	}

	Window *w = BringWindowToFrontById(WC_INDUSTRY_CARGOES, 0);
	if (w != nullptr) {
		w->InvalidateData(id);
		return;
	}
	new IndustryCargoesWindow(id);
}

/** Open the industry and cargoes window with an industry. */
void ShowIndustryCargoesWindow()
{
	ShowIndustryCargoesWindow(NUM_INDUSTRYTYPES);
}

void ShowIndustryTooltip(Window *w, const TileIndex tile)
{
	if (!_settings_client.gui.industry_tooltip_show) return;
	if (!(_settings_client.gui.industry_tooltip_show_name || _settings_client.gui.industry_tooltip_show_produced ||
			_settings_client.gui.industry_tooltip_show_required || _settings_client.gui.industry_tooltip_show_stockpiled)) return;

	const Industry *industry = Industry::GetByTile(tile);
	const IndustrySpec *industry_spec = GetIndustrySpec(industry->type);

	std::string msg;

	if (_settings_client.gui.industry_tooltip_show_name) {
		// Print out the name of the industry.
		SetDParam(0, industry_spec->name);
		msg = GetString(STR_INDUSTRY_VIEW_NAME_TOOLTIP);
	}

	if (_settings_client.gui.industry_tooltip_show_required || _settings_client.gui.industry_tooltip_show_stockpiled) {
		// Have to query the stockpiling right now, in case callback 37 returns fail.
		bool stockpiling = industry_spec->callback_mask.Test(IndustryCallbackMask::ProductionCargoArrival) ||
				industry_spec->callback_mask.Test(IndustryCallbackMask::Production256Ticks);

		if (_settings_client.gui.industry_tooltip_show_required) {
			// Print out required cargo.
			bool first = true;
			std::string required_cargo_list;

			for (const auto &a : industry->Accepted()) {
				if (a.cargo == INVALID_CARGO) {
					continue;
				}

				CargoSuffix suffix;
				GetCargoSuffix(CARGOSUFFIX_IN, CST_VIEW, industry, industry->type, industry_spec, a.cargo, &a - industry->accepted.get(), suffix);

				const bool is_stockpile_with_suffix = (suffix.display == CSD_CARGO_AMOUNT_TEXT);
				const bool is_stockpile_without_suffix = (suffix.display == CSD_CARGO_AMOUNT);
				const bool is_proper_stockpile_without_suffix = (is_stockpile_without_suffix && stockpiling); // If callback 37 fails, the result is interpreted as a stockpile, for some reason.
				if (is_stockpile_with_suffix || is_proper_stockpile_without_suffix) {
					if (_settings_client.gui.industry_tooltip_show_stockpiled) continue;
				}

				StringID format = STR_INDUSTRY_VIEW_REQUIRED_TOOLTIP_NEXT;
				if (first) {
					format = STR_INDUSTRY_VIEW_REQUIRED_TOOLTIP_FIRST;
					first = false;
				}

				SetDParam(0, CargoSpec::Get(a.cargo)->name);
				SetDParamStr(1, suffix.text);
				required_cargo_list += GetString(format);
			}

			if (!required_cargo_list.empty()) {
				if (!msg.empty()) msg += '\n';
				msg += required_cargo_list;
			}
		}

		// Print out stockpiled cargo.

		if (stockpiling && _settings_client.gui.industry_tooltip_show_stockpiled) {
			for (const auto &a : industry->Accepted()) {
				if (a.cargo == INVALID_CARGO) continue;

				CargoSuffix suffix;
				GetCargoSuffix(CARGOSUFFIX_IN, CST_VIEW, industry, industry->type, industry_spec, a.cargo, &a - industry->accepted.get(), suffix);

				if (suffix.display == CSD_CARGO || suffix.display == CSD_CARGO_TEXT) {
					continue;
				}

				if (!msg.empty()) msg += '\n';

				SetDParam(0, a.cargo);
				SetDParam(1, a.waiting);
				SetDParamStr(2, suffix.text);
				msg += GetString(STR_INDUSTRY_VIEW_STOCKPILED_TOOLTIP);
			}
		}
	}

	if (_settings_client.gui.industry_tooltip_show_produced) {
		// Print out amounts of produced cargo.

		for (const auto &p : industry->Produced()) {
			if (p.cargo == INVALID_CARGO) continue;

			if (!msg.empty()) msg += '\n';

			CargoSuffix suffix;
			GetCargoSuffix(CARGOSUFFIX_OUT, CST_VIEW, industry, industry->type, industry_spec, p.cargo, &p - industry->produced.get(), suffix);

			SetDParam(0, p.cargo);
			SetDParam(1, p.history[LAST_MONTH].production);
			SetDParamStr(2, suffix.text);
			SetDParam(3, ToPercent8(p.history[LAST_MONTH].PctTransported()));
			msg += GetString(STR_INDUSTRY_VIEW_TRANSPORTED_TOOLTIP_EXTENSION);
		}
	}

	if (!msg.empty()) {
		_temp_special_strings[0] = std::move(msg);
		GuiShowTooltips(w, SPECSTR_TEMP_START, TCC_HOVER_VIEWPORT);
	}
}
