/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file schdispatch_gui.cpp GUI code for Scheduled Dispatch */

#include "stdafx.h"
#include "command_func.h"
#include "gui.h"
#include "window_gui.h"
#include "window_func.h"
#include "textbuf_gui.h"
#include "core/string_builder.hpp"
#include "strings_func.h"
#include "vehicle_base.h"
#include "string_func.h"
#include "spritecache.h"
#include "gfx_func.h"
#include "company_base.h"
#include "company_func.h"
#include "date_func.h"
#include "date_gui.h"
#include "vehicle_gui.h"
#include "settings_type.h"
#include "viewport_func.h"
#include "zoom_func.h"
#include "dropdown_func.h"
#include "dropdown_common_type.h"
#include "core/geometry_func.hpp"
#include "tilehighlight_func.h"
#include "timetable_cmd.h"

#include <vector>
#include <algorithm>

#include "table/strings.h"
#include "table/string_colours.h"
#include "table/sprites.h"

#include "safeguards.h"

enum SchdispatchWidgets : WidgetID {
	WID_SCHDISPATCH_CAPTION,         ///< Caption of window.
	WID_SCHDISPATCH_RENAME,          ///< Rename button.
	WID_SCHDISPATCH_MOVE_LEFT,       ///< Move current schedule left (-1).
	WID_SCHDISPATCH_MOVE_RIGHT,      ///< Move current schedule right (+1).
	WID_SCHDISPATCH_MATRIX,          ///< Matrix of vehicles.
	WID_SCHDISPATCH_V_SCROLL,        ///< Vertical scrollbar.
	WID_SCHDISPATCH_SUMMARY_PANEL,   ///< Summary panel

	WID_SCHDISPATCH_ENABLED,         ///< Enable button.
	WID_SCHDISPATCH_HEADER,          ///< Header text.
	WID_SCHDISPATCH_PREV,            ///< Previous schedule.
	WID_SCHDISPATCH_NEXT,            ///< Next schedule.
	WID_SCHDISPATCH_ADD_SCHEDULE,    ///< Add schedule.

	WID_SCHDISPATCH_ADD,             ///< Add Departure Time button
	WID_SCHDISPATCH_SET_DURATION,    ///< Duration button
	WID_SCHDISPATCH_SET_START_DATE,  ///< Start Date button
	WID_SCHDISPATCH_SET_DELAY,       ///< Delay button
	WID_SCHDISPATCH_MANAGEMENT,      ///< Management button
	WID_SCHDISPATCH_ADJUST,          ///< Adjust departure times
	WID_SCHDISPATCH_REMOVE,          ///< Remove departure times
	WID_SCHDISPATCH_MANAGE_SLOT,     ///< Manage slot button
};

/**
 * Callback for when a time has been chosen to start the schedule
 * @param window the window related to the setting of the date
 * @param date the actually chosen date
 * @param callback_data callback data
 */
static void SetScheduleStartDateCallback(const Window *w, StateTicks date, void *callback_data)
{
	Command<CMD_SCH_DISPATCH_SET_START_DATE>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, w->window_number, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(callback_data)), date);
}

/**
 * Callback for when a time has been chosen to add to the schedule
 */
static void ScheduleAddIntl(VehicleID veh, uint schedule_index, StateTicks date, uint extra_slots, uint offset, bool wrap_mode = false)
{
	Vehicle *v = Vehicle::GetIfValid(veh);
	if (v == nullptr || !v->IsPrimaryVehicle() || schedule_index >= v->orders->GetScheduledDispatchScheduleCount()) return;

	const DispatchSchedule &ds = v->orders->GetDispatchScheduleByIndex(schedule_index);

	/* Make sure the time is the closest future to the timetable start */
	StateTicks start_tick = ds.GetScheduledDispatchStartTick();
	uint32_t duration = ds.GetScheduledDispatchDuration();
	while (date > start_tick) date -= duration;
	while (date < start_tick) date += duration;

	if (extra_slots > 0 && offset > 0 && !wrap_mode) {
		StateTicks end_tick = start_tick + duration;
		int64_t max_extra_slots = (end_tick - 1 - date).base() / offset;
		if (max_extra_slots < extra_slots) extra_slots = static_cast<uint>(std::max<int64_t>(0, max_extra_slots));
		extra_slots = std::min<uint>(extra_slots, UINT16_MAX);
	}

	Command<CMD_SCH_DISPATCH_ADD>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, veh, schedule_index, (uint32_t)(date - start_tick).base(), offset, extra_slots);
}

/**
 * Callback for when a time has been chosen to add to the schedule
 * @param window the window related to the setting of the date
 * @param date the actually chosen date
 * @param callback_data callback data
 */
static void ScheduleAddCallback(const Window *w, StateTicks date, void *callback_data)
{
	ScheduleAddIntl(w->window_number, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(callback_data)), date, 0, 0);
}

/**
 * Calculate the maximum number of vehicle required to run this timetable according to the dispatch schedule
 * @param timetable_duration  timetable duration in scaled tick
 * @param schedule_duration  scheduled dispatch duration in scaled tick
 * @param offsets list of all dispatch offsets in the schedule
 * @return maximum number of vehicles required
 */
static int CalculateMaxRequiredVehicle(Ticks timetable_duration, uint32_t schedule_duration, const std::vector<DispatchSlot> &slots)
{
	if (timetable_duration == INVALID_TICKS) return -1;
	if (slots.size() == 0) return -1;

	/* Number of time required to ensure all vehicle are counted */
	int required_loop = CeilDiv(timetable_duration, schedule_duration) + 1;

	/* Create indice array to count maximum overlapping range */
	std::vector<std::pair<uint32_t, int>> indices;
	for (int i = 0; i < required_loop; i++) {
		for (const DispatchSlot &slot : slots) {
			if (slot.offset >= schedule_duration) continue;
			indices.push_back(std::make_pair(i * schedule_duration + slot.offset, 1));
			indices.push_back(std::make_pair(i * schedule_duration + slot.offset + timetable_duration, -1));
		}
	}
	if (indices.empty()) return -1;
	std::sort(indices.begin(), indices.end());
	int current_count = 0;
	int vehicle_count = 0;
	for (const auto& inc : indices) {
		current_count += inc.second;
		if (current_count > vehicle_count) vehicle_count = current_count;
	}
	return vehicle_count;
}

static void AddNewScheduledDispatchSchedule(VehicleID vindex)
{
	StateTicks start_tick;
	uint32_t duration;

	const Company *c = Company::GetIfValid(_local_company);
	if (c != nullptr && c->settings.default_sched_dispatch_duration != 0) {
		/* Use duration from setting, set start time to be integer multiple of duration */

		const TickMinutes now = _settings_time.NowInTickMinutes();
		start_tick = _settings_time.FromTickMinutes(now - (now.base() % c->settings.default_sched_dispatch_duration));

		duration = c->settings.default_sched_dispatch_duration * _settings_time.ticks_per_minute;
	} else if (_settings_time.time_in_minutes) {
		/* Set to 00:00 of today, and 1 day */

		start_tick = _settings_time.FromTickMinutes(_settings_time.NowInTickMinutes().ToSameDayClockTime(0, 0));

		duration = 24 * 60 * _settings_time.ticks_per_minute;
	} else {
		/* Set Jan 1st and 365 day, calendar and economy time must be locked together for this to result in a useful schedule */
		start_tick = DateToStateTicks(EconTime::DateAtStartOfYear(EconTime::CurYear()));
		duration = (EconTime::UsingWallclockUnits() ? EconTime::DAYS_IN_ECONOMY_WALLCLOCK_YEAR : DAYS_IN_YEAR) * DAY_TICKS;
	}

	Command<CMD_SCH_DISPATCH_ADD_NEW_SCHEDULE>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, CommandCallback::AddNewSchDispatchSchedule, vindex, start_tick, duration);
}

struct SchdispatchWindow : GeneralVehicleWindow {
	int schedule_index;
	int clicked_widget;     ///< The widget that was clicked (used to determine what to do in OnQueryTextFinished)
	int click_subaction;    ///< Subaction for clicked_widget
	Scrollbar *vscroll;     ///< Vertical scrollbar
	uint num_columns;       ///< Number of columns.

	StateTicks next_departure_update = STATE_TICKS_INT_MAX; ///< Time after which the last departure value should be re-drawn
	uint warning_count = 0;
	uint extra_line_count = 0;

	int base_width = 0;
	int header_width = 0;
	int delete_flag_width = 0;
	int delete_flag_height = 0;
	int arrow_flag_width = 0;
	int arrow_flag_height = 0;

	bool remove_slot_mode = false;
	uint32_t selected_slot = UINT32_MAX;
	uint32_t adjust_slot_offset = UINT32_MAX;

	enum ManagementDropdown {
		SCH_MD_RESET_LAST_DISPATCHED,
		SCH_MD_CLEAR_SCHEDULE,
		SCH_MD_REMOVE_SCHEDULE,
		SCH_MD_DUPLICATE_SCHEDULE,
		SCH_MD_APPEND_VEHICLE_SCHEDULES,
		SCH_MD_REUSE_DEPARTURE_SLOTS,
		SCH_MD_RENAME_TAG,
	};

	struct DispatchSlotPositionHandler {
		StateTicks start_tick;
		uint num_columns;
		uint last_column = 0;
		int last_row = -1;
		int last_hour = INT_MIN;

		DispatchSlotPositionHandler(StateTicks start_tick, uint num_columns) : start_tick(start_tick), num_columns(num_columns) {}

		void AddSlot(DispatchSlot slot)
		{
			int hour = -1;
			if (_settings_time.time_in_minutes) {
				ClockFaceMinutes slot_minutes = _settings_time.ToTickMinutes(this->start_tick + slot.offset).ToClockFaceMinutes();
				hour = slot_minutes.ClockHour();
			}
			if (hour != this->last_hour || this->last_column + 1 == this->num_columns) {
				this->last_hour = hour;
				this->last_row++;
				this->last_column = 0;
			} else {
				this->last_column++;
			}
		}

		int GetNumberOfRows() const
		{
			return this->last_row + 1;
		}
	};

