/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file town_gui.cpp GUI for towns. */

#include "stdafx.h"
#include "town.h"
#include "viewport_func.h"
#include "error.h"
#include "gui.h"
#include "house.h"
#include "newgrf_cargo.h"
#include "newgrf_house.h"
#include "newgrf_text.h"
#include "picker_gui.h"
#include "command_func.h"
#include "company_func.h"
#include "company_base.h"
#include "company_gui.h"
#include "network/network.h"
#include "string_func.h"
#include "strings_func.h"
#include "sound_func.h"
#include "tilehighlight_func.h"
#include "sortlist_type.h"
#include "road_cmd.h"
#include "landscape.h"
#include "querystring_gui.h"
#include "window_func.h"
#include "townname_func.h"
#include "core/backup_type.hpp"
#include "core/geometry_func.hpp"
#include "core/string_consumer.hpp"
#include "genworld.h"
#include "fios.h"
#include "stringfilter_type.h"
#include "dropdown_func.h"
#include "newgrf_config.h"
#include "newgrf_house.h"
#include "date_func.h"
#include "core/random_func.hpp"
#include "town_kdtree.h"
#include "zoom_func.h"
#include "hotkeys.h"
#include "town_cmd.h"

#include "widgets/town_widget.h"
#include "table/strings.h"
#include "newgrf_debug.h"
#include <algorithm>

#include <sstream>

#include "safeguards.h"

TownKdtree _town_local_authority_kdtree{};

typedef GUIList<const Town*, const bool &> GUITownList;

static constexpr NWidgetPart _nested_town_authority_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_BROWN),
		NWidget(WWT_CAPTION, COLOUR_BROWN, WID_TA_CAPTION), SetStringTip(STR_LOCAL_AUTHORITY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_TEXTBTN, COLOUR_BROWN, WID_TA_ZONE_BUTTON), SetMinimalSize(50, 0), SetStringTip(STR_LOCAL_AUTHORITY_ZONE, STR_LOCAL_AUTHORITY_ZONE_TOOLTIP),
		NWidget(WWT_SHADEBOX, COLOUR_BROWN),
		NWidget(WWT_DEFSIZEBOX, COLOUR_BROWN),
		NWidget(WWT_STICKYBOX, COLOUR_BROWN),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_BROWN, WID_TA_RATING_INFO), SetMinimalSize(317, 92), SetResize(1, 1), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_BROWN, WID_TA_COMMAND_LIST), SetMinimalSize(305, 52), SetResize(1, 0), SetToolTip(STR_LOCAL_AUTHORITY_ACTIONS_TOOLTIP), SetScrollbar(WID_TA_SCROLLBAR), EndContainer(),
		NWidget(NWID_VSCROLLBAR, COLOUR_BROWN, WID_TA_SCROLLBAR),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_BROWN, WID_TA_ACTION_INFO), SetMinimalSize(317, 52), SetResize(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_SELECTION, INVALID_COLOUR, WID_TA_BTN_SEL),
			NWidget(WWT_PUSHTXTBTN, COLOUR_BROWN, WID_TA_EXECUTE),  SetMinimalSize(317, 12), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_LOCAL_AUTHORITY_DO_IT_BUTTON, STR_LOCAL_AUTHORITY_DO_IT_TOOLTIP),
			NWidget(WWT_DROPDOWN, COLOUR_BROWN, WID_TA_SETTING),  SetMinimalSize(317, 12), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_JUST_STRING1, STR_LOCAL_AUTHORITY_SETTING_OVERRIDE_TOOLTIP),
		EndContainer(),
		NWidget(WWT_RESIZEBOX, COLOUR_BROWN),
	EndContainer()
};

/** Town authority window. */
struct TownAuthorityWindow : Window {
private:
	Town *town;    ///< Town being displayed.
	int sel_index; ///< Currently selected town action, \c 0 to \c TACT_COUNT-1, \c -1 means no action selected.
	Scrollbar *vscroll;
	uint displayed_actions_on_previous_painting; ///< Actions that were available on the previous call to OnPaint()

	Dimension icon_size;      ///< Dimensions of company icon
	Dimension exclusive_size; ///< Dimensions of exclusive icon

	/**
	 * Get the position of the Nth set bit.
	 *
	 * If there is no Nth bit set return -1
	 *
	 * @param bits The value to search in
	 * @param n The Nth set bit from which we want to know the position
	 * @return The position of the Nth set bit
	 */
	static int GetNthSetBit(uint32_t bits, int n)
	{
		if (n >= 0) {
			for (uint i : SetBitIterator(bits)) {
				n--;
				if (n < 0) return i;
			}
		}
		return -1;
	}

	static bool ChangeSettingsDisabled()
	{
		return IsNonAdminNetworkClient() &&
				!(_local_company != COMPANY_SPECTATOR && _settings_game.difficulty.override_town_settings_in_multiplayer);
	}

	static const uint SETTING_OVERRIDE_COUNT = 6;

public:
	TownAuthorityWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc), sel_index(-1), displayed_actions_on_previous_painting(0)
	{
		this->town = Town::Get(window_number);
		this->InitNested(window_number);
		this->vscroll = this->GetScrollbar(WID_TA_SCROLLBAR);
		this->vscroll->SetCapacity((this->GetWidget<NWidgetBase>(WID_TA_COMMAND_LIST)->current_y - WidgetDimensions::scaled.framerect.Vertical()) / GetCharacterHeight(FS_NORMAL));
	}

	void OnInit() override
	{
		this->icon_size      = GetSpriteSize(SPR_COMPANY_ICON);
		this->exclusive_size = GetSpriteSize(SPR_EXCLUSIVE_TRANSPORT);
	}

	void OnPaint() override
	{
		int numact;
		uint buttons = GetMaskOfTownActions(&numact, _local_company, this->town);
		numact += SETTING_OVERRIDE_COUNT;
		if (buttons != displayed_actions_on_previous_painting) this->SetDirty();
		displayed_actions_on_previous_painting = buttons;

		this->vscroll->SetCount(numact + 1);

		if (this->sel_index != -1 && this->sel_index < 0x100 && !HasBit(buttons, this->sel_index)) {
			this->sel_index = -1;
		}

		this->SetWidgetLoweredState(WID_TA_ZONE_BUTTON, this->town->show_zone);
		this->SetWidgetDisabledState(WID_TA_EXECUTE, this->sel_index == -1 || this->sel_index >= 0x100);
		this->SetWidgetDisabledState(WID_TA_SETTING, ChangeSettingsDisabled());
		this->GetWidget<NWidgetStacked>(WID_TA_BTN_SEL)->SetDisplayedPlane(this->sel_index >= 0x100 ? 1 : 0);

		this->DrawWidgets();
		if (!this->IsShaded()) this->DrawRatings();
	}

	/** Draw the contents of the ratings panel. May request a resize of the window if the contents does not fit. */
	void DrawRatings()
	{
		Rect r = this->GetWidget<NWidgetBase>(WID_TA_RATING_INFO)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);

		int text_y_offset      = (this->resize.step_height - GetCharacterHeight(FS_NORMAL)) / 2;
		int icon_y_offset      = (this->resize.step_height - this->icon_size.height) / 2;
		int exclusive_y_offset = (this->resize.step_height - this->exclusive_size.height) / 2;

		DrawString(r.left, r.right, r.top + text_y_offset, STR_LOCAL_AUTHORITY_COMPANY_RATINGS);
		r.top += this->resize.step_height;

		bool rtl = _current_text_dir == TD_RTL;
		Rect icon      = r.WithWidth(this->icon_size.width, rtl);
		Rect exclusive = r.Indent(this->icon_size.width + WidgetDimensions::scaled.hsep_normal, rtl).WithWidth(this->exclusive_size.width, rtl);
		Rect text      = r.Indent(this->icon_size.width + WidgetDimensions::scaled.hsep_normal + this->exclusive_size.width + WidgetDimensions::scaled.hsep_normal, rtl);

		/* Draw list of companies */
		for (const Company *c : Company::Iterate()) {
			if ((this->town->have_ratings.Test(c->index) || this->town->exclusivity == c->index)) {
				DrawCompanyIcon(c->index, icon.left, text.top + icon_y_offset);

				SetDParam(0, c->index);
				SetDParam(1, c->index);

				int rating = this->town->ratings[c->index];
				StringID str = STR_CARGO_RATING_APPALLING;
				if (rating > RATING_APPALLING) str++;
				if (rating > RATING_VERYPOOR)  str++;
				if (rating > RATING_POOR)      str++;
				if (rating > RATING_MEDIOCRE)  str++;
				if (rating > RATING_GOOD)      str++;
				if (rating > RATING_VERYGOOD)  str++;
				if (rating > RATING_EXCELLENT) str++;

				SetDParam(2, str);
				if (this->town->exclusivity == c->index) {
					DrawSprite(SPR_EXCLUSIVE_TRANSPORT, COMPANY_SPRITE_COLOUR(c->index), exclusive.left, text.top + exclusive_y_offset);
				}

				DrawString(text.left, text.right, text.top + text_y_offset, STR_LOCAL_AUTHORITY_COMPANY_RATING);
				text.top += this->resize.step_height;
			}
		}

		text.bottom = text.top - 1;
		if (text.bottom > r.bottom) {
			/* If the company list is too big to fit, mark ourself dirty and draw again. */
			ResizeWindow(this, 0, text.bottom - r.bottom, false);
		}
	}

	void SetStringParameters(WidgetID widget) const override
	{
		if (widget == WID_TA_CAPTION) {
			SetDParam(0, this->window_number);
		} else if (widget == WID_TA_SETTING) {
			SetDParam(0, STR_EMPTY);
			if (this->sel_index >= 0x100 && this->sel_index < (int)(0x100 + SETTING_OVERRIDE_COUNT)) {
				if (!HasBit(this->town->override_flags, this->sel_index - 0x100)) {
					SetDParam(0, STR_COLOUR_DEFAULT);
				} else {
					int idx = this->sel_index - 0x100;
					switch (idx) {
						case TSOF_OVERRIDE_BUILD_ROADS:
						case TSOF_OVERRIDE_BUILD_LEVEL_CROSSINGS:
						case TSOF_OVERRIDE_BUILD_BRIDGES:
							SetDParam(0, HasBit(this->town->override_values, idx) ? STR_CONFIG_SETTING_ON : STR_CONFIG_SETTING_OFF);
							break;
						case TSOF_OVERRIDE_BUILD_TUNNELS:
							SetDParam(0, STR_CONFIG_SETTING_TOWN_TUNNELS_FORBIDDEN + this->town->build_tunnels);
							break;
						case TSOF_OVERRIDE_BUILD_INCLINED_ROADS:
							SetDParam(0, STR_CONFIG_SETTING_TOWN_MAX_ROAD_SLOPE_VALUE + ((this->town->max_road_slope == 0) ? 1 : 0));
							SetDParam(1, this->town->max_road_slope);
							break;
						case TSOF_OVERRIDE_GROWTH:
							SetDParam(0, HasBit(this->town->override_values, idx) ? STR_CONFIG_SETTING_ON : STR_CONFIG_SETTING_TOWN_GROWTH_NONE);
							break;
					}
				}
			}
		}
	}

	std::pair<StringID, TextColour> PrepareActionInfoString(int action_index) const
	{
		TextColour colour = TC_FROMSTRING;
		StringID text = STR_NULL;
		if (action_index >= 0x100) {
			SetDParam(1, STR_EMPTY);
			switch (action_index - 0x100) {
				case TSOF_OVERRIDE_BUILD_ROADS:
					SetDParam(1, STR_CONFIG_SETTING_ALLOW_TOWN_ROADS_HELPTEXT);
					break;
				case TSOF_OVERRIDE_BUILD_LEVEL_CROSSINGS:
					SetDParam(1, STR_CONFIG_SETTING_ALLOW_TOWN_LEVEL_CROSSINGS_HELPTEXT);
					break;
				case TSOF_OVERRIDE_BUILD_TUNNELS:
					SetDParam(1, STR_CONFIG_SETTING_TOWN_TUNNELS_HELPTEXT);
					break;
				case TSOF_OVERRIDE_BUILD_INCLINED_ROADS:
					SetDParam(1, STR_CONFIG_SETTING_TOWN_MAX_ROAD_SLOPE_HELPTEXT);
					break;
				case TSOF_OVERRIDE_GROWTH:
					SetDParam(1, STR_CONFIG_SETTING_TOWN_GROWTH_HELPTEXT);
					break;
				case TSOF_OVERRIDE_BUILD_BRIDGES:
					SetDParam(1, STR_CONFIG_SETTING_ALLOW_TOWN_BRIDGES_HELPTEXT);
					break;
			}
			text = STR_LOCAL_AUTHORITY_SETTING_OVERRIDE_TEXT;
			SetDParam(0, STR_LOCAL_AUTHORITY_SETTING_OVERRIDE_ALLOW_ROADS + action_index - 0x100);
		} else {
			colour = TC_YELLOW;
			switch (action_index) {
				case 0:
					text = STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_SMALL_ADVERTISING;
					break;
				case 1:
					text = STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_MEDIUM_ADVERTISING;
					break;
				case 2:
					text = STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_LARGE_ADVERTISING;
					break;
				case 3:
					text = EconTime::UsingWallclockUnits() ? STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_ROAD_RECONSTRUCTION_MINUTES : STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_ROAD_RECONSTRUCTION_MONTHS;
					break;
				case 4:
					text = STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_STATUE_OF_COMPANY;
					break;
				case 5:
					text = STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_NEW_BUILDINGS;
					break;
				case 6:
					text = EconTime::UsingWallclockUnits() ? STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_EXCLUSIVE_TRANSPORT_MINUTES : STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_EXCLUSIVE_TRANSPORT_MONTHS;
					break;
				case 7:
					text = STR_LOCAL_AUTHORITY_ACTION_TOOLTIP_BRIBE;
					break;
			}
			SetDParam(0, _price[PR_TOWN_ACTION] * _town_action_costs[action_index] >> 8);
		}

		return { text, colour };
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_TA_ACTION_INFO:
				if (this->sel_index != -1) {
					auto [text, colour] = this->PrepareActionInfoString(this->sel_index);
					DrawStringMultiLine(r.Shrink(WidgetDimensions::scaled.framerect), text, colour);
				}
				break;
			case WID_TA_COMMAND_LIST: {
				int numact;
				uint buttons = GetMaskOfTownActions(&numact, _local_company, this->town);
				numact += SETTING_OVERRIDE_COUNT;
				Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
				int y = ir.top;
				int pos = this->vscroll->GetPosition();

				if (--pos < 0) {
					DrawString(ir.left, ir.right, y, STR_LOCAL_AUTHORITY_ACTIONS_TITLE);
					y += GetCharacterHeight(FS_NORMAL);
				}

				for (int i = 0; buttons; i++, buttons >>= 1) {
					if ((buttons & 1) && --pos < 0) {
						DrawString(ir.left, ir.right, y,
								STR_LOCAL_AUTHORITY_ACTION_SMALL_ADVERTISING_CAMPAIGN + i, this->sel_index == i ? TC_WHITE : TC_ORANGE);
						y += GetCharacterHeight(FS_NORMAL);
					}
				}
				for (int i = 0; i < (int)SETTING_OVERRIDE_COUNT; i++) {
					if (--pos < 0) {
						const bool disabled = ChangeSettingsDisabled();
						const bool selected = (this->sel_index == (0x100 + i));
						const TextColour tc = disabled ? (TC_NO_SHADE | (selected ? TC_SILVER : TC_GREY)) : (selected ? TC_WHITE : TC_ORANGE);
						const bool overridden = HasBit(this->town->override_flags, i);
						SetDParam(0, STR_LOCAL_AUTHORITY_SETTING_OVERRIDE_ALLOW_ROADS + i);
						SetDParam(1, overridden ? STR_JUST_STRING1 : STR_LOCAL_AUTHORITY_SETTING_OVERRIDE_DEFAULT);
						switch (i) {
							case TSOF_OVERRIDE_BUILD_ROADS:
								SetDParam(2, this->town->GetAllowBuildRoads() ? STR_CONFIG_SETTING_ON : STR_CONFIG_SETTING_OFF);
								break;

							case TSOF_OVERRIDE_BUILD_LEVEL_CROSSINGS:
								SetDParam(2, this->town->GetAllowBuildLevelCrossings() ? STR_CONFIG_SETTING_ON : STR_CONFIG_SETTING_OFF);
								break;

							case TSOF_OVERRIDE_BUILD_TUNNELS: {
								TownTunnelMode tunnel_mode = this->town->GetBuildTunnelMode();
								SetDParam(2, STR_CONFIG_SETTING_TOWN_TUNNELS_FORBIDDEN + tunnel_mode);
								break;
							}

							case TSOF_OVERRIDE_BUILD_INCLINED_ROADS: {
								uint8_t max_slope = this->town->GetBuildMaxRoadSlope();
								SetDParam(2, STR_CONFIG_SETTING_TOWN_MAX_ROAD_SLOPE_VALUE + ((max_slope == 0) ? 1 : 0));
								SetDParam(3, max_slope);
								break;
							}

							case TSOF_OVERRIDE_GROWTH:
								SetDParam(2, this->town->IsTownGrowthDisabledByOverride() ? STR_CONFIG_SETTING_TOWN_GROWTH_NONE : STR_CONFIG_SETTING_DEFAULT_ALLOW_TOWN_GROWTH_ALLOWED);
								break;

							case TSOF_OVERRIDE_BUILD_BRIDGES:
								SetDParam(2, this->town->GetAllowBuildBridges() ? STR_CONFIG_SETTING_ON : STR_CONFIG_SETTING_OFF);
								break;
						}
						DrawString(ir.left, ir.right, y,
								STR_LOCAL_AUTHORITY_SETTING_OVERRIDE_STR, tc);
						y += GetCharacterHeight(FS_NORMAL);
					}
				}
				break;
			}
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_TA_ACTION_INFO: {
				assert(size.width > padding.width && size.height > padding.height);
				Dimension d = {0, 0};
				for (int i = 0; i < TACT_COUNT; i++) {
					auto [text, _] = this->PrepareActionInfoString(i);
					d = maxdim(d, GetStringMultiLineBoundingBox(text, size));
				}
				for (int i = TSOF_OVERRIDE_BEGIN; i < TSOF_OVERRIDE_END; i++) {
					auto [text, _] = this->PrepareActionInfoString(i + 0x100);
					d = maxdim(d, GetStringMultiLineBoundingBox(text, size));
				}
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_TA_COMMAND_LIST:
				size.height = (5 + SETTING_OVERRIDE_COUNT) * GetCharacterHeight(FS_NORMAL) + padding.height;
				size.width = GetStringBoundingBox(STR_LOCAL_AUTHORITY_ACTIONS_TITLE).width;
				for (uint i = 0; i < TACT_COUNT; i++ ) {
					size.width = std::max(size.width, GetStringBoundingBox(STR_LOCAL_AUTHORITY_ACTION_SMALL_ADVERTISING_CAMPAIGN + i).width + padding.width);
				}
				size.width += padding.width;
				break;

			case WID_TA_RATING_INFO:
				resize.height = std::max({this->icon_size.height + WidgetDimensions::scaled.vsep_normal, this->exclusive_size.height + WidgetDimensions::scaled.vsep_normal, (uint)GetCharacterHeight(FS_NORMAL)});
				size.height = 9 * resize.height + padding.height;
				break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_TA_ZONE_BUTTON: {
				bool new_show_state = !this->town->show_zone;
				TownID index = this->town->index;

				new_show_state ? _town_local_authority_kdtree.Insert(index) : _town_local_authority_kdtree.Remove(index);

				this->town->show_zone = new_show_state;
				this->SetWidgetLoweredState(widget, new_show_state);
				this->SetWidgetDirty(widget);
				MarkWholeNonMapViewportsDirty();
				break;
			}

			case WID_TA_COMMAND_LIST: {
				int y = this->GetRowFromWidget(pt.y, WID_TA_COMMAND_LIST, 1, GetCharacterHeight(FS_NORMAL));
				if (!IsInsideMM(y, 0, 5 + SETTING_OVERRIDE_COUNT)) return;

				const uint setting_override_offset = 32 - SETTING_OVERRIDE_COUNT;

				y = GetNthSetBit(GetMaskOfTownActions(nullptr, _local_company, this->town) | (UINT32_MAX << setting_override_offset), y + this->vscroll->GetPosition() - 1);
				if (y >= (int)setting_override_offset) {
					this->sel_index = y + 0x100 - setting_override_offset;
					this->SetDirty();
					break;
				} else if (y >= 0) {
					this->sel_index = y;
					this->SetDirty();
				}
				/* When double-clicking, continue */
				if (click_count == 1 || y < 0) break;
				[[fallthrough]];
			}

			case WID_TA_EXECUTE:
				Command<CMD_DO_TOWN_ACTION>::Post(STR_ERROR_CAN_T_DO_THIS, this->town->xy, this->window_number, this->sel_index);
				break;

			case WID_TA_SETTING: {
				uint8_t idx = this->sel_index - 0x100;
				switch (idx) {
					case TSOF_OVERRIDE_BUILD_ROADS:
					case TSOF_OVERRIDE_BUILD_LEVEL_CROSSINGS:
					case TSOF_OVERRIDE_BUILD_BRIDGES: {
						int value = HasBit(this->town->override_flags, idx) ? (HasBit(this->town->override_values, idx) ? 2 : 1) : 0;
						const StringID names[] = {
							STR_COLOUR_DEFAULT,
							STR_CONFIG_SETTING_OFF,
							STR_CONFIG_SETTING_ON,
						};
						ShowDropDownMenu(this, names, value, WID_TA_SETTING, 0, 0);
						break;
					}
					case TSOF_OVERRIDE_BUILD_TUNNELS: {
						const StringID names[] = {
							STR_COLOUR_DEFAULT,
							STR_CONFIG_SETTING_TOWN_TUNNELS_FORBIDDEN,
							STR_CONFIG_SETTING_TOWN_TUNNELS_ALLOWED_OBSTRUCTION,
							STR_CONFIG_SETTING_TOWN_TUNNELS_ALLOWED,
						};
						ShowDropDownMenu(this, names, HasBit(this->town->override_flags, idx) ? this->town->build_tunnels + 1 : 0, WID_TA_SETTING, 0, 0);
						break;
					}
					case TSOF_OVERRIDE_BUILD_INCLINED_ROADS: {
						DropDownList dlist;
						dlist.push_back(MakeDropDownListStringItem(STR_COLOUR_DEFAULT, 0, false));
						dlist.push_back(MakeDropDownListStringItem(STR_CONFIG_SETTING_TOWN_MAX_ROAD_SLOPE_ZERO, 1, false));
						for (int i = 1; i <= 8; i++) {
							SetDParam(0, i);
							dlist.push_back(MakeDropDownListStringItem(STR_CONFIG_SETTING_TOWN_MAX_ROAD_SLOPE_VALUE, i + 1, false));
						}
						ShowDropDownList(this, std::move(dlist), HasBit(this->town->override_flags, idx) ? this->town->max_road_slope + 1 : 0, WID_TA_SETTING);
						break;
					}
					case TSOF_OVERRIDE_GROWTH: {
						int value = HasBit(this->town->override_flags, idx) ? (HasBit(this->town->override_values, idx) ? 2 : 1) : 0;
						const StringID names[] = {
							STR_COLOUR_DEFAULT,
							STR_CONFIG_SETTING_TOWN_GROWTH_NONE,
							STR_CONFIG_SETTING_DEFAULT_ALLOW_TOWN_GROWTH_ALLOWED,
						};
						ShowDropDownMenu(this, names, value, WID_TA_SETTING, 0, 0);
						break;
					}
				}
				break;
			}
		}
	}


	virtual void OnDropdownSelect(WidgetID widget, int index) override
	{
		switch (widget) {
			case WID_TA_SETTING: {
				if (index < 0) break;
				auto payload = CmdPayload<CMD_TOWN_SETTING_OVERRIDE>::Make(this->window_number, static_cast<TownSettingOverrideFlags>(this->sel_index - 0x100), index > 0, (index > 0) ? index - 1 : 0);
				if (IsNonAdminNetworkClient()) {
					DoCommandP<CMD_TOWN_SETTING_OVERRIDE_NON_ADMIN>(payload, STR_ERROR_CAN_T_DO_THIS);
				} else {
					DoCommandP<CMD_TOWN_SETTING_OVERRIDE>(payload, STR_ERROR_CAN_T_DO_THIS);
				}
				break;
			}

			default: NOT_REACHED();
		}

		this->SetDirty();
	}

	void OnHundredthTick() override
	{
		this->SetDirty();
	}
};

static WindowDesc _town_authority_desc(__FILE__, __LINE__,
	WDP_AUTO, "view_town_authority", 317, 222,
	WC_TOWN_AUTHORITY, WC_NONE,
	{},
	_nested_town_authority_widgets
);

static void ShowTownAuthorityWindow(uint town)
{
	AllocateWindowDescFront<TownAuthorityWindow>(_town_authority_desc, town);
}


/* Town view window. */
struct TownViewWindow : Window {
private:
	Town *town; ///< Town displayed by the window.

public:
	static const int WID_TV_HEIGHT_NORMAL = 150;

	TownViewWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->CreateNestedTree();

		this->town = Town::Get(window_number);
		if (this->town->larger_town) this->GetWidget<NWidgetCore>(WID_TV_CAPTION)->SetString(STR_TOWN_VIEW_CITY_CAPTION);

		this->FinishInitNested(window_number);