	SchdispatchWindow(WindowDesc &desc, WindowNumber window_number) :
			GeneralVehicleWindow(desc, Vehicle::Get(window_number))
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_SCHDISPATCH_V_SCROLL);
		this->FinishInitNested(window_number);

		this->owner = this->vehicle->owner;
		this->schedule_index = -1;
		this->AutoSelectSchedule();
	}

	void Close(int data = 0) override
	{
		FocusWindowById(WC_VEHICLE_VIEW, this->window_number);
		this->GeneralVehicleWindow::Close();
	}

	bool TimeUnitsUsable() const
	{
		return _settings_time.time_in_minutes || !EconTime::UsingWallclockUnits();
	}

	bool IsScheduleSelected() const
	{
		return this->vehicle->orders != nullptr && this->schedule_index >= 0 && (uint)this->schedule_index < this->vehicle->orders->GetScheduledDispatchScheduleCount();
	}

	void AutoSelectSchedule()
	{
		if (!this->IsScheduleSelected()) {
			if (this->vehicle->orders != nullptr && this->vehicle->orders->GetScheduledDispatchScheduleCount() > 0) {
				this->schedule_index = Clamp<int>(this->schedule_index, 0, this->vehicle->orders->GetScheduledDispatchScheduleCount() - 1);
			} else {
				this->schedule_index = -1;
			}
			this->selected_slot = UINT32_MAX;
		}
	}

	const DispatchSchedule &GetSelectedSchedule() const
	{
		return this->vehicle->orders->GetDispatchScheduleByIndex(this->schedule_index);
	}

	const DispatchSlot *GetSelectedDispatchSlot() const
	{
		if (!this->IsScheduleSelected()) return nullptr;

		const DispatchSchedule &ds = this->GetSelectedSchedule();
		if (this->selected_slot != UINT32_MAX) {
			for (const DispatchSlot &slot : ds.GetScheduledDispatch()) {
				if (slot.offset == this->selected_slot) {
					return &slot;
				}
			}
		}
		return nullptr;
	}

	virtual void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, Dimension &fill, Dimension &resize) override
	{
		switch (widget) {
			case WID_SCHDISPATCH_MATRIX: {
				uint min_height = 0;

				SetDParamMaxValue(0, _settings_time.time_in_minutes ? 0 : EconTime::MAX_YEAR.base() * DAYS_IN_YEAR);
				Dimension unumber = GetStringBoundingBox(STR_SCHDISPATCH_DATE_WALLCLOCK_TINY_FLAGGED);

				const Sprite *spr = GetSprite(SPR_FLAG_VEH_STOPPED, SpriteType::Normal, ZoomMask(ZOOM_LVL_GUI));
				this->delete_flag_width = UnScaleGUI(spr->width);
				this->delete_flag_height = UnScaleGUI(spr->height);

				const Sprite *spr_left_arrow = GetSprite(SPR_ARROW_LEFT, SpriteType::Normal, ZoomMask(ZOOM_LVL_GUI));
				const Sprite *spr_right_arrow = GetSprite(SPR_ARROW_RIGHT, SpriteType::Normal, ZoomMask(ZOOM_LVL_GUI));
				this->arrow_flag_width = UnScaleGUI(std::max(spr_left_arrow->width, spr_right_arrow->width));
				this->arrow_flag_height = UnScaleGUI(std::max(spr_left_arrow->height, spr_right_arrow->height));

				min_height = std::max<uint>(unumber.height + WidgetDimensions::scaled.matrix.top, UnScaleGUI(spr->height));
				this->header_width = std::max(this->delete_flag_width, this->arrow_flag_width);
				this->base_width = unumber.width + this->header_width + 4;

				resize.height = min_height;
				resize.width = base_width + WidgetDimensions::scaled.framerect.left + WidgetDimensions::scaled.framerect.right;
				size.width = resize.width * 3;
				size.height = resize.height * 3;

				fill.width = resize.width;
				fill.height = resize.height;
				break;
			}

			case WID_SCHDISPATCH_SUMMARY_PANEL:
				size.height = (6 + this->extra_line_count) * GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.framerect.Vertical() + (WidgetDimensions::scaled.vsep_wide * 2);
				uint warning_count = this->warning_count;
				if (warning_count > 0) {
					const Dimension warning_dimensions = GetSpriteSize(SPR_WARNING_SIGN);
					size.height += warning_count * std::max<int>(warning_dimensions.height, GetCharacterHeight(FS_NORMAL));
				}
				break;
		}
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	virtual void OnInvalidateData(int data = 0, bool gui_scope = true) override
	{
		switch (data) {
			case VIWD_MODIFY_ORDERS:
				if (!gui_scope) break;
				this->AutoSelectSchedule();
				this->ReInit();
				break;
		}
	}

	virtual void OnPaint() override
	{
		const Vehicle *v = this->vehicle;

		const bool unviewable = (v->orders == nullptr) || !this->TimeUnitsUsable();
		const bool uneditable = (v->orders == nullptr) || (v->owner != _local_company);
		const bool unusable = unviewable || uneditable;

		this->SetWidgetDisabledState(WID_SCHDISPATCH_ENABLED, uneditable || (!HasBit(v->vehicle_flags, VF_SCHEDULED_DISPATCH) && (unviewable || HasBit(v->vehicle_flags, VF_TIMETABLE_SEPARATION) || v->HasUnbunchingOrder())));

		this->SetWidgetDisabledState(WID_SCHDISPATCH_RENAME, unusable || v->orders->GetScheduledDispatchScheduleCount() == 0);
		this->SetWidgetDisabledState(WID_SCHDISPATCH_PREV, unviewable || this->schedule_index <= 0);
		this->SetWidgetDisabledState(WID_SCHDISPATCH_NEXT, unviewable || this->schedule_index >= (int)(v->orders->GetScheduledDispatchScheduleCount() - 1));
		this->SetWidgetDisabledState(WID_SCHDISPATCH_MOVE_LEFT, unviewable || this->schedule_index <= 0);
		this->SetWidgetDisabledState(WID_SCHDISPATCH_MOVE_RIGHT, unviewable || this->schedule_index >= (int)(v->orders->GetScheduledDispatchScheduleCount() - 1));
		this->SetWidgetDisabledState(WID_SCHDISPATCH_ADD_SCHEDULE, unusable || v->orders->GetScheduledDispatchScheduleCount() >= 4096);

		const bool disabled = unusable || !HasBit(v->vehicle_flags, VF_SCHEDULED_DISPATCH)  || !this->IsScheduleSelected();
		const bool no_editable_slots = disabled || this->GetSelectedSchedule().GetScheduledDispatch().empty();
		this->SetWidgetDisabledState(WID_SCHDISPATCH_ADD, disabled);
		this->SetWidgetDisabledState(WID_SCHDISPATCH_SET_DURATION, disabled);
		this->SetWidgetDisabledState(WID_SCHDISPATCH_SET_START_DATE, disabled);
		this->SetWidgetDisabledState(WID_SCHDISPATCH_SET_DELAY, disabled);
		this->SetWidgetDisabledState(WID_SCHDISPATCH_MANAGEMENT, disabled);
		this->SetWidgetDisabledState(WID_SCHDISPATCH_ADJUST, no_editable_slots);

		if (no_editable_slots || this->GetSelectedDispatchSlot() == nullptr) {
			this->selected_slot = UINT32_MAX;
		}
		this->SetWidgetDisabledState(WID_SCHDISPATCH_MANAGE_SLOT, this->selected_slot == UINT32_MAX);

		NWidgetCore *remove_slot_widget = this->GetWidget<NWidgetCore>(WID_SCHDISPATCH_REMOVE);
		remove_slot_widget->SetDisabled(no_editable_slots);
		if (no_editable_slots) {
			remove_slot_widget->SetLowered(false);
			this->remove_slot_mode = false;
		}

		NWidgetCore *start_date_widget = this->GetWidget<NWidgetCore>(WID_SCHDISPATCH_SET_START_DATE);
		if (_settings_time.time_in_minutes) {
			start_date_widget->SetStringTip(STR_SCHDISPATCH_START_TIME, STR_SCHDISPATCH_SET_START_TIME);
		} else {
			start_date_widget->SetStringTip(STR_SCHDISPATCH_START, STR_SCHDISPATCH_SET_START);
		}

		if (this->IsScheduleSelected()) {
			const DispatchSchedule &ds = this->GetSelectedSchedule();
			DispatchSlotPositionHandler handler(ds.GetScheduledDispatchStartTick(), this->num_columns);
			for (const DispatchSlot &slot : ds.GetScheduledDispatch()) {
				handler.AddSlot(slot);
			}
			this->vscroll->SetCount(handler.GetNumberOfRows());
		} else {
			this->vscroll->SetCount(0);
		}

		this->SetWidgetLoweredState(WID_SCHDISPATCH_ENABLED, HasBit(v->vehicle_flags, VF_SCHEDULED_DISPATCH));
		this->DrawWidgets();
	}

	virtual void SetStringParameters(WidgetID widget) const override
	{
		switch (widget) {
			case WID_SCHDISPATCH_CAPTION:
				SetDParam(0, this->vehicle->index);
				break;

			case WID_SCHDISPATCH_HEADER:
				if (this->IsScheduleSelected()) {
					const DispatchSchedule &ds = this->GetSelectedSchedule();
					if (ds.ScheduleName().empty()) {
						SetDParam(0, STR_SCHDISPATCH_SCHEDULE_ID);
						SetDParam(1, this->schedule_index + 1);
						SetDParam(2, this->vehicle->orders->GetScheduledDispatchScheduleCount());
					} else {
						SetDParam(0, STR_SCHDISPATCH_NAMED_SCHEDULE_ID);
						SetDParamStr(1, ds.ScheduleName().c_str());
						SetDParam(2, this->schedule_index + 1);
						SetDParam(3, this->vehicle->orders->GetScheduledDispatchScheduleCount());
					}
				} else {
					SetDParam(0, STR_SCHDISPATCH_NO_SCHEDULES);
				}
				break;
		}
	}

	virtual bool OnTooltip(Point pt, WidgetID widget, TooltipCloseCondition close_cond) override
	{
		switch (widget) {
			case WID_SCHDISPATCH_ENABLED: {
				if (!this->TimeUnitsUsable()) {
					SetDParam(0, STR_SCHDISPATCH_ENABLED_TOOLTIP);
					SetDParam(1, STR_CANNOT_ENABLE_BECAUSE_TIME_UNITS_UNUSABLE);
					GuiShowTooltips(this, STR_TOOLTIP_SEPARATION_CANNOT_ENABLE, close_cond, 2);
				} else if (HasBit(this->vehicle->vehicle_flags, VF_TIMETABLE_SEPARATION)) {
					SetDParam(0, STR_SCHDISPATCH_ENABLED_TOOLTIP);
					SetDParam(1, STR_CANNOT_ENABLE_BECAUSE_AUTO_SEPARATION);
					GuiShowTooltips(this, STR_TOOLTIP_SEPARATION_CANNOT_ENABLE, close_cond, 2);
				} else if (this->vehicle->HasUnbunchingOrder()) {
					SetDParam(0, STR_SCHDISPATCH_ENABLED_TOOLTIP);
					SetDParam(1, STR_CANNOT_ENABLE_BECAUSE_UNBUNCHING);
					GuiShowTooltips(this, STR_TOOLTIP_SEPARATION_CANNOT_ENABLE, close_cond, 2);
				} else {
					GuiShowTooltips(this, STR_SCHDISPATCH_ENABLED_TOOLTIP, close_cond);
				}
				return true;
			}

			case WID_SCHDISPATCH_ADD: {
				if (_settings_time.time_in_minutes) {
					SetDParam(0, STR_SCHDISPATCH_ADD_TOOLTIP);
					GuiShowTooltips(this, STR_SCHDISPATCH_ADD_TOOLTIP_EXTRA, close_cond, 1);
					return true;
				}
				break;
			}

			case WID_SCHDISPATCH_ADJUST: {
				SetDParam(0, STR_SCHDISPATCH_ADJUST_TOOLTIP);
				GuiShowTooltips(this, STR_SCHDISPATCH_ADJUST_TOOLTIP_SELECTED, close_cond, 1);
				return true;
			}

			case WID_SCHDISPATCH_MANAGEMENT: {
				_temp_special_strings[0] = GetString(STR_SCHDISPATCH_RESET_LAST_DISPATCH_TOOLTIP);
				auto add_suffix = [&](StringID str) {
					SetDParam(0, str);
					_temp_special_strings[0] += GetString(STR_SCHDISPATCH_MANAGE_TOOLTIP_SUFFIX);
				};
				add_suffix(STR_SCHDISPATCH_CLEAR_TOOLTIP);
				add_suffix(STR_SCHDISPATCH_REMOVE_SCHEDULE_TOOLTIP);
				add_suffix(STR_SCHDISPATCH_DUPLICATE_SCHEDULE_TOOLTIP);
				add_suffix(STR_SCHDISPATCH_APPEND_VEHICLE_SCHEDULES_TOOLTIP);
				add_suffix(STR_SCHDISPATCH_REUSE_DEPARTURE_SLOTS_TOOLTIP);
				add_suffix(STR_SCHDISPATCH_RENAME_DEPARTURE_TAG_TOOLTIP);
				GuiShowTooltips(this, SPECSTR_TEMP_START, close_cond);
				return true;
			}

			case WID_SCHDISPATCH_MANAGE_SLOT: {
				_temp_special_strings[0] = GetString(STR_SCHDISPATCH_REUSE_THIS_DEPARTURE_SLOT_TOOLTIP);
				auto add_suffix = [&](StringID str) {
					SetDParam(0, str);
					_temp_special_strings[0] += GetString(STR_SCHDISPATCH_MANAGE_TOOLTIP_SUFFIX);
				};
				add_suffix(STR_SCHDISPATCH_TAG_DEPARTURE_TOOLTIP);
				GuiShowTooltips(this, SPECSTR_TEMP_START, close_cond);
				return true;
			}

			case WID_SCHDISPATCH_MATRIX: {
				if (!this->TimeUnitsUsable()) return false;
				NWidgetBase *nwi = this->GetWidget<NWidgetBase>(WID_SCHDISPATCH_MATRIX);
				const DispatchSlot *slot;
				bool is_header;
				std::tie(slot, is_header) = this->GetSlotFromMatrixPoint(pt.x - nwi->pos_x, pt.y - nwi->pos_y);
				if (slot == nullptr) return false;

				if (is_header && this->remove_slot_mode) {
					GuiShowTooltips(this, STR_SCHDISPATCH_REMOVE_SLOT, close_cond);
				} else {
					const DispatchSchedule &ds = this->GetSelectedSchedule();
					const StateTicks start_tick = ds.GetScheduledDispatchStartTick();

					SetDParam(0, start_tick + slot->offset);
					_temp_special_strings[0] = GetString(STR_SCHDISPATCH_SLOT_TOOLTIP);
					if (_settings_time.time_in_minutes) {
						ClockFaceMinutes start_minutes = _settings_time.ToTickMinutes(start_tick).ToClockFaceMinutes();
						if (start_minutes != 0) {
							TickMinutes offset_minutes = TickMinutes{slot->offset / _settings_time.ticks_per_minute};
							SetDParam(0, offset_minutes.ClockHHMM());
							_temp_special_strings[0] += GetString(STR_SCHDISPATCH_SLOT_TOOLTIP_RELATIVE);
						}
					}

					bool have_extra = false;
					auto show_time = [&](StringID msg, StateTicks dispatch_tick) {
						if (!have_extra) _temp_special_strings[0] += '\n';
						_temp_special_strings[0] += GetString(msg);
						if (_settings_time.time_in_minutes) {
							ClockFaceMinutes mins = _settings_time.ToTickMinutes(dispatch_tick).ToClockFaceMinutes();
							if (mins != _settings_time.ToTickMinutes(start_tick + slot->offset).ToClockFaceMinutes()) {
								SetDParam(0, dispatch_tick);
								_temp_special_strings[0] += GetString(STR_SCHDISPATCH_SLOT_TOOLTIP_TIME_SUFFIX);
							}
						}
						have_extra = true;
					};

					auto record_iter = this->vehicle->dispatch_records.find(static_cast<uint16_t>(this->schedule_index));
					if (record_iter != this->vehicle->dispatch_records.end()) {
						const LastDispatchRecord &record = record_iter->second;
						int32_t veh_dispatch = ((record.dispatched - start_tick) % ds.GetScheduledDispatchDuration()).base();
						if (veh_dispatch < 0) veh_dispatch += ds.GetScheduledDispatchDuration();
						if (veh_dispatch == (int32_t)slot->offset) {
							show_time(STR_SCHDISPATCH_SLOT_TOOLTIP_VEHICLE, record.dispatched);
						}
					}

					int32_t last_dispatch = ds.GetScheduledDispatchLastDispatch();
					if (last_dispatch != INVALID_SCHEDULED_DISPATCH_OFFSET && (last_dispatch % ds.GetScheduledDispatchDuration() == slot->offset)) {
						show_time(STR_SCHDISPATCH_SLOT_TOOLTIP_LAST, start_tick + last_dispatch);
					}

					StateTicks next_slot = GetScheduledDispatchTime(ds, _state_ticks).first;
					if (next_slot != INVALID_STATE_TICKS && ((next_slot - ds.GetScheduledDispatchStartTick()).AsTicks() % ds.GetScheduledDispatchDuration() == slot->offset)) {
						show_time(STR_SCHDISPATCH_SLOT_TOOLTIP_NEXT, next_slot);
					}

					auto flags = slot->flags;
					if (ds.GetScheduledDispatchReuseSlots()) ClrBit(flags, DispatchSlot::SDSF_REUSE_SLOT);
					if (flags != 0) {
						_temp_special_strings[0] += '\n';
						if (HasBit(flags, DispatchSlot::SDSF_REUSE_SLOT)) {
							_temp_special_strings[0] += GetString(STR_SCHDISPATCH_SLOT_TOOLTIP_REUSE);
						}

						for (uint8_t flag_bit = DispatchSlot::SDSF_FIRST_TAG; flag_bit <= DispatchSlot::SDSF_LAST_TAG; flag_bit++) {
							if (HasBit(flags, flag_bit)) {
								SetDParam(0, 1 + flag_bit - DispatchSlot::SDSF_FIRST_TAG);
								std::string_view name = ds.GetSupplementaryName(SDSNT_DEPARTURE_TAG, flag_bit - DispatchSlot::SDSF_FIRST_TAG);
								SetDParamStr(1, name);
								_temp_special_strings[0] += GetString(name.empty() ? STR_SCHDISPATCH_SLOT_TOOLTIP_TAG : STR_SCHDISPATCH_SLOT_TOOLTIP_TAG_NAMED);
							}
						}
					}
					GuiShowTooltips(this, SPECSTR_TEMP_START, close_cond);
				}
				return true;
			}

			default:
				break;
		}

		return false;
	}

	/**
	 * Draw a time in the box with the top left corner at x,y.
	 * @param time  Time to draw.
	 * @param left  Left side of the box to draw in.
	 * @param right Right side of the box to draw in.
	 * @param y     Top of the box to draw in.
	 */
	void DrawScheduledTime(const StateTicks time, int left, int right, int y, TextColour colour, bool last, bool next, bool veh, bool flagged) const
	{
		bool rtl = _current_text_dir == TD_RTL;

		int text_left  = rtl ? right - this->base_width - 1 : left + this->header_width;
		int text_right = rtl ? right - this->header_width : left + this->base_width - 1;

		if (this->remove_slot_mode) {
			int diff_y = (this->resize.step_height - this->delete_flag_height) / 2 - 2;
			int offset_x = (this->header_width - this->delete_flag_width) / 2;
			DrawSprite(SPR_FLAG_VEH_STOPPED, PAL_NONE, offset_x + (rtl ? right - this->delete_flag_width : left), y + diff_y);
		} else {
			auto draw_arrow = [&](bool right_arrow) {
				SpriteID sprite = right_arrow ? SPR_ARROW_RIGHT : SPR_ARROW_LEFT;
				int diff_y = (this->resize.step_height - this->arrow_flag_height) / 2;
				int offset_x = (this->header_width - this->arrow_flag_width) / 2;
				DrawSprite(sprite, PAL_NONE, offset_x + (rtl ? right - this->delete_flag_width : left), y + diff_y);
			};
			if (veh) {
				int width = ScaleSpriteTrad(1);
				int x = left - WidgetDimensions::scaled.framerect.left;
				int top = y - WidgetDimensions::scaled.framerect.top;
				DrawRectOutline({ x, top, x + (int)this->resize.step_width - width, top + (int)this->resize.step_height - width }, PC_LIGHT_BLUE, width);
			}
			if (next) {
				draw_arrow(!rtl);
			} else if (last) {
				draw_arrow(rtl);
			}
		}

		SetDParam(0, time);
		DrawString(text_left, text_right, y + (this->resize.step_height - GetCharacterHeight(FS_NORMAL)) / 2,
				flagged ? STR_SCHDISPATCH_DATE_WALLCLOCK_TINY_FLAGGED : STR_JUST_TT_TIME, colour, SA_HOR_CENTER);
	}

	virtual void OnGameTick() override
	{
		if (_state_ticks >= this->next_departure_update) {
			this->next_departure_update = STATE_TICKS_INT_MAX;
			SetWidgetDirty(WID_SCHDISPATCH_SUMMARY_PANEL);
		}
	}

	virtual void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		const Vehicle *v = this->vehicle;

		switch (widget) {
			case WID_SCHDISPATCH_MATRIX: {
				/* If order is not initialized, don't draw */
				if (!this->IsScheduleSelected() || !this->TimeUnitsUsable()) break;

				bool rtl = _current_text_dir == TD_RTL;

				/* Set the row and number of boxes in each row based on the number of boxes drawn in the matrix */
				const NWidgetCore *wid = this->GetWidget<NWidgetCore>(WID_SCHDISPATCH_MATRIX);
				const uint16_t rows_in_display = wid->current_y / wid->resize_y;

				const DispatchSchedule &ds = this->GetSelectedSchedule();
				const StateTicks start_tick = ds.GetScheduledDispatchStartTick();
				const StateTicks end_tick = ds.GetScheduledDispatchStartTick() + ds.GetScheduledDispatchDuration();

				StateTicks slot = GetScheduledDispatchTime(ds, _state_ticks).first;
				int32_t next_offset = (slot != INVALID_STATE_TICKS) ? (slot - ds.GetScheduledDispatchStartTick()).AsTicks() % ds.GetScheduledDispatchDuration() : INT32_MIN;

				int32_t last_dispatch;
				if (ds.GetScheduledDispatchLastDispatch() != INVALID_SCHEDULED_DISPATCH_OFFSET) {
					last_dispatch = ds.GetScheduledDispatchLastDispatch() % ds.GetScheduledDispatchDuration();
				} else {
					last_dispatch = INT32_MIN;
				}

				int32_t veh_dispatch;
				auto record_iter = v->dispatch_records.find(static_cast<uint16_t>(this->schedule_index));
				if (record_iter != v->dispatch_records.end()) {
					const LastDispatchRecord &record = record_iter->second;
					veh_dispatch = ((record.dispatched - start_tick) % ds.GetScheduledDispatchDuration()).base();
					if (veh_dispatch < 0) veh_dispatch += ds.GetScheduledDispatchDuration();
				} else {
					veh_dispatch = INT32_MIN;
				}

				const int begin_row = this->vscroll->GetPosition();
				const int end_row = begin_row + rows_in_display;

				DispatchSlotPositionHandler handler(start_tick, this->num_columns);
				for (const DispatchSlot &slot : ds.GetScheduledDispatch()) {
					handler.AddSlot(slot);
					if (handler.last_row < begin_row || handler.last_row >= end_row) continue;

					int x = r.left + (rtl ? (this->num_columns - handler.last_column - 1) : handler.last_column) * this->resize.step_width;
					int y = r.top + WidgetDimensions::scaled.framerect.top + ((handler.last_row - begin_row) * this->resize.step_height);

					StateTicks draw_time = start_tick + slot.offset;
					bool last = last_dispatch == (int32_t)slot.offset;
					bool next = next_offset == (int32_t)slot.offset;
					bool veh = veh_dispatch == (int32_t)slot.offset;
					TextColour colour;
					if (this->selected_slot == slot.offset) {
						colour = TC_WHITE;
					} else {
						colour = draw_time >= end_tick ? TC_RED : TC_BLACK;
					}
					auto flags = slot.flags;
					if (ds.GetScheduledDispatchReuseSlots()) ClrBit(flags, DispatchSlot::SDSF_REUSE_SLOT);
					this->DrawScheduledTime(draw_time, x + WidgetDimensions::scaled.framerect.left, x + this->resize.step_width - 1 - (2 * WidgetDimensions::scaled.framerect.left),
							y, colour, last, next, veh, flags != 0);
				}
				break;
			}

			case WID_SCHDISPATCH_SUMMARY_PANEL: {
				const_cast<SchdispatchWindow*>(this)->next_departure_update = STATE_TICKS_INT_MAX;
				Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
				int y = ir.top;

				if (!this->TimeUnitsUsable()) {
					const Dimension warning_dimensions = GetSpriteSize(SPR_WARNING_SIGN);
					int left = ir.left;
					int right = ir.right;
					const bool rtl = (_current_text_dir == TD_RTL);
					DrawSprite(SPR_WARNING_SIGN, 0, rtl ? right - warning_dimensions.width - 5 : left + 5, y);
					if (rtl) {
						right -= (warning_dimensions.width + 10);
					} else {
						left += (warning_dimensions.width + 10);
					}
					DrawStringMultiLine(left, right, y, ir.bottom, STR_CANNOT_ENABLE_BECAUSE_TIME_UNITS_UNUSABLE, TC_BLACK);
					break;
				}

				auto set_next_departure_update = [&](StateTicks time) {
					if (time < this->next_departure_update) const_cast<SchdispatchWindow*>(this)->next_departure_update = time;
				};

				auto draw_warning_generic = [&](StringID text, TextColour colour) {
					const Dimension warning_dimensions = GetSpriteSize(SPR_WARNING_SIGN);
					int step_height = std::max<int>(warning_dimensions.height, GetCharacterHeight(FS_NORMAL));
					int left = ir.left;
					int right = ir.right;
					const bool rtl = (_current_text_dir == TD_RTL);
					DrawSprite(SPR_WARNING_SIGN, 0, rtl ? right - warning_dimensions.width - 5 : left + 5, y + (step_height - warning_dimensions.height) / 2);
					if (rtl) {
						right -= (warning_dimensions.width + 10);
					} else {
						left += (warning_dimensions.width + 10);
					}
					DrawString(left, right, y + (step_height - GetCharacterHeight(FS_NORMAL)) / 2, text, colour);
					y += step_height;
				};

				if (!HasBit(v->vehicle_flags, VF_SCHEDULED_DISPATCH) || !this->IsScheduleSelected()) {
					y += GetCharacterHeight(FS_NORMAL);
					DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_SUMMARY_NOT_ENABLED);
					y += GetCharacterHeight(FS_NORMAL) * 2;

					if (HasBit(v->vehicle_flags, VF_TIMETABLE_SEPARATION)) {
						draw_warning_generic(STR_CANNOT_ENABLE_BECAUSE_AUTO_SEPARATION, TC_BLACK);
					} else if (v->HasUnbunchingOrder()) {
						draw_warning_generic(STR_CANNOT_ENABLE_BECAUSE_UNBUNCHING, TC_BLACK);
					}
				} else {
					const DispatchSchedule &ds = this->GetSelectedSchedule();

					uint warnings = 0;
					uint extra_lines = 0;

					auto draw_warning = [&](StringID text) {
						draw_warning_generic(text, TC_FROMSTRING);
						warnings++;
					};

					auto departure_time_warnings = [&](StateTicks time) {
						if (_settings_time.time_in_minutes && time > (_state_ticks + (1350 * (uint)_settings_time.ticks_per_minute))) {
							/* If the departure slot is more than 23 hours ahead of now, show a warning */
							const TickMinutes now = _settings_time.NowInTickMinutes();
							const TickMinutes target = _settings_time.ToTickMinutes(time);
							const TickMinutes delta = target - now;
							if (delta >= (23 * 60)) {
								const uint hours = delta.base() / 60;
								SetDParam(0, hours);
								draw_warning(STR_SCHDISPATCH_MORE_THAN_N_HOURS_IN_FUTURE);

								set_next_departure_update(_settings_time.FromTickMinutes(target - (hours * 60) + 1));
							}
						}
					};

					bool have_conditional = false;
					int schedule_order_index = -1;
					for (int n = 0; n < v->GetNumOrders(); n++) {
						const Order *order = v->GetOrder(n);
						if (order->IsType(OT_CONDITIONAL)) {
							have_conditional = true;
						}
						if (order->GetDispatchScheduleIndex() == this->schedule_index) {
							schedule_order_index = n;
						}
					}
					if (schedule_order_index < 0) {
						draw_warning(STR_SCHDISPATCH_NOT_ASSIGNED_TO_ORDER);
					} else {
						const Order *order = v->GetOrder(schedule_order_index);
						SetDParam(0, schedule_order_index + 1);

						switch (order->GetType()) {
							case OT_GOTO_STATION:
								SetDParam(1, STR_STATION_NAME);
								SetDParam(2, order->GetDestination().ToStationID());
								break;

							case OT_GOTO_WAYPOINT:
								SetDParam(1, STR_WAYPOINT_NAME);
								SetDParam(2, order->GetDestination().ToStationID());
								break;

							case OT_GOTO_DEPOT:
								if (order->GetDepotActionType() & ODATFB_NEAREST_DEPOT) {
									if (v->type == VEH_AIRCRAFT) {
										SetDParam(1, STR_ORDER_GO_TO_NEAREST_HANGAR);
									} else {
										SetDParam(1, STR_ORDER_GO_TO_NEAREST_DEPOT);
									}
								} else {
									SetDParam(1, STR_DEPOT_NAME);
									SetDParam(2, v->type);
									SetDParam(3, order->GetDestination().ToDepotID());
								}
								break;

							default:
								SetDParam(1, STR_INVALID_ORDER);
								break;
						}

						DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_ASSIGNED_TO_ORDER);
						y += GetCharacterHeight(FS_NORMAL);
						extra_lines++;
					}

					y += WidgetDimensions::scaled.vsep_wide;

					auto show_last_departure = [&](const StateTicks last_departure, bool vehicle_mode, std::string details) {
						StringID str;
						if (_state_ticks < last_departure) {
							str = STR_SCHDISPATCH_SUMMARY_LAST_DEPARTURE_FUTURE;
							set_next_departure_update(last_departure);
						} else {
							str = STR_SCHDISPATCH_SUMMARY_LAST_DEPARTURE_PAST;
						}
						if (vehicle_mode) str += (STR_SCHDISPATCH_SUMMARY_VEHICLE_DEPARTURE_PAST - STR_SCHDISPATCH_SUMMARY_LAST_DEPARTURE_PAST);

						SetDParam(0, last_departure);
						if (details.empty()) {
							SetDParam(1, STR_EMPTY);
						} else {
							SetDParam(1, STR_SCHDISPATCH_SUMMARY_DEPARTURE_DETAILS);
							SetDParamStr(2, std::move(details));
						}

						DrawString(ir.left, ir.right, y, str);
						y += GetCharacterHeight(FS_NORMAL);

						departure_time_warnings(last_departure);

						if (_settings_time.time_in_minutes && last_departure < (_state_ticks + (1350 * (uint)_settings_time.ticks_per_minute))) {
							/* If the departure slot is more than 23 hours behind now, show a warning */
							const TickMinutes now = _settings_time.NowInTickMinutes();
							const TickMinutes target = _settings_time.ToTickMinutes(last_departure);
							const TickMinutes delta = now - target;
							if (delta >= (23 * 60)) {
								const uint hours = delta.base() / 60;
								SetDParam(0, hours);
								DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_MORE_THAN_N_HOURS_IN_PAST);
								extra_lines++;
								y += GetCharacterHeight(FS_NORMAL);

								set_next_departure_update(_settings_time.FromTickMinutes(target + ((hours + 1) * 60) + 1));
							}
						}
					};

					auto record_iter = v->dispatch_records.find(static_cast<uint16_t>(this->schedule_index));
					if (record_iter != v->dispatch_records.end()) {
						const LastDispatchRecord &record = record_iter->second;
						format_buffer details;
						auto add_detail = [&](StringID str) {
							AppendStringInPlace(details, details.empty() ? STR_JUST_STRING : STR_SCHDISPATCH_SUMMARY_DEPARTURE_DETAIL_SEPARATOR, str);
						};
						if (HasBit(record.record_flags, LastDispatchRecord::RF_FIRST_SLOT)) add_detail(STR_SCHDISPATCH_SUMMARY_DEPARTURE_DETAIL_WAS_FIRST);
						if (HasBit(record.record_flags, LastDispatchRecord::RF_LAST_SLOT)) add_detail(STR_SCHDISPATCH_SUMMARY_DEPARTURE_DETAIL_WAS_LAST);

						for (uint8_t flag_bit = DispatchSlot::SDSF_FIRST_TAG; flag_bit <= DispatchSlot::SDSF_LAST_TAG; flag_bit++) {
							if (HasBit(record.slot_flags, flag_bit)) {
								std::string_view name = ds.GetSupplementaryName(SDSNT_DEPARTURE_TAG, flag_bit - DispatchSlot::SDSF_FIRST_TAG);
								auto tmp_params = MakeParameters(1 + flag_bit - DispatchSlot::SDSF_FIRST_TAG, std::string{name});
								_temp_special_strings[1] = GetStringWithArgs(name.empty() ? STR_SCHDISPATCH_SUMMARY_DEPARTURE_DETAIL_TAG : STR_SCHDISPATCH_SUMMARY_DEPARTURE_DETAIL_TAG_NAMED, tmp_params);
								add_detail(SPECSTR_TEMP_START + 1);
							}
						}

						show_last_departure(record.dispatched, true, details.to_string());
					} else {
						DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_SUMMARY_VEHICLE_NO_LAST_DEPARTURE);
						y += GetCharacterHeight(FS_NORMAL);
					}

					if (ds.GetScheduledDispatchLastDispatch() != INVALID_SCHEDULED_DISPATCH_OFFSET) {
						show_last_departure(ds.GetScheduledDispatchStartTick() + ds.GetScheduledDispatchLastDispatch(), false, "");
					} else {
						DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_SUMMARY_NO_LAST_DEPARTURE);
						y += GetCharacterHeight(FS_NORMAL);
					}

					const StateTicks next_departure = GetScheduledDispatchTime(ds, _state_ticks).first;
					if (next_departure != INVALID_STATE_TICKS) {
						set_next_departure_update(next_departure + ds.GetScheduledDispatchDelay());
						SetDParam(0, next_departure);
						DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_SUMMARY_NEXT_AVAILABLE_DEPARTURE);
					}
					y += GetCharacterHeight(FS_NORMAL);

					departure_time_warnings(next_departure);

					y += WidgetDimensions::scaled.vsep_wide;

					if (ds.GetScheduledDispatchReuseSlots()) {
						DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_SUMMARY_REUSE_SLOTS_ENABLED);
						extra_lines++;
						y += GetCharacterHeight(FS_NORMAL);
					}

					SetTimetableParams(0, ds.GetScheduledDispatchDuration(), true);
					DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_SUMMARY_L2);
					y += GetCharacterHeight(FS_NORMAL);

					SetDParam(0, ds.GetScheduledDispatchStartTick());
					SetDParam(1, ds.GetScheduledDispatchStartTick() + ds.GetScheduledDispatchDuration());
					DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_SUMMARY_L3);
					y += GetCharacterHeight(FS_NORMAL);

					SetTimetableParams(0, ds.GetScheduledDispatchDelay());
					DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_SUMMARY_L4);
					y += GetCharacterHeight(FS_NORMAL);

					if (!ds.GetScheduledDispatchReuseSlots() && !have_conditional) {
						const int required_vehicle = CalculateMaxRequiredVehicle(v->orders->GetTimetableTotalDuration(), ds.GetScheduledDispatchDuration(), ds.GetScheduledDispatch());
						if (required_vehicle > 0) {
							SetDParam(0, required_vehicle);
							DrawString(ir.left, ir.right, y, STR_SCHDISPATCH_SUMMARY_L1);
							extra_lines++;
							y += GetCharacterHeight(FS_NORMAL);
						}
					}

					uint32_t duration = ds.GetScheduledDispatchDuration();
					for (const DispatchSlot &slot : ds.GetScheduledDispatch()) {
						if (slot.offset >= duration) {
							draw_warning(STR_SCHDISPATCH_SLOT_OUTSIDE_SCHEDULE);
							break;
						}
					}

					if (warnings != this->warning_count || extra_lines != this->extra_line_count) {
						SchdispatchWindow *mutable_this = const_cast<SchdispatchWindow *>(this);
						mutable_this->warning_count = warnings;
						mutable_this->extra_line_count = extra_lines;
						mutable_this->ReInit();
					}
				}

				break;
			}
		}
	}

	/**
	 * Get slot and whether it's in the header section in the departure time matrix.
	 * @param x Horizontal position in the matrix widget in pixels.
	 * @param y Vertical position in the matrix widget in pixels.
	 * @return slot, and whether the position was in the header section
	 */
	std::pair<const DispatchSlot *, bool> GetSlotFromMatrixPoint(int x, int y) const
	{
		if (!this->IsScheduleSelected()) return { nullptr, false };

		const NWidgetCore *matrix_widget = this->GetWidget<NWidgetCore>(WID_SCHDISPATCH_MATRIX);
		/* In case of RTL the widgets are swapped as a whole */
		if (_current_text_dir == TD_RTL) x = matrix_widget->current_x - x;

		uint xt = x / this->resize.step_width;
		int xm = x % this->resize.step_width;
		if (xt >= this->num_columns) return { nullptr, false };

		int32_t row = y / this->resize.step_height;
		if (row >= this->vscroll->GetCapacity()) return { nullptr, false };

		row += this->vscroll->GetPosition();

		const DispatchSchedule &ds = this->GetSelectedSchedule();
		DispatchSlotPositionHandler handler(ds.GetScheduledDispatchStartTick(), this->num_columns);
		for (const DispatchSlot &slot : ds.GetScheduledDispatch()) {
			handler.AddSlot(slot);
			if (handler.last_row == row && handler.last_column == xt) {
				return { &slot, xm <= this->header_width };
			}
		}

		return { nullptr, false };
	}

	/**
	 * Handle click in the departure time matrix.
	 * @param x Horizontal position in the matrix widget in pixels.
	 * @param y Vertical position in the matrix widget in pixels.
	 */
	void TimeClick(int x, int y)
	{
		const DispatchSlot *slot;
		bool is_header;
		std::tie(slot, is_header) = this->GetSlotFromMatrixPoint(x, y);

		if (slot == nullptr) {
			if (this->selected_slot != UINT32_MAX) {
				this->selected_slot = UINT32_MAX;
				this->SetWidgetDirty(WID_SCHDISPATCH_MATRIX);
			}
			return;
		}

		if (is_header && this->remove_slot_mode) {
			Command<CMD_SCH_DISPATCH_REMOVE>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, this->vehicle->index, this->schedule_index, slot->offset);
			return;
		}

		if (this->selected_slot == slot->offset) {
			this->selected_slot = UINT32_MAX;
		} else {
			this->selected_slot = slot->offset;
		}
		this->SetWidgetDirty(WID_SCHDISPATCH_MATRIX);
	}

	int32_t ProcessDurationForQueryString(int32_t duration) const
	{
		if (!_settings_client.gui.timetable_in_ticks) duration = RoundDivSU(duration, TimetableDisplayUnitSize());
		return duration;
	}

	int GetQueryStringCaptionOffset() const
	{
		if (_settings_client.gui.timetable_in_ticks) return 2;
		if (_settings_time.time_in_minutes) return 0;
		return 1;
	}

	virtual void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		const Vehicle *v = this->vehicle;

		this->clicked_widget = widget;
		this->CloseChildWindows(WC_QUERY_STRING);

		switch (widget) {
			case WID_SCHDISPATCH_MATRIX: { /* List */
				NWidgetBase *nwi = this->GetWidget<NWidgetBase>(WID_SCHDISPATCH_MATRIX);
				this->TimeClick(pt.x - nwi->pos_x, pt.y - nwi->pos_y);
				break;
			}

			case WID_SCHDISPATCH_ENABLED: {
				bool enable = !HasBit(v->vehicle_flags, VF_SCHEDULED_DISPATCH);

				Command<CMD_SCH_DISPATCH>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, v->index, enable);
				if (enable && this->vehicle->orders != nullptr && this->vehicle->orders->GetScheduledDispatchScheduleCount() == 0) {
					AddNewScheduledDispatchSchedule(v->index);
				}
				break;
			}

			case WID_SCHDISPATCH_ADD: {
				if (!this->IsScheduleSelected()) break;
				if (_settings_time.time_in_minutes && _ctrl_pressed) {
					void ShowScheduledDispatchAddSlotsWindow(SchdispatchWindow *parent, int window_number);
					ShowScheduledDispatchAddSlotsWindow(this, v->index);
				} else if (_settings_time.time_in_minutes && _settings_client.gui.timetable_start_text_entry) {
					ShowQueryString({}, STR_SCHDISPATCH_ADD_CAPTION, 31, this, CS_NUMERAL, QSF_NONE);
				} else {
					ShowSetDateWindow(this, v->index, _state_ticks, EconTime::CurYear(), EconTime::CurYear() + 15,
							ScheduleAddCallback, reinterpret_cast<void *>(static_cast<uintptr_t>(this->schedule_index)), STR_SCHDISPATCH_ADD, STR_SCHDISPATCH_ADD_TOOLTIP);
				}
				break;
			}

			case WID_SCHDISPATCH_SET_DURATION: {
				if (!this->IsScheduleSelected()) break;
				CharSetFilter charset_filter = _settings_client.gui.timetable_in_ticks ? CS_NUMERAL : CS_NUMERAL_DECIMAL;
				std::string str = GetString(STR_JUST_INT, ProcessDurationForQueryString(this->GetSelectedSchedule().GetScheduledDispatchDuration()));
				ShowQueryString(str, STR_SCHDISPATCH_DURATION_CAPTION_MINUTE + this->GetQueryStringCaptionOffset(), 31, this, charset_filter, QSF_NONE);
				break;
			}

			case WID_SCHDISPATCH_SET_START_DATE: {
				if (!this->IsScheduleSelected()) break;
				if (_settings_time.time_in_minutes && _settings_client.gui.timetable_start_text_entry) {
					ShowQueryString(GetString(STR_JUST_INT, _settings_time.NowInTickMinutes().ClockHHMM()), STR_SCHDISPATCH_START_CAPTION_MINUTE, 31, this, CS_NUMERAL, QSF_ACCEPT_UNCHANGED);
				} else {
					ShowSetDateWindow(this, v->index, _state_ticks, EconTime::CurYear(), EconTime::CurYear() + 15,
							SetScheduleStartDateCallback, reinterpret_cast<void *>(static_cast<uintptr_t>(this->schedule_index)), STR_SCHDISPATCH_SET_START, STR_SCHDISPATCH_START_TOOLTIP);
				}
				break;
			}

			case WID_SCHDISPATCH_SET_DELAY: {
				if (!this->IsScheduleSelected()) break;
				CharSetFilter charset_filter = _settings_client.gui.timetable_in_ticks ? CS_NUMERAL : CS_NUMERAL_DECIMAL;
				std::string str = GetString(STR_JUST_INT, ProcessDurationForQueryString(this->GetSelectedSchedule().GetScheduledDispatchDelay()));
				ShowQueryString(str, STR_SCHDISPATCH_DELAY_CAPTION_MINUTE + this->GetQueryStringCaptionOffset(), 31, this, charset_filter, QSF_NONE);
				break;
			}

			case WID_SCHDISPATCH_MANAGEMENT: {
				if (!this->IsScheduleSelected()) break;
				const DispatchSchedule &schedule = this->GetSelectedSchedule();
				DropDownList list;
				auto add_item = [&](StringID string, int result) {
					std::unique_ptr<DropDownListStringItem> item = std::make_unique<DropDownListStringItem>(string, result, false);
					item->SetColourFlags(TC_FORCED);
					list.emplace_back(std::move(item));
				};
				add_item(STR_SCHDISPATCH_RESET_LAST_DISPATCH, SCH_MD_RESET_LAST_DISPATCHED);
				list.push_back(MakeDropDownListDividerItem());
				add_item(STR_SCHDISPATCH_CLEAR, SCH_MD_CLEAR_SCHEDULE);
				add_item(STR_SCHDISPATCH_REMOVE_SCHEDULE, SCH_MD_REMOVE_SCHEDULE);
				add_item(STR_SCHDISPATCH_DUPLICATE_SCHEDULE, SCH_MD_DUPLICATE_SCHEDULE);
				add_item(STR_SCHDISPATCH_APPEND_VEHICLE_SCHEDULES, SCH_MD_APPEND_VEHICLE_SCHEDULES);
				list.push_back(MakeDropDownListDividerItem());
				list.push_back(MakeDropDownListCheckedItem(schedule.GetScheduledDispatchReuseSlots(), STR_SCHDISPATCH_REUSE_DEPARTURE_SLOTS, SCH_MD_REUSE_DEPARTURE_SLOTS, false));
				list.push_back(MakeDropDownListDividerItem());
				for (uint8_t tag = 0; tag < DispatchSchedule::DEPARTURE_TAG_COUNT; tag++) {
					SetDParam(0, tag + 1);
					std::string_view name = schedule.GetSupplementaryName(SDSNT_DEPARTURE_TAG, tag);
					SetDParamStr(1, name);
					add_item(name.empty() ? STR_SCHDISPATCH_RENAME_DEPARTURE_TAG : STR_SCHDISPATCH_RENAME_DEPARTURE_TAG_NAMED, SCH_MD_RENAME_TAG | (tag << 16));
				}
				ShowDropDownList(this, std::move(list), -1, WID_SCHDISPATCH_MANAGEMENT, 0, DDMF_NONE, DDSF_SHARED);
				break;
			}

			case WID_SCHDISPATCH_PREV:
				if (!this->IsScheduleSelected()) break;
				if (this->schedule_index > 0) {
					this->schedule_index--;
					this->selected_slot = UINT32_MAX;
				}
				this->ReInit();
				break;

			case WID_SCHDISPATCH_NEXT:
				if (!this->IsScheduleSelected()) break;
				if (this->schedule_index < (int)(this->vehicle->orders->GetScheduledDispatchScheduleCount() - 1)) {
					this->schedule_index++;
					this->selected_slot = UINT32_MAX;
				}
				this->ReInit();
				break;

			case WID_SCHDISPATCH_ADD_SCHEDULE:
				AddNewScheduledDispatchSchedule(this->vehicle->index);
				break;

			case WID_SCHDISPATCH_RENAME:
				if (!this->IsScheduleSelected()) break;
				ShowQueryString(this->GetSelectedSchedule().ScheduleName(), STR_SCHDISPATCH_RENAME_SCHEDULE_CAPTION,
						MAX_LENGTH_VEHICLE_NAME_CHARS, this, CS_ALPHANUMERAL, QSF_ENABLE_DEFAULT | QSF_LEN_IN_CHARS);
				break;

			case WID_SCHDISPATCH_ADJUST: {
				if (!this->IsScheduleSelected()) break;
				CharSetFilter charset_filter = _settings_client.gui.timetable_in_ticks ? CS_NUMERAL_SIGNED : CS_NUMERAL_DECIMAL_SIGNED;
				StringID caption = STR_SCHDISPATCH_ADJUST_CAPTION_MINUTE + this->GetQueryStringCaptionOffset();

				if (_ctrl_pressed) {
					const DispatchSlot *selected_slot = this->GetSelectedDispatchSlot();
					if (selected_slot != nullptr) {
						const DispatchSchedule &ds = this->GetSelectedSchedule();
						SetDParam(0, ds.GetScheduledDispatchStartTick() + selected_slot->offset);
						SetDParam(1, caption);
						std::string caption_str = GetString(STR_SCHDISPATCH_ADJUST_CAPTION_SLOT_PREFIXED);

						this->adjust_slot_offset = selected_slot->offset;
						ShowQueryString(GetString(STR_JUST_INT, 0), std::move(caption_str), 31, this, charset_filter, QSF_NONE);
					}
				} else {
					this->adjust_slot_offset = UINT32_MAX;
					ShowQueryString(GetString(STR_JUST_INT, 0), caption, 31, this, charset_filter, QSF_NONE);
				}
				break;
			}

			case WID_SCHDISPATCH_REMOVE: {
				if (!this->IsScheduleSelected()) break;
				this->remove_slot_mode = !this->remove_slot_mode;
				this->SetWidgetLoweredState(WID_SCHDISPATCH_REMOVE, this->remove_slot_mode);
				break;
			}

			case WID_SCHDISPATCH_MANAGE_SLOT: {
				const DispatchSlot *selected_slot = this->GetSelectedDispatchSlot();
				if (selected_slot == nullptr) break;
				const DispatchSchedule &schedule = this->GetSelectedSchedule();

				DropDownList list;
				auto add_item = [&](StringID str, uint bit, bool disabled) {
					list.push_back(MakeDropDownListCheckedItem(HasBit(selected_slot->flags, bit), str, bit, disabled));
				};
				add_item(STR_SCHDISPATCH_REUSE_THIS_DEPARTURE_SLOT, DispatchSlot::SDSF_REUSE_SLOT, schedule.GetScheduledDispatchReuseSlots());
				list.push_back(MakeDropDownListDividerItem());
				for (uint8_t flag_bit = DispatchSlot::SDSF_FIRST_TAG; flag_bit <= DispatchSlot::SDSF_LAST_TAG; flag_bit++) {
					SetDParam(0, 1 + flag_bit - DispatchSlot::SDSF_FIRST_TAG);
					std::string_view name = schedule.GetSupplementaryName(SDSNT_DEPARTURE_TAG, flag_bit - DispatchSlot::SDSF_FIRST_TAG);
					SetDParamStr(1, name);
					add_item(name.empty() ? STR_SCHDISPATCH_TAG_DEPARTURE : STR_SCHDISPATCH_TAG_DEPARTURE_NAMED, flag_bit, false);
				}

				ShowDropDownList(this, std::move(list), -1, WID_SCHDISPATCH_MANAGE_SLOT, 0, DDMF_NONE, DDSF_SHARED);
				break;
			}

			case WID_SCHDISPATCH_MOVE_LEFT:
				if (!this->IsScheduleSelected()) break;
				if (this->schedule_index > 0) {
					Command<CMD_SCH_DISPATCH_SWAP_SCHEDULES>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, CommandCallback::SwapSchDispatchSchedules, this->vehicle->index, this->schedule_index - 1, this->schedule_index);
				}
				break;

			case WID_SCHDISPATCH_MOVE_RIGHT:
				if (!this->IsScheduleSelected()) break;
				if (this->schedule_index < (int)(this->vehicle->orders->GetScheduledDispatchScheduleCount() - 1)) {
					Command<CMD_SCH_DISPATCH_SWAP_SCHEDULES>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, CommandCallback::SwapSchDispatchSchedules, this->vehicle->index, this->schedule_index + 1, this->schedule_index);
				}
				break;
		}

		this->SetDirty();
	}

	static void ClearScheduleCallback(Window *win, bool confirmed)
	{
		if (confirmed) {
			SchdispatchWindow *w = (SchdispatchWindow*)win;
			if (w->IsScheduleSelected()) {
				Command<CMD_SCH_DISPATCH_CLEAR>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, w->vehicle->index, w->schedule_index);
			}
		}
	}

	static void RemoveScheduleCallback(Window *win, bool confirmed)
	{
		if (confirmed) {
			SchdispatchWindow *w = (SchdispatchWindow*)win;
			if (w->IsScheduleSelected()) {
				Command<CMD_SCH_DISPATCH_REMOVE_SCHEDULE>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, w->vehicle->index, w->schedule_index);
			}
		}
	}

	void OnDropdownSelect(WidgetID widget, int index) override
	{
		if (!this->TimeUnitsUsable()) return;

		switch (widget) {
			case WID_SCHDISPATCH_MANAGEMENT: {
				if (!this->IsScheduleSelected()) break;
				switch((ManagementDropdown)index & 0xFFFF) {
					case SCH_MD_RESET_LAST_DISPATCHED:
						Command<CMD_SCH_DISPATCH_RESET_LAST_DISPATCH>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, this->vehicle->index, this->schedule_index);
						break;

					case SCH_MD_CLEAR_SCHEDULE:
						if (this->GetSelectedSchedule().GetScheduledDispatch().empty()) return;
						SetDParam(0, (uint)this->GetSelectedSchedule().GetScheduledDispatch().size());
						ShowQuery(STR_SCHDISPATCH_QUERY_CLEAR_SCHEDULE_CAPTION, STR_SCHDISPATCH_QUERY_CLEAR_SCHEDULE_TEXT, this, ClearScheduleCallback);

						break;

					case SCH_MD_REMOVE_SCHEDULE:
						SetDParam(0, (uint)this->GetSelectedSchedule().GetScheduledDispatch().size());
						ShowQuery(STR_SCHDISPATCH_QUERY_REMOVE_SCHEDULE_CAPTION, STR_SCHDISPATCH_QUERY_REMOVE_SCHEDULE_TEXT, this, RemoveScheduleCallback);
						break;

					case SCH_MD_DUPLICATE_SCHEDULE:
						Command<CMD_SCH_DISPATCH_DUPLICATE_SCHEDULE>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, this->vehicle->index, this->schedule_index);
						break;

					case SCH_MD_APPEND_VEHICLE_SCHEDULES: {
						static const CursorID clone_icons[] = {
							SPR_CURSOR_CLONE_TRAIN, SPR_CURSOR_CLONE_ROADVEH,
							SPR_CURSOR_CLONE_SHIP, SPR_CURSOR_CLONE_AIRPLANE
						};
						SetObjectToPlaceWnd(clone_icons[this->vehicle->type], PAL_NONE, HT_VEHICLE, this);
						break;
					}

					case SCH_MD_REUSE_DEPARTURE_SLOTS: {
						Command<CMD_SCH_DISPATCH_SET_REUSE_SLOTS>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, this->vehicle->index, this->schedule_index, !this->GetSelectedSchedule().GetScheduledDispatchReuseSlots());
						break;
					}

					case SCH_MD_RENAME_TAG: {
						this->clicked_widget = WID_SCHDISPATCH_MANAGEMENT;
						this->click_subaction = index;
						std::string_view str = this->GetSelectedSchedule().GetSupplementaryName(SDSNT_DEPARTURE_TAG, index >> 16);
						ShowQueryString(str, STR_SCHDISPATCH_RENAME_DEPARTURE_TAG_CAPTION, MAX_LENGTH_VEHICLE_NAME_CHARS, this, CS_ALPHANUMERAL, QSF_ENABLE_DEFAULT | QSF_LEN_IN_CHARS);
						break;
					}
				}
				break;
			}

			case WID_SCHDISPATCH_MANAGE_SLOT: {
				const DispatchSlot *selected_slot = this->GetSelectedDispatchSlot();
				if (selected_slot == nullptr) break;

				uint16_t mask = 1 << index;
				uint16_t values = HasBit(selected_slot->flags, index) ? 0 : mask;
				Command<CMD_SCH_DISPATCH_SET_SLOT_FLAGS>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, this->vehicle->index, this->schedule_index, this->selected_slot, values, mask);
				break;
			}

			default:
				break;
		}
	}

	virtual void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!this->TimeUnitsUsable()) return;

		if (!str.has_value()) return;
		const Vehicle *v = this->vehicle;

		switch (this->clicked_widget) {
			default: NOT_REACHED();

			case WID_SCHDISPATCH_ADD: {
				if (!this->IsScheduleSelected()) break;

				if (str->empty()) break;

				char *end;
				int32_t val = std::strtoul(str->c_str(), &end, 10);
				if (val >= 0 && end != nullptr && *end == 0) {
					uint minutes = (val % 100) % 60;
					uint hours = (val / 100) % 24;
					StateTicks slot = _settings_time.FromTickMinutes(_settings_time.NowInTickMinutes().ToSameDayClockTime(hours, minutes));
					ScheduleAddIntl(v->index, this->schedule_index, slot, 0, 0);
				}
				break;
			}

			case WID_SCHDISPATCH_SET_START_DATE: {
				if (!this->IsScheduleSelected()) break;

				if (str->empty()) break;

				char *end;
				int32_t val = std::strtoul(str->c_str(), &end, 10);
				if (val >= 0 && end != nullptr && *end == 0) {
					uint minutes = (val % 100) % 60;
					uint hours = (val / 100) % 24;
					StateTicks start = _settings_time.FromTickMinutes(_settings_time.NowInTickMinutes().ToSameDayClockTime(hours, minutes));
					Command<CMD_SCH_DISPATCH_SET_START_DATE>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, v->index, this->schedule_index, start);
				}
				break;
			}

			case WID_SCHDISPATCH_SET_DURATION: {
				if (!this->IsScheduleSelected()) break;
				Ticks val = ParseTimetableDuration(str->c_str());

				if (val > 0) {
					Command<CMD_SCH_DISPATCH_SET_DURATION>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, v->index, this->schedule_index, val);
				}
				break;
			}

			case WID_SCHDISPATCH_SET_DELAY: {
				if (!this->IsScheduleSelected()) break;

				if (str->empty()) break;

				Command<CMD_SCH_DISPATCH_SET_DELAY>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, v->index, this->schedule_index, ParseTimetableDuration(str->c_str()));
				break;
			}

			case WID_SCHDISPATCH_RENAME: {
				if (!this->IsScheduleSelected()) break;

				Command<CMD_SCH_DISPATCH_RENAME_SCHEDULE>::Post(STR_ERROR_CAN_T_RENAME_SCHEDULE, v->index, this->schedule_index, *str);
				break;
			}

			case WID_SCHDISPATCH_ADJUST: {
				if (!this->IsScheduleSelected()) break;
				Ticks val = ParseTimetableDuration(str->c_str());

				if (val != 0) {
					if (this->adjust_slot_offset != UINT32_MAX) {
						Command<CMD_SCH_DISPATCH_ADJUST_SLOT>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, CommandCallback::AdjustSchDispatchSlot, v->index, this->schedule_index, this->adjust_slot_offset, val);
					} else {
						Command<CMD_SCH_DISPATCH_ADJUST>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, v->index, this->schedule_index, val);
					}
				}
				break;
			}

			case WID_SCHDISPATCH_MANAGEMENT: {
				switch (this->click_subaction & 0xFFFF) {
					case SCH_MD_RENAME_TAG:
						Command<CMD_SCH_DISPATCH_RENAME_TAG>::Post(STR_ERROR_CAN_T_RENAME_DEPARTURE_TAG, v->index, this->schedule_index, this->click_subaction >> 16, *str);
						break;
				}
				break;
			}
		}

		this->SetDirty();
	}

	virtual void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_SCHDISPATCH_MATRIX);
		NWidgetCore *nwi = this->GetWidget<NWidgetCore>(WID_SCHDISPATCH_MATRIX);
		this->num_columns = nwi->current_x / nwi->resize_x;
	}

	bool OnVehicleSelect(const Vehicle *v) override
	{
		if (v->orders == nullptr || v->orders->GetScheduledDispatchScheduleCount() == 0) return false;

		Command<CMD_SCH_DISPATCH_APPEND_VEH_SCHEDULE>::Post(STR_ERROR_CAN_T_TIMETABLE_VEHICLE, this->vehicle->index, v->index);
		ResetObjectToPlace();
		return true;
	}

	const Vehicle *GetVehicle()
	{
		return this->vehicle;
	}

	void AddMultipleDepartureSlots(uint start, uint step, uint end)
	{
		bool wrap_mode = false;
		if (end < start) {
			const DispatchSchedule &ds = this->GetSelectedSchedule();
			if (ds.GetScheduledDispatchDuration() == (1440 * _settings_time.ticks_per_minute)) {
				/* 24 hour timetabling */
				end += 1440;
				wrap_mode = true;
			}
		}
		if (end < start || step == 0 || !this->IsScheduleSelected()) return;

		StateTicks slot = _settings_time.FromTickMinutes(_settings_time.NowInTickMinutes().ToSameDayClockTime(0, start));
		ScheduleAddIntl(this->vehicle->index, this->schedule_index, slot, (end - start) / step, step * _settings_time.ticks_per_minute, wrap_mode);
	}
};