		this->flags.Set(WindowFlag::DisableVpScroll);
		NWidgetViewport *nvp = this->GetWidget<NWidgetViewport>(WID_TV_VIEWPORT);
		nvp->InitializeViewport(this, this->town->xy.base(), ScaleZoomGUI(ZOOM_LVL_TOWN));
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		SetViewportCatchmentTown(Town::Get(this->window_number), false);
		this->Window::Close();
	}

	void SetStringParameters(WidgetID widget) const override
	{
		if (widget == WID_TV_CAPTION) SetDParam(0, this->town->index);
	}

	void OnPaint() override
	{
		extern const Town *_viewport_highlight_town;
		this->SetWidgetLoweredState(WID_TV_CATCHMENT, _viewport_highlight_town == this->town);
		this->SetWidgetDisabledState(WID_TV_CHANGE_NAME, IsNonAdminNetworkClient() &&
				!(_local_company != COMPANY_SPECTATOR && _settings_game.difficulty.rename_towns_in_multiplayer));

		this->DrawWidgets();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_TV_INFO) return;

		Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);

		SetDParam(0, this->town->cache.population);
		SetDParam(1, this->town->cache.num_houses);
		DrawString(tr, STR_TOWN_VIEW_POPULATION_HOUSES);
		tr.top += GetCharacterHeight(FS_NORMAL);

		StringID str_last_period;
		if (EconTime::UsingWallclockUnits()) {
			str_last_period = ReplaceWallclockMinutesUnit() ? STR_TOWN_VIEW_CARGO_LAST_PRODUCTION_INTERVAL_MAX : STR_TOWN_VIEW_CARGO_LAST_MINUTE_MAX;
		} else {
			str_last_period = STR_TOWN_VIEW_CARGO_LAST_MONTH_MAX;
		}

		for (auto tpe : {TPE_PASSENGERS, TPE_MAIL}) {
			for (CargoType cid : CargoSpec::town_production_cargoes[tpe]) {
				SetDParam(0, 1ULL << cid);
				SetDParam(1, this->town->supplied[cid].old_act);
				SetDParam(2, this->town->supplied[cid].old_max);
				DrawString(tr, str_last_period);
				tr.top += GetCharacterHeight(FS_NORMAL);
			}
		}

		bool first = true;
		for (int i = TAE_BEGIN; i < TAE_END; i++) {
			if (this->town->goal[i] == 0) continue;
			if (this->town->goal[i] == TOWN_GROWTH_WINTER && (TileHeight(this->town->xy) < LowestSnowLine() || this->town->cache.population <= 90)) continue;
			if (this->town->goal[i] == TOWN_GROWTH_DESERT && (GetTropicZone(this->town->xy) != TROPICZONE_DESERT || this->town->cache.population <= 60)) continue;

			if (first) {
				DrawString(tr, STR_TOWN_VIEW_CARGO_FOR_TOWNGROWTH);
				tr.top += GetCharacterHeight(FS_NORMAL);
				first = false;
			}

			bool rtl = _current_text_dir == TD_RTL;

			const CargoSpec *cargo = FindFirstCargoWithTownAcceptanceEffect((TownAcceptanceEffect)i);
			if (cargo == nullptr) {
				DrawString(tr.Indent(20, rtl), STR_NEWGRF_INVALID_CARGO, TC_RED);
				tr.top += GetCharacterHeight(FS_NORMAL);
				continue;
			}

			StringID string;

			if (this->town->goal[i] == TOWN_GROWTH_DESERT || this->town->goal[i] == TOWN_GROWTH_WINTER) {
				/* For 'original' gameplay, don't show the amount required (you need 1 or more ..) */
				string = STR_TOWN_VIEW_CARGO_FOR_TOWNGROWTH_DELIVERED_GENERAL;
				if (this->town->received[i].old_act == 0) {
					string = STR_TOWN_VIEW_CARGO_FOR_TOWNGROWTH_REQUIRED_GENERAL;

					if (this->town->goal[i] == TOWN_GROWTH_WINTER && TileHeight(this->town->xy) < GetSnowLine()) {
						string = STR_TOWN_VIEW_CARGO_FOR_TOWNGROWTH_REQUIRED_WINTER;
					}
				}

				SetDParam(0, cargo->name);
			} else {
				string = STR_TOWN_VIEW_CARGO_FOR_TOWNGROWTH_DELIVERED;
				if (this->town->received[i].old_act < this->town->goal[i]) {
					string = STR_TOWN_VIEW_CARGO_FOR_TOWNGROWTH_REQUIRED;
				}

				SetDParam(0, cargo->Index());
				SetDParam(1, this->town->received[i].old_act);
				SetDParam(2, cargo->Index());
				SetDParam(3, this->town->goal[i]);
			}
			DrawString(tr.Indent(20, rtl), string);
			tr.top += GetCharacterHeight(FS_NORMAL);
		}

		if (HasBit(this->town->flags, TOWN_IS_GROWING)) {
			SetDParam(0, RoundDivSU(this->town->growth_rate + 1, DAY_TICKS));
			DrawString(tr, this->town->fund_buildings_months == 0 ? STR_TOWN_VIEW_TOWN_GROWS_EVERY : STR_TOWN_VIEW_TOWN_GROWS_EVERY_FUNDED);
			tr.top += GetCharacterHeight(FS_NORMAL);
		} else {
			DrawString(tr, STR_TOWN_VIEW_TOWN_GROW_STOPPED);
			tr.top += GetCharacterHeight(FS_NORMAL);
		}

		/* only show the town noise, if the noise option is activated. */
		if (_settings_game.economy.station_noise_level) {
			uint16_t max_noise = this->town->MaxTownNoise();
			SetDParam(0, this->town->noise_reached);
			SetDParam(1, max_noise);
			DrawString(tr, max_noise == UINT16_MAX ? STR_TOWN_VIEW_NOISE_IN_TOWN_NO_LIMIT : STR_TOWN_VIEW_NOISE_IN_TOWN);
			tr.top += GetCharacterHeight(FS_NORMAL);
		}

		if (!this->town->text.empty()) {
			SetDParamStr(0, this->town->text);
			tr.top = DrawStringMultiLine(tr, STR_JUST_RAW_STRING, TC_BLACK);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_TV_CENTER_VIEW: // scroll to location
				if (_ctrl_pressed) {
					ShowExtraViewportWindow(this->town->xy);
				} else {
					ScrollMainWindowToTile(this->town->xy);
				}
				break;

			case WID_TV_SHOW_AUTHORITY: // town authority
				ShowTownAuthorityWindow(this->window_number);
				break;

			case WID_TV_CHANGE_NAME: // rename
				ShowQueryString(GetString(STR_TOWN_NAME, this->window_number), STR_TOWN_VIEW_RENAME_TOWN_BUTTON, MAX_LENGTH_TOWN_NAME_CHARS, this, CS_ALPHANUMERAL, QSF_ENABLE_DEFAULT | QSF_LEN_IN_CHARS);
				break;

			case WID_TV_CATCHMENT:
				SetViewportCatchmentTown(Town::Get(this->window_number), !this->IsWidgetLowered(WID_TV_CATCHMENT));
				break;

			case WID_TV_EXPAND: // expand town - only available on Scenario editor
				Command<CMD_EXPAND_TOWN>::Post(STR_ERROR_CAN_T_EXPAND_TOWN, static_cast<TownID>(this->window_number), 0, {TownExpandMode::Buildings, TownExpandMode::Roads});
				break;

			case WID_TV_EXPAND_BUILDINGS: // expand buildings of town - only available on Scenario editor
				Command<CMD_EXPAND_TOWN>::Post(STR_ERROR_CAN_T_EXPAND_TOWN, static_cast<TownID>(this->window_number), 0, {TownExpandMode::Buildings});
				break;

			case WID_TV_EXPAND_ROADS: // expand roads of town - only available on Scenario editor
				Command<CMD_EXPAND_TOWN>::Post(STR_ERROR_CAN_T_EXPAND_TOWN, static_cast<TownID>(this->window_number), 0, {TownExpandMode::Roads});
				break;

			case WID_TV_DELETE: // delete town - only available on Scenario editor
				Command<CMD_DELETE_TOWN>::Post(STR_ERROR_TOWN_CAN_T_DELETE, this->window_number);
				break;
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_TV_INFO:
				size.height = GetDesiredInfoHeight(size.width) + padding.height;
				break;
		}
	}

	/**
	 * Gets the desired height for the information panel.
	 * @return the desired height in pixels.
	 */
	uint GetDesiredInfoHeight(int width) const
	{
		uint aimed_height = static_cast<uint>(1 + CountBits(CargoSpec::town_production_cargo_mask[TPE_PASSENGERS] | CargoSpec::town_production_cargo_mask[TPE_MAIL])) * GetCharacterHeight(FS_NORMAL);

		bool first = true;
		for (int i = TAE_BEGIN; i < TAE_END; i++) {
			if (this->town->goal[i] == 0) continue;
			if (this->town->goal[i] == TOWN_GROWTH_WINTER && (TileHeight(this->town->xy) < LowestSnowLine() || this->town->cache.population <= 90)) continue;
			if (this->town->goal[i] == TOWN_GROWTH_DESERT && (GetTropicZone(this->town->xy) != TROPICZONE_DESERT || this->town->cache.population <= 60)) continue;

			if (first) {
				aimed_height += GetCharacterHeight(FS_NORMAL);
				first = false;
			}
			aimed_height += GetCharacterHeight(FS_NORMAL);
		}
		aimed_height += GetCharacterHeight(FS_NORMAL);

		if (_settings_game.economy.station_noise_level) aimed_height += GetCharacterHeight(FS_NORMAL);

		if (!this->town->text.empty()) {
			SetDParamStr(0, this->town->text);
			aimed_height += GetStringHeight(STR_JUST_RAW_STRING, width - WidgetDimensions::scaled.framerect.Horizontal());
		}

		return aimed_height;
	}

	void ResizeWindowAsNeeded()
	{
		const NWidgetBase *nwid_info = this->GetWidget<NWidgetBase>(WID_TV_INFO);
		uint aimed_height = GetDesiredInfoHeight(nwid_info->current_x);
		if (aimed_height > nwid_info->current_y || (aimed_height < nwid_info->current_y && nwid_info->current_y > nwid_info->smallest_y)) {
			this->ReInit();
		}
	}

	void OnResize() override
	{
		if (this->viewport != nullptr) {
			NWidgetViewport *nvp = this->GetWidget<NWidgetViewport>(WID_TV_VIEWPORT);
			nvp->UpdateViewportCoordinates(this);

			ScrollWindowToTile(this->town->xy, this, true); // Re-center viewport.
		}
	}

	void OnMouseWheel(int wheel) override
	{
		if (_settings_client.gui.scrollwheel_scrolling != SWS_OFF) {
			DoZoomInOutWindow(wheel < 0 ? ZOOM_IN : ZOOM_OUT, this);
		}
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		/* Called when setting station noise or required cargoes have changed, in order to resize the window */
		this->SetDirty(); // refresh display for current size. This will allow to avoid glitches when downgrading
		this->ResizeWindowAsNeeded();
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value()) return;

		if (IsNonAdminNetworkClient()) {
			Command<CMD_RENAME_TOWN_NON_ADMIN>::Post(STR_ERROR_CAN_T_RENAME_TOWN, this->window_number, *str);
		} else {
			Command<CMD_RENAME_TOWN>::Post(STR_ERROR_CAN_T_RENAME_TOWN, this->window_number, *str);
		}
	}

	bool IsNewGRFInspectable() const override
	{
		return ::IsNewGRFInspectable(GSF_FAKE_TOWNS, this->window_number);
	}

	void ShowNewGRFInspectWindow() const override
	{
		::ShowNewGRFInspectWindow(GSF_FAKE_TOWNS, this->window_number);
	}
};

static constexpr NWidgetPart _nested_town_game_view_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_BROWN),
		NWidget(WWT_PUSHIMGBTN, COLOUR_BROWN, WID_TV_CHANGE_NAME), SetAspect(WidgetDimensions::ASPECT_RENAME), SetSpriteTip(SPR_RENAME, STR_TOWN_VIEW_RENAME_TOOLTIP),
		NWidget(WWT_CAPTION, COLOUR_BROWN, WID_TV_CAPTION), SetStringTip(STR_TOWN_VIEW_TOWN_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_PUSHIMGBTN, COLOUR_BROWN, WID_TV_CENTER_VIEW), SetAspect(WidgetDimensions::ASPECT_LOCATION), SetSpriteTip(SPR_GOTO_LOCATION, STR_TOWN_VIEW_CENTER_TOOLTIP),
		NWidget(WWT_DEBUGBOX, COLOUR_BROWN),
		NWidget(WWT_SHADEBOX, COLOUR_BROWN),
		NWidget(WWT_DEFSIZEBOX, COLOUR_BROWN),
		NWidget(WWT_STICKYBOX, COLOUR_BROWN),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_BROWN),
		NWidget(WWT_INSET, COLOUR_BROWN), SetPadding(2, 2, 2, 2),
			NWidget(NWID_VIEWPORT, INVALID_COLOUR, WID_TV_VIEWPORT), SetMinimalSize(254, 86), SetFill(1, 0), SetResize(1, 1),
		EndContainer(),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_BROWN, WID_TV_INFO), SetMinimalSize(260, 32), SetResize(1, 0), SetFill(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, COLOUR_BROWN, WID_TV_SHOW_AUTHORITY), SetMinimalSize(80, 12), SetFill(1, 1), SetResize(1, 0), SetStringTip(STR_TOWN_VIEW_LOCAL_AUTHORITY_BUTTON, STR_TOWN_VIEW_LOCAL_AUTHORITY_TOOLTIP),
		NWidget(WWT_TEXTBTN, COLOUR_BROWN, WID_TV_CATCHMENT), SetMinimalSize(40, 12), SetFill(1, 1), SetResize(1, 0), SetStringTip(STR_BUTTON_CATCHMENT, STR_TOOLTIP_CATCHMENT),
		NWidget(WWT_RESIZEBOX, COLOUR_BROWN),
	EndContainer(),
};

static WindowDesc _town_game_view_desc(__FILE__, __LINE__,
	WDP_AUTO, "view_town", 260, TownViewWindow::WID_TV_HEIGHT_NORMAL,
	WC_TOWN_VIEW, WC_NONE,
	{},
	_nested_town_game_view_widgets
);

static constexpr NWidgetPart _nested_town_editor_view_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_BROWN),
		NWidget(WWT_PUSHIMGBTN, COLOUR_BROWN, WID_TV_CHANGE_NAME), SetAspect(WidgetDimensions::ASPECT_RENAME), SetSpriteTip(SPR_RENAME, STR_TOWN_VIEW_RENAME_TOOLTIP),
		NWidget(WWT_CAPTION, COLOUR_BROWN, WID_TV_CAPTION), SetStringTip(STR_TOWN_VIEW_TOWN_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_PUSHIMGBTN, COLOUR_BROWN, WID_TV_CENTER_VIEW), SetAspect(WidgetDimensions::ASPECT_LOCATION), SetSpriteTip(SPR_GOTO_LOCATION, STR_TOWN_VIEW_CENTER_TOOLTIP),
		NWidget(WWT_DEBUGBOX, COLOUR_BROWN),
		NWidget(WWT_SHADEBOX, COLOUR_BROWN),
		NWidget(WWT_DEFSIZEBOX, COLOUR_BROWN),
		NWidget(WWT_STICKYBOX, COLOUR_BROWN),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_BROWN),
		NWidget(WWT_INSET, COLOUR_BROWN), SetPadding(2, 2, 2, 2),
			NWidget(NWID_VIEWPORT, INVALID_COLOUR, WID_TV_VIEWPORT), SetMinimalSize(254, 86), SetFill(1, 1), SetResize(1, 1),
		EndContainer(),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_BROWN, WID_TV_INFO), SetMinimalSize(260, 32), SetResize(1, 0), SetFill(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_BROWN, WID_TV_EXPAND), SetFill(1, 1), SetResize(1, 0), SetStringTip(STR_TOWN_VIEW_EXPAND_BUTTON, STR_TOWN_VIEW_EXPAND_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, COLOUR_BROWN, WID_TV_EXPAND_BUILDINGS), SetFill(1, 1), SetResize(1, 0), SetStringTip(STR_TOWN_VIEW_EXPAND_BUILDINGS_BUTTON, STR_TOWN_VIEW_EXPAND_BUILDINGS_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, COLOUR_BROWN, WID_TV_EXPAND_ROADS), SetFill(1, 1), SetResize(1, 0), SetStringTip(STR_TOWN_VIEW_EXPAND_ROADS_BUTTON, STR_TOWN_VIEW_EXPAND_ROADS_TOOLTIP),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_BROWN, WID_TV_DELETE), SetFill(1, 1), SetResize(1, 0), SetStringTip(STR_TOWN_VIEW_DELETE_BUTTON, STR_TOWN_VIEW_DELETE_TOOLTIP),
		NWidget(WWT_TEXTBTN, COLOUR_BROWN, WID_TV_CATCHMENT), SetFill(1, 1), SetResize(1, 0), SetStringTip(STR_BUTTON_CATCHMENT, STR_TOOLTIP_CATCHMENT),
		NWidget(WWT_RESIZEBOX, COLOUR_BROWN),
	EndContainer(),
};

static WindowDesc _town_editor_view_desc(__FILE__, __LINE__,
	WDP_AUTO, "view_town_scen", 260, TownViewWindow::WID_TV_HEIGHT_NORMAL,
	WC_TOWN_VIEW, WC_NONE,
	{},
	_nested_town_editor_view_widgets
);

void ShowTownViewWindow(TownID town)
{
	if (_game_mode == GM_EDITOR) {
		AllocateWindowDescFront<TownViewWindow>(_town_editor_view_desc, town);
	} else {
		AllocateWindowDescFront<TownViewWindow>(_town_game_view_desc, town);
	}
}

static constexpr NWidgetPart _nested_town_directory_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_BROWN),
		NWidget(WWT_CAPTION, COLOUR_BROWN, WID_TD_CAPTION), SetStringTip(STR_TOWN_DIRECTORY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_BROWN),
		NWidget(WWT_DEFSIZEBOX, COLOUR_BROWN),
		NWidget(WWT_STICKYBOX, COLOUR_BROWN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_TEXTBTN, COLOUR_BROWN, WID_TD_SORT_ORDER), SetStringTip(STR_BUTTON_SORT_BY, STR_TOOLTIP_SORT_ORDER),
				NWidget(WWT_DROPDOWN, COLOUR_BROWN, WID_TD_SORT_CRITERIA), SetStringTip(STR_JUST_STRING, STR_TOOLTIP_SORT_CRITERIA),
				NWidget(WWT_EDITBOX, COLOUR_BROWN, WID_TD_FILTER), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_LIST_FILTER_OSKTITLE, STR_LIST_FILTER_TOOLTIP),
			EndContainer(),
			NWidget(WWT_PANEL, COLOUR_BROWN, WID_TD_LIST), SetToolTip(STR_TOWN_DIRECTORY_LIST_TOOLTIP),
							SetFill(1, 0), SetResize(1, 1), SetScrollbar(WID_TD_SCROLLBAR), EndContainer(),
			NWidget(WWT_PANEL, COLOUR_BROWN),
				NWidget(WWT_TEXT, INVALID_COLOUR, WID_TD_WORLD_POPULATION), SetPadding(2, 0, 2, 2), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TOWN_POPULATION),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_VSCROLLBAR, COLOUR_BROWN, WID_TD_SCROLLBAR),
			NWidget(WWT_RESIZEBOX, COLOUR_BROWN),
		EndContainer(),
	EndContainer(),
};

/** Enum referring to the Hotkeys in the town directory window */
enum TownDirectoryHotkeys : int32_t {
	TDHK_FOCUS_FILTER_BOX, ///< Focus the filter box
};

/** Town directory window class. */
struct TownDirectoryWindow : public Window {
private:
	/* Runtime saved values */
	static Listing last_sorting;

	/* Constants for sorting towns */
	static inline const StringID sorter_names[] = {
		STR_SORT_BY_NAME,
		STR_SORT_BY_POPULATION,
		STR_SORT_BY_RATING,
		STR_SORT_BY_GROWTH_SPEED,
	};
	static const std::initializer_list<GUITownList::SortFunction * const> sorter_funcs;

	enum class SorterTypes {
		Name,
		Population,
		Rating,
		GrowthSpeed,
	};

	StringFilter string_filter;             ///< Filter for towns
	QueryString townname_editbox;           ///< Filter editbox

	GUITownList towns{TownDirectoryWindow::last_sorting.order};

	Scrollbar *vscroll;

	void BuildSortTownList()
	{
		if (this->towns.NeedRebuild()) {
			this->towns.clear();
			this->towns.reserve(Town::GetNumItems());

			for (const Town *t : Town::Iterate()) {
				if (this->string_filter.IsEmpty()) {
					this->towns.push_back(t);
					continue;
				}
				this->string_filter.ResetState();
				this->string_filter.AddLine(t->GetCachedName());
				if (this->string_filter.GetState()) this->towns.push_back(t);
			}

			this->towns.RebuildDone();
			this->vscroll->SetCount(this->towns.size()); // Update scrollbar as well.
		}
		/* Always sort the towns. */
		this->towns.Sort();
		this->SetWidgetDirty(WID_TD_LIST); // Force repaint of the displayed towns.
	}

	/** Sort by town name */
	static bool TownNameSorter(const Town * const &a, const Town * const &b, const bool &)
	{
		return StrNaturalCompare(a->GetCachedName(), b->GetCachedName()) < 0; // Sort by name (natural sorting).
	}

	/** Sort by population (default descending, as big towns are of the most interest). */
	static bool TownPopulationSorter(const Town * const &a, const Town * const &b, const bool &order)
	{
		uint32_t a_population = a->cache.population;
		uint32_t b_population = b->cache.population;
		if (a_population == b_population) return TownDirectoryWindow::TownNameSorter(a, b, order);
		return a_population < b_population;
	}

	/** Sort by town rating */
	static bool TownRatingSorter(const Town * const &a, const Town * const &b, const bool &order)
	{
		bool before = !order; // Value to get 'a' before 'b'.

		/* Towns without rating are always after towns with rating. */
		if (a->have_ratings.Test(_local_company)) {
			if (b->have_ratings.Test(_local_company)) {
				int16_t a_rating = a->ratings[_local_company];
				int16_t b_rating = b->ratings[_local_company];
				if (a_rating == b_rating) return TownDirectoryWindow::TownNameSorter(a, b, order);
				return a_rating < b_rating;
			}
			return before;
		}
		if (b->have_ratings.Test(_local_company)) return !before;

		/* Sort unrated towns always on ascending town name. */
		if (before) return TownDirectoryWindow::TownNameSorter(a, b, order);
		return TownDirectoryWindow::TownNameSorter(b, a, order);
	}

	/** Sort by town growth speed/status */
	static bool TownGrowthSpeedSorter(const Town * const &a, const Town * const &b, const bool &order)
	{
		/* Group: 0 = Growth Disabled, 1 = Not Growing, 2 = Growing */
		auto GetGrowthGroup = [](const Town *t) -> int {
			if (t->IsTownGrowthDisabledByOverride()) return 0;
			return HasBit(t->flags, TOWN_IS_GROWING) ? 2 : 1;
		};

		int group_a = GetGrowthGroup(a);
		int group_b = GetGrowthGroup(b);

		if (group_a != group_b) return group_a < group_b;

		/* If growth group is equal, sort by town name. */
		return TownDirectoryWindow::TownNameSorter(a, b, order);
	}

	/**Get the string to display the town growth status. */
	static StringID GetTownGrowthStatusString(const Town *t)
	{
		if (t->IsTownGrowthDisabledByOverride()) return STR_TOWN_GROWTH_STATUS_GROWTH_DISABLED;
		return HasBit(t->flags, TOWN_IS_GROWING) ? STR_TOWN_GROWTH_STATUS_GROWING : STR_TOWN_GROWTH_STATUS_NOT_GROWING;
	}

	bool IsInvalidSortCritera() const
	{
		return !_settings_client.gui.show_town_growth_status && this->towns.SortType() == to_underlying(SorterTypes::GrowthSpeed);
	}

public:
	TownDirectoryWindow(WindowDesc &desc) : Window(desc), townname_editbox(MAX_LENGTH_TOWN_NAME_CHARS * MAX_CHAR_LENGTH, MAX_LENGTH_TOWN_NAME_CHARS)
	{
		this->CreateNestedTree();

		this->vscroll = this->GetScrollbar(WID_TD_SCROLLBAR);

		this->towns.SetListing(this->last_sorting);
		this->towns.SetSortFuncs(TownDirectoryWindow::sorter_funcs);
		if (this->IsInvalidSortCritera()) {
			this->towns.SetSortType(0);
			this->last_sorting = this->towns.GetListing();
		}
		this->towns.ForceRebuild();
		this->BuildSortTownList();

		this->FinishInitNested(0);

		this->querystrings[WID_TD_FILTER] = &this->townname_editbox;
		this->townname_editbox.cancel_button = QueryString::ACTION_CLEAR;
	}

	void SetStringParameters(WidgetID widget) const override
	{
		switch (widget) {
			case WID_TD_CAPTION:
				SetDParam(0, this->vscroll->GetCount());
				SetDParam(1, Town::GetNumItems());
				break;

			case WID_TD_WORLD_POPULATION:
				SetDParam(0, GetWorldPopulation());
				break;

			case WID_TD_SORT_CRITERIA:
				SetDParam(0, TownDirectoryWindow::sorter_names[this->towns.SortType()]);
				break;
		}
	}