void CcAddNewSchDispatchSchedule(const CommandCost &result, VehicleID veh, StateTicks start_tick, uint32_t duration)
{
	SchdispatchWindow *w = dynamic_cast<SchdispatchWindow *>(FindWindowById(WC_SCHDISPATCH_SLOTS, veh));
	if (w != nullptr) {
		w->schedule_index = INT_MAX;
		w->AutoSelectSchedule();
		w->ReInit();
	}
}

void CcSwapSchDispatchSchedules(const CommandCost &result, VehicleID veh, uint32_t schedule_index_1, uint32_t schedule_index_2)
{
	SchdispatchWindow *w = dynamic_cast<SchdispatchWindow *>(FindWindowById(WC_SCHDISPATCH_SLOTS, veh));
	if (w != nullptr) {
		w->schedule_index = schedule_index_1;
		w->AutoSelectSchedule();
		w->ReInit();
	}
}

void CcAdjustSchDispatchSlot(const CommandCost &result, VehicleID veh, uint32_t schedule_index, uint32_t offset, int32_t adjustment)
{
	if (!result.Succeeded() || !result.HasResultData()) return;

	SchdispatchWindow *w = dynamic_cast<SchdispatchWindow *>(FindWindowById(WC_SCHDISPATCH_SLOTS, veh));
	if (w != nullptr && w->schedule_index == static_cast<int>(schedule_index) && w->selected_slot == offset) {
		w->selected_slot = result.GetResultData();
	}
}