	/**
	 * Get the string to draw the town name.
	 * @param t Town to draw.
	 * @return The string to use.
	 */
	static StringID GetTownString(const Town *t)
	{
		return t->larger_town ? STR_TOWN_DIRECTORY_CITY : STR_TOWN_DIRECTORY_TOWN;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_TD_SORT_ORDER:
				this->DrawSortButtonState(widget, this->towns.IsDescSortOrder() ? SBS_DOWN : SBS_UP);
				break;

			case WID_TD_LIST: {
				Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);
				if (this->towns.empty()) { // No towns available.
					DrawString(tr, STR_TOWN_DIRECTORY_NONE);
					break;
				}

				/* At least one town available. */
				bool rtl = _current_text_dir == TD_RTL;
				Dimension icon_size = GetSpriteSize(SPR_TOWN_RATING_GOOD);
				int icon_x = tr.WithWidth(icon_size.width, rtl).left;
				tr = tr.Indent(icon_size.width + WidgetDimensions::scaled.hsep_normal, rtl);

				auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->towns);
				for (auto it = first; it != last; ++it) {
					const Town *t = *it;
					assert(t->xy != INVALID_TILE);

					/* Draw rating icon. */
					if (_game_mode == GM_EDITOR || !t->have_ratings.Test(_local_company)) {
						DrawSprite(SPR_TOWN_RATING_NA, PAL_NONE, icon_x, tr.top + (this->resize.step_height - icon_size.height) / 2);
					} else {
						SpriteID icon = SPR_TOWN_RATING_APALLING;
						if (t->ratings[_local_company] > RATING_VERYPOOR) icon = SPR_TOWN_RATING_MEDIOCRE;
						if (t->ratings[_local_company] > RATING_GOOD)     icon = SPR_TOWN_RATING_GOOD;
						DrawSprite(icon, PAL_NONE, icon_x, tr.top + (this->resize.step_height - icon_size.height) / 2);
					}

					format_buffer buffer;
					AppendStringInPlace(buffer, GetTownString(t), t->index, t->cache.population);
					if (_settings_client.gui.show_town_growth_status) {
						AppendStringInPlaceWithArgs(buffer, GetTownGrowthStatusString(t), {});
					}

					DrawString(tr.left, tr.right, tr.top + (this->resize.step_height - GetCharacterHeight(FS_NORMAL)) / 2, (std::string_view)buffer);

					tr.top += this->resize.step_height;
				}
				break;
			}
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_TD_SORT_ORDER: {
				Dimension d = GetStringBoundingBox(this->GetWidget<NWidgetCore>(widget)->GetString());
				d.width += padding.width + Window::SortButtonWidth() * 2; // Doubled since the string is centred and it also looks better.
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}
			case WID_TD_SORT_CRITERIA: {
				Dimension d = GetStringListBoundingBox(TownDirectoryWindow::sorter_names);
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}
			case WID_TD_LIST: {
				Dimension d = GetStringBoundingBox(STR_TOWN_DIRECTORY_NONE);
				for (uint i = 0; i < this->towns.size(); i++) {
					const Town *t = this->towns[i];

					assert(t != nullptr);

					SetDParam(0, t->index);
					SetDParam(1, t->cache.population);
					SetDParamMaxDigits(1, 8);

					d = maxdim(d, GetStringBoundingBox(GetTownString(t)));
				}
				if (_settings_client.gui.show_town_growth_status) {
					Dimension suffix{};
					for (StringID str : { STR_TOWN_GROWTH_STATUS_GROWTH_DISABLED, STR_TOWN_GROWTH_STATUS_GROWING, STR_TOWN_GROWTH_STATUS_NOT_GROWING }) {
						suffix = maxdim(suffix, GetStringBoundingBox(str));
					}
					d.width += suffix.width;
					d.height = std::max(d.height, suffix.height);
				}
				Dimension icon_size = GetSpriteSize(SPR_TOWN_RATING_GOOD);
				d.width += icon_size.width + 2;
				d.height = std::max(d.height, icon_size.height);
				resize.height = d.height;
				d.height *= 5;
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}
			case WID_TD_WORLD_POPULATION: {
				SetDParamMaxDigits(0, 10);
				Dimension d = GetStringBoundingBox(STR_TOWN_POPULATION);
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_TD_SORT_ORDER: // Click on sort order button
				if (this->towns.SortType() != 2) { // A different sort than by rating.
					this->towns.ToggleSortOrder();
					this->last_sorting = this->towns.GetListing(); // Store new sorting order.
				} else {
					/* Some parts are always sorted ascending on name. */
					this->last_sorting.order = !this->last_sorting.order;
					this->towns.SetListing(this->last_sorting);
					this->towns.ForceResort();
					this->towns.Sort();
				}
				this->SetDirty();
				break;

			case WID_TD_SORT_CRITERIA: { // Click on sort criteria dropdown
				uint32_t hidden_mask = 0;
				if (!_settings_client.gui.show_town_growth_status) SetBit(hidden_mask, to_underlying(SorterTypes::GrowthSpeed));
				ShowDropDownMenu(this, TownDirectoryWindow::sorter_names, this->towns.SortType(), WID_TD_SORT_CRITERIA, 0, hidden_mask);
				break;
			}

			case WID_TD_LIST: { // Click on Town Matrix
				auto it = this->vscroll->GetScrolledItemFromWidget(this->towns, pt.y, this, WID_TD_LIST, WidgetDimensions::scaled.framerect.top);
				if (it == this->towns.end()) return; // click out of town bounds

				const Town *t = *it;
				assert(t != nullptr);
				if (_ctrl_pressed) {
					ShowExtraViewportWindow(t->xy);
				} else {
					ScrollMainWindowToTile(t->xy);
				}
				break;
			}
		}
	}

	void OnDropdownSelect(WidgetID widget, int index) override
	{
		if (widget != WID_TD_SORT_CRITERIA) return;

		if (this->towns.SortType() != index) {
			this->towns.SetSortType(index);
			this->last_sorting = this->towns.GetListing(); // Store new sorting order.
			this->BuildSortTownList();
		}
	}

	void OnPaint() override
	{
		if (this->towns.NeedRebuild()) this->BuildSortTownList();
		this->DrawWidgets();
	}

	void OnHundredthTick() override
	{
		this->BuildSortTownList();
		this->SetDirty();
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_TD_LIST, WidgetDimensions::scaled.framerect.Vertical());
	}

	void OnEditboxChanged(WidgetID wid) override
	{
		if (wid == WID_TD_FILTER) {
			this->string_filter.SetFilterTerm(this->townname_editbox.text.GetText());
			this->InvalidateData(TDIWD_FORCE_REBUILD);
		}
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		switch (data) {
			case TDIWD_FORCE_REBUILD:
				/* This needs to be done in command-scope to enforce rebuilding before resorting invalid data */
				this->towns.ForceRebuild();
				break;

			case TDIWD_POPULATION_CHANGE:
				if (this->towns.SortType() == 1) this->towns.ForceResort();
				break;

			case TDIWD_SHOW_GROWTH_CHANGE:
				if (this->IsInvalidSortCritera()) {
					this->towns.SetSortType(0);
					this->last_sorting = this->towns.GetListing();
					this->BuildSortTownList();
				}
				this->ReInit();
				break;

			default:
				this->towns.ForceResort();
		}
	}

	EventState OnHotkey(int hotkey) override
	{
		switch (hotkey) {
			case TDHK_FOCUS_FILTER_BOX:
				this->SetFocusedWidget(WID_TD_FILTER);
				SetFocusedWindow(this); // The user has asked to give focus to the text box, so make sure this window is focused.
				break;
			default:
				return ES_NOT_HANDLED;
		}
		return ES_HANDLED;
	}

	static HotkeyList hotkeys;
};

static Hotkey towndirectory_hotkeys[] = {
	Hotkey('F', "focus_filter_box", TDHK_FOCUS_FILTER_BOX),
};
HotkeyList TownDirectoryWindow::hotkeys("towndirectory", towndirectory_hotkeys);

Listing TownDirectoryWindow::last_sorting = {false, 0};

/** Available town directory sorting functions. */
const std::initializer_list<GUITownList::SortFunction * const> TownDirectoryWindow::sorter_funcs = {
	&TownNameSorter,
	&TownPopulationSorter,
	&TownRatingSorter,
	&TownGrowthSpeedSorter,
};

static WindowDesc _town_directory_desc(__FILE__, __LINE__,
	WDP_AUTO, "list_towns", 208, 202,
	WC_TOWN_DIRECTORY, WC_NONE,
	{},
	_nested_town_directory_widgets,
	&TownDirectoryWindow::hotkeys
);

void ShowTownDirectory()
{
	if (BringWindowToFrontById(WC_TOWN_DIRECTORY, 0)) return;
	new TownDirectoryWindow(_town_directory_desc);
}

void CcFoundTown(const CommandCost &result, TileIndex tile)
{
	if (result.Failed()) return;

	if (_settings_client.sound.confirm) SndPlayTileFx(SND_1F_CONSTRUCTION_OTHER, tile);
	if (!_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();
}

void CcFoundRandomTown(const CommandCost &result)
{
	if (result.Succeeded() && result.HasResultData()) ScrollMainWindowToTile(Town::Get(result.GetResultData())->xy);
}

static constexpr NWidgetPart _nested_found_town_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN), SetStringTip(STR_FOUND_TOWN_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_STICKYBOX, COLOUR_DARK_GREEN),
	EndContainer(),
	/* Construct new town(s) buttons. */
	NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_NEW_TOWN), SetStringTip(STR_FOUND_TOWN_NEW_TOWN_BUTTON, STR_FOUND_TOWN_NEW_TOWN_TOOLTIP), SetFill(1, 0),
			NWidget(NWID_SELECTION, INVALID_COLOUR, WID_TF_TOWN_ACTION_SEL),
				NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
					NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_TF_RANDOM_TOWN), SetStringTip(STR_FOUND_TOWN_RANDOM_TOWN_BUTTON, STR_FOUND_TOWN_RANDOM_TOWN_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_TF_MANY_RANDOM_TOWNS), SetStringTip(STR_FOUND_TOWN_MANY_RANDOM_TOWNS, STR_FOUND_TOWN_RANDOM_TOWNS_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_TF_LOAD_FROM_FILE), SetStringTip(STR_FOUND_TOWN_LOAD_FROM_FILE, STR_FOUND_TOWN_LOAD_FROM_FILE_TOOLTIP), SetFill(1, 0),
				EndContainer(),
			EndContainer(),

			/* Town name selection. */
			NWidget(WWT_LABEL, INVALID_COLOUR), SetStringTip(STR_FOUND_TOWN_NAME_TITLE),
			NWidget(WWT_EDITBOX, COLOUR_GREY, WID_TF_TOWN_NAME_EDITBOX), SetStringTip(STR_FOUND_TOWN_NAME_EDITOR_TITLE, STR_FOUND_TOWN_NAME_EDITOR_TOOLTIP), SetFill(1, 0),
			NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_TF_TOWN_NAME_RANDOM), SetStringTip(STR_FOUND_TOWN_NAME_RANDOM_BUTTON, STR_FOUND_TOWN_NAME_RANDOM_TOOLTIP), SetFill(1, 0),

			/* Town size selection. */
			NWidget(WWT_LABEL, INVALID_COLOUR), SetStringTip(STR_FOUND_TOWN_INITIAL_SIZE_TITLE),
			NWidget(NWID_VERTICAL),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_SIZE_SMALL), SetStringTip(STR_FOUND_TOWN_INITIAL_SIZE_SMALL_BUTTON, STR_FOUND_TOWN_INITIAL_SIZE_TOOLTIP), SetFill(1, 0),
					NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_SIZE_MEDIUM), SetStringTip(STR_FOUND_TOWN_INITIAL_SIZE_MEDIUM_BUTTON, STR_FOUND_TOWN_INITIAL_SIZE_TOOLTIP), SetFill(1, 0),
				EndContainer(),
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(NWID_SELECTION, INVALID_COLOUR, WID_TF_SIZE_SEL),
						NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_SIZE_LARGE), SetStringTip(STR_FOUND_TOWN_INITIAL_SIZE_LARGE_BUTTON, STR_FOUND_TOWN_INITIAL_SIZE_TOOLTIP), SetFill(1, 0),
					EndContainer(),
					NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_SIZE_RANDOM), SetStringTip(STR_FOUND_TOWN_SIZE_RANDOM, STR_FOUND_TOWN_INITIAL_SIZE_TOOLTIP), SetFill(1, 0),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_CITY), SetStringTip(STR_FOUND_TOWN_CITY, STR_FOUND_TOWN_CITY_TOOLTIP), SetFill(1, 0),

			/* Town roads selection. */
			NWidget(NWID_SELECTION, INVALID_COLOUR, WID_TF_ROAD_LAYOUT_SEL),
				NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
					NWidget(WWT_LABEL, INVALID_COLOUR), SetStringTip(STR_FOUND_TOWN_ROAD_LAYOUT),
					NWidget(NWID_VERTICAL),
						NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_LAYOUT_ORIGINAL), SetStringTip(STR_FOUND_TOWN_SELECT_LAYOUT_ORIGINAL, STR_FOUND_TOWN_SELECT_LAYOUT_TOOLTIP), SetFill(1, 0),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_LAYOUT_BETTER), SetStringTip(STR_FOUND_TOWN_SELECT_LAYOUT_BETTER_ROADS, STR_FOUND_TOWN_SELECT_LAYOUT_TOOLTIP), SetFill(1, 0),
						EndContainer(),
						NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_LAYOUT_GRID2), SetStringTip(STR_FOUND_TOWN_SELECT_LAYOUT_2X2_GRID, STR_FOUND_TOWN_SELECT_LAYOUT_TOOLTIP), SetFill(1, 0),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_LAYOUT_GRID3), SetStringTip(STR_FOUND_TOWN_SELECT_LAYOUT_3X3_GRID, STR_FOUND_TOWN_SELECT_LAYOUT_TOOLTIP), SetFill(1, 0),
						EndContainer(),
						NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_LAYOUT_RANDOM), SetStringTip(STR_FOUND_TOWN_SELECT_LAYOUT_RANDOM, STR_FOUND_TOWN_SELECT_LAYOUT_TOOLTIP), SetFill(1, 0),
					EndContainer(),
				EndContainer(),
			EndContainer(),

			/* Town expansion selection. */
			NWidget(NWID_SELECTION, INVALID_COLOUR, WID_TF_TOWN_EXPAND_SEL),
				NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
					NWidget(WWT_LABEL, INVALID_COLOUR), SetStringTip(STR_FOUND_TOWN_EXPAND_MODE),
					NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_TF_EXPAND_ALL_TOWNS), SetStringTip(STR_FOUND_TOWN_EXPAND_ALL_TOWNS, STR_FOUND_TOWN_EXPAND_ALL_TOWNS_TOOLTIP), SetFill(1, 0),
					NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
						NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_EXPAND_BUILDINGS), SetStringTip(STR_FOUND_TOWN_EXPAND_BUILDINGS, STR_FOUND_TOWN_EXPAND_BUILDINGS_TOOLTIP), SetFill(1, 0),
						NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_TF_EXPAND_ROADS), SetStringTip(STR_FOUND_TOWN_EXPAND_ROADS, STR_FOUND_TOWN_EXPAND_ROADS_TOOLTIP), SetFill(1, 0),
					EndContainer(),
				EndContainer(),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

/** Found a town window class. */
struct FoundTownWindow : Window {
private:
	TownSize town_size;     ///< Selected town size
	TownLayout town_layout; ///< Selected town layout
	bool city;              ///< Are we building a city?
	QueryString townname_editbox; ///< Townname editbox
	bool townnamevalid = false; ///< Is generated town name valid?
	uint32_t townnameparts = 0; ///< Generated town name
	TownNameParams params; ///< Town name parameters
	static inline TownExpandModes expand_modes{TownExpandMode::Buildings, TownExpandMode::Roads};

public:
	FoundTownWindow(WindowDesc &desc, WindowNumber window_number) :
			Window(desc),
			town_size(TSZ_MEDIUM),
			town_layout(_settings_game.economy.town_layout),
			townname_editbox(MAX_LENGTH_TOWN_NAME_CHARS * MAX_CHAR_LENGTH, MAX_LENGTH_TOWN_NAME_CHARS),
			params(_settings_game.game_creation.town_name)
	{
		this->InitNested(window_number);
		this->querystrings[WID_TF_TOWN_NAME_EDITBOX] = &this->townname_editbox;
		this->RandomTownName();
		this->UpdateButtons(true);
	}

	void OnInit() override
	{
		if (_game_mode == GM_EDITOR) return;

		this->GetWidget<NWidgetStacked>(WID_TF_TOWN_ACTION_SEL)->SetDisplayedPlane(SZSP_HORIZONTAL);
		this->GetWidget<NWidgetStacked>(WID_TF_TOWN_EXPAND_SEL)->SetDisplayedPlane(SZSP_HORIZONTAL);
		this->GetWidget<NWidgetStacked>(WID_TF_SIZE_SEL)->SetDisplayedPlane(SZSP_VERTICAL);
		if (_settings_game.economy.found_town != TF_CUSTOM_LAYOUT) {
			this->GetWidget<NWidgetStacked>(WID_TF_ROAD_LAYOUT_SEL)->SetDisplayedPlane(SZSP_HORIZONTAL);
		} else {
			this->GetWidget<NWidgetStacked>(WID_TF_ROAD_LAYOUT_SEL)->SetDisplayedPlane(0);
		}
	}

	void RandomTownName()
	{
		this->townnamevalid = GenerateTownName(_interactive_random, &this->townnameparts);

		if (!this->townnamevalid) {
			this->townname_editbox.text.DeleteAll();
		} else {
			this->townname_editbox.text.Assign(GetTownName(&this->params, this->townnameparts));
		}
		UpdateOSKOriginalText(this, WID_TF_TOWN_NAME_EDITBOX);

		this->SetWidgetDirty(WID_TF_TOWN_NAME_EDITBOX);
	}

	void UpdateButtons(bool check_availability)
	{
		if (check_availability && _game_mode != GM_EDITOR) {
			if (_settings_game.economy.found_town != TF_CUSTOM_LAYOUT) this->town_layout = _settings_game.economy.town_layout;
			this->ReInit();
		}

		for (WidgetID i = WID_TF_SIZE_SMALL; i <= WID_TF_SIZE_RANDOM; i++) {
			this->SetWidgetLoweredState(i, i == WID_TF_SIZE_SMALL + this->town_size);
		}

		this->SetWidgetLoweredState(WID_TF_CITY, this->city);

		for (WidgetID i = WID_TF_LAYOUT_ORIGINAL; i <= WID_TF_LAYOUT_RANDOM; i++) {
			this->SetWidgetLoweredState(i, i == WID_TF_LAYOUT_ORIGINAL + this->town_layout);
		}

		this->SetWidgetLoweredState(WID_TF_EXPAND_BUILDINGS, FoundTownWindow::expand_modes.Test(TownExpandMode::Buildings));
		this->SetWidgetLoweredState(WID_TF_EXPAND_ROADS, FoundTownWindow::expand_modes.Test(TownExpandMode::Roads));

		this->SetDirty();
	}

	void ExecuteFoundTownCommand(TileIndex tile, bool random, StringID errstr, CommandCallback cc)
	{
		std::string name;

		if (!this->townnamevalid) {
			name = this->townname_editbox.text.GetText();
		} else {
			/* If user changed the name, send it */
			std::string original_name = GetTownName(&this->params, this->townnameparts);
			if (original_name != this->townname_editbox.text.GetText()) name = this->townname_editbox.text.GetText();
		}

		bool success = Command<CMD_FOUND_TOWN>::Post(errstr, cc,
				tile, this->town_size, this->city, this->town_layout, random, townnameparts, std::move(name));

		/* Rerandomise name, if success and no cost-estimation. */
		if (success && !_shift_pressed) this->RandomTownName();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_TF_NEW_TOWN:
				HandlePlacePushButton(this, WID_TF_NEW_TOWN, SPR_CURSOR_TOWN, HT_RECT);
				break;

			case WID_TF_RANDOM_TOWN:
				this->ExecuteFoundTownCommand({}, true, STR_ERROR_CAN_T_GENERATE_TOWN, CommandCallback::FoundRandomTown);
				break;

			case WID_TF_TOWN_NAME_RANDOM:
				this->RandomTownName();
				this->SetFocusedWidget(WID_TF_TOWN_NAME_EDITBOX);
				break;

			case WID_TF_MANY_RANDOM_TOWNS: {
				std::string default_town_number = fmt::format("{}", GetDefaultTownsForMapSize());
				ShowQueryString(default_town_number, STR_MAPGEN_NUMBER_OF_TOWNS, 5, this, CS_NUMERAL, QSF_ACCEPT_UNCHANGED);
				break;
			}
			case WID_TF_LOAD_FROM_FILE:
				ShowSaveLoadDialog(FT_TOWN_DATA, SLO_LOAD);
				break;

			case WID_TF_EXPAND_ALL_TOWNS:
				for (Town *t : Town::Iterate()) {
					Command<CMD_EXPAND_TOWN>::Do(DC_EXEC, t->index, 0, FoundTownWindow::expand_modes);
				}
				break;

			case WID_TF_SIZE_SMALL: case WID_TF_SIZE_MEDIUM: case WID_TF_SIZE_LARGE: case WID_TF_SIZE_RANDOM:
				this->town_size = (TownSize)(widget - WID_TF_SIZE_SMALL);
				this->UpdateButtons(false);
				break;

			case WID_TF_CITY:
				this->city ^= true;
				this->SetWidgetLoweredState(WID_TF_CITY, this->city);
				this->SetDirty();
				break;

			case WID_TF_EXPAND_BUILDINGS:
				FoundTownWindow::expand_modes.Flip(TownExpandMode::Buildings);
				this->UpdateButtons(false);
				break;

			case WID_TF_EXPAND_ROADS:
				FoundTownWindow::expand_modes.Flip(TownExpandMode::Roads);
				this->UpdateButtons(false);
				break;

			case WID_TF_LAYOUT_ORIGINAL: case WID_TF_LAYOUT_BETTER: case WID_TF_LAYOUT_GRID2:
			case WID_TF_LAYOUT_GRID3: case WID_TF_LAYOUT_RANDOM:
				this->town_layout = (TownLayout)(widget - WID_TF_LAYOUT_ORIGINAL);

				/* If we are in the editor, sync the settings of the current game to the chosen layout,
				 * so that importing towns from file uses the selected layout. */
				if (_game_mode == GM_EDITOR) _settings_game.economy.town_layout = this->town_layout;

				this->UpdateButtons(false);
				break;
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		/* Was 'cancel' pressed? */
		if (!str.has_value()) return;

		auto value = ParseInteger(*str);
		if (!value.has_value()) return;

		Backup<bool> old_generating_world(_generating_world, true, FILE_LINE);
		UpdateNearestTownForRoadTiles(true);
		if (!GenerateTowns(this->town_layout, value)) {
			ShowErrorMessage(STR_ERROR_CAN_T_GENERATE_TOWN, STR_ERROR_NO_SPACE_FOR_TOWN, WL_INFO);
		}
		UpdateNearestTownForRoadTiles(false);
		old_generating_world.Restore();
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		this->ExecuteFoundTownCommand(tile, false, STR_ERROR_CAN_T_FOUND_TOWN_HERE, CommandCallback::FoundTown);
	}

	void OnPlaceObjectAbort() override
	{
		this->RaiseButtons();
		this->UpdateButtons(false);
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		this->UpdateButtons(true);
	}
};

static WindowDesc _found_town_desc(__FILE__, __LINE__,
	WDP_AUTO, "build_town", 160, 162,
	WC_FOUND_TOWN, WC_NONE,
	WindowDefaultFlag::Construction,
	_nested_found_town_widgets
);

void ShowFoundTownWindow()
{
	if (_game_mode != GM_EDITOR && !Company::IsValidID(_local_company)) return;
	AllocateWindowDescFront<FoundTownWindow>(_found_town_desc, 0);
}

/**
 * Window for selecting towns to build a house in.
 */
struct SelectTownWindow : Window {
	TownList towns;       ///< list of towns
	CommandContainer<CMD_PLACE_HOUSE> cmd; ///< command to build the house
	Scrollbar *vscroll;   ///< scrollbar for the town list

	SelectTownWindow(WindowDesc &desc, const CommandContainer<CMD_PLACE_HOUSE> &cmd) : Window(desc), cmd(cmd)
	{
		std::vector<std::pair<uint, TownID>> town_set;
		constexpr uint MAX_TOWN_COUNT = 16;
		for (const Town *t : Town::Iterate()) {
			uint dist_sq = DistanceSquare(cmd.tile, t->xy);
			if (town_set.size() >= MAX_TOWN_COUNT && dist_sq >= town_set.front().first) {
				/* We already have enough entries and this town is further away than the furthest existing one, don't bother adding it */
				continue;
			}

			/* Add to heap */
			town_set.emplace_back(dist_sq, t->index);
			std::push_heap(town_set.begin(), town_set.end());

			if (town_set.size() > MAX_TOWN_COUNT) {
				/* Remove largest from heap */
				std::pop_heap(town_set.begin(), town_set.end());
				town_set.pop_back();
			}
		}
		std::sort_heap(town_set.begin(), town_set.end());
		for (auto &it : town_set) {
			this->towns.push_back(it.second);
		}

		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_ST_SCROLLBAR);
		this->vscroll->SetCount((uint)this->towns.size());
		this->FinishInitNested();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, Dimension &fill, Dimension &resize) override
	{
		if (widget != WID_ST_PANEL) return;

		/* Determine the widest string */
		Dimension d = { 0, 0 };
		for (uint i = 0; i < this->towns.size(); i++) {
			SetDParam(0, this->towns[i]);
			d = maxdim(d, GetStringBoundingBox(STR_SELECT_TOWN_LIST_ITEM));
		}

		resize.height = d.height;
		d.height *= 5;
		d.width += WidgetDimensions::scaled.framerect.Horizontal();
		d.height += WidgetDimensions::scaled.framerect.Vertical();
		size = d;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_ST_PANEL) return;

		Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
		uint y = ir.top;
		uint end = std::min<uint>(this->vscroll->GetCount(), this->vscroll->GetPosition() + this->vscroll->GetCapacity());
		for (uint i = this->vscroll->GetPosition(); i < end; i++) {
			SetDParam(0, this->towns[i]);
			DrawString(ir.left, ir.right, y, STR_SELECT_TOWN_LIST_ITEM);
			y += this->resize.step_height;
		}
	}

	void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		if (widget != WID_ST_PANEL) return;

		uint pos = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_ST_PANEL, WidgetDimensions::scaled.framerect.top);
		if (pos >= this->towns.size()) return;

		/* Place a house */
		TownID &town_id = std::get<2>(this->cmd.payload.GetValues());
		town_id = this->towns[pos];
		DoCommandPContainer(this->cmd);

		/* Close the window */
		this->Close();
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_ST_PANEL, WidgetDimensions::scaled.framerect.Vertical());
	}
};