static constexpr NWidgetPart _nested_schdispatch_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_SCHDISPATCH_RENAME), SetAspect(WidgetDimensions::ASPECT_RENAME), SetSpriteTip(SPR_RENAME, STR_SCHDISPATCH_RENAME_SCHEDULE_TOOLTIP),
		NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_SCHDISPATCH_MOVE_LEFT), SetMinimalSize(12, 14), SetSpriteTip(SPR_ARROW_LEFT, STR_SCHDISPATCH_MOVE_SCHEDULE),
		NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_SCHDISPATCH_MOVE_RIGHT), SetMinimalSize(12, 14), SetSpriteTip(SPR_ARROW_RIGHT, STR_SCHDISPATCH_MOVE_SCHEDULE),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_SCHDISPATCH_CAPTION), SetStringTip(STR_SCHDISPATCH_CAPTION, STR_NULL),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_DEFSIZEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SCHDISPATCH_ENABLED), SetStringTip(STR_SCHDISPATCH_ENABLED, STR_NULL), SetFill(1, 1), SetResize(1, 0),
			NWidget(WWT_TEXT, INVALID_COLOUR, WID_SCHDISPATCH_HEADER), SetAlignment(SA_CENTER), SetStringTip(STR_JUST_STRING3, STR_NULL), SetFill(1, 1), SetResize(1, 0),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SCHDISPATCH_PREV), SetStringTip(STR_SCHDISPATCH_PREV_SCHEDULE, STR_SCHDISPATCH_PREV_SCHEDULE_TOOLTIP), SetFill(1, 1), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SCHDISPATCH_NEXT), SetStringTip(STR_SCHDISPATCH_NEXT_SCHEDULE, STR_SCHDISPATCH_NEXT_SCHEDULE_TOOLTIP), SetFill(1, 1), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SCHDISPATCH_ADD_SCHEDULE), SetStringTip(STR_SCHDISPATCH_ADD_SCHEDULE, STR_SCHDISPATCH_ADD_SCHEDULE_TOOLTIP), SetFill(1, 1), SetResize(1, 0),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_MATRIX, COLOUR_GREY, WID_SCHDISPATCH_MATRIX), SetResize(1, 1), SetScrollbar(WID_SCHDISPATCH_V_SCROLL),
			NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_SCHDISPATCH_V_SCROLL),
		EndContainer(),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SCHDISPATCH_ADD), SetStringTip(STR_SCHDISPATCH_ADD, STR_SCHDISPATCH_ADD_TOOLTIP), SetFill(1, 1), SetResize(1, 0),
			NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SCHDISPATCH_ADJUST), SetStringTip(STR_SCHDISPATCH_ADJUST, STR_SCHDISPATCH_ADJUST_TOOLTIP), SetFill(1, 1), SetResize(1, 0),
			NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_SCHDISPATCH_REMOVE), SetStringTip(STR_SCHDISPATCH_REMOVE, STR_SCHDISPATCH_REMOVE_TOOLTIP), SetFill(1, 1), SetResize(1, 0),
			NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_SCHDISPATCH_MANAGE_SLOT), SetStringTip(STR_SCHDISPATCH_MANAGE_SLOT, STR_NULL), SetFill(1, 1), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_SCHDISPATCH_SUMMARY_PANEL), SetMinimalSize(400, 22), SetResize(1, 0), EndContainer(),
		NWidget(NWID_HORIZONTAL),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SCHDISPATCH_SET_START_DATE), SetStringTip(STR_SCHDISPATCH_START, STR_SCHDISPATCH_START_TOOLTIP), SetFill(1, 1), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SCHDISPATCH_SET_DURATION), SetStringTip(STR_SCHDISPATCH_DURATION, STR_SCHDISPATCH_DURATION_TOOLTIP), SetFill(1, 1), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SCHDISPATCH_SET_DELAY), SetStringTip(STR_SCHDISPATCH_DELAY, STR_SCHDISPATCH_DELAY_TOOLTIP), SetFill(1, 1), SetResize(1, 0),
				NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_SCHDISPATCH_MANAGEMENT), SetStringTip(STR_SCHDISPATCH_MANAGE, STR_NULL), SetFill(1, 1), SetResize(1, 0),
			EndContainer(),
			NWidget(WWT_RESIZEBOX, COLOUR_GREY),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _schdispatch_desc(__FILE__, __LINE__,
	WDP_AUTO, "scheduled_dispatch_slots", 400, 130,
	WC_SCHDISPATCH_SLOTS, WC_VEHICLE_TIMETABLE,
	WindowDefaultFlag::Construction,
	_nested_schdispatch_widgets
);