static const NWidgetPart _nested_select_town_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN, WID_ST_CAPTION), SetStringTip(STR_SELECT_TOWN_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_DEFSIZEBOX, COLOUR_DARK_GREEN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_DARK_GREEN, WID_ST_PANEL), SetResize(1, 0), SetScrollbar(WID_ST_SCROLLBAR), EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_VSCROLLBAR, COLOUR_DARK_GREEN, WID_ST_SCROLLBAR),
			NWidget(WWT_RESIZEBOX, COLOUR_DARK_GREEN),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _select_town_desc(__FILE__, __LINE__,
	WDP_AUTO, "select_town", 100, 0,
	WC_SELECT_TOWN, WC_NONE,
	WindowDefaultFlag::Construction,
	_nested_select_town_widgets
);

static void ShowSelectTownWindow(const CommandContainer<CMD_PLACE_HOUSE> &cmd)
{
	CloseWindowByClass(WC_SELECT_TOWN);
	new SelectTownWindow(_select_town_desc, cmd);
}

void InitializeTownGui()
{
	_town_local_authority_kdtree.Clear();
}

/**
 * Draw representation of a house tile for GUI purposes.
 * @param x Position x of image.
 * @param y Position y of image.
 * @param spec House spec to draw.
 * @param house_id House ID to draw.
 * @param view The house's 'view'.
 */
void DrawNewHouseTileInGUI(int x, int y, const HouseSpec *spec, HouseID house_id, int view)
{
	HouseResolverObject object(house_id, INVALID_TILE, nullptr, CBID_NO_CALLBACK, 0, 0, true, view);
	const SpriteGroup *group = object.Resolve();
	if (group == nullptr || group->type != SGT_TILELAYOUT) return;

	uint8_t stage = TOWN_HOUSE_COMPLETED;
	const DrawTileSprites *dts = reinterpret_cast<const TileLayoutSpriteGroup *>(group)->ProcessRegisters(&stage);

	PaletteID palette = GENERAL_SPRITE_COLOUR(spec->random_colour[0]);
	if (spec->callback_mask.Test(HouseCallbackMask::Colour)) {
		uint16_t callback = GetHouseCallback(CBID_HOUSE_COLOUR, 0, 0, house_id, nullptr, INVALID_TILE, true, view);
		if (callback != CALLBACK_FAILED) {
			/* If bit 14 is set, we should use a 2cc colour map, else use the callback value. */
			palette = HasBit(callback, 14) ? GB(callback, 0, 8) + SPR_2CCMAP_BASE : callback;
		}
	}

	SpriteID image = dts->ground.sprite;
	PaletteID pal  = dts->ground.pal;

	if (HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE)) image += stage;
	if (HasBit(pal, SPRITE_MODIFIER_CUSTOM_SPRITE)) pal += stage;

	if (GB(image, 0, SPRITE_WIDTH) != 0) {
		DrawSprite(image, GroundSpritePaletteTransform(image, pal, palette), x, y);
	}

	DrawNewGRFTileSeqInGUI(x, y, dts, stage, palette);
}

/**
 * Draw a house that does not exist.
 * @param x Position x of image.
 * @param y Position y of image.
 * @param house_id House ID to draw.
 * @param view The house's 'view'.
 */
void DrawHouseInGUI(int x, int y, HouseID house_id, int view)
{
	auto draw = [](int x, int y, HouseID house_id, int view) {
		if (house_id >= NEW_HOUSE_OFFSET) {
			/* Houses don't necessarily need new graphics. If they don't have a
			 * spritegroup associated with them, then the sprite for the substitute
			 * house id is drawn instead. */
			const HouseSpec *spec = HouseSpec::Get(house_id);
			if (spec->grf_prop.GetSpriteGroup() != nullptr) {
				DrawNewHouseTileInGUI(x, y, spec, house_id, view);
				return;
			} else {
				house_id = HouseSpec::Get(house_id)->grf_prop.subst_id;
			}
		}

		/* Retrieve data from the draw town tile struct */
		const DrawBuildingsTileStruct &dcts = GetTownDrawTileData()[house_id << 4 | view << 2 | TOWN_HOUSE_COMPLETED];
		DrawSprite(dcts.ground.sprite, dcts.ground.pal, x, y);

		/* Add a house on top of the ground? */
		if (dcts.building.sprite != 0) {
			Point pt = RemapCoords(dcts.subtile_x, dcts.subtile_y, 0);
			DrawSprite(dcts.building.sprite, dcts.building.pal, x + ScaleSpriteTrad(pt.x), y + ScaleSpriteTrad(pt.y));
		}
	};

	/* Houses can have 1x1, 1x2, 2x1 and 2x2 layouts which are individual HouseIDs. For the GUI we need
	 * draw all of the tiles with appropriate positions. */
	int x_delta = ScaleSpriteTrad(TILE_PIXELS);
	int y_delta = ScaleSpriteTrad(TILE_PIXELS / 2);

	const HouseSpec *hs = HouseSpec::Get(house_id);
	if (hs->building_flags.Test(BuildingFlag::Size2x2)) {
		draw(x, y - y_delta - y_delta, house_id, view); // North corner.
		draw(x + x_delta, y - y_delta, house_id + 1, view); // West corner.
		draw(x - x_delta, y - y_delta, house_id + 2, view); // East corner.
		draw(x, y, house_id + 3, view); // South corner.
	} else if (hs->building_flags.Test(BuildingFlag::Size2x1)) {
		draw(x + x_delta / 2, y - y_delta, house_id, view); // North east tile.
		draw(x - x_delta / 2, y, house_id + 1, view); // South west tile.
	} else if (hs->building_flags.Test(BuildingFlag::Size1x2)) {
		draw(x - x_delta / 2, y - y_delta, house_id, view); // North west tile.
		draw(x + x_delta / 2, y, house_id + 1, view); // South east tile.
	} else {
		draw(x, y, house_id, view);
	}
}

/**
 * Get name for a prototype house.
 * @param hs HouseSpec of house.
 * @return StringID of name for house.
 */
static StringID GetHouseName(const HouseSpec *hs)
{
	uint16_t callback_res = GetHouseCallback(CBID_HOUSE_CUSTOM_NAME, 1, 0, hs->Index(), nullptr, INVALID_TILE, true);
	if (callback_res != CALLBACK_FAILED && callback_res != 0x400) {
		if (callback_res > 0x400) {
			ErrorUnknownCallbackResult(hs->grf_prop.grfid, CBID_HOUSE_CUSTOM_NAME, callback_res);
		} else {
			StringID new_name = GetGRFStringID(hs->grf_prop.grffile->grfid, GRFSTR_MISC_GRF_TEXT + callback_res);
			if (new_name != STR_NULL && new_name != STR_UNDEFINED) {
				return new_name;
			}
		}
	}

	return hs->building_name;
}

class HousePickerCallbacks : public PickerCallbacks {
public:
	HousePickerCallbacks() : PickerCallbacks("fav_houses") {}

	/**
	 * Set climate mask for filtering buildings from current landscape.
	 */
	void SetClimateMask()
	{
		switch (_settings_game.game_creation.landscape) {
			case LandscapeType::Temperate: this->climate_mask = HZ_TEMP; break;
			case LandscapeType::Arctic:    this->climate_mask = HZ_SUBARTC_ABOVE | HZ_SUBARTC_BELOW; break;
			case LandscapeType::Tropic:    this->climate_mask = HZ_SUBTROPIC; break;
			case LandscapeType::Toyland:   this->climate_mask = HZ_TOYLND; break;
			default: NOT_REACHED();
		}

		/* In some cases, not all 'classes' (house zones) have distinct houses, so we need to disable those.
		 * As we need to check all types, and this cannot change with the picker window open, pre-calculate it.
		 * This loop calls GetTypeName() instead of directly checking properties so that there is no discrepancy. */
		this->class_mask = 0;

		int num_classes = this->GetClassCount();
		for (int cls_id = 0; cls_id < num_classes; ++cls_id) {
			int num_types = this->GetTypeCount(cls_id);
			for (int id = 0; id < num_types; ++id) {
				if (this->GetTypeName(cls_id, id) != INVALID_STRING_ID) {
					SetBit(this->class_mask, cls_id);
					break;
				}
			}
		}
	}

	HouseZones climate_mask;
	uint8_t class_mask; ///< Mask of available 'classes'.

	static inline int sel_class; ///< Currently selected 'class'.
	static inline int sel_type; ///< Currently selected HouseID.
	static inline int sel_view; ///< Currently selected 'view'. This is not controllable as its based on random data.

	/* Houses do not have classes like NewGRFClass. We'll make up fake classes based on town zone
	 * availability instead. */
	static inline const std::array<StringID, HZB_END> zone_names = {
		STR_HOUSE_PICKER_CLASS_ZONE1,
		STR_HOUSE_PICKER_CLASS_ZONE2,
		STR_HOUSE_PICKER_CLASS_ZONE3,
		STR_HOUSE_PICKER_CLASS_ZONE4,
		STR_HOUSE_PICKER_CLASS_ZONE5,
	};

	GrfSpecFeature GetFeature() const override { return GSF_HOUSES; }

	StringID GetClassTooltip() const override { return STR_PICKER_HOUSE_CLASS_TOOLTIP; }
	StringID GetTypeTooltip() const override { return STR_PICKER_HOUSE_TYPE_TOOLTIP; }
	bool IsActive() const override { return true; }

	bool HasClassChoice() const override { return true; }
	int GetClassCount() const override { return static_cast<int>(zone_names.size()); }

	void Close([[maybe_unused]] int data) override { ResetObjectToPlace(); }

	int GetSelectedClass() const override { return HousePickerCallbacks::sel_class; }
	void SetSelectedClass(int cls_id) const override { HousePickerCallbacks::sel_class = cls_id; }

	StringID GetClassName(int id) const override
	{
		if (id >= GetClassCount()) return INVALID_STRING_ID;
		if (!HasBit(this->class_mask, id)) return INVALID_STRING_ID;
		return zone_names[id];
	}

	int GetTypeCount(int cls_id) const override
	{
		if (cls_id < GetClassCount()) return static_cast<int>(HouseSpec::Specs().size());
		return 0;
	}

	PickerItem GetPickerItem(int cls_id, int id) const override
	{
		const auto *spec = HouseSpec::Get(id);
		if (!spec->grf_prop.HasGrfFile()) return {0, spec->Index(), cls_id, id};
		return {spec->grf_prop.grfid, spec->grf_prop.local_id, cls_id, id};
	}

	int GetSelectedType() const override { return sel_type; }
	void SetSelectedType(int id) const override { sel_type = id; }

	StringID GetTypeName(int cls_id, int id) const override
	{
		const HouseSpec *spec = HouseSpec::Get(id);
		if (spec == nullptr) return INVALID_STRING_ID;
		if (!spec->enabled) return INVALID_STRING_ID;
		if ((spec->building_availability & climate_mask) == 0) return INVALID_STRING_ID;
		if (!HasBit(spec->building_availability, cls_id)) return INVALID_STRING_ID;
		for (int i = 0; i < cls_id; i++) {
			/* Don't include if it's already included in an earlier zone. */
			if (HasBit(spec->building_availability, i)) return INVALID_STRING_ID;
		}

		return GetHouseName(spec);
	}

	std::span<const BadgeID> GetTypeBadges(int cls_id, int id) const override
	{
		const auto *spec = HouseSpec::Get(id);
		if (spec == nullptr) return {};
		if (!spec->enabled) return {};
		if ((spec->building_availability & climate_mask) == 0) return {};
		if (!HasBit(spec->building_availability, cls_id)) return {};
		for (int i = 0; i < cls_id; i++) {
			/* Don't include if it's already included in an earlier zone. */
			if (HasBit(spec->building_availability, i)) return {};
		}

		return spec->badges;
	}

	bool IsTypeAvailable(int, int id) const override
	{
		const HouseSpec *hs = HouseSpec::Get(id);
		return hs->enabled;
	}

	void DrawType(int x, int y, int, int id) const override
	{
		DrawHouseInGUI(x, y, id, HousePickerCallbacks::sel_view);
	}