/**
 * Show the slot dispatching slots
 * @param v The vehicle to show the slot dispatching slots for
 */
void ShowSchdispatchWindow(const Vehicle *v)
{
	AllocateWindowDescFront<SchdispatchWindow>(_schdispatch_desc, v->index);
}

enum ScheduledDispatchAddSlotsWindowWidgets : WidgetID {
	WID_SCHDISPATCH_ADD_SLOT_START_HOUR,
	WID_SCHDISPATCH_ADD_SLOT_START_MINUTE,
	WID_SCHDISPATCH_ADD_SLOT_STEP_HOUR,
	WID_SCHDISPATCH_ADD_SLOT_STEP_MINUTE,
	WID_SCHDISPATCH_ADD_SLOT_END_HOUR,
	WID_SCHDISPATCH_ADD_SLOT_END_MINUTE,
	WID_SCHDISPATCH_ADD_SLOT_ADD_BUTTON,
	WID_SCHDISPATCH_ADD_SLOT_START_TEXT,
	WID_SCHDISPATCH_ADD_SLOT_STEP_TEXT,
	WID_SCHDISPATCH_ADD_SLOT_END_TEXT,
};

struct ScheduledDispatchAddSlotsWindow : Window {
	ClockFaceMinutes start;
	ClockFaceMinutes step;
	ClockFaceMinutes end;