	void FillUsedItems(btree::btree_set<PickerItem> &items) override
	{
		auto id_count = GetBuildingHouseIDCounts();
		for (auto it = id_count.begin(); it != id_count.end(); ++it) {
			if (*it == 0) continue;
			HouseID house = static_cast<HouseID>(std::distance(id_count.begin(), it));
			const HouseSpec *hs = HouseSpec::Get(house);
			int class_index = FindFirstBit(hs->building_availability & HZ_ZONALL);
			items.insert({0, house, class_index, house});
		}
	}

	btree::btree_set<PickerItem> UpdateSavedItems(const btree::btree_set<PickerItem> &src) override
	{
		if (src.empty()) return src;

		const auto specs = HouseSpec::Specs();
		btree::btree_set<PickerItem> dst;
		for (const auto &item : src) {
			if (item.grfid == 0) {
				dst.insert(item);
			} else {
				/* Search for spec by grfid and local index. */
				auto it = std::ranges::find_if(specs, [&item](const HouseSpec &spec) { return spec.grf_prop.grfid == item.grfid && spec.grf_prop.local_id == item.local_id; });
				if (it == specs.end()) {
					/* Not preset, hide from UI. */
					dst.insert({item.grfid, item.local_id, -1, -1});
				} else {
					int class_index = FindFirstBit(it->building_availability & HZ_ZONALL);
					dst.insert( {item.grfid, item.local_id, class_index, it->Index()});
				}
			}
		}

		return dst;
	}

	static HousePickerCallbacks instance;
};
/* static */ HousePickerCallbacks HousePickerCallbacks::instance;

/**
 * Get the cargo types produced by a house.
 * @param hs HouseSpec of the house.
 * @returns CargoArray of cargo types produced by the house.
 */
static CargoArray GetProducedCargoOfHouse(const HouseSpec *hs)
{
	/* We don't care how much cargo is produced, but BuildCargoAcceptanceString shows fractions when less then 8. */
	static const uint MIN_CARGO = 8;

	CargoArray production{};
	if (hs->callback_mask.Test(HouseCallbackMask::ProduceCargo)) {
		for (uint i = 0; i < 256; i++) {
			uint16_t callback = GetHouseCallback(CBID_HOUSE_PRODUCE_CARGO, i, 0, hs->Index(), nullptr, INVALID_TILE, true);

			if (callback == CALLBACK_FAILED || callback == CALLBACK_HOUSEPRODCARGO_END) break;

			CargoType cargo = GetCargoTranslation(GB(callback, 8, 7), hs->grf_prop.grffile);
			if (!IsValidCargoType(cargo)) continue;

			uint amt = GB(callback, 0, 8);
			if (amt == 0) continue;

			production[cargo] = MIN_CARGO;
		}
	} else {
		/* Cargo is not controlled by NewGRF, town production effect is used instead. */
		for (CargoType cid : CargoSpec::town_production_cargoes[TPE_PASSENGERS]) production[cid] = MIN_CARGO;
		for (CargoType cid : CargoSpec::town_production_cargoes[TPE_MAIL]) production[cid] = MIN_CARGO;
	}
	return production;
}

struct BuildHouseWindow : public PickerWindow {
	std::string house_info;
	bool house_protected;

	BuildHouseWindow(WindowDesc &desc, WindowNumber wno, Window *parent) : PickerWindow(desc, parent, wno, HousePickerCallbacks::instance)
	{
		HousePickerCallbacks::instance.SetClimateMask();
		this->ConstructWindow();
		this->InvalidateData();
	}

	void UpdateSelectSize(const HouseSpec *spec)
	{
		if (spec == nullptr) {
			SetTileSelectSize(1, 1);
			ResetObjectToPlace();
		} else {
			SetObjectToPlaceWnd(SPR_CURSOR_TOWN, PAL_NONE, HT_RECT | HT_DIAGONAL, this);
			if (spec->building_flags.Test(BuildingFlag::Size2x2)) {
				SetTileSelectSize(2, 2);
			} else if (spec->building_flags.Test(BuildingFlag::Size2x1)) {
				SetTileSelectSize(2, 1);
			} else if (spec->building_flags.Test(BuildingFlag::Size1x2)) {
				SetTileSelectSize(1, 2);
			} else if (spec->building_flags.Test(BuildingFlag::Size1x1)) {
				SetTileSelectSize(1, 1);
			}
		}
	}

	/**
	 * Get a date range string for house availability year.
	 * @param buffer Target to write formatted string with the date range formatted appropriately.
	 * @param min_year Earliest year house can be built.
	 * @param max_year Latest year house can be built.
	 */
	static void GetHouseYear(format_buffer &buffer, CalTime::Year min_year, CalTime::Year max_year)
	{
		if (min_year == CalTime::MIN_YEAR) {
			if (max_year == CalTime::MAX_YEAR) {
				AppendStringInPlace(buffer, STR_HOUSE_PICKER_YEARS_ANY);
				return;
			}
			SetDParam(0, max_year);
			AppendStringInPlace(buffer, STR_HOUSE_PICKER_YEARS_UNTIL);
			return;
		}
		if (max_year == CalTime::MAX_YEAR) {
			SetDParam(0, min_year);
			AppendStringInPlace(buffer, STR_HOUSE_PICKER_YEARS_FROM);
			return;
		}
		SetDParam(0, min_year);
		SetDParam(1, max_year);
		AppendStringInPlace(buffer, STR_HOUSE_PICKER_YEARS);
		return;
	}

	/**
	 * Get information string for a house.
	 * @param hs HouseSpec to get information string for.
	 * @return Formatted string with information for house.
	 */
	static std::string GetHouseInformation(const HouseSpec *hs)
	{
		format_buffer line;

		SetDParam(0, GetHouseName(hs));
		AppendStringInPlace(line, STR_HOUSE_PICKER_NAME);
		line.push_back('\n');

		SetDParam(0, hs->population);
		AppendStringInPlace(line, STR_HOUSE_PICKER_POPULATION);
		line.push_back('\n');

		GetHouseYear(line, hs->min_year, hs->max_year);
		line.push_back('\n');

		uint8_t size = 0;
		if (hs->building_flags.Test(BuildingFlag::Size1x1)) size = 0x11;
		if (hs->building_flags.Test(BuildingFlag::Size2x1)) size = 0x21;
		if (hs->building_flags.Test(BuildingFlag::Size1x2)) size = 0x12;
		if (hs->building_flags.Test(BuildingFlag::Size2x2)) size = 0x22;
		SetDParam(0, GB(size, 0, 4));
		SetDParam(1, GB(size, 4, 4));
		AppendStringInPlace(line, STR_HOUSE_PICKER_SIZE);

		auto cargo_string = BuildCargoAcceptanceString(GetAcceptedCargoOfHouse(hs), STR_HOUSE_PICKER_CARGO_ACCEPTED);
		if (cargo_string.has_value()) {
			line.push_back('\n');
			line.append(*cargo_string);
		}

		cargo_string = BuildCargoAcceptanceString(GetProducedCargoOfHouse(hs), STR_HOUSE_PICKER_CARGO_PRODUCED);
		if (cargo_string.has_value()) {
			line.push_back('\n');
			line.append(*cargo_string);
		}

		return line.to_string();
	}

	void OnInit() override
	{
		this->InvalidateData(PICKER_INVALIDATION_ALL);
		this->PickerWindow::OnInit();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_BH_INFO) {
			if (!this->house_info.empty()) DrawStringMultiLine(r, this->house_info);
		} else {
			this->PickerWindow::DrawWidget(r, widget);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BH_PROTECT_OFF:
			case WID_BH_PROTECT_ON:
				this->house_protected = (widget == WID_BH_PROTECT_ON);
				this->SetWidgetLoweredState(WID_BH_PROTECT_OFF, !this->house_protected);
				this->SetWidgetLoweredState(WID_BH_PROTECT_ON, this->house_protected);

				if (_settings_client.sound.click_beep) SndPlayFx(SND_15_BEEP);
				this->SetDirty();
				break;

			default:
				this->PickerWindow::OnClick(pt, widget, click_count);
				break;
		}
	}

	void OnInvalidateData(int data = 0, bool gui_scope = true) override
	{
		this->PickerWindow::OnInvalidateData(data, gui_scope);
		if (!gui_scope) return;

		const HouseSpec *spec = HouseSpec::Get(HousePickerCallbacks::sel_type);

		if ((data & PickerWindow::PFI_POSITION) != 0) {
			UpdateSelectSize(spec);
			this->house_info = GetHouseInformation(spec);
		}

		/* If house spec already has the protected flag, handle it automatically and disable the buttons. */
		bool hasflag = spec->extra_flags.Test(HouseExtraFlag::BuildingIsProtected);
		if (hasflag) this->house_protected = true;

		this->SetWidgetLoweredState(WID_BH_PROTECT_OFF, !this->house_protected);
		this->SetWidgetLoweredState(WID_BH_PROTECT_ON, this->house_protected);

		this->SetWidgetDisabledState(WID_BH_PROTECT_OFF, hasflag);
		this->SetWidgetDisabledState(WID_BH_PROTECT_ON, hasflag);
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		const HouseSpec *spec = HouseSpec::Get(HousePickerCallbacks::sel_type);
		CommandContainer<CMD_PLACE_HOUSE> cmd_container(STR_ERROR_CAN_T_BUILD_HOUSE, tile,
				CmdPayload<CMD_PLACE_HOUSE>::Make(spec->Index(), this->house_protected, INVALID_TOWN), CommandCallback::PlaySound_CONSTRUCTION_OTHER);
		if (_ctrl_pressed) {
			ShowSelectTownWindow(cmd_container);
		} else {
			DoCommandPContainer(cmd_container);
		}
	}

	IntervalTimer<TimerWindow> view_refresh_interval = {std::chrono::milliseconds(2500), [this](auto) {
		/* There are four different 'views' that are random based on house tile position. As this is not
		 * user-controllable, instead we automatically cycle through them. */
		HousePickerCallbacks::sel_view = (HousePickerCallbacks::sel_view + 1) % 4;
		this->SetDirty();
	}};

	static inline HotkeyList hotkeys{"buildhouse", {
		Hotkey('F', "focus_filter_box", PCWHK_FOCUS_FILTER_BOX),
	}};
};

/** Nested widget definition for the build NewGRF rail waypoint window */
static constexpr NWidgetPart _nested_build_house_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN), SetStringTip(STR_HOUSE_PICKER_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_DEFSIZEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_STICKYBOX, COLOUR_DARK_GREEN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			NWidgetFunction(MakePickerClassWidgets),
			NWidget(WWT_PANEL, COLOUR_DARK_GREEN),
				NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_picker, 0), SetPadding(WidgetDimensions::unscaled.picker),
					NWidget(WWT_EMPTY, INVALID_COLOUR, WID_BH_INFO), SetFill(1, 1), SetMinimalTextLines(10, 0),
					NWidget(WWT_LABEL, INVALID_COLOUR), SetStringTip(STR_HOUSE_PICKER_PROTECT_TITLE, STR_NULL), SetFill(1, 0),
					NWidget(NWID_HORIZONTAL), SetPIPRatio(1, 0, 1),
						NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_BH_PROTECT_OFF), SetMinimalSize(60, 12), SetStringTip(STR_HOUSE_PICKER_PROTECT_OFF, STR_HOUSE_PICKER_PROTECT_TOOLTIP),
						NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_BH_PROTECT_ON), SetMinimalSize(60, 12), SetStringTip(STR_HOUSE_PICKER_PROTECT_ON, STR_HOUSE_PICKER_PROTECT_TOOLTIP),
					EndContainer(),
				EndContainer(),
			EndContainer(),

		EndContainer(),
		NWidgetFunction(MakePickerTypeWidgets),
	EndContainer(),
};

static WindowDesc _build_house_desc(__FILE__, __LINE__,
	WDP_AUTO, "build_house", 0, 0,
	WC_BUILD_HOUSE, WC_BUILD_TOOLBAR,
	WindowDefaultFlag::Construction,
	_nested_build_house_widgets,
	&BuildHouseWindow::hotkeys
);

void ShowBuildHousePicker(Window *parent)
{
	if (BringWindowToFrontById(WC_BUILD_HOUSE, 0)) return;
	new BuildHouseWindow(_build_house_desc, 0, parent);
}

void ShowBuildHousePickerAndSelect(TileIndex tile)
{
	assert_tile(IsTileType(tile, MP_HOUSE), tile);

	HouseID house = GetHouseType(tile);
	GetHouseNorthPart(house);

	const HouseSpec *hs = HouseSpec::Get(house);
	if (hs == nullptr || !hs->enabled || !HousePickerCallbacks::instance.IsActive()) return;

	BuildHouseWindow *w = AllocateWindowDescFront<BuildHouseWindow, true>(_build_house_desc, 0, nullptr);
	if (w != nullptr) {
		w->PickItem(FindFirstBit(hs->building_availability & HZ_ZONALL), house);
	}
}