	ScheduledDispatchAddSlotsWindow(WindowDesc &desc, WindowNumber window_number, SchdispatchWindow *parent) :
			Window(desc)
	{
		this->start = _settings_time.NowInTickMinutes().ToClockFaceMinutes();
		this->step = ClockFaceMinutes{30};
		this->end = this->start + 60;
		this->parent = parent;
		this->CreateNestedTree();
		this->FinishInitNested(window_number);
	}

	Point OnInitialPosition(int16_t sm_width, int16_t sm_height, int window_number) override
	{
		Point pt = { this->parent->left + this->parent->width / 2 - sm_width / 2, this->parent->top + this->parent->height / 2 - sm_height / 2 };
		return pt;
	}

	virtual void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, Dimension &fill, Dimension &resize) override
	{
		Dimension d = {0, 0};
		switch (widget) {
			default: return;

			case WID_SCHDISPATCH_ADD_SLOT_START_TEXT:
			case WID_SCHDISPATCH_ADD_SLOT_STEP_TEXT:
			case WID_SCHDISPATCH_ADD_SLOT_END_TEXT:
				d = maxdim(d, GetStringBoundingBox(STR_SCHDISPATCH_ADD_DEPARTURE_SLOTS_START));
				d = maxdim(d, GetStringBoundingBox(STR_SCHDISPATCH_ADD_DEPARTURE_SLOTS_STEP));
				d = maxdim(d, GetStringBoundingBox(STR_SCHDISPATCH_ADD_DEPARTURE_SLOTS_END));
				break;

			case WID_SCHDISPATCH_ADD_SLOT_START_HOUR:
			case WID_SCHDISPATCH_ADD_SLOT_STEP_HOUR:
			case WID_SCHDISPATCH_ADD_SLOT_END_HOUR:
				for (uint i = 0; i < 24; i++) {
					SetDParam(0, i);
					d = maxdim(d, GetStringBoundingBox(STR_JUST_INT));
				}
				break;

			case WID_SCHDISPATCH_ADD_SLOT_START_MINUTE:
			case WID_SCHDISPATCH_ADD_SLOT_STEP_MINUTE:
			case WID_SCHDISPATCH_ADD_SLOT_END_MINUTE:
				for (uint i = 0; i < 60; i++) {
					SetDParam(0, i);
					d = maxdim(d, GetStringBoundingBox(STR_JUST_INT));
				}
				break;
		}

		d.width += padding.width;
		d.height += padding.height;
		size = d;
	}

	virtual void SetStringParameters(WidgetID widget) const override
	{
		switch (widget) {
			case WID_SCHDISPATCH_ADD_SLOT_START_HOUR:   SetDParam(0, start.ClockHour()); break;
			case WID_SCHDISPATCH_ADD_SLOT_START_MINUTE: SetDParam(0, start.ClockMinute()); break;
			case WID_SCHDISPATCH_ADD_SLOT_STEP_HOUR:    SetDParam(0, step.ClockHour()); break;
			case WID_SCHDISPATCH_ADD_SLOT_STEP_MINUTE:  SetDParam(0, step.ClockMinute()); break;
			case WID_SCHDISPATCH_ADD_SLOT_END_HOUR:     SetDParam(0, end.ClockHour()); break;
			case WID_SCHDISPATCH_ADD_SLOT_END_MINUTE:   SetDParam(0, end.ClockMinute()); break;
		}
	}

	virtual void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		auto handle_hours_dropdown = [&](ClockFaceMinutes current) {
			DropDownList list;
			for (uint i = 0; i < 24; i++) {
				SetDParam(0, i);
				list.push_back(MakeDropDownListStringItem(STR_JUST_INT, i, false));
			}
			ShowDropDownList(this, std::move(list), current.ClockHour(), widget);
		};

		auto handle_minutes_dropdown = [&](ClockFaceMinutes current) {
			DropDownList list;
			for (uint i = 0; i < 60; i++) {
				SetDParam(0, i);
				list.push_back(MakeDropDownListStringItem(STR_JUST_INT, i, false));
			}
			ShowDropDownList(this, std::move(list), current.ClockMinute(), widget);
		};

		switch (widget) {
			case WID_SCHDISPATCH_ADD_SLOT_START_HOUR:
				handle_hours_dropdown(this->start);
				break;
			case WID_SCHDISPATCH_ADD_SLOT_START_MINUTE:
				handle_minutes_dropdown(this->start);
				break;
			case WID_SCHDISPATCH_ADD_SLOT_STEP_HOUR:
				handle_hours_dropdown(this->step);
				break;
			case WID_SCHDISPATCH_ADD_SLOT_STEP_MINUTE:
				handle_minutes_dropdown(this->step);
				break;
			case WID_SCHDISPATCH_ADD_SLOT_END_HOUR:
				handle_hours_dropdown(this->end);
				break;
			case WID_SCHDISPATCH_ADD_SLOT_END_MINUTE:
				handle_minutes_dropdown(this->end);
				break;

			case WID_SCHDISPATCH_ADD_SLOT_ADD_BUTTON:
				static_cast<SchdispatchWindow *>(this->parent)->AddMultipleDepartureSlots(this->start.base(), this->step.base(), this->end.base());
				this->Close();
				break;
		}
	}

	virtual void OnDropdownSelect(WidgetID widget, int index) override
	{
		switch (widget) {
			case WID_SCHDISPATCH_ADD_SLOT_START_HOUR:
				this->start = ClockFaceMinutes::FromClockFace(index, this->start.ClockMinute());
				break;
			case WID_SCHDISPATCH_ADD_SLOT_START_MINUTE:
				this->start = ClockFaceMinutes::FromClockFace(this->start.ClockHour(), index);
				break;
			case WID_SCHDISPATCH_ADD_SLOT_STEP_HOUR:
				this->step = ClockFaceMinutes::FromClockFace(index, this->step.ClockMinute());
				break;
			case WID_SCHDISPATCH_ADD_SLOT_STEP_MINUTE:
				this->step = ClockFaceMinutes::FromClockFace(this->step.ClockHour(), index);
				break;
			case WID_SCHDISPATCH_ADD_SLOT_END_HOUR:
				this->end = ClockFaceMinutes::FromClockFace(index, this->end.ClockMinute());
				break;
			case WID_SCHDISPATCH_ADD_SLOT_END_MINUTE:
				this->end = ClockFaceMinutes::FromClockFace(this->end.ClockHour(), index);
				break;
		}


		this->SetWidgetDirty(widget);
	}
};

static constexpr NWidgetPart _nested_scheduled_dispatch_add_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_BROWN),
		NWidget(WWT_CAPTION, COLOUR_BROWN), SetStringTip(STR_TIME_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_BROWN),
		NWidget(NWID_VERTICAL), SetPIP(6, 6, 6),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize), SetPIP(6, 6, 6),
				NWidget(WWT_TEXT, INVALID_COLOUR, WID_SCHDISPATCH_ADD_SLOT_START_TEXT), SetStringTip(STR_SCHDISPATCH_ADD_DEPARTURE_SLOTS_START, STR_NULL),
				NWidget(WWT_DROPDOWN, COLOUR_ORANGE, WID_SCHDISPATCH_ADD_SLOT_START_HOUR), SetFill(1, 0), SetStringTip(STR_JUST_INT, STR_DATE_MINUTES_HOUR_TOOLTIP),
				NWidget(WWT_DROPDOWN, COLOUR_ORANGE, WID_SCHDISPATCH_ADD_SLOT_START_MINUTE), SetFill(1, 0), SetStringTip(STR_JUST_INT, STR_DATE_MINUTES_MINUTE_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize), SetPIP(6, 6, 6),
				NWidget(WWT_TEXT, INVALID_COLOUR, WID_SCHDISPATCH_ADD_SLOT_STEP_TEXT), SetStringTip(STR_SCHDISPATCH_ADD_DEPARTURE_SLOTS_STEP, STR_NULL),
				NWidget(WWT_DROPDOWN, COLOUR_ORANGE, WID_SCHDISPATCH_ADD_SLOT_STEP_HOUR), SetFill(1, 0), SetStringTip(STR_JUST_INT, STR_DATE_MINUTES_HOUR_TOOLTIP),
				NWidget(WWT_DROPDOWN, COLOUR_ORANGE, WID_SCHDISPATCH_ADD_SLOT_STEP_MINUTE), SetFill(1, 0), SetStringTip(STR_JUST_INT, STR_DATE_MINUTES_MINUTE_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize), SetPIP(6, 6, 6),
				NWidget(WWT_TEXT, INVALID_COLOUR, WID_SCHDISPATCH_ADD_SLOT_END_TEXT), SetStringTip(STR_SCHDISPATCH_ADD_DEPARTURE_SLOTS_END, STR_NULL),
				NWidget(WWT_DROPDOWN, COLOUR_ORANGE, WID_SCHDISPATCH_ADD_SLOT_END_HOUR), SetFill(1, 0), SetStringTip(STR_JUST_INT, STR_DATE_MINUTES_HOUR_TOOLTIP),
				NWidget(WWT_DROPDOWN, COLOUR_ORANGE, WID_SCHDISPATCH_ADD_SLOT_END_MINUTE), SetFill(1, 0), SetStringTip(STR_JUST_INT, STR_DATE_MINUTES_MINUTE_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(NWID_SPACER), SetFill(1, 0),
				NWidget(WWT_PUSHTXTBTN, COLOUR_BROWN, WID_SCHDISPATCH_ADD_SLOT_ADD_BUTTON), SetMinimalSize(100, 12), SetStringTip(STR_SCHDISPATCH_ADD, STR_SCHDISPATCH_ADD_TOOLTIP),
				NWidget(NWID_SPACER), SetFill(1, 0),
			EndContainer(),
		EndContainer(),
	EndContainer()
};

static WindowDesc _scheduled_dispatch_add_desc(__FILE__, __LINE__,
	WDP_CENTER, nullptr, 0, 0,
	WC_SET_DATE, WC_NONE,
	{},
	_nested_scheduled_dispatch_add_widgets
);

void ShowScheduledDispatchAddSlotsWindow(SchdispatchWindow *parent, int window_number)
{
	CloseWindowByClass(WC_SET_DATE);

	new ScheduledDispatchAddSlotsWindow(_scheduled_dispatch_add_desc, window_number, parent);
}

void SchdispatchInvalidateWindows(const Vehicle *v)
{
	if (_pause_mode != PM_UNPAUSED) InvalidateWindowClassesData(WC_DEPARTURES_BOARD, 0);

	if (!HaveWindowByClass(WC_VEHICLE_TIMETABLE) && !HaveWindowByClass(WC_SCHDISPATCH_SLOTS) && !HaveWindowByClass(WC_VEHICLE_ORDERS)) return;

	v = v->FirstShared();
	for (Window *w : Window::Iterate()) {
		if (w->window_class == WC_VEHICLE_TIMETABLE) {
			if (static_cast<GeneralVehicleWindow *>(w)->vehicle->FirstShared() == v) w->SetDirty();
		}
		if (w->window_class == WC_SCHDISPATCH_SLOTS || w->window_class == WC_VEHICLE_ORDERS) {
			if (static_cast<GeneralVehicleWindow *>(w)->vehicle->FirstShared() == v) w->InvalidateData(VIWD_MODIFY_ORDERS, false);
		}
	}
}
