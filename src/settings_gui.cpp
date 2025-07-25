/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file settings_gui.cpp GUI for settings. */

#include "stdafx.h"
#include "currency.h"
#include "error.h"
#include "settings_gui.h"
#include "textbuf_gui.h"
#include "command_func.h"
#include "network/network.h"
#include "network/network_content.h"
#include "town.h"
#include "settings_internal.h"
#include "strings_func.h"
#include "window_func.h"
#include "string_func.h"
#include "dropdown_type.h"
#include "dropdown_func.h"
#include "dropdown_common_type.h"
#include "slider_func.h"
#include "highscore.h"
#include "base_media_base.h"
#include "company_base.h"
#include "company_func.h"
#include "viewport_func.h"
#include "core/geometry_func.hpp"
#include "ai/ai.hpp"
#include "blitter/factory.hpp"
#include "language.h"
#include "textfile_gui.h"
#include "stringfilter_type.h"
#include "querystring_gui.h"
#include "fontcache.h"
#include "zoom_func.h"
#include "rev.h"
#include "video/video_driver.hpp"
#include "music/music_driver.hpp"
#include "gui.h"
#include "mixer.h"
#include "newgrf_config.h"
#include "scope.h"
#include "network/core/config.h"
#include "network/network_gui.h"
#include "network/network_survey.h"
#include "video/video_driver.hpp"
#include "social_integration.h"
#include "sound_func.h"

#include <vector>
#include <functional>
#include <iterator>
#include <set>

#include "safeguards.h"

extern void FlushDeparturesWindowTextCaches();

#if defined(WITH_FREETYPE) || defined(_WIN32) || defined(WITH_COCOA)
#	define HAS_TRUETYPE_FONT
#endif

static const StringID _autosave_dropdown[] = {
	STR_GAME_OPTIONS_AUTOSAVE_DROPDOWN_OFF,
	STR_GAME_OPTIONS_AUTOSAVE_DROPDOWN_EVERY_10_MINUTES,
	STR_GAME_OPTIONS_AUTOSAVE_DROPDOWN_EVERY_30_MINUTES,
	STR_GAME_OPTIONS_AUTOSAVE_DROPDOWN_EVERY_60_MINUTES,
	STR_GAME_OPTIONS_AUTOSAVE_DROPDOWN_EVERY_120_MINUTES,
	STR_GAME_OPTIONS_AUTOSAVE_DROPDOWN_EVERY_MINUTES_CUSTOM_LABEL,
	INVALID_STRING_ID,
};

/** Available settings for autosave intervals. */
static const uint32_t _autosave_dropdown_to_minutes[] = {
	0, ///< never
	10,
	30,
	60,
	120,
};

static Dimension _circle_size; ///< Dimension of the circle +/- icon. This is here as not all users are within the class of the settings window.

static const void *ResolveObject(const GameSettings *settings_ptr, const IntSettingDesc *sd);

/**
 * Get index of the current screen resolution.
 * @return Index of the current screen resolution if it is a known resolution, _resolutions.size() otherwise.
 */
static uint GetCurrentResolutionIndex()
{
	auto it = std::ranges::find(_resolutions, Dimension(_screen.width, _screen.height));
	return std::distance(_resolutions.begin(), it);
}

static void ShowCustCurrency();

/** Window for displaying the textfile of a BaseSet. */
struct BaseSetTextfileWindow : public TextfileWindow {
	const std::string name; ///< Name of the content.
	const StringID content_type; ///< STR_CONTENT_TYPE_xxx for title.

	BaseSetTextfileWindow(TextfileType file_type, const std::string &name, const std::string &textfile, StringID content_type) : TextfileWindow(file_type), name(name), content_type(content_type)
	{
		this->ConstructWindow();
		this->LoadTextfile(textfile, BASESET_DIR);
	}

	void SetStringParameters(WidgetID widget) const override
	{
		if (widget == WID_TF_CAPTION) {
			SetDParam(0, content_type);
			SetDParamStr(1, this->name);
		}
	}
};

/**
 * Open the BaseSet version of the textfile window.
 * @param file_type The type of textfile to display.
 * @param baseset The BaseSet to use.
 * @param content_type STR_CONTENT_TYPE_xxx for title.
 */
template <class TBaseSet>
void ShowBaseSetTextfileWindow(TextfileType file_type, const TBaseSet *baseset, StringID content_type)
{
	CloseWindowById(WC_TEXTFILE, file_type);
	auto textfile = baseset->GetTextfile(file_type);
	if (textfile.has_value()) {
		new BaseSetTextfileWindow(file_type, baseset->name, textfile.value(), content_type);
	}
}

template <class T>
DropDownList BuildSetDropDownList(int *selected_index)
{
	int n = T::GetNumSets();
	*selected_index = T::GetIndexOfUsedSet();
	DropDownList list;
	for (int i = 0; i < n; i++) {
		list.push_back(MakeDropDownListStringItem(T::GetSet(i)->GetListLabel(), i));
	}
	return list;
}

std::set<int> _refresh_rates = { 30, 60, 75, 90, 100, 120, 144, 240 };

/**
 * Add the refresh rate from the config and the refresh rates from all the monitors to
 * our list of refresh rates shown in the GUI.
 */
static void AddCustomRefreshRates()
{
	/* Add the refresh rate as selected in the config. */
	_refresh_rates.insert(_settings_client.gui.refresh_rate);

	/* Add all the refresh rates of all monitors connected to the machine.  */
	std::vector<int> monitorRates = VideoDriver::GetInstance()->GetListOfMonitorRefreshRates();
	std::copy(monitorRates.begin(), monitorRates.end(), std::inserter(_refresh_rates, _refresh_rates.end()));
}

static const int SCALE_NMARKS = (MAX_INTERFACE_SCALE - MIN_INTERFACE_SCALE) / 25 + 1; // Show marks at 25% increments
static const int VOLUME_NMARKS = 9; // Show 5 values and 4 empty marks.

static std::optional<std::string> ScaleMarkFunc(int, int, int value)
{
	/* Label only every 100% mark. */
	if (value % 100 != 0) return std::string{};

	return GetString(STR_GAME_OPTIONS_GUI_SCALE_MARK, value / 100, 0);
}

static std::optional<std::string> VolumeMarkFunc(int, int mark, int value)
{
	/* Label only every other mark. */
	if (mark % 2 != 0) return std::string{};

	// 0-127 does not map nicely to 0-100. Dividing first gives us nice round numbers.
	return GetString(STR_GAME_OPTIONS_VOLUME_MARK, value / 31 * 25);
}

static constexpr NWidgetPart _nested_social_plugins_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_FRAME, COLOUR_GREY, WID_GO_SOCIAL_PLUGIN_TITLE), SetStringTip(STR_JUST_STRING2),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_SOCIAL_PLUGIN_PLATFORM),
				NWidget(WWT_TEXT, INVALID_COLOUR, WID_GO_SOCIAL_PLUGIN_PLATFORM), SetMinimalSize(100, 12), SetStringTip(STR_JUST_RAW_STRING), SetAlignment(SA_RIGHT),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_SOCIAL_PLUGIN_STATE),
				NWidget(WWT_TEXT, INVALID_COLOUR, WID_GO_SOCIAL_PLUGIN_STATE), SetMinimalSize(100, 12), SetStringTip(STR_JUST_STRING1), SetAlignment(SA_RIGHT),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static constexpr NWidgetPart _nested_social_plugins_none_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_SOCIAL_PLUGINS_NONE),
	EndContainer(),
};

class NWidgetSocialPlugins : public NWidgetVertical {
public:
	NWidgetSocialPlugins()
	{
		this->plugins = SocialIntegration::GetPlugins();

		if (this->plugins.empty()) {
			auto widget = MakeNWidgets(_nested_social_plugins_none_widgets, nullptr);
			this->Add(std::move(widget));
		} else {
			for (size_t i = 0; i < this->plugins.size(); i++) {
				auto widget = MakeNWidgets(_nested_social_plugins_widgets, nullptr);
				this->Add(std::move(widget));
			}
		}

		this->SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0);
	}

	void FillWidgetLookup(WidgetLookup &widget_lookup) override
	{
		widget_lookup[WID_GO_SOCIAL_PLUGINS] = this;
		NWidgetVertical::FillWidgetLookup(widget_lookup);
	}

	void SetupSmallestSize(Window *w) override
	{
		this->current_index = -1;
		NWidgetVertical::SetupSmallestSize(w);
	}

	/**
	 * Find of all the plugins the one where the member is the widest (in pixels).
	 *
	 * @param member The member to check with.
	 * @return The plugin that has the widest value (in pixels) for the given member.
	 */
	template <typename T>
	std::string &GetWidestPlugin(T SocialIntegrationPlugin::*member) const
	{
		std::string *longest = &(this->plugins[0]->*member);
		int longest_length = 0;

		for (auto *plugin : this->plugins) {
			int length = GetStringBoundingBox(plugin->*member).width;
			if (length > longest_length) {
				longest_length = length;
				longest = &(plugin->*member);
			}
		}

		return *longest;
	}

	void SetStringParameters(int widget) const
	{
		switch (widget) {
			case WID_GO_SOCIAL_PLUGIN_TITLE:
				/* For SetupSmallestSize, use the longest string we have. */
				if (this->current_index < 0) {
					SetDParam(0, STR_GAME_OPTIONS_SOCIAL_PLUGIN_TITLE);
					SetDParamStr(1, GetWidestPlugin(&SocialIntegrationPlugin::name));
					SetDParamStr(2, GetWidestPlugin(&SocialIntegrationPlugin::version));
					break;
				}

				if (this->plugins[this->current_index]->name.empty()) {
					SetDParam(0, STR_JUST_RAW_STRING);
					SetDParamStr(1, this->plugins[this->current_index]->basepath);
				} else {
					SetDParam(0, STR_GAME_OPTIONS_SOCIAL_PLUGIN_TITLE);
					SetDParamStr(1, this->plugins[this->current_index]->name);
					SetDParamStr(2, this->plugins[this->current_index]->version);
				}
				break;

			case WID_GO_SOCIAL_PLUGIN_PLATFORM:
				/* For SetupSmallestSize, use the longest string we have. */
				if (this->current_index < 0) {
					SetDParamStr(0, GetWidestPlugin(&SocialIntegrationPlugin::social_platform));
					break;
				}

				SetDParamStr(0, this->plugins[this->current_index]->social_platform);
				break;

			case WID_GO_SOCIAL_PLUGIN_STATE: {
				static const std::pair<SocialIntegrationPlugin::State, StringID> state_to_string[] = {
					{ SocialIntegrationPlugin::RUNNING, STR_GAME_OPTIONS_SOCIAL_PLUGIN_STATE_RUNNING },
					{ SocialIntegrationPlugin::FAILED, STR_GAME_OPTIONS_SOCIAL_PLUGIN_STATE_FAILED },
					{ SocialIntegrationPlugin::PLATFORM_NOT_RUNNING, STR_GAME_OPTIONS_SOCIAL_PLUGIN_STATE_PLATFORM_NOT_RUNNING },
					{ SocialIntegrationPlugin::UNLOADED, STR_GAME_OPTIONS_SOCIAL_PLUGIN_STATE_UNLOADED },
					{ SocialIntegrationPlugin::DUPLICATE, STR_GAME_OPTIONS_SOCIAL_PLUGIN_STATE_DUPLICATE },
					{ SocialIntegrationPlugin::UNSUPPORTED_API, STR_GAME_OPTIONS_SOCIAL_PLUGIN_STATE_UNSUPPORTED_API },
					{ SocialIntegrationPlugin::INVALID_SIGNATURE, STR_GAME_OPTIONS_SOCIAL_PLUGIN_STATE_INVALID_SIGNATURE },
				};

				/* For SetupSmallestSize, use the longest string we have. */
				if (this->current_index < 0) {
					auto longest_plugin = GetWidestPlugin(&SocialIntegrationPlugin::social_platform);

					/* Set the longest plugin when looking for the longest status. */
					SetDParamStr(0, longest_plugin);

					StringID longest = STR_NULL;
					int longest_length = 0;
					for (auto state : state_to_string) {
						int length = GetStringBoundingBox(state.second).width;
						if (length > longest_length) {
							longest_length = length;
							longest = state.second;
						}
					}

					SetDParam(0, longest);
					SetDParamStr(1, longest_plugin);
					break;
				}

				auto plugin = this->plugins[this->current_index];

				/* Default string, in case no state matches. */
				SetDParam(0, STR_GAME_OPTIONS_SOCIAL_PLUGIN_STATE_FAILED);
				SetDParamStr(1, plugin->social_platform);

				/* Find the string for the state. */
				for (auto state : state_to_string) {
					if (plugin->state == state.first) {
						SetDParam(0, state.second);
						break;
					}
				}
			}
			break;
		}
	}

	void Draw(const Window *w) override
	{
		this->current_index = 0;

		for (auto &wid : this->children) {
			wid->Draw(w);
			this->current_index++;
		}
	}

private:
	int current_index = -1;
	std::vector<SocialIntegrationPlugin *> plugins;
};

/** Construct nested container widget for managing the list of social plugins. */
std::unique_ptr<NWidgetBase> MakeNWidgetSocialPlugins()
{
	return std::make_unique<NWidgetSocialPlugins>();
}

struct GameOptionsWindow : Window {
	GameSettings *opt;
	bool reload;
	int gui_scale;
	static inline WidgetID active_tab = WID_GO_TAB_GENERAL;

	enum class QueryTextItem {
		None,
		AutosaveCustomRealTimeMinutes,
	};
	QueryTextItem current_query_text_item = QueryTextItem::None;

	GameOptionsWindow(WindowDesc &desc) : Window(desc)
	{
		this->opt = &GetGameSettings();
		this->reload = false;
		this->gui_scale = _gui_scale;

		AddCustomRefreshRates();

		this->InitNested(WN_GAME_OPTIONS_GAME_OPTIONS);
		this->OnInvalidateData(0);

		this->SetTab(GameOptionsWindow::active_tab);

		if constexpr (!NetworkSurveyHandler::IsSurveyPossible()) this->GetWidget<NWidgetStacked>(WID_GO_SURVEY_SEL)->SetDisplayedPlane(SZSP_NONE);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		CloseWindowById(WC_CUSTOM_CURRENCY, 0);
		CloseWindowByClass(WC_TEXTFILE);
		if (this->reload) _switch_mode = SM_MENU;
		this->Window::Close();
	}

	/**
	 * Build the dropdown list for a specific widget.
	 * @param widget         Widget to build list for
	 * @param selected_index Currently selected item
	 * @return the built dropdown list, or nullptr if the widget has no dropdown menu.
	 */
	DropDownList BuildDropDownList(WidgetID widget, int *selected_index) const
	{
		DropDownList list;
		switch (widget) {
			case WID_GO_CURRENCY_DROPDOWN: { // Setup currencies dropdown
				*selected_index = this->opt->locale.currency;
				uint64_t disabled = _game_mode == GM_MENU ? 0LL : ~GetMaskOfAllowedCurrencies();

				/* Add non-custom currencies; sorted naturally */
				for (const CurrencySpec &currency : _currency_specs) {
					int i = &currency - _currency_specs.data();
					if (i == CURRENCY_CUSTOM) continue;
					if (currency.code.empty()) {
						list.push_back(MakeDropDownListStringItem(currency.name, i, HasBit(disabled, i)));
					} else {
						SetDParam(0, currency.name);
						SetDParamStr(1, currency.code);
						list.push_back(MakeDropDownListStringItem(STR_GAME_OPTIONS_CURRENCY_CODE, i, HasBit(disabled, i)));
					}
				}
				std::sort(list.begin(), list.end(), DropDownListStringItem::NatSortFunc);

				/* Append custom currency at the end */
				list.push_back(MakeDropDownListDividerItem()); // separator line
				list.push_back(MakeDropDownListStringItem(STR_GAME_OPTIONS_CURRENCY_CUSTOM, CURRENCY_CUSTOM, HasBit(disabled, CURRENCY_CUSTOM)));
				break;
			}

			case WID_GO_AUTOSAVE_DROPDOWN: { // Setup autosave dropdown
				*selected_index = 5;
				int index = 0;
				for (auto &minutes : _autosave_dropdown_to_minutes) {
					if (_settings_client.gui.autosave_interval == minutes) {
						*selected_index = index;
						break;
					}
					index++;
				}

				const StringID *items = _autosave_dropdown;
				for (uint i = 0; *items != INVALID_STRING_ID; items++, i++) {
					list.push_back(MakeDropDownListStringItem(*items, i));
				}
				break;
			}

			case WID_GO_LANG_DROPDOWN: { // Setup interface language dropdown
				for (uint i = 0; i < _languages.size(); i++) {
					bool hide_language = IsReleasedVersion() && !_languages[i].IsReasonablyFinished();
					if (hide_language) continue;
					bool hide_percentage = IsReleasedVersion() || _languages[i].missing < _settings_client.gui.missing_strings_threshold;
					if (&_languages[i] == _current_language) {
						*selected_index = i;
						SetDParamStr(0, _languages[i].own_name);
					} else {
						/* Especially with sprite-fonts, not all localized
						 * names can be rendered. So instead, we use the
						 * international names for anything but the current
						 * selected language. This avoids showing a few ????
						 * entries in the dropdown list. */
						SetDParamStr(0, _languages[i].name);
					}
					SetDParam(1, (LANGUAGE_TOTAL_STRINGS - _languages[i].missing) * 100 / LANGUAGE_TOTAL_STRINGS);
					list.push_back(MakeDropDownListStringItem(hide_percentage ? STR_JUST_RAW_STRING : STR_GAME_OPTIONS_LANGUAGE_PERCENTAGE, i));
				}
				std::sort(list.begin(), list.end(), DropDownListStringItem::NatSortFunc);
				break;
			}

			case WID_GO_RESOLUTION_DROPDOWN: // Setup resolution dropdown
				if (_resolutions.empty()) break;

				*selected_index = GetCurrentResolutionIndex();
				for (uint i = 0; i < _resolutions.size(); i++) {
					SetDParam(0, _resolutions[i].width);
					SetDParam(1, _resolutions[i].height);
					list.push_back(MakeDropDownListStringItem(STR_GAME_OPTIONS_RESOLUTION_ITEM, i));
				}
				break;

			case WID_GO_REFRESH_RATE_DROPDOWN: // Setup refresh rate dropdown
				for (auto it = _refresh_rates.begin(); it != _refresh_rates.end(); it++) {
					auto i = std::distance(_refresh_rates.begin(), it);
					if (*it == _settings_client.gui.refresh_rate) *selected_index = i;
					SetDParam(0, *it);
					list.push_back(MakeDropDownListStringItem(STR_GAME_OPTIONS_REFRESH_RATE_ITEM, i));
				}
				break;

			case WID_GO_BASE_GRF_DROPDOWN:
				list = BuildSetDropDownList<BaseGraphics>(selected_index);
				break;

			case WID_GO_BASE_SFX_DROPDOWN:
				list = BuildSetDropDownList<BaseSounds>(selected_index);
				break;

			case WID_GO_BASE_MUSIC_DROPDOWN:
				list = BuildSetDropDownList<BaseMusic>(selected_index);
				break;
		}

		return list;
	}

	void SetStringParameters(WidgetID widget) const override
	{
		switch (widget) {
			case WID_GO_CURRENCY_DROPDOWN: {
				const CurrencySpec &currency = _currency_specs[this->opt->locale.currency];
				if (currency.code.empty()) {
					SetDParam(0, currency.name);
				} else {
					SetDParam(0, STR_GAME_OPTIONS_CURRENCY_CODE);
					SetDParam(1, currency.name);
					SetDParamStr(2, currency.code);
				}
				break;
			}
			case WID_GO_AUTOSAVE_DROPDOWN: {
				SetDParam(0, STR_GAME_OPTIONS_AUTOSAVE_DROPDOWN_EVERY_MINUTES_CUSTOM);
				SetDParam(1, _settings_client.gui.autosave_interval);
				int index = 0;
				for (auto &minutes : _autosave_dropdown_to_minutes) {
					if (_settings_client.gui.autosave_interval == minutes) {
						SetDParam(0, _autosave_dropdown[index]);
						break;
					}
					index++;
				}
				break;
			}
			case WID_GO_LANG_DROPDOWN:         SetDParamStr(0, _current_language->own_name); break;
			case WID_GO_BASE_GRF_DROPDOWN:     SetDParamStr(0, BaseGraphics::GetUsedSet()->GetListLabel()); break;
			case WID_GO_BASE_SFX_DROPDOWN:     SetDParamStr(0, BaseSounds::GetUsedSet()->GetListLabel()); break;
			case WID_GO_BASE_MUSIC_DROPDOWN:   SetDParamStr(0, BaseMusic::GetUsedSet()->GetListLabel()); break;
			case WID_GO_REFRESH_RATE_DROPDOWN: SetDParam(0, _settings_client.gui.refresh_rate); break;
			case WID_GO_RESOLUTION_DROPDOWN: {
				auto current_resolution = GetCurrentResolutionIndex();

				if (current_resolution == _resolutions.size()) {
					SetDParam(0, STR_GAME_OPTIONS_RESOLUTION_OTHER);
				} else {
					SetDParam(0, STR_GAME_OPTIONS_RESOLUTION_ITEM);
					SetDParam(1, _resolutions[current_resolution].width);
					SetDParam(2, _resolutions[current_resolution].height);
				}
				break;
			}

			case WID_GO_SOCIAL_PLUGIN_TITLE:
			case WID_GO_SOCIAL_PLUGIN_PLATFORM:
			case WID_GO_SOCIAL_PLUGIN_STATE: {
				const NWidgetSocialPlugins *plugin = this->GetWidget<NWidgetSocialPlugins>(WID_GO_SOCIAL_PLUGINS);
				assert(plugin != nullptr);

				plugin->SetStringParameters(widget);
				break;
			}
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_GO_BASE_GRF_DESCRIPTION:
				SetDParamStr(0, BaseGraphics::GetUsedSet()->GetDescription(GetCurrentLanguageIsoCode()));
				DrawStringMultiLine(r.left, r.right, r.top, UINT16_MAX, STR_JUST_RAW_STRING, TC_BLACK);
				break;

			case WID_GO_BASE_SFX_DESCRIPTION:
				SetDParamStr(0, BaseSounds::GetUsedSet()->GetDescription(GetCurrentLanguageIsoCode()));
				DrawStringMultiLine(r.left, r.right, r.top, UINT16_MAX, STR_JUST_RAW_STRING, TC_BLACK);
				break;

			case WID_GO_BASE_MUSIC_DESCRIPTION:
				SetDParamStr(0, BaseMusic::GetUsedSet()->GetDescription(GetCurrentLanguageIsoCode()));
				DrawStringMultiLine(r.left, r.right, r.top, UINT16_MAX, STR_JUST_RAW_STRING, TC_BLACK);
				break;

			case WID_GO_GUI_SCALE:
				DrawSliderWidget(r, MIN_INTERFACE_SCALE, MAX_INTERFACE_SCALE, SCALE_NMARKS, this->gui_scale, ScaleMarkFunc);
				break;

			case WID_GO_VIDEO_DRIVER_INFO:
				SetDParamStr(0, VideoDriver::GetInstance()->GetInfoString());
				DrawStringMultiLine(r, STR_GAME_OPTIONS_VIDEO_DRIVER_INFO);
				break;

			case WID_GO_BASE_SFX_VOLUME:
				DrawSliderWidget(r, 0, INT8_MAX, VOLUME_NMARKS, _settings_client.music.effect_vol, VolumeMarkFunc);
				break;

			case WID_GO_BASE_MUSIC_VOLUME:
				DrawSliderWidget(r, 0, INT8_MAX, VOLUME_NMARKS, _settings_client.music.music_vol, VolumeMarkFunc);
				break;
		}
	}

	void SetTab(WidgetID widget)
	{
		this->SetWidgetsLoweredState(false, WID_GO_TAB_GENERAL, WID_GO_TAB_GRAPHICS, WID_GO_TAB_SOUND, WID_GO_TAB_SOCIAL);
		this->LowerWidget(widget);
		GameOptionsWindow::active_tab = widget;

		int pane;
		switch (widget) {
			case WID_GO_TAB_GENERAL: pane = 0; break;
			case WID_GO_TAB_GRAPHICS: pane = 1; break;
			case WID_GO_TAB_SOUND: pane = 2; break;
			case WID_GO_TAB_SOCIAL: pane = 3; break;
			default: NOT_REACHED();
		}

		this->GetWidget<NWidgetStacked>(WID_GO_TAB_SELECTION)->SetDisplayedPlane(pane);
		this->SetDirty();
	}

	void OnResize() override
	{
		bool changed = false;

		NWidgetResizeBase *wid = this->GetWidget<NWidgetResizeBase>(WID_GO_BASE_GRF_DESCRIPTION);
		int y = 0;
		for (int i = 0; i < BaseGraphics::GetNumSets(); i++) {
			SetDParamStr(0, BaseGraphics::GetSet(i)->GetDescription(GetCurrentLanguageIsoCode()));
			y = std::max(y, GetStringHeight(STR_JUST_RAW_STRING, wid->current_x));
		}
		changed |= wid->UpdateVerticalSize(y);

		wid = this->GetWidget<NWidgetResizeBase>(WID_GO_BASE_SFX_DESCRIPTION);
		y = 0;
		for (int i = 0; i < BaseSounds::GetNumSets(); i++) {
			SetDParamStr(0, BaseSounds::GetSet(i)->GetDescription(GetCurrentLanguageIsoCode()));
			y = std::max(y, GetStringHeight(STR_JUST_RAW_STRING, wid->current_x));
		}
		changed |= wid->UpdateVerticalSize(y);

		wid = this->GetWidget<NWidgetResizeBase>(WID_GO_BASE_MUSIC_DESCRIPTION);
		y = 0;
		for (int i = 0; i < BaseMusic::GetNumSets(); i++) {
			SetDParamStr(0, BaseMusic::GetSet(i)->GetDescription(GetCurrentLanguageIsoCode()));
			y = std::max(y, GetStringHeight(STR_JUST_RAW_STRING, wid->current_x));
		}
		changed |= wid->UpdateVerticalSize(y);

		wid = this->GetWidget<NWidgetResizeBase>(WID_GO_VIDEO_DRIVER_INFO);
		SetDParamStr(0, VideoDriver::GetInstance()->GetInfoString());
		y = GetStringHeight(STR_GAME_OPTIONS_VIDEO_DRIVER_INFO, wid->current_x);
		changed |= wid->UpdateVerticalSize(y);

		if (changed) this->ReInit(0, 0, this->flags.Test(WindowFlag::Centred));
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_GO_TEXT_SFX_VOLUME:
			case WID_GO_TEXT_MUSIC_VOLUME: {
				Dimension d = maxdim(GetStringBoundingBox(STR_GAME_OPTIONS_SFX_VOLUME), GetStringBoundingBox(STR_GAME_OPTIONS_MUSIC_VOLUME));
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_GO_CURRENCY_DROPDOWN:
			case WID_GO_AUTOSAVE_DROPDOWN:
			case WID_GO_LANG_DROPDOWN:
			case WID_GO_RESOLUTION_DROPDOWN:
			case WID_GO_REFRESH_RATE_DROPDOWN:
			case WID_GO_BASE_GRF_DROPDOWN:
			case WID_GO_BASE_SFX_DROPDOWN:
			case WID_GO_BASE_MUSIC_DROPDOWN: {
				int selected;
				size.width = std::max(size.width, GetDropDownListDimension(this->BuildDropDownList(widget, &selected)).width + padding.width);
				break;
			}
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget >= WID_GO_BASE_GRF_TEXTFILE && widget < WID_GO_BASE_GRF_TEXTFILE + TFT_CONTENT_END) {
			if (BaseGraphics::GetUsedSet() == nullptr) return;

			ShowBaseSetTextfileWindow((TextfileType)(widget - WID_GO_BASE_GRF_TEXTFILE), BaseGraphics::GetUsedSet(), STR_CONTENT_TYPE_BASE_GRAPHICS);
			return;
		}
		if (widget >= WID_GO_BASE_SFX_TEXTFILE && widget < WID_GO_BASE_SFX_TEXTFILE + TFT_CONTENT_END) {
			if (BaseSounds::GetUsedSet() == nullptr) return;

			ShowBaseSetTextfileWindow((TextfileType)(widget - WID_GO_BASE_SFX_TEXTFILE), BaseSounds::GetUsedSet(), STR_CONTENT_TYPE_BASE_SOUNDS);
			return;
		}
		if (widget >= WID_GO_BASE_MUSIC_TEXTFILE && widget < WID_GO_BASE_MUSIC_TEXTFILE + TFT_CONTENT_END) {
			if (BaseMusic::GetUsedSet() == nullptr) return;

			ShowBaseSetTextfileWindow((TextfileType)(widget - WID_GO_BASE_MUSIC_TEXTFILE), BaseMusic::GetUsedSet(), STR_CONTENT_TYPE_BASE_MUSIC);
			return;
		}
		switch (widget) {
			case WID_GO_TAB_GENERAL:
			case WID_GO_TAB_GRAPHICS:
			case WID_GO_TAB_SOUND:
			case WID_GO_TAB_SOCIAL:
				this->SetTab(widget);
				break;

			case WID_GO_SURVEY_PARTICIPATE_BUTTON:
				switch (_settings_client.network.participate_survey) {
					case PS_ASK:
					case PS_NO:
						_settings_client.network.participate_survey = PS_YES;
						break;

					case PS_YES:
						_settings_client.network.participate_survey = PS_NO;
						break;
				}

				this->SetWidgetLoweredState(WID_GO_SURVEY_PARTICIPATE_BUTTON, _settings_client.network.participate_survey == PS_YES);
				this->SetWidgetDirty(WID_GO_SURVEY_PARTICIPATE_BUTTON);
				break;

			case WID_GO_SURVEY_LINK_BUTTON:
				OpenBrowser(NETWORK_SURVEY_DETAILS_LINK);
				break;

			case WID_GO_SURVEY_PREVIEW_BUTTON:
				ShowSurveyResultTextfileWindow();
				break;

			case WID_GO_FULLSCREEN_BUTTON: // Click fullscreen on/off
				/* try to toggle full-screen on/off */
				if (!ToggleFullScreen(!_fullscreen)) {
					ShowErrorMessage(STR_ERROR_FULLSCREEN_FAILED, INVALID_STRING_ID, WL_ERROR);
				}
				this->SetWidgetLoweredState(WID_GO_FULLSCREEN_BUTTON, _fullscreen);
				this->SetWidgetDirty(WID_GO_FULLSCREEN_BUTTON);
				break;

			case WID_GO_VIDEO_ACCEL_BUTTON:
				_video_hw_accel = !_video_hw_accel;
				ShowErrorMessage(STR_GAME_OPTIONS_VIDEO_ACCELERATION_RESTART, INVALID_STRING_ID, WL_INFO);
				this->SetWidgetLoweredState(WID_GO_VIDEO_ACCEL_BUTTON, _video_hw_accel);
				this->SetWidgetDirty(WID_GO_VIDEO_ACCEL_BUTTON);
#ifndef __APPLE__
				this->SetWidgetLoweredState(WID_GO_VIDEO_VSYNC_BUTTON, _video_hw_accel && _video_vsync);
				this->SetWidgetDisabledState(WID_GO_VIDEO_VSYNC_BUTTON, !_video_hw_accel);
				this->SetWidgetDirty(WID_GO_VIDEO_VSYNC_BUTTON);
#endif
				break;

			case WID_GO_VIDEO_VSYNC_BUTTON:
				if (!_video_hw_accel) break;

				_video_vsync = !_video_vsync;
				VideoDriver::GetInstance()->ToggleVsync(_video_vsync);

				this->SetWidgetLoweredState(WID_GO_VIDEO_VSYNC_BUTTON, _video_vsync);
				this->SetWidgetDirty(WID_GO_VIDEO_VSYNC_BUTTON);
				this->SetWidgetDisabledState(WID_GO_REFRESH_RATE_DROPDOWN, _video_vsync);
				this->SetWidgetDirty(WID_GO_REFRESH_RATE_DROPDOWN);
				break;

			case WID_GO_GUI_SCALE_BEVEL_BUTTON: {
				_settings_client.gui.scale_bevels = !_settings_client.gui.scale_bevels;

				this->SetWidgetLoweredState(WID_GO_GUI_SCALE_BEVEL_BUTTON, _settings_client.gui.scale_bevels);
				this->SetDirty();

				SetupWidgetDimensions();
				ReInitAllWindows(true);
				break;
			}

#ifdef HAS_TRUETYPE_FONT
			case WID_GO_GUI_FONT_SPRITE:
				_fcsettings.prefer_sprite = !_fcsettings.prefer_sprite;

				this->SetWidgetLoweredState(WID_GO_GUI_FONT_SPRITE, _fcsettings.prefer_sprite);
				this->SetWidgetDisabledState(WID_GO_GUI_FONT_AA, _fcsettings.prefer_sprite);
				this->SetDirty();

				InitFontCache(false);
				InitFontCache(true);
				ClearFontCache();

				FontChanged();
				break;

			case WID_GO_GUI_FONT_AA:
				_fcsettings.global_aa = !_fcsettings.global_aa;

				this->SetWidgetLoweredState(WID_GO_GUI_FONT_AA, _fcsettings.global_aa);
				MarkWholeScreenDirty();

				ClearFontCache();
				break;
#endif /* HAS_TRUETYPE_FONT */

			case WID_GO_GUI_SCALE_MAIN_TOOLBAR: {
				_settings_client.gui.bigger_main_toolbar = !_settings_client.gui.bigger_main_toolbar;

				this->SetWidgetLoweredState(WID_GO_GUI_SCALE_MAIN_TOOLBAR, _settings_client.gui.bigger_main_toolbar);
				this->SetDirty();

				ReInitAllWindows(true);
				break;
			}

			case WID_GO_GUI_SCALE:
				if (ClickSliderWidget(this->GetWidget<NWidgetBase>(widget)->GetCurrentRect(), pt, MIN_INTERFACE_SCALE, MAX_INTERFACE_SCALE, _ctrl_pressed ? 0 : SCALE_NMARKS, this->gui_scale)) {
					this->SetWidgetDirty(widget);
				}

				if (click_count > 0) this->mouse_capture_widget = widget;
				break;

			case WID_GO_GUI_SCALE_AUTO:
			{
				if (_gui_scale_cfg == -1) {
					_gui_scale_cfg = _gui_scale;
					this->SetWidgetLoweredState(WID_GO_GUI_SCALE_AUTO, false);
				} else {
					_gui_scale_cfg = -1;
					this->SetWidgetLoweredState(WID_GO_GUI_SCALE_AUTO, true);
					if (AdjustGUIZoom(AGZM_MANUAL)) ReInitAllWindows(true);
					this->gui_scale = _gui_scale;
				}
				this->SetWidgetDirty(widget);
				break;
			}

			case WID_GO_BASE_GRF_PARAMETERS: {
				auto *used_set = BaseGraphics::GetUsedSet();
				if (used_set == nullptr || !used_set->IsConfigurable()) break;
				GRFConfig &extra_cfg = used_set->GetOrCreateExtraConfig();
				if (extra_cfg.param.empty()) extra_cfg.SetParameterDefaults();
				OpenGRFParameterWindow(true, extra_cfg, _game_mode == GM_MENU);
				if (_game_mode == GM_MENU) this->reload = true;
				break;
			}

			case WID_GO_BASE_SFX_VOLUME:
			case WID_GO_BASE_MUSIC_VOLUME: {
				uint8_t &vol = (widget == WID_GO_BASE_MUSIC_VOLUME) ? _settings_client.music.music_vol : _settings_client.music.effect_vol;
				if (ClickSliderWidget(this->GetWidget<NWidgetBase>(widget)->GetCurrentRect(), pt, 0, INT8_MAX, 0, vol)) {
					if (widget == WID_GO_BASE_MUSIC_VOLUME) {
						MusicDriver::GetInstance()->SetVolume(vol);
					} else {
						SetEffectVolume(vol);
					}
					this->SetWidgetDirty(widget);
					SetWindowClassesDirty(WC_MUSIC_WINDOW);
				}

				if (click_count > 0) this->mouse_capture_widget = widget;
				break;
			}

			case WID_GO_BASE_MUSIC_JUKEBOX: {
				ShowMusicWindow();
				break;
			}

			case WID_GO_BASE_GRF_OPEN_URL:
				if (BaseGraphics::GetUsedSet() == nullptr || BaseGraphics::GetUsedSet()->url.empty()) return;
				OpenBrowser(BaseGraphics::GetUsedSet()->url);
				break;

			case WID_GO_BASE_SFX_OPEN_URL:
				if (BaseSounds::GetUsedSet() == nullptr || BaseSounds::GetUsedSet()->url.empty()) return;
				OpenBrowser(BaseSounds::GetUsedSet()->url);
				break;

			case WID_GO_BASE_MUSIC_OPEN_URL:
				if (BaseMusic::GetUsedSet() == nullptr || BaseMusic::GetUsedSet()->url.empty()) return;
				OpenBrowser(BaseMusic::GetUsedSet()->url);
				break;

			case WID_GO_BASE_GRF_CONTENT_DOWNLOAD:
				ShowNetworkContentListWindow(nullptr, CONTENT_TYPE_BASE_GRAPHICS);
				break;

			case WID_GO_BASE_SFX_CONTENT_DOWNLOAD:
				ShowNetworkContentListWindow(nullptr, CONTENT_TYPE_BASE_SOUNDS);
				break;

			case WID_GO_BASE_MUSIC_CONTENT_DOWNLOAD:
				ShowNetworkContentListWindow(nullptr, CONTENT_TYPE_BASE_MUSIC);
				break;

			case WID_GO_CURRENCY_DROPDOWN:
			case WID_GO_AUTOSAVE_DROPDOWN:
			case WID_GO_LANG_DROPDOWN:
			case WID_GO_RESOLUTION_DROPDOWN:
			case WID_GO_REFRESH_RATE_DROPDOWN:
			case WID_GO_BASE_GRF_DROPDOWN:
			case WID_GO_BASE_SFX_DROPDOWN:
			case WID_GO_BASE_MUSIC_DROPDOWN: {
				int selected;
				DropDownList list = this->BuildDropDownList(widget, &selected);
				if (!list.empty()) {
					ShowDropDownList(this, std::move(list), selected, widget);
				} else {
					if (widget == WID_GO_RESOLUTION_DROPDOWN) ShowErrorMessage(STR_ERROR_RESOLUTION_LIST_FAILED, INVALID_STRING_ID, WL_ERROR);
				}
				break;
			}
		}
	}

	void OnMouseLoop() override
	{
		if (_left_button_down || this->gui_scale == _gui_scale) return;

		_gui_scale_cfg = this->gui_scale;

		if (AdjustGUIZoom(AGZM_MANUAL)) {
			ReInitAllWindows(true);
			this->SetWidgetLoweredState(WID_GO_GUI_SCALE_AUTO, false);
			this->SetDirty();
		}
	}

	void OnDropdownSelect(WidgetID widget, int index) override
	{
		switch (widget) {
			case WID_GO_CURRENCY_DROPDOWN: // Currency
				if (index == CURRENCY_CUSTOM) ShowCustCurrency();
				this->opt->locale.currency = index;
				ReInitAllWindows(false);
				break;

			case WID_GO_AUTOSAVE_DROPDOWN: // Autosave options
				if (index == 5) {
					this->current_query_text_item = QueryTextItem::AutosaveCustomRealTimeMinutes;
					ShowQueryString(GetString(STR_JUST_INT, _settings_client.gui.autosave_interval), STR_GAME_OPTIONS_AUTOSAVE_MINUTES_QUERY_CAPT, 4, this, CS_NUMERAL, QSF_ACCEPT_UNCHANGED);
				} else {
					_settings_client.gui.autosave_interval = _autosave_dropdown_to_minutes[index];
					ChangeAutosaveFrequency(false);
					this->SetDirty();
				}
				break;

			case WID_GO_LANG_DROPDOWN: // Change interface language
				ReadLanguagePack(&_languages[index]);
				CloseWindowByClass(WC_QUERY_STRING);
				CheckForMissingGlyphs();
				ClearAllCachedNames();
				UpdateAllVirtCoords();
				CheckBlitter();
				ReInitAllWindows(false);
				FlushDeparturesWindowTextCaches();
				break;

			case WID_GO_RESOLUTION_DROPDOWN: // Change resolution
				if ((uint)index < _resolutions.size() && ChangeResInGame(_resolutions[index].width, _resolutions[index].height)) {
					this->SetDirty();
				}
				break;

			case WID_GO_REFRESH_RATE_DROPDOWN: {
				_settings_client.gui.refresh_rate = *std::next(_refresh_rates.begin(), index);
				if (_settings_client.gui.refresh_rate > 60) {
					/* Show warning to the user that this refresh rate might not be suitable on
					 * larger maps with many NewGRFs and vehicles. */
					ShowErrorMessage(STR_GAME_OPTIONS_REFRESH_RATE_WARNING, INVALID_STRING_ID, WL_INFO);
				}
				break;
			}

			case WID_GO_BASE_GRF_DROPDOWN:
				if (_game_mode == GM_MENU) {
					CloseWindowByClass(WC_GRF_PARAMETERS);
					auto set = BaseGraphics::GetSet(index);
					BaseGraphics::SetSet(set);
					this->reload = true;
					this->InvalidateData();
				}
				break;

			case WID_GO_BASE_SFX_DROPDOWN:
				ChangeSoundSet(index);
				break;

			case WID_GO_BASE_MUSIC_DROPDOWN:
				ChangeMusicSet(index);
				break;
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		auto guard = scope_guard([this]() {
			this->current_query_text_item = QueryTextItem::None;
		});

		/* Was 'cancel' pressed? */
		if (!str.has_value()) return;

		if (!str->empty()) {
			int value = atoi(str->c_str());
			switch (this->current_query_text_item) {
				case QueryTextItem::None:
					break;

				case QueryTextItem::AutosaveCustomRealTimeMinutes:
					_settings_client.gui.autosave_interval = Clamp(value, 1, 8000);
					ChangeAutosaveFrequency(false);
					this->SetDirty();
					break;
			}
		}
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data. @see GameOptionsInvalidationData
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		this->SetWidgetLoweredState(WID_GO_SURVEY_PARTICIPATE_BUTTON, _settings_client.network.participate_survey == PS_YES);
		this->SetWidgetLoweredState(WID_GO_FULLSCREEN_BUTTON, _fullscreen);
		this->SetWidgetLoweredState(WID_GO_VIDEO_ACCEL_BUTTON, _video_hw_accel);
		this->SetWidgetDisabledState(WID_GO_REFRESH_RATE_DROPDOWN, _video_vsync);

#ifndef __APPLE__
		this->SetWidgetLoweredState(WID_GO_VIDEO_VSYNC_BUTTON, _video_hw_accel && _video_vsync);
		this->SetWidgetDisabledState(WID_GO_VIDEO_VSYNC_BUTTON, !_video_hw_accel);
#endif

		this->SetWidgetLoweredState(WID_GO_GUI_SCALE_AUTO, _gui_scale_cfg == -1);
		this->SetWidgetLoweredState(WID_GO_GUI_SCALE_BEVEL_BUTTON, _settings_client.gui.scale_bevels);
#ifdef HAS_TRUETYPE_FONT
		this->SetWidgetLoweredState(WID_GO_GUI_FONT_SPRITE, _fcsettings.prefer_sprite);
		this->SetWidgetLoweredState(WID_GO_GUI_FONT_AA, _fcsettings.global_aa);
		this->SetWidgetDisabledState(WID_GO_GUI_FONT_AA, _fcsettings.prefer_sprite);
#endif /* HAS_TRUETYPE_FONT */

		this->SetWidgetLoweredState(WID_GO_GUI_SCALE_MAIN_TOOLBAR, _settings_client.gui.bigger_main_toolbar);

		this->SetWidgetDisabledState(WID_GO_BASE_GRF_DROPDOWN, _game_mode != GM_MENU);

		this->SetWidgetDisabledState(WID_GO_BASE_GRF_PARAMETERS, BaseGraphics::GetUsedSet() == nullptr || !BaseGraphics::GetUsedSet()->IsConfigurable());

		this->SetWidgetDisabledState(WID_GO_BASE_GRF_OPEN_URL, BaseGraphics::GetUsedSet() == nullptr || BaseGraphics::GetUsedSet()->url.empty());
		this->SetWidgetDisabledState(WID_GO_BASE_SFX_OPEN_URL, BaseSounds::GetUsedSet() == nullptr || BaseSounds::GetUsedSet()->url.empty());
		this->SetWidgetDisabledState(WID_GO_BASE_MUSIC_OPEN_URL, BaseMusic::GetUsedSet() == nullptr || BaseMusic::GetUsedSet()->url.empty());

		for (TextfileType tft = TFT_CONTENT_BEGIN; tft < TFT_CONTENT_END; tft++) {
			this->SetWidgetDisabledState(WID_GO_BASE_GRF_TEXTFILE + tft, BaseGraphics::GetUsedSet() == nullptr || !BaseGraphics::GetUsedSet()->GetTextfile(tft).has_value());
			this->SetWidgetDisabledState(WID_GO_BASE_SFX_TEXTFILE + tft, BaseSounds::GetUsedSet() == nullptr || !BaseSounds::GetUsedSet()->GetTextfile(tft).has_value());
			this->SetWidgetDisabledState(WID_GO_BASE_MUSIC_TEXTFILE + tft, BaseMusic::GetUsedSet() == nullptr || !BaseMusic::GetUsedSet()->GetTextfile(tft).has_value());
		}

		this->SetWidgetsDisabledState(!_network_available, WID_GO_BASE_GRF_CONTENT_DOWNLOAD, WID_GO_BASE_SFX_CONTENT_DOWNLOAD, WID_GO_BASE_MUSIC_CONTENT_DOWNLOAD);
	}
};

static constexpr NWidgetPart _nested_game_options_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(WWT_TEXTBTN, COLOUR_YELLOW, WID_GO_TAB_GENERAL),  SetMinimalTextLines(2, 0), SetStringTip(STR_GAME_OPTIONS_TAB_GENERAL, STR_GAME_OPTIONS_TAB_GENERAL_TOOLTIP), SetFill(1, 0),
			NWidget(WWT_TEXTBTN, COLOUR_YELLOW, WID_GO_TAB_GRAPHICS), SetMinimalTextLines(2, 0), SetStringTip(STR_GAME_OPTIONS_TAB_GRAPHICS, STR_GAME_OPTIONS_TAB_GRAPHICS_TOOLTIP), SetFill(1, 0),
			NWidget(WWT_TEXTBTN, COLOUR_YELLOW, WID_GO_TAB_SOUND),    SetMinimalTextLines(2, 0), SetStringTip(STR_GAME_OPTIONS_TAB_SOUND, STR_GAME_OPTIONS_TAB_SOUND_TOOLTIP), SetFill(1, 0),
			NWidget(WWT_TEXTBTN, COLOUR_YELLOW, WID_GO_TAB_SOCIAL),   SetMinimalTextLines(2, 0), SetStringTip(STR_GAME_OPTIONS_TAB_SOCIAL, STR_GAME_OPTIONS_TAB_SOCIAL_TOOLTIP), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY),
		NWidget(NWID_SELECTION, INVALID_COLOUR, WID_GO_TAB_SELECTION),
			/* General tab */
			NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.sparse), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0),
				NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_LANGUAGE),
					NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_GO_LANG_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_JUST_RAW_STRING, STR_GAME_OPTIONS_LANGUAGE_TOOLTIP), SetFill(1, 0),
				EndContainer(),

				NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_AUTOSAVE_FRAME),
					NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_GO_AUTOSAVE_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_JUST_STRING2, STR_GAME_OPTIONS_AUTOSAVE_DROPDOWN_TOOLTIP), SetFill(1, 0),
				EndContainer(),

				NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_CURRENCY_UNITS_FRAME),
					NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_GO_CURRENCY_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_JUST_STRING2, STR_GAME_OPTIONS_CURRENCY_UNITS_DROPDOWN_TOOLTIP), SetFill(1, 0),
				EndContainer(),

				NWidget(NWID_SELECTION, INVALID_COLOUR, WID_GO_SURVEY_SEL),
					NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_PARTICIPATE_SURVEY_FRAME), SetPIP(0, WidgetDimensions::unscaled.vsep_sparse, 0),
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_PARTICIPATE_SURVEY),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_SURVEY_PARTICIPATE_BUTTON), SetAspect(WidgetDimensions::ASPECT_SETTINGS_BUTTON), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_PARTICIPATE_SURVEY_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_SURVEY_PREVIEW_BUTTON), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_GAME_OPTIONS_PARTICIPATE_SURVEY_PREVIEW, STR_GAME_OPTIONS_PARTICIPATE_SURVEY_PREVIEW_TOOLTIP),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_SURVEY_LINK_BUTTON), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_GAME_OPTIONS_PARTICIPATE_SURVEY_LINK, STR_GAME_OPTIONS_PARTICIPATE_SURVEY_LINK_TOOLTIP),
						EndContainer(),
					EndContainer(),
				EndContainer(),
			EndContainer(),

			/* Graphics tab */
			NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.sparse), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0),
				NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_GUI_SCALE_FRAME),
					NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
						NWidget(WWT_EMPTY, INVALID_COLOUR, WID_GO_GUI_SCALE), SetMinimalSize(67, 0), SetMinimalTextLines(1, 12 + WidgetDimensions::unscaled.vsep_normal, FS_SMALL), SetFill(0, 0), SetToolTip(STR_GAME_OPTIONS_GUI_SCALE_TOOLTIP),
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_GUI_SCALE_AUTO),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_GUI_SCALE_AUTO), SetAspect(WidgetDimensions::ASPECT_SETTINGS_BUTTON), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_GUI_SCALE_AUTO_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_GUI_SCALE_BEVELS),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_GUI_SCALE_BEVEL_BUTTON), SetAspect(WidgetDimensions::ASPECT_SETTINGS_BUTTON), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_GUI_SCALE_BEVELS_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_GUI_SCALE_MAIN_TOOLBAR, STR_NULL),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_GUI_SCALE_MAIN_TOOLBAR), SetAspect(WidgetDimensions::ASPECT_SETTINGS_BUTTON), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_GUI_SCALE_MAIN_TOOLBAR_TOOLTIP),
						EndContainer(),
#ifdef HAS_TRUETYPE_FONT
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_GUI_FONT_SPRITE),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_GUI_FONT_SPRITE), SetAspect(WidgetDimensions::ASPECT_SETTINGS_BUTTON), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_GUI_FONT_SPRITE_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_GUI_FONT_AA),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_GUI_FONT_AA), SetAspect(WidgetDimensions::ASPECT_SETTINGS_BUTTON), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_GUI_FONT_AA_TOOLTIP),
						EndContainer(),
#endif /* HAS_TRUETYPE_FONT */
					EndContainer(),
				EndContainer(),

				NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_GRAPHICS),
					NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_RESOLUTION),
							NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_GO_RESOLUTION_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_JUST_STRING2, STR_GAME_OPTIONS_RESOLUTION_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_REFRESH_RATE),
							NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_GO_REFRESH_RATE_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_GAME_OPTIONS_REFRESH_RATE_ITEM, STR_GAME_OPTIONS_REFRESH_RATE_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_FULLSCREEN),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_FULLSCREEN_BUTTON), SetAspect(WidgetDimensions::ASPECT_SETTINGS_BUTTON), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_FULLSCREEN_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_VIDEO_ACCELERATION),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_VIDEO_ACCEL_BUTTON), SetAspect(WidgetDimensions::ASPECT_SETTINGS_BUTTON), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_VIDEO_ACCELERATION_TOOLTIP),
						EndContainer(),
#ifndef __APPLE__
						NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
							NWidget(WWT_TEXT, INVALID_COLOUR), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_GAME_OPTIONS_VIDEO_VSYNC),
							NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_GO_VIDEO_VSYNC_BUTTON), SetAspect(WidgetDimensions::ASPECT_SETTINGS_BUTTON), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_VIDEO_VSYNC_TOOLTIP),
						EndContainer(),
#endif
						NWidget(NWID_HORIZONTAL),
							NWidget(WWT_EMPTY, INVALID_COLOUR, WID_GO_VIDEO_DRIVER_INFO), SetMinimalTextLines(1, 0), SetFill(1, 0),
						EndContainer(),
					EndContainer(),
				EndContainer(),

				NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_BASE_GRF), SetPIP(0, WidgetDimensions::unscaled.vsep_sparse, 0), SetFill(1, 0),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
						NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_GO_BASE_GRF_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_JUST_RAW_STRING, STR_GAME_OPTIONS_BASE_GRF_TOOLTIP), SetFill(1, 0),
						NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_GRF_PARAMETERS), SetStringTip(STR_NEWGRF_SETTINGS_SET_PARAMETERS),
						NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_GRF_CONTENT_DOWNLOAD), SetStringTip(STR_GAME_OPTIONS_ONLINE_CONTENT, STR_GAME_OPTIONS_ONLINE_CONTENT_TOOLTIP),
					EndContainer(),
					NWidget(WWT_TEXT, INVALID_COLOUR, WID_GO_BASE_GRF_DESCRIPTION), SetMinimalSize(200, 0), SetStringTip(STR_EMPTY, STR_GAME_OPTIONS_BASE_GRF_DESCRIPTION_TOOLTIP), SetFill(1, 0),
					NWidget(NWID_VERTICAL),
						NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_GRF_OPEN_URL), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_CONTENT_OPEN_URL, STR_CONTENT_OPEN_URL_TOOLTIP),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_GRF_TEXTFILE + TFT_README), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TEXTFILE_VIEW_README, STR_TEXTFILE_VIEW_README_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_GRF_TEXTFILE + TFT_CHANGELOG), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TEXTFILE_VIEW_CHANGELOG, STR_TEXTFILE_VIEW_CHANGELOG_TOOLTIP),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_GRF_TEXTFILE + TFT_LICENSE), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TEXTFILE_VIEW_LICENCE, STR_TEXTFILE_VIEW_LICENCE_TOOLTIP),
						EndContainer(),
					EndContainer(),
				EndContainer(),
			EndContainer(),

			/* Sound/Music tab */
			NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.sparse), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0),
				NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_VOLUME), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
						NWidget(WWT_TEXT, INVALID_COLOUR, WID_GO_TEXT_SFX_VOLUME), SetMinimalSize(0, 12), SetStringTip(STR_GAME_OPTIONS_SFX_VOLUME),
						NWidget(WWT_EMPTY, INVALID_COLOUR, WID_GO_BASE_SFX_VOLUME), SetMinimalSize(67, 0), SetMinimalTextLines(1, 12 + WidgetDimensions::unscaled.vsep_normal, FS_SMALL), SetFill(1, 0), SetToolTip(STR_MUSIC_TOOLTIP_DRAG_SLIDERS_TO_SET_MUSIC),
					EndContainer(),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
						NWidget(WWT_TEXT, INVALID_COLOUR, WID_GO_TEXT_MUSIC_VOLUME), SetMinimalSize(0, 12), SetStringTip(STR_GAME_OPTIONS_MUSIC_VOLUME),
						NWidget(WWT_EMPTY, INVALID_COLOUR, WID_GO_BASE_MUSIC_VOLUME), SetMinimalSize(67, 0), SetMinimalTextLines(1, 12 + WidgetDimensions::unscaled.vsep_normal, FS_SMALL), SetFill(1, 0), SetToolTip(STR_MUSIC_TOOLTIP_DRAG_SLIDERS_TO_SET_MUSIC),
					EndContainer(),
				EndContainer(),

				NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_BASE_SFX), SetPIP(0, WidgetDimensions::unscaled.vsep_sparse, 0),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
						NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_GO_BASE_SFX_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_JUST_RAW_STRING, STR_GAME_OPTIONS_BASE_SFX_TOOLTIP), SetFill(1, 0),
						NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_SFX_CONTENT_DOWNLOAD), SetStringTip(STR_GAME_OPTIONS_ONLINE_CONTENT, STR_GAME_OPTIONS_ONLINE_CONTENT_TOOLTIP),
					EndContainer(),
					NWidget(WWT_EMPTY, INVALID_COLOUR, WID_GO_BASE_SFX_DESCRIPTION), SetMinimalSize(200, 0), SetMinimalTextLines(1, 0), SetToolTip(STR_GAME_OPTIONS_BASE_SFX_DESCRIPTION_TOOLTIP), SetFill(1, 0),
					NWidget(NWID_VERTICAL),
						NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_SFX_OPEN_URL), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_CONTENT_OPEN_URL, STR_CONTENT_OPEN_URL_TOOLTIP),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_SFX_TEXTFILE + TFT_README), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TEXTFILE_VIEW_README, STR_TEXTFILE_VIEW_README_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_SFX_TEXTFILE + TFT_CHANGELOG), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TEXTFILE_VIEW_CHANGELOG, STR_TEXTFILE_VIEW_CHANGELOG_TOOLTIP),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_SFX_TEXTFILE + TFT_LICENSE), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TEXTFILE_VIEW_LICENCE, STR_TEXTFILE_VIEW_LICENCE_TOOLTIP),
						EndContainer(),
					EndContainer(),
				EndContainer(),

				NWidget(WWT_FRAME, COLOUR_GREY), SetStringTip(STR_GAME_OPTIONS_BASE_MUSIC), SetPIP(0, WidgetDimensions::unscaled.vsep_sparse, 0),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
						NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_GO_BASE_MUSIC_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_JUST_RAW_STRING, STR_GAME_OPTIONS_BASE_MUSIC_TOOLTIP), SetFill(1, 0),
						NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_MUSIC_CONTENT_DOWNLOAD), SetStringTip(STR_GAME_OPTIONS_ONLINE_CONTENT, STR_GAME_OPTIONS_ONLINE_CONTENT_TOOLTIP),
					EndContainer(),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
						NWidget(WWT_EMPTY, INVALID_COLOUR, WID_GO_BASE_MUSIC_DESCRIPTION), SetMinimalSize(200, 0), SetMinimalTextLines(1, 0), SetToolTip(STR_GAME_OPTIONS_BASE_MUSIC_DESCRIPTION_TOOLTIP), SetFill(1, 0),
						NWidget(NWID_VERTICAL), SetPIPRatio(0, 0, 1),
							NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_GO_BASE_MUSIC_JUKEBOX), SetMinimalSize(22, 22), SetSpriteTip(SPR_IMG_MUSIC, STR_TOOLBAR_TOOLTIP_SHOW_SOUND_MUSIC_WINDOW),
						EndContainer(),
					EndContainer(),
					NWidget(NWID_VERTICAL),
						NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_MUSIC_OPEN_URL), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_CONTENT_OPEN_URL, STR_CONTENT_OPEN_URL_TOOLTIP),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_MUSIC_TEXTFILE + TFT_README), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TEXTFILE_VIEW_README, STR_TEXTFILE_VIEW_README_TOOLTIP),
						EndContainer(),
						NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_MUSIC_TEXTFILE + TFT_CHANGELOG), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TEXTFILE_VIEW_CHANGELOG, STR_TEXTFILE_VIEW_CHANGELOG_TOOLTIP),
							NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_GO_BASE_MUSIC_TEXTFILE + TFT_LICENSE), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_TEXTFILE_VIEW_LICENCE, STR_TEXTFILE_VIEW_LICENCE_TOOLTIP),
						EndContainer(),
					EndContainer(),
				EndContainer(),
			EndContainer(),

			/* Social tab */
			NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.sparse), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0),
				NWidgetFunction(MakeNWidgetSocialPlugins),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _game_options_desc(__FILE__, __LINE__,
	WDP_CENTER, nullptr, 0, 0,
	WC_GAME_OPTIONS, WC_NONE,
	{},
	_nested_game_options_widgets
);

/** Open the game options window. */
void ShowGameOptions()
{
	CloseWindowByClass(WC_GAME_OPTIONS);
	new GameOptionsWindow(_game_options_desc);
}

static int SETTING_HEIGHT = 11;    ///< Height of a single setting in the tree view in pixels

/**
 * Flags for #SettingEntry
 * @note The #SEF_BUTTONS_MASK matches expectations of the formal parameter 'state' of #DrawArrowButtons
 */
enum SettingEntryFlags : uint8_t {
	SEF_LEFT_DEPRESSED  = 0x01, ///< Of a numeric setting entry, the left button is depressed
	SEF_RIGHT_DEPRESSED = 0x02, ///< Of a numeric setting entry, the right button is depressed
	SEF_BUTTONS_MASK = (SEF_LEFT_DEPRESSED | SEF_RIGHT_DEPRESSED), ///< Bit-mask for button flags

	SEF_LAST_FIELD = 0x04, ///< This entry is the last one in a (sub-)page
	SEF_FILTERED   = 0x08, ///< Entry is hidden by the string filter
};

/** How the list of advanced settings is filtered. */
enum RestrictionMode : uint8_t {
	RM_BASIC,                            ///< Display settings associated to the "basic" list.
	RM_ADVANCED,                         ///< Display settings associated to the "advanced" list.
	RM_ALL,                              ///< List all settings regardless of the default/newgame/... values.
	RM_CHANGED_AGAINST_DEFAULT,          ///< Show only settings which are different compared to default values.
	RM_CHANGED_AGAINST_NEW,              ///< Show only settings which are different compared to the user's new game setting values.
	RM_PATCH,                            ///< Show only "patch" settings which are not in vanilla.
	RM_END,                              ///< End for iteration.
};
DECLARE_INCREMENT_DECREMENT_OPERATORS(RestrictionMode)

/** Filter for settings list. */
struct SettingFilter {
	StringFilter string;     ///< Filter string.
	RestrictionMode min_cat; ///< Minimum category needed to display all filtered strings (#RM_BASIC, #RM_ADVANCED, or #RM_ALL).
	bool type_hides;         ///< Whether the type hides filtered strings.
	RestrictionMode mode;    ///< Filter based on category.
	SettingType type;        ///< Filter based on type.
};

/** Data structure describing a single setting in a tab */
struct BaseSettingEntry {
	uint8_t flags; ///< Flags of the setting entry. @see SettingEntryFlags
	uint8_t level; ///< Nesting level of this setting entry

	BaseSettingEntry() : flags(0), level(0) {}
	virtual ~BaseSettingEntry() = default;

	virtual void Init(uint8_t level = 0);
	virtual void FoldAll() {}
	virtual void UnFoldAll() {}
	virtual void ResetAll() = 0;

	/**
	 * Set whether this is the last visible entry of the parent node.
	 * @param last_field Value to set
	 */
	void SetLastField(bool last_field) { if (last_field) SETBITS(this->flags, SEF_LAST_FIELD); else CLRBITS(this->flags, SEF_LAST_FIELD); }

	virtual uint Length() const = 0;
	virtual void GetFoldingState([[maybe_unused]] bool &all_folded, [[maybe_unused]] bool &all_unfolded) const {}
	virtual bool IsVisible(const BaseSettingEntry *item) const;
	virtual BaseSettingEntry *FindEntry(uint row, uint *cur_row);
	virtual uint GetMaxHelpHeight([[maybe_unused]] int maxw) { return 0; }

	/**
	 * Check whether an entry is hidden due to filters
	 * @return true if hidden.
	 */
	bool IsFiltered() const { return (this->flags & SEF_FILTERED) != 0; }

	virtual bool UpdateFilterState(SettingFilter &filter, bool force_visible) = 0;

	virtual uint Draw(GameSettings *settings_ptr, int left, int right, int y, uint first_row, uint max_row, BaseSettingEntry *selected, uint cur_row = 0, uint parent_last = 0) const;

protected:
	virtual void DrawSetting(GameSettings *settings_ptr, int left, int right, int y, bool highlight) const = 0;
};

/** Standard setting */
struct SettingEntry : BaseSettingEntry {
	const char *name;              ///< Name of the setting
	const IntSettingDesc *setting; ///< Setting description of the setting

	SettingEntry(const char *name);

	void Init(uint8_t level = 0) override;
	void ResetAll() override;
	uint Length() const override;
	uint GetMaxHelpHeight(int maxw) override;
	bool UpdateFilterState(SettingFilter &filter, bool force_visible) override;

	void SetButtons(uint8_t new_val);
	bool IsGUIEditable() const;

protected:
	SettingEntry(const IntSettingDesc *setting);
	virtual void DrawSetting(GameSettings *settings_ptr, int left, int right, int y, bool highlight) const override;
	virtual void DrawSettingString(uint left, uint right, int y, bool highlight, int32_t value) const;

private:
	bool IsVisibleByRestrictionMode(RestrictionMode mode) const;
};

/** Cargodist per-cargo setting */
struct CargoDestPerCargoSettingEntry : SettingEntry {
	CargoType cargo;

	CargoDestPerCargoSettingEntry(CargoType cargo, const IntSettingDesc *setting);
	void Init(uint8_t level = 0) override;
	bool UpdateFilterState(SettingFilter &filter, bool force_visible) override;

protected:
	void DrawSettingString(uint left, uint right, int y, bool highlight, int32_t value) const override;
};

/** Conditionally hidden standard setting */
struct ConditionallyHiddenSettingEntry : SettingEntry {
	std::function<bool()> hide_callback;

	ConditionallyHiddenSettingEntry(const char *name, std::function<bool()> hide_callback)
		: SettingEntry(name), hide_callback(hide_callback) {}

	bool UpdateFilterState(SettingFilter &filter, bool force_visible) override;
};

/** Containers for BaseSettingEntry */
struct SettingsContainer {
	typedef std::vector<BaseSettingEntry*> EntryVector;
	EntryVector entries; ///< Settings on this page

	template <typename T>
	T *Add(T *item)
	{
		this->entries.push_back(item);
		return item;
	}

	void Init(uint8_t level = 0);
	void ResetAll();
	void FoldAll();
	void UnFoldAll();

	uint Length() const;
	void GetFoldingState(bool &all_folded, bool &all_unfolded) const;
	bool IsVisible(const BaseSettingEntry *item) const;
	BaseSettingEntry *FindEntry(uint row, uint *cur_row);
	uint GetMaxHelpHeight(int maxw);

	bool UpdateFilterState(SettingFilter &filter, bool force_visible);

	uint Draw(GameSettings *settings_ptr, int left, int right, int y, uint first_row, uint max_row, BaseSettingEntry *selected, uint cur_row = 0, uint parent_last = 0) const;
};

/** Data structure describing one page of settings in the settings window. */
struct SettingsPage : BaseSettingEntry, SettingsContainer {
	StringID title;     ///< Title of the sub-page
	bool folded;        ///< Sub-page is folded (not visible except for its title)
	std::function<bool()> hide_callback; ///< optional callback, returns true if this shouldbe hidden

	SettingsPage(StringID title);

	void Init(uint8_t level = 0) override;
	void ResetAll() override;
	void FoldAll() override;
	void UnFoldAll() override;

	uint Length() const override;
	void GetFoldingState(bool &all_folded, bool &all_unfolded) const override;
	bool IsVisible(const BaseSettingEntry *item) const override;
	BaseSettingEntry *FindEntry(uint row, uint *cur_row) override;
	uint GetMaxHelpHeight(int maxw) override { return SettingsContainer::GetMaxHelpHeight(maxw); }

	bool UpdateFilterState(SettingFilter &filter, bool force_visible) override;

	uint Draw(GameSettings *settings_ptr, int left, int right, int y, uint first_row, uint max_row, BaseSettingEntry *selected, uint cur_row = 0, uint parent_last = 0) const override;

protected:
	void DrawSetting(GameSettings *settings_ptr, int left, int right, int y, bool highlight) const override;
};

/* == BaseSettingEntry methods == */

/**
 * Initialization of a setting entry
 * @param level      Page nesting level of this entry
 */
void BaseSettingEntry::Init(uint8_t level)
{
	this->level = level;
}

/**
 * Check whether an entry is visible and not folded or filtered away.
 * Note: This does not consider the scrolling range; it might still require scrolling to make the setting really visible.
 * @param item Entry to search for.
 * @return true if entry is visible.
 */
bool BaseSettingEntry::IsVisible(const BaseSettingEntry *item) const
{
	if (this->IsFiltered()) return false;
	return this == item;
}

/**
 * Find setting entry at row \a row_num
 * @param row_num Index of entry to return
 * @param cur_row Current row number
 * @return The requested setting entry or \c nullptr if it not found (folded or filtered)
 */
BaseSettingEntry *BaseSettingEntry::FindEntry(uint row_num, uint *cur_row)
{
	if (this->IsFiltered()) return nullptr;
	if (row_num == *cur_row) return this;
	(*cur_row)++;
	return nullptr;
}

/**
 * Draw a row in the settings panel.
 *
 * The scrollbar uses rows of the page, while the page data structure is a tree of #SettingsPage and #SettingEntry objects.
 * As a result, the drawing routing traverses the tree from top to bottom, counting rows in \a cur_row until it reaches \a first_row.
 * Then it enables drawing rows while traversing until \a max_row is reached, at which point drawing is terminated.
 *
 * The \a parent_last parameter ensures that the vertical lines at the left are
 * only drawn when another entry follows, that it prevents output like
 * \verbatim
 *  |-- setting
 *  |-- (-) - Title
 *  |    |-- setting
 *  |    |-- setting
 * \endverbatim
 * The left-most vertical line is not wanted. It is prevented by setting the
 * appropriate bit in the \a parent_last parameter.
 *
 * @param settings_ptr Pointer to current values of all settings
 * @param left         Left-most position in window/panel to start drawing \a first_row
 * @param right        Right-most x position to draw strings at.
 * @param y            Upper-most position in window/panel to start drawing \a first_row
 * @param first_row    First row number to draw
 * @param max_row      Row-number to stop drawing (the row-number of the row below the last row to draw)
 * @param selected     Selected entry by the user.
 * @param cur_row      Current row number (internal variable)
 * @param parent_last  Last-field booleans of parent page level (page level \e i sets bit \e i to 1 if it is its last field)
 * @return Row number of the next row to draw
 */
uint BaseSettingEntry::Draw(GameSettings *settings_ptr, int left, int right, int y, uint first_row, uint max_row, BaseSettingEntry *selected, uint cur_row, uint parent_last) const
{
	if (this->IsFiltered()) return cur_row;
	if (cur_row >= max_row) return cur_row;

	bool rtl = _current_text_dir == TD_RTL;
	int offset = (rtl ? -(int)_circle_size.width : (int)_circle_size.width) / 2;
	int level_width = rtl ? -WidgetDimensions::scaled.hsep_indent : WidgetDimensions::scaled.hsep_indent;

	int x = rtl ? right : left;
	if (cur_row >= first_row) {
		int colour = GetColourGradient(COLOUR_ORANGE, SHADE_NORMAL);
		y += (cur_row - first_row) * SETTING_HEIGHT; // Compute correct y start position

		/* Draw vertical for parent nesting levels */
		for (uint lvl = 0; lvl < this->level; lvl++) {
			if (!HasBit(parent_last, lvl)) GfxDrawLine(x + offset, y, x + offset, y + SETTING_HEIGHT - 1, colour);
			x += level_width;
		}
		/* draw own |- prefix */
		int halfway_y = y + SETTING_HEIGHT / 2;
		int bottom_y = (flags & SEF_LAST_FIELD) ? halfway_y : y + SETTING_HEIGHT - 1;
		GfxDrawLine(x + offset, y, x + offset, bottom_y, colour);
		/* Small horizontal line from the last vertical line */
		GfxDrawLine(x + offset, halfway_y, x + level_width - (rtl ? -WidgetDimensions::scaled.hsep_normal : WidgetDimensions::scaled.hsep_normal), halfway_y, colour);
		x += level_width;

		this->DrawSetting(settings_ptr, rtl ? left : x, rtl ? x : right, y, this == selected);
	}
	cur_row++;

	return cur_row;
}

/* == SettingEntry methods == */

/**
 * Constructor for a single setting in the 'advanced settings' window
 * @param name Name of the setting in the setting table
 */
SettingEntry::SettingEntry(const char *name)
{
	this->name = name;
	this->setting = nullptr;
}

SettingEntry::SettingEntry(const IntSettingDesc *setting)
{
	this->name = nullptr;
	this->setting = setting;
}

/**
 * Initialization of a setting entry
 * @param level      Page nesting level of this entry
 */
void SettingEntry::Init(uint8_t level)
{
	BaseSettingEntry::Init(level);
	const SettingDesc *st = GetSettingFromName(this->name);
	assert_msg(st != nullptr, "name: {}", this->name);
	this->setting = st->AsIntSetting();
}

/* Sets the given setting entry to its default value */
void SettingEntry::ResetAll()
{
	SetSettingValue(this->setting, this->setting->GetDefaultValue());
}

/**
 * Set the button-depressed flags (#SEF_LEFT_DEPRESSED and #SEF_RIGHT_DEPRESSED) to a specified value
 * @param new_val New value for the button flags
 * @see SettingEntryFlags
 */
void SettingEntry::SetButtons(uint8_t new_val)
{
	assert((new_val & ~SEF_BUTTONS_MASK) == 0); // Should not touch any flags outside the buttons
	this->flags = (this->flags & ~SEF_BUTTONS_MASK) | new_val;
}

/** Return number of rows needed to display the (filtered) entry */
uint SettingEntry::Length() const
{
	return this->IsFiltered() ? 0 : 1;
}

/**
 * Get the biggest height of the help text(s), if the width is at least \a maxw. Help text gets wrapped if needed.
 * @param maxw Maximal width of a line help text.
 * @return Biggest height needed to display any help text of this node (and its descendants).
 */
uint SettingEntry::GetMaxHelpHeight(int maxw)
{
	return GetStringHeight(this->setting->GetHelp(), maxw);
}

bool SettingEntry::IsGUIEditable() const
{
	bool editable = this->setting->IsEditable();
	if (editable && this->setting->guiproc != nullptr) {
		SettingOnGuiCtrlData data;
		data.type = SOGCT_GUI_DISABLE;
		data.val = 0;
		if (this->setting->guiproc(data)) {
			editable = (data.val == 0);
		}
	}
	return editable;
}

/**
 * Checks whether an entry shall be made visible based on the restriction mode.
 * @param mode The current status of the restriction drop down box.
 * @return true if the entry shall be visible.
 */
bool SettingEntry::IsVisibleByRestrictionMode(RestrictionMode mode) const
{
	/* There shall not be any restriction, i.e. all settings shall be visible. */
	if (mode == RM_ALL) return true;

	const IntSettingDesc *sd = this->setting;

	if (mode == RM_BASIC) return (this->setting->cat & SC_BASIC_LIST) != 0;
	if (mode == RM_ADVANCED) return (this->setting->cat & SC_ADVANCED_LIST) != 0;
	if (mode == RM_PATCH) return this->setting->flags.Test(SettingFlag::Patch);

	/* Read the current value. */
	const void *object = ResolveObject(&GetGameSettings(), sd);
	int64_t current_value = sd->Read(object);
	int64_t filter_value;

	if (mode == RM_CHANGED_AGAINST_DEFAULT) {
		/* This entry shall only be visible, if the value deviates from its default value. */

		/* Read the default value. */
		filter_value = sd->GetDefaultValue();
	} else {
		assert(mode == RM_CHANGED_AGAINST_NEW);
		/* This entry shall only be visible, if the value deviates from
		 * its value is used when starting a new game. */

		/* Make sure we're not comparing the new game settings against itself. */
		assert(&GetGameSettings() != &_settings_newgame);

		/* Read the new game's value. */
		filter_value = sd->Read(ResolveObject(&_settings_newgame, sd));
	}

	return current_value != filter_value;
}

/**
 * Update the filter state.
 * @param filter Filter
 * @param force_visible Whether to force all items visible, no matter what (due to filter text; not affected by restriction drop down box).
 * @return true if item remains visible
 */
bool SettingEntry::UpdateFilterState(SettingFilter &filter, bool force_visible)
{
	if (this->setting->flags.Test(SettingFlag::NoNewgame) && _game_mode == GM_MENU) {
		SETBITS(this->flags, SEF_FILTERED);
		return false;
	}
	CLRBITS(this->flags, SEF_FILTERED);

	bool visible = true;

	const IntSettingDesc *sd = this->setting;
	if (!force_visible && !filter.string.IsEmpty()) {
		/* Process the search text filter for this item. */
		filter.string.ResetState();

		SetDParam(0, STR_EMPTY);
		filter.string.AddLine(sd->GetTitle());
		filter.string.AddLine(sd->GetHelp());

		visible = filter.string.GetState();
	}

	if (visible) {
		if (filter.type != ST_ALL && sd->GetType() != filter.type) {
			filter.type_hides = true;
			visible = false;
		}
		if (!this->IsVisibleByRestrictionMode(filter.mode)) {
			if (filter.mode == RM_PATCH) filter.min_cat = RM_ALL;
			while (filter.min_cat < RM_ALL && (filter.min_cat == filter.mode || !this->IsVisibleByRestrictionMode(filter.min_cat))) filter.min_cat++;
			visible = false;
		}
	}

	if (!visible) SETBITS(this->flags, SEF_FILTERED);
	return visible;
}

static const void *ResolveObject(const GameSettings *settings_ptr, const IntSettingDesc *sd)
{
	if (sd->flags.Test(SettingFlag::PerCompany)) {
		if (Company::IsValidID(_local_company) && _game_mode != GM_MENU) {
			return &Company::Get(_local_company)->settings;
		}
		return &_settings_client.company;
	}
	return settings_ptr;
}

/**
 * Function to draw setting value (button + text + current value)
 * @param settings_ptr Pointer to current values of all settings
 * @param left         Left-most position in window/panel to start drawing
 * @param right        Right-most position in window/panel to draw
 * @param y            Upper-most position in window/panel to start drawing
 * @param highlight    Highlight entry.
 */
void SettingEntry::DrawSetting(GameSettings *settings_ptr, int left, int right, int y, bool highlight) const
{
	const IntSettingDesc *sd = this->setting;
	int state = this->flags & SEF_BUTTONS_MASK;

	bool rtl = _current_text_dir == TD_RTL;
	uint buttons_left = rtl ? right + 1 - SETTING_BUTTON_WIDTH : left;
	uint text_left  = left + (rtl ? 0 : SETTING_BUTTON_WIDTH + WidgetDimensions::scaled.hsep_wide);
	uint text_right = right - (rtl ? SETTING_BUTTON_WIDTH + WidgetDimensions::scaled.hsep_wide : 0);
	uint button_y = y + (SETTING_HEIGHT - SETTING_BUTTON_HEIGHT) / 2;

	/* We do not allow changes of some items when we are a client in a networkgame */
	bool editable = this->IsGUIEditable();

	auto [min_val, max_val] = sd->GetRange();
	int32_t value = sd->Read(ResolveObject(settings_ptr, sd));
	if (sd->IsBoolSetting()) {
		/* Draw checkbox for boolean-value either on/off */
		DrawBoolButton(buttons_left, button_y, value != 0, editable);
	} else if (sd->flags.Any({SettingFlag::GuiDropdown, SettingFlag::Enum})) {
		/* Draw [v] button for settings of an enum-type */
		DrawDropDownButton(buttons_left, button_y, COLOUR_YELLOW, state != 0, editable);
	} else {
		/* Draw [<][>] boxes for settings of an integer-type */
		DrawArrowButtons(buttons_left, button_y, COLOUR_YELLOW, state,
				editable && value != (sd->flags.Test(SettingFlag::GuiZeroIsSpecial) ? 0 : min_val), editable && static_cast<uint32_t>(value) != max_val);
	}
	this->DrawSettingString(text_left, text_right, y + (SETTING_HEIGHT - GetCharacterHeight(FS_NORMAL)) / 2, highlight, value);
}

void SettingEntry::DrawSettingString(uint left, uint right, int y, bool highlight, int32_t value) const
{
	const IntSettingDesc *sd = this->setting;
	auto [param1, param2] = sd->GetValueParams(value);
	int edge = DrawString(left, right, y, GetString(sd->GetTitle(), STR_CONFIG_SETTING_VALUE, param1, param2), highlight ? TC_WHITE : TC_LIGHT_BLUE);

	if (this->setting->guiproc != nullptr && edge != 0) {
		SettingOnGuiCtrlData data;
		data.type = SOGCT_GUI_SPRITE;
		data.val = value;
		if (this->setting->guiproc(data)) {
			SpriteID sprite = (SpriteID)data.output;
			const Dimension warning_dimensions = GetSpriteSize(sprite);
			if ((int)warning_dimensions.height <= SETTING_HEIGHT) {
				DrawSprite(sprite, 0, (_current_text_dir == TD_RTL) ? edge - warning_dimensions.width - 5 : edge + 5,
						y + (((int)GetCharacterHeight(FS_NORMAL) - (int)warning_dimensions.height) / 2));
			}
		}
	}
}

/* == CargoDestPerCargoSettingEntry methods == */

CargoDestPerCargoSettingEntry::CargoDestPerCargoSettingEntry(CargoType cargo, const IntSettingDesc *setting)
	: SettingEntry(setting), cargo(cargo) {}

void CargoDestPerCargoSettingEntry::Init(uint8_t level)
{
	BaseSettingEntry::Init(level);
}

void CargoDestPerCargoSettingEntry::DrawSettingString(uint left, uint right, int y, bool highlight, int32_t value) const
{
	assert(this->setting->str == STR_CONFIG_SETTING_DISTRIBUTION_PER_CARGO);
	auto [param1, param2] = this->setting->GetValueParams(value);
	std::string str = GetString(STR_CONFIG_SETTING_DISTRIBUTION_PER_CARGO_PARAM, CargoSpec::Get(this->cargo)->name, STR_CONFIG_SETTING_VALUE, param1, param2);
	DrawString(left, right, y, str, highlight ? TC_WHITE : TC_LIGHT_BLUE);
}

bool CargoDestPerCargoSettingEntry::UpdateFilterState(SettingFilter &filter, bool force_visible)
{
	if (!HasBit(_cargo_mask, this->cargo)) {
		SETBITS(this->flags, SEF_FILTERED);
		return false;
	} else {
		return SettingEntry::UpdateFilterState(filter, force_visible);
	}
}

bool ConditionallyHiddenSettingEntry::UpdateFilterState(SettingFilter &filter, bool force_visible)
{
	if (this->hide_callback && this->hide_callback()) {
		SETBITS(this->flags, SEF_FILTERED);
		return false;
	} else {
		return SettingEntry::UpdateFilterState(filter, force_visible);
	}
}

/* == SettingsContainer methods == */

/**
 * Initialization of an entire setting page
 * @param level Nesting level of this page (internal variable, do not provide a value for it when calling)
 */
void SettingsContainer::Init(uint8_t level)
{
	for (auto &it : this->entries) {
		it->Init(level);
	}
}

/** Resets all settings to their default values */
void SettingsContainer::ResetAll()
{
	for (auto settings_entry : this->entries) {
		settings_entry->ResetAll();
	}
}

/** Recursively close all folds of sub-pages */
void SettingsContainer::FoldAll()
{
	for (auto &it : this->entries) {
		it->FoldAll();
	}
}

/** Recursively open all folds of sub-pages */
void SettingsContainer::UnFoldAll()
{
	for (auto &it : this->entries) {
		it->UnFoldAll();
	}
}

/**
 * Recursively accumulate the folding state of the tree.
 * @param[in,out] all_folded Set to false, if one entry is not folded.
 * @param[in,out] all_unfolded Set to false, if one entry is folded.
 */
void SettingsContainer::GetFoldingState(bool &all_folded, bool &all_unfolded) const
{
	for (auto &it : this->entries) {
		it->GetFoldingState(all_folded, all_unfolded);
	}
}

/**
 * Update the filter state.
 * @param filter Filter
 * @param force_visible Whether to force all items visible, no matter what
 * @return true if item remains visible
 */
bool SettingsContainer::UpdateFilterState(SettingFilter &filter, bool force_visible)
{
	bool visible = false;
	bool first_visible = true;
	for (EntryVector::reverse_iterator it = this->entries.rbegin(); it != this->entries.rend(); ++it) {
		visible |= (*it)->UpdateFilterState(filter, force_visible);
		(*it)->SetLastField(first_visible);
		if (visible && first_visible) first_visible = false;
	}
	return visible;
}


/**
 * Check whether an entry is visible and not folded or filtered away.
 * Note: This does not consider the scrolling range; it might still require scrolling to make the setting really visible.
 * @param item Entry to search for.
 * @return true if entry is visible.
 */
bool SettingsContainer::IsVisible(const BaseSettingEntry *item) const
{
	for (const auto &it : this->entries) {
		if (it->IsVisible(item)) return true;
	}
	return false;
}

/** Return number of rows needed to display the whole page */
uint SettingsContainer::Length() const
{
	uint length = 0;
	for (const auto &it : this->entries) {
		length += it->Length();
	}
	return length;
}

/**
 * Find the setting entry at row number \a row_num
 * @param row_num Index of entry to return
 * @param cur_row Variable used for keeping track of the current row number. Should point to memory initialized to \c 0 when first called.
 * @return The requested setting entry or \c nullptr if it does not exist
 */
BaseSettingEntry *SettingsContainer::FindEntry(uint row_num, uint *cur_row)
{
	BaseSettingEntry *pe = nullptr;
	for (const auto &it : this->entries) {
		pe = it->FindEntry(row_num, cur_row);
		if (pe != nullptr) {
			break;
		}
	}
	return pe;
}

/**
 * Get the biggest height of the help texts, if the width is at least \a maxw. Help text gets wrapped if needed.
 * @param maxw Maximal width of a line help text.
 * @return Biggest height needed to display any help text of this (sub-)tree.
 */
uint SettingsContainer::GetMaxHelpHeight(int maxw)
{
	uint biggest = 0;
	for (const auto &it : this->entries) {
		biggest = std::max(biggest, it->GetMaxHelpHeight(maxw));
	}
	return biggest;
}

/**
 * Draw a row in the settings panel.
 *
 * @param settings_ptr Pointer to current values of all settings
 * @param left         Left-most position in window/panel to start drawing \a first_row
 * @param right        Right-most x position to draw strings at.
 * @param y            Upper-most position in window/panel to start drawing \a first_row
 * @param first_row    First row number to draw
 * @param max_row      Row-number to stop drawing (the row-number of the row below the last row to draw)
 * @param selected     Selected entry by the user.
 * @param cur_row      Current row number (internal variable)
 * @param parent_last  Last-field booleans of parent page level (page level \e i sets bit \e i to 1 if it is its last field)
 * @return Row number of the next row to draw
 */
uint SettingsContainer::Draw(GameSettings *settings_ptr, int left, int right, int y, uint first_row, uint max_row, BaseSettingEntry *selected, uint cur_row, uint parent_last) const
{
	for (const auto &it : this->entries) {
		cur_row = it->Draw(settings_ptr, left, right, y, first_row, max_row, selected, cur_row, parent_last);
		if (cur_row >= max_row) break;
	}
	return cur_row;
}

/* == SettingsPage methods == */

/**
 * Constructor for a sub-page in the 'advanced settings' window
 * @param title Title of the sub-page
 */
SettingsPage::SettingsPage(StringID title)
{
	this->title = title;
	this->folded = true;
}

/**
 * Initialization of an entire setting page
 * @param level Nesting level of this page (internal variable, do not provide a value for it when calling)
 */
void SettingsPage::Init(uint8_t level)
{
	BaseSettingEntry::Init(level);
	SettingsContainer::Init(level + 1);
}

/** Resets all settings to their default values */
void SettingsPage::ResetAll()
{
	for (auto settings_entry : this->entries) {
		settings_entry->ResetAll();
	}
}

/** Recursively close all (filtered) folds of sub-pages */
void SettingsPage::FoldAll()
{
	if (this->IsFiltered()) return;
	this->folded = true;

	SettingsContainer::FoldAll();
}

/** Recursively open all (filtered) folds of sub-pages */
void SettingsPage::UnFoldAll()
{
	if (this->IsFiltered()) return;
	this->folded = false;

	SettingsContainer::UnFoldAll();
}

/**
 * Recursively accumulate the folding state of the (filtered) tree.
 * @param[in,out] all_folded Set to false, if one entry is not folded.
 * @param[in,out] all_unfolded Set to false, if one entry is folded.
 */
void SettingsPage::GetFoldingState(bool &all_folded, bool &all_unfolded) const
{
	if (this->IsFiltered()) return;

	if (this->folded) {
		all_unfolded = false;
	} else {
		all_folded = false;
	}

	SettingsContainer::GetFoldingState(all_folded, all_unfolded);
}

/**
 * Update the filter state.
 * @param filter Filter
 * @param force_visible Whether to force all items visible, no matter what (due to filter text; not affected by restriction drop down box).
 * @return true if item remains visible
 */
bool SettingsPage::UpdateFilterState(SettingFilter &filter, bool force_visible)
{
	if (!force_visible && !filter.string.IsEmpty()) {
		filter.string.ResetState();
		filter.string.AddLine(this->title);
		force_visible = filter.string.GetState();
	}

	bool visible = SettingsContainer::UpdateFilterState(filter, force_visible);
	if (this->hide_callback && this->hide_callback()) visible = false;
	if (visible) {
		CLRBITS(this->flags, SEF_FILTERED);
	} else {
		SETBITS(this->flags, SEF_FILTERED);
	}
	return visible;
}

/**
 * Check whether an entry is visible and not folded or filtered away.
 * Note: This does not consider the scrolling range; it might still require scrolling to make the setting really visible.
 * @param item Entry to search for.
 * @return true if entry is visible.
 */
bool SettingsPage::IsVisible(const BaseSettingEntry *item) const
{
	if (this->IsFiltered()) return false;
	if (this == item) return true;
	if (this->folded) return false;

	return SettingsContainer::IsVisible(item);
}

/** Return number of rows needed to display the (filtered) entry */
uint SettingsPage::Length() const
{
	if (this->IsFiltered()) return 0;
	if (this->folded) return 1; // Only displaying the title

	return 1 + SettingsContainer::Length();
}

/**
 * Find setting entry at row \a row_num
 * @param row_num Index of entry to return
 * @param cur_row Current row number
 * @return The requested setting entry or \c nullptr if it not found (folded or filtered)
 */
BaseSettingEntry *SettingsPage::FindEntry(uint row_num, uint *cur_row)
{
	if (this->IsFiltered()) return nullptr;
	if (row_num == *cur_row) return this;
	(*cur_row)++;
	if (this->folded) return nullptr;

	return SettingsContainer::FindEntry(row_num, cur_row);
}

/**
 * Draw a row in the settings panel.
 *
 * @param settings_ptr Pointer to current values of all settings
 * @param left         Left-most position in window/panel to start drawing \a first_row
 * @param right        Right-most x position to draw strings at.
 * @param y            Upper-most position in window/panel to start drawing \a first_row
 * @param first_row    First row number to draw
 * @param max_row      Row-number to stop drawing (the row-number of the row below the last row to draw)
 * @param selected     Selected entry by the user.
 * @param cur_row      Current row number (internal variable)
 * @param parent_last  Last-field booleans of parent page level (page level \e i sets bit \e i to 1 if it is its last field)
 * @return Row number of the next row to draw
 */
uint SettingsPage::Draw(GameSettings *settings_ptr, int left, int right, int y, uint first_row, uint max_row, BaseSettingEntry *selected, uint cur_row, uint parent_last) const
{
	if (this->IsFiltered()) return cur_row;
	if (cur_row >= max_row) return cur_row;

	cur_row = BaseSettingEntry::Draw(settings_ptr, left, right, y, first_row, max_row, selected, cur_row, parent_last);

	if (!this->folded) {
		if (this->flags & SEF_LAST_FIELD) {
			assert(this->level < 8 * sizeof(parent_last));
			SetBit(parent_last, this->level); // Add own last-field state
		}

		cur_row = SettingsContainer::Draw(settings_ptr, left, right, y, first_row, max_row, selected, cur_row, parent_last);
	}

	return cur_row;
}

/**
 * Function to draw setting value (button + text + current value)
 * @param left         Left-most position in window/panel to start drawing
 * @param right        Right-most position in window/panel to draw
 * @param y            Upper-most position in window/panel to start drawing
 */
void SettingsPage::DrawSetting(GameSettings *, int left, int right, int y, bool) const
{
	bool rtl = _current_text_dir == TD_RTL;
	DrawSprite((this->folded ? SPR_CIRCLE_FOLDED : SPR_CIRCLE_UNFOLDED), PAL_NONE, rtl ? right - _circle_size.width : left, y + (SETTING_HEIGHT - _circle_size.height) / 2);
	DrawString(rtl ? left : left + _circle_size.width + WidgetDimensions::scaled.hsep_normal, rtl ? right - _circle_size.width - WidgetDimensions::scaled.hsep_normal : right, y + (SETTING_HEIGHT - GetCharacterHeight(FS_NORMAL)) / 2, this->title, TC_ORANGE);
}

/** Construct settings tree */
static SettingsContainer &GetSettingsTree()
{
	static SettingsContainer *main = nullptr;

	if (main == nullptr)
	{
		/* Build up the dynamic settings-array only once per OpenTTD session */
		main = new SettingsContainer();

		SettingsPage *localisation = main->Add(new SettingsPage(STR_CONFIG_SETTING_LOCALISATION));
		{
			localisation->Add(new SettingEntry("locale.units_velocity"));
			localisation->Add(new SettingEntry("locale.units_velocity_nautical"));
			localisation->Add(new SettingEntry("locale.units_power"));
			localisation->Add(new SettingEntry("locale.units_weight"));
			localisation->Add(new SettingEntry("locale.units_volume"));
			localisation->Add(new SettingEntry("locale.units_force"));
			localisation->Add(new SettingEntry("locale.units_height"));
			localisation->Add(new SettingEntry("gui.date_format_in_default_names"));
			localisation->Add(new SettingEntry("client_locale.sync_locale_network_server"));
		}

		SettingsPage *graphics = main->Add(new SettingsPage(STR_CONFIG_SETTING_GRAPHICS));
		{
			graphics->Add(new SettingEntry("gui.zoom_min"));
			graphics->Add(new SettingEntry("gui.zoom_max"));
			graphics->Add(new SettingEntry("gui.sprite_zoom_min"));
			graphics->Add(new SettingEntry("gui.shade_trees_on_slopes"));
			graphics->Add(new SettingEntry("gui.smallmap_land_colour"));
			graphics->Add(new SettingEntry("gui.linkgraph_colours"));
			graphics->Add(new SettingEntry("gui.graph_line_thickness"));
		}

		SettingsPage *sound = main->Add(new SettingsPage(STR_CONFIG_SETTING_SOUND));
		{
			sound->Add(new SettingEntry("sound.click_beep"));
			sound->Add(new SettingEntry("sound.confirm"));
			sound->Add(new SettingEntry("sound.news_ticker"));
			sound->Add(new SettingEntry("sound.news_full"));
			sound->Add(new SettingEntry("sound.new_year"));
			sound->Add(new SettingEntry("sound.disaster"));
			sound->Add(new SettingEntry("sound.vehicle"));
			sound->Add(new SettingEntry("sound.ambient"));
		}

		SettingsPage *interface = main->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE));
		{
			SettingsPage *general = interface->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_GENERAL));
			{
				general->Add(new SettingEntry("gui.osk_activation"));
				general->Add(new SettingEntry("gui.errmsg_duration"));
				general->Add(new SettingEntry("gui.window_snap_radius"));
				general->Add(new SettingEntry("gui.window_soft_limit"));
				general->Add(new SettingEntry("gui.right_click_wnd_close"));
			}

			SettingsPage *tooltips = interface->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_TOOLTIPS));
			{
				tooltips->Add(new SettingEntry("gui.hover_delay_ms"));
				tooltips->Add(new ConditionallyHiddenSettingEntry("gui.instant_tile_tooltip", []() -> bool { return _settings_client.gui.hover_delay_ms != 0; }));
				tooltips->Add(new SettingEntry("gui.town_name_tooltip_mode"));
				tooltips->Add(new SettingEntry("gui.industry_tooltip_show"));
				tooltips->Add(new ConditionallyHiddenSettingEntry("gui.industry_tooltip_show_name", []() -> bool { return !_settings_client.gui.industry_tooltip_show; }));
				tooltips->Add(new ConditionallyHiddenSettingEntry("gui.industry_tooltip_show_required", []() -> bool { return !_settings_client.gui.industry_tooltip_show; }));
				tooltips->Add(new ConditionallyHiddenSettingEntry("gui.industry_tooltip_show_stockpiled", []() -> bool { return !_settings_client.gui.industry_tooltip_show; }));
				tooltips->Add(new ConditionallyHiddenSettingEntry("gui.industry_tooltip_show_produced", []() -> bool { return !_settings_client.gui.industry_tooltip_show; }));
				tooltips->Add(new SettingEntry("gui.depot_tooltip_mode"));
				tooltips->Add(new SettingEntry("gui.waypoint_viewport_tooltip_name"));
				tooltips->Add(new SettingEntry("gui.station_viewport_tooltip_name"));
				tooltips->Add(new SettingEntry("gui.station_viewport_tooltip_cargo"));
				tooltips->Add(new SettingEntry("gui.station_rating_tooltip_mode"));
			}

			SettingsPage *save = interface->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_SAVE));
			{
				save->Add(new SettingEntry("gui.autosave_interval"));
				save->Add(new SettingEntry("gui.autosave_realtime"));
				save->Add(new SettingEntry("gui.autosave_on_network_disconnect"));
				save->Add(new SettingEntry("gui.savegame_overwrite_confirm"));
			}

			SettingsPage *viewports = interface->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_VIEWPORTS));
			{
				SettingsPage *viewport_map = viewports->Add(new SettingsPage(STR_CONFIG_SETTING_VIEWPORT_MAP_OPTIONS));
				{
					viewport_map->Add(new SettingEntry("gui.default_viewport_map_mode"));
					viewport_map->Add(new SettingEntry("gui.action_when_viewport_map_is_dblclicked"));
					viewport_map->Add(new SettingEntry("gui.show_scrolling_viewport_on_map"));
					viewport_map->Add(new SettingEntry("gui.show_slopes_on_viewport_map"));
					viewport_map->Add(new SettingEntry("gui.show_height_on_viewport_map"));
					viewport_map->Add(new SettingEntry("gui.show_bridges_on_map"));
					viewport_map->Add(new SettingEntry("gui.show_tunnels_on_map"));
					viewport_map->Add(new SettingEntry("gui.use_owner_colour_for_tunnelbridge"));
				}
				SettingsPage *viewport_route_overlay = viewports->Add(new SettingsPage(STR_CONFIG_SETTING_VEHICLE_ROUTE_OVERLAY));
				{
					viewport_route_overlay->Add(new SettingEntry("gui.show_vehicle_route_mode"));
					viewport_route_overlay->Add(new ConditionallyHiddenSettingEntry("gui.show_vehicle_route_steps", []() -> bool { return _settings_client.gui.show_vehicle_route_mode == 0; }));
					viewport_route_overlay->Add(new ConditionallyHiddenSettingEntry("gui.show_vehicle_route", []() -> bool { return _settings_client.gui.show_vehicle_route_mode == 0; }));
					viewport_route_overlay->Add(new ConditionallyHiddenSettingEntry("gui.dash_level_of_route_lines", []() -> bool { return _settings_client.gui.show_vehicle_route_mode == 0 || !_settings_client.gui.show_vehicle_route; }));
				}

				viewports->Add(new SettingEntry("gui.auto_scrolling"));
				viewports->Add(new SettingEntry("gui.scroll_mode"));
				viewports->Add(new SettingEntry("gui.smooth_scroll"));
				/* While the horizontal scrollwheel scrolling is written as general code, only
				 *  the cocoa (OSX) driver generates input for it.
				 *  Since it's also able to completely disable the scrollwheel will we display it on all platforms anyway */
				viewports->Add(new SettingEntry("gui.scrollwheel_scrolling"));
				viewports->Add(new SettingEntry("gui.scrollwheel_multiplier"));
#ifdef __APPLE__
				/* We might need to emulate a right mouse button on mac */
				viewports->Add(new SettingEntry("gui.right_mouse_btn_emulation"));
#endif
				viewports->Add(new SettingEntry("gui.population_in_label"));
				viewports->Add(new SettingEntry("gui.city_in_label"));
				viewports->Add(new SettingEntry("gui.liveries"));
				viewports->Add(new SettingEntry("gui.measure_tooltip"));
				viewports->Add(new SettingEntry("gui.loading_indicators"));
				viewports->Add(new SettingEntry("gui.show_track_reservation"));
				viewports->Add(new SettingEntry("gui.disable_water_animation"));
			}

			SettingsPage *construction = interface->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_CONSTRUCTION));
			{
				construction->Add(new SettingEntry("gui.link_terraform_toolbar"));
				construction->Add(new SettingEntry("gui.persistent_buildingtools"));
				construction->Add(new SettingEntry("gui.default_rail_type"));
				construction->Add(new SettingEntry("gui.default_road_type"));
				construction->Add(new SettingEntry("gui.demolish_confirm_mode"));
				construction->Add(new SettingEntry("gui.show_rail_polyline_tool"));
			}

			SettingsPage *vehicle_windows = interface->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_VEHICLE_WINDOWS));
			{
				vehicle_windows->Add(new SettingEntry("gui.advanced_vehicle_list"));
				vehicle_windows->Add(new SettingEntry("gui.show_newgrf_name"));
				vehicle_windows->Add(new SettingEntry("gui.show_cargo_in_vehicle_lists"));
				vehicle_windows->Add(new SettingEntry("gui.show_wagon_intro_year"));
				vehicle_windows->Add(new SettingEntry("gui.show_train_length_in_details"));
				vehicle_windows->Add(new SettingEntry("gui.show_train_weight_ratios_in_details"));
				vehicle_windows->Add(new SettingEntry("gui.show_vehicle_group_in_details"));
				vehicle_windows->Add(new SettingEntry("gui.show_vehicle_list_company_colour"));
				vehicle_windows->Add(new SettingEntry("gui.show_adv_load_mode_features"));
				vehicle_windows->Add(new SettingEntry("gui.disable_top_veh_list_mass_actions"));
				vehicle_windows->Add(new SettingEntry("gui.show_depot_sell_gui"));
				vehicle_windows->Add(new SettingEntry("gui.open_vehicle_gui_clone_share"));
				vehicle_windows->Add(new SettingEntry("gui.vehicle_names"));
				vehicle_windows->Add(new SettingEntry("gui.dual_pane_train_purchase_window"));
				vehicle_windows->Add(new ConditionallyHiddenSettingEntry("gui.dual_pane_train_purchase_window_dual_buttons", []() -> bool { return !_settings_client.gui.dual_pane_train_purchase_window; }));
				vehicle_windows->Add(new SettingEntry("gui.show_order_occupancy_by_default"));
				vehicle_windows->Add(new SettingEntry("gui.show_group_hierarchy_name"));
				vehicle_windows->Add(new ConditionallyHiddenSettingEntry("gui.show_vehicle_group_hierarchy_name", []() -> bool { return !_settings_client.gui.show_group_hierarchy_name; }));
				vehicle_windows->Add(new SettingEntry("gui.enable_single_veh_shared_order_gui"));
				vehicle_windows->Add(new SettingEntry("gui.show_order_number_vehicle_view"));
				vehicle_windows->Add(new SettingEntry("gui.shorten_vehicle_view_status"));
				vehicle_windows->Add(new SettingEntry("gui.show_speed_first_vehicle_view"));
				vehicle_windows->Add(new SettingEntry("gui.hide_default_stop_location"));
				vehicle_windows->Add(new SettingEntry("gui.show_running_costs_calendar_year"));
			}

			SettingsPage *departureboards = interface->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_DEPARTUREBOARDS));
			{
				departureboards->Add(new SettingEntry("gui.max_departures"));
				departureboards->Add(new ConditionallyHiddenSettingEntry("gui.max_departure_time", []() -> bool { return _settings_time.time_in_minutes; }));
				departureboards->Add(new ConditionallyHiddenSettingEntry("gui.max_departure_time_minutes", []() -> bool { return !_settings_time.time_in_minutes; }));
				departureboards->Add(new SettingEntry("gui.departure_calc_frequency"));
				departureboards->Add(new SettingEntry("gui.departure_show_vehicle"));
				departureboards->Add(new SettingEntry("gui.departure_show_group"));
				departureboards->Add(new SettingEntry("gui.departure_show_company"));
				departureboards->Add(new SettingEntry("gui.departure_show_vehicle_type"));
				departureboards->Add(new SettingEntry("gui.departure_show_vehicle_color"));
				departureboards->Add(new SettingEntry("gui.departure_larger_font"));
				departureboards->Add(new SettingEntry("gui.departure_destination_type"));
				departureboards->Add(new SettingEntry("gui.departure_smart_terminus"));
				departureboards->Add(new SettingEntry("gui.departure_conditionals"));
				departureboards->Add(new SettingEntry("gui.departure_merge_identical"));
			}

			SettingsPage *timetable = interface->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_TIMETABLE));
			{
				SettingsPage *clock = timetable->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_TIMETABLE_CLOCK));
				{
					clock->Add(new SettingEntry("gui.override_time_settings"));
					SettingsPage *game = clock->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_TIME_SAVEGAME));
					{
						game->hide_callback = []() -> bool {
							return _game_mode == GM_MENU;
						};
						game->Add(new SettingEntry("game_time.time_in_minutes"));
						game->Add(new SettingEntry("game_time.ticks_per_minute"));
						game->Add(new SettingEntry("game_time.clock_offset"));
					}
					SettingsPage *client = clock->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_TIME_CLIENT));
					{
						client->hide_callback = []() -> bool {
							return _game_mode != GM_MENU && !_settings_client.gui.override_time_settings;
						};
						client->Add(new SettingEntry("gui.time_in_minutes"));
						client->Add(new SettingEntry("gui.ticks_per_minute"));
						client->Add(new SettingEntry("gui.clock_offset"));
					}

					clock->Add(new SettingEntry("gui.date_with_time"));
				}

				timetable->Add(new SettingEntry("gui.timetable_in_ticks"));
				timetable->Add(new SettingEntry("gui.timetable_leftover_ticks"));
				timetable->Add(new SettingEntry("gui.timetable_arrival_departure"));
				timetable->Add(new SettingEntry("gui.timetable_start_text_entry"));
			}

			SettingsPage *signals = interface->Add(new SettingsPage(STR_CONFIG_SETTING_INTERFACE_SIGNALS));
			{
				signals->Add(new SettingEntry("construction.train_signal_side"));
				signals->Add(new SettingEntry("gui.semaphore_build_before"));
				signals->Add(new SettingEntry("gui.signal_gui_mode"));
				signals->Add(new SettingEntry("gui.cycle_signal_types"));
				signals->Add(new SettingEntry("gui.drag_signals_fixed_distance"));
				signals->Add(new SettingEntry("gui.drag_signals_skip_stations"));
				signals->Add(new SettingEntry("gui.drag_signals_stop_restricted_signal"));
				signals->Add(new SettingEntry("gui.auto_remove_signals"));
				signals->Add(new SettingEntry("gui.show_restricted_signal_recolour"));
				signals->Add(new SettingEntry("gui.show_all_signal_default"));
				signals->Add(new SettingEntry("gui.show_progsig_ui"));
				signals->Add(new SettingEntry("gui.show_noentrysig_ui"));
				signals->Add(new SettingEntry("gui.show_adv_tracerestrict_features"));
				signals->Add(new SettingEntry("gui.adv_sig_bridge_tun_modes"));
			}

			interface->Add(new SettingEntry("gui.toolbar_pos"));
			interface->Add(new SettingEntry("gui.statusbar_pos"));
			interface->Add(new SettingEntry("gui.prefer_teamchat"));
			interface->Add(new SettingEntry("gui.sort_track_types_by_speed"));
			interface->Add(new SettingEntry("gui.show_town_growth_status"));
			interface->Add(new SettingEntry("gui.allow_hiding_waypoint_labels"));
		}

		SettingsPage *advisors = main->Add(new SettingsPage(STR_CONFIG_SETTING_ADVISORS));
		{
			advisors->Add(new SettingEntry("gui.coloured_news_year"));
			advisors->Add(new SettingEntry("news_display.general"));
			advisors->Add(new SettingEntry("news_display.new_vehicles"));
			advisors->Add(new SettingEntry("news_display.accident"));
			advisors->Add(new SettingEntry("news_display.accident_other"));
			advisors->Add(new SettingEntry("news_display.company_info"));
			advisors->Add(new SettingEntry("news_display.acceptance"));
			advisors->Add(new SettingEntry("news_display.arrival_player"));
			advisors->Add(new SettingEntry("news_display.arrival_other"));
			advisors->Add(new SettingEntry("news_display.advice"));
			advisors->Add(new SettingEntry("gui.order_review_system"));
			advisors->Add(new SettingEntry("gui.no_depot_order_warn"));
			advisors->Add(new SettingEntry("gui.vehicle_income_warn"));
			advisors->Add(new SettingEntry("gui.lost_vehicle_warn"));
			advisors->Add(new SettingEntry("gui.old_vehicle_warn"));
			advisors->Add(new SettingEntry("gui.restriction_wait_vehicle_warn"));
			advisors->Add(new SettingEntry("gui.show_finances"));
			advisors->Add(new SettingEntry("news_display.economy"));
			advisors->Add(new SettingEntry("news_display.subsidies"));
			advisors->Add(new SettingEntry("news_display.open"));
			advisors->Add(new SettingEntry("news_display.close"));
			advisors->Add(new SettingEntry("news_display.production_player"));
			advisors->Add(new SettingEntry("news_display.production_other"));
			advisors->Add(new SettingEntry("news_display.production_nobody"));
		}

		SettingsPage *company = main->Add(new SettingsPage(STR_CONFIG_SETTING_COMPANY));
		{
			company->Add(new SettingEntry("gui.starting_colour"));
			company->Add(new SettingEntry("gui.starting_colour_secondary"));
			company->Add(new SettingEntry("company.engine_renew"));
			company->Add(new SettingEntry("company.engine_renew_months"));
			company->Add(new SettingEntry("company.engine_renew_money"));
			company->Add(new SettingEntry("vehicle.servint_ispercent"));
			company->Add(new SettingEntry("vehicle.servint_trains"));
			company->Add(new SettingEntry("vehicle.servint_roadveh"));
			company->Add(new SettingEntry("vehicle.servint_ships"));
			company->Add(new SettingEntry("vehicle.servint_aircraft"));
			company->Add(new SettingEntry("vehicle.auto_timetable_by_default"));
			company->Add(new SettingEntry("vehicle.auto_separation_by_default"));
			company->Add(new SettingEntry("auto_timetable_separation_rate"));
			company->Add(new SettingEntry("timetable_autofill_rounding"));
			company->Add(new SettingEntry("order_occupancy_smoothness"));
			company->Add(new SettingEntry("company.infra_others_buy_in_depot[0]"));
			company->Add(new SettingEntry("company.infra_others_buy_in_depot[1]"));
			company->Add(new SettingEntry("company.infra_others_buy_in_depot[2]"));
			company->Add(new SettingEntry("company.infra_others_buy_in_depot[3]"));
			company->Add(new SettingEntry("company.advance_order_on_clone"));
			company->Add(new SettingEntry("company.copy_clone_add_to_group"));
			company->Add(new SettingEntry("company.remain_if_next_order_same_station"));
			company->Add(new SettingEntry("company.default_sched_dispatch_duration"));
		}

		SettingsPage *accounting = main->Add(new SettingsPage(STR_CONFIG_SETTING_ACCOUNTING));
		{
			accounting->Add(new SettingEntry("difficulty.infinite_money"));
			accounting->Add(new SettingEntry("economy.inflation"));
			accounting->Add(new SettingEntry("economy.inflation_fixed_dates"));
			accounting->Add(new SettingEntry("difficulty.initial_interest"));
			accounting->Add(new SettingEntry("difficulty.max_loan"));
			accounting->Add(new SettingEntry("difficulty.subsidy_multiplier"));
			accounting->Add(new SettingEntry("difficulty.subsidy_duration"));
			accounting->Add(new SettingEntry("economy.feeder_payment_share"));
			accounting->Add(new SettingEntry("economy.infrastructure_maintenance"));
			accounting->Add(new SettingEntry("difficulty.vehicle_costs"));
			accounting->Add(new SettingEntry("difficulty.vehicle_costs_in_depot"));
			accounting->Add(new SettingEntry("difficulty.vehicle_costs_when_stopped"));
			accounting->Add(new SettingEntry("difficulty.construction_cost"));
			accounting->Add(new SettingEntry("economy.payment_algorithm"));
		}

		SettingsPage *vehicles = main->Add(new SettingsPage(STR_CONFIG_SETTING_VEHICLES));
		{
			SettingsPage *physics = vehicles->Add(new SettingsPage(STR_CONFIG_SETTING_VEHICLES_PHYSICS));
			{
				physics->Add(new SettingEntry("vehicle.train_acceleration_model"));
				physics->Add(new SettingEntry("vehicle.train_braking_model"));
				physics->Add(new ConditionallyHiddenSettingEntry("vehicle.realistic_braking_aspect_limited", []() -> bool { return GetGameSettings().vehicle.train_braking_model != TBM_REALISTIC; }));
				physics->Add(new ConditionallyHiddenSettingEntry("vehicle.limit_train_acceleration", []() -> bool { return GetGameSettings().vehicle.train_braking_model != TBM_REALISTIC; }));
				physics->Add(new ConditionallyHiddenSettingEntry("vehicle.train_acc_braking_percent", []() -> bool { return GetGameSettings().vehicle.train_braking_model != TBM_REALISTIC; }));
				physics->Add(new ConditionallyHiddenSettingEntry("vehicle.track_edit_ignores_realistic_braking", []() -> bool { return GetGameSettings().vehicle.train_braking_model != TBM_REALISTIC; }));
				physics->Add(new SettingEntry("vehicle.train_slope_steepness"));
				physics->Add(new SettingEntry("vehicle.wagon_speed_limits"));
				physics->Add(new SettingEntry("vehicle.train_speed_adaptation"));
				physics->Add(new SettingEntry("vehicle.freight_trains"));
				physics->Add(new SettingEntry("vehicle.roadveh_acceleration_model"));
				physics->Add(new SettingEntry("vehicle.roadveh_slope_steepness"));
				physics->Add(new SettingEntry("vehicle.smoke_amount"));
				physics->Add(new SettingEntry("vehicle.plane_speed"));
				physics->Add(new SettingEntry("vehicle.ship_collision_avoidance"));
				physics->Add(new SettingEntry("vehicle.roadveh_articulated_overtaking"));
				physics->Add(new SettingEntry("vehicle.roadveh_cant_quantum_tunnel"));
				physics->Add(new SettingEntry("vehicle.slow_road_vehicles_in_curves"));
			}

			SettingsPage *routing = vehicles->Add(new SettingsPage(STR_CONFIG_SETTING_VEHICLES_ROUTING));
			{
				routing->Add(new SettingEntry("vehicle.road_side"));
				routing->Add(new SettingEntry("difficulty.line_reverse_mode"));
				routing->Add(new SettingEntry("pf.reverse_at_signals"));
				routing->Add(new SettingEntry("pf.back_of_one_way_pbs_waiting_point"));
				routing->Add(new SettingEntry("pf.forbid_90_deg"));
				routing->Add(new SettingEntry("pf.reroute_rv_on_layout_change"));
				routing->Add(new SettingEntry("vehicle.drive_through_train_depot"));
			}

			SettingsPage *orders = vehicles->Add(new SettingsPage(STR_CONFIG_SETTING_VEHICLES_ORDERS));
			{
				orders->Add(new SettingEntry("gui.new_nonstop"));
				orders->Add(new SettingEntry("gui.quick_goto"));
				orders->Add(new SettingEntry("gui.stop_location"));
				orders->Add(new SettingEntry("order.nonstop_only"));
			}

			vehicles->Add(new SettingEntry("vehicle.adjacent_crossings"));
			vehicles->Add(new SettingEntry("vehicle.safer_crossings"));
			vehicles->Add(new SettingEntry("vehicle.non_leading_engines_keep_name"));
		}

		SettingsPage *limitations = main->Add(new SettingsPage(STR_CONFIG_SETTING_LIMITATIONS));
		{
			limitations->Add(new SettingEntry("construction.command_pause_level"));
			limitations->Add(new SettingEntry("construction.autoslope"));
			limitations->Add(new SettingEntry("construction.extra_dynamite"));
			limitations->Add(new SettingEntry("construction.map_height_limit"));
			limitations->Add(new SettingEntry("construction.max_bridge_length"));
			limitations->Add(new SettingEntry("construction.max_bridge_height"));
			limitations->Add(new SettingEntry("construction.max_tunnel_length"));
			limitations->Add(new SettingEntry("construction.chunnel"));
			limitations->Add(new SettingEntry("station.never_expire_airports"));
			limitations->Add(new SettingEntry("vehicle.never_expire_vehicles"));
			limitations->Add(new SettingEntry("vehicle.no_expire_vehicles_after"));
			limitations->Add(new SettingEntry("vehicle.no_introduce_vehicles_after"));
			limitations->Add(new SettingEntry("vehicle.max_trains"));
			limitations->Add(new SettingEntry("vehicle.max_roadveh"));
			limitations->Add(new SettingEntry("vehicle.max_aircraft"));
			limitations->Add(new SettingEntry("vehicle.max_ships"));
			limitations->Add(new SettingEntry("vehicle.max_train_length"));
			limitations->Add(new SettingEntry("vehicle.through_load_speed_limit"));
			limitations->Add(new SettingEntry("vehicle.rail_depot_speed_limit"));
			limitations->Add(new SettingEntry("station.station_spread"));
			limitations->Add(new SettingEntry("station.distant_join_stations"));
			limitations->Add(new SettingEntry("station.modified_catchment"));
			limitations->Add(new SettingEntry("station.catchment_increase"));
			limitations->Add(new SettingEntry("construction.road_stop_on_town_road"));
			limitations->Add(new SettingEntry("construction.road_stop_on_competitor_road"));
			limitations->Add(new SettingEntry("construction.crossing_with_competitor"));
			limitations->Add(new SettingEntry("construction.convert_town_road_no_houses"));
			limitations->Add(new SettingEntry("vehicle.disable_elrails"));
			limitations->Add(new SettingEntry("order.station_length_loading_penalty"));
			limitations->Add(new SettingEntry("construction.maximum_signal_evaluations"));
			limitations->Add(new SettingEntry("construction.enable_build_river"));
			limitations->Add(new SettingEntry("construction.enable_remove_water"));
			limitations->Add(new SettingEntry("construction.road_custom_bridge_heads"));
			limitations->Add(new SettingEntry("construction.rail_custom_bridge_heads"));
			limitations->Add(new SettingEntry("construction.allow_grf_objects_under_bridges"));
			limitations->Add(new SettingEntry("construction.allow_stations_under_bridges"));
			limitations->Add(new SettingEntry("construction.allow_road_stops_under_bridges"));
			limitations->Add(new SettingEntry("construction.allow_docks_under_bridges"));
			limitations->Add(new SettingEntry("construction.purchase_land_permitted"));
			limitations->Add(new SettingEntry("construction.build_object_area_permitted"));
			limitations->Add(new SettingEntry("construction.no_expire_objects_after"));
			limitations->Add(new SettingEntry("construction.ignore_object_intro_dates"));
		}

		SettingsPage *disasters = main->Add(new SettingsPage(STR_CONFIG_SETTING_ACCIDENTS));
		{
			disasters->Add(new SettingEntry("difficulty.disasters"));
			disasters->Add(new SettingEntry("difficulty.economy"));
			disasters->Add(new SettingEntry("vehicle.plane_crashes"));
			disasters->Add(new SettingEntry("vehicle.no_train_crash_other_company"));
			disasters->Add(new SettingEntry("difficulty.vehicle_breakdowns"));
			disasters->Add(new SettingEntry("vehicle.improved_breakdowns"));
			disasters->Add(new SettingEntry("vehicle.pay_for_repair"));
			disasters->Add(new SettingEntry("vehicle.repair_cost"));
			disasters->Add(new SettingEntry("order.no_servicing_if_no_breakdowns"));
			disasters->Add(new SettingEntry("order.serviceathelipad"));
		}

		SettingsPage *genworld = main->Add(new SettingsPage(STR_CONFIG_SETTING_GENWORLD));
		{
			SettingsPage *rivers = genworld->Add(new SettingsPage(STR_CONFIG_SETTING_GENWORLD_RIVERS_LAKES));
			{
				rivers->Add(new SettingEntry("game_creation.amount_of_rivers"));
				rivers->Add(new SettingEntry("game_creation.min_river_length"));
				rivers->Add(new SettingEntry("game_creation.river_route_random"));
				rivers->Add(new SettingEntry("game_creation.rivers_top_of_hill"));
				rivers->Add(new SettingEntry("game_creation.river_tropics_width"));
				rivers->Add(new SettingEntry("game_creation.lake_tropics_width"));
				rivers->Add(new SettingEntry("game_creation.coast_tropics_width"));
				rivers->Add(new SettingEntry("game_creation.lake_size"));
				rivers->Add(new SettingEntry("game_creation.lakes_allowed_in_deserts"));
			}
			genworld->Add(new SettingEntry("game_creation.landscape"));
			genworld->Add(new SettingEntry("game_creation.land_generator"));
			genworld->Add(new SettingEntry("difficulty.terrain_type"));
			genworld->Add(new SettingEntry("game_creation.tgen_smoothness"));
			genworld->Add(new SettingEntry("game_creation.variety"));
			genworld->Add(new SettingEntry("game_creation.climate_threshold_mode"));
			auto coverage_hide = []() -> bool { return GetGameSettings().game_creation.climate_threshold_mode != 0; };
			auto snow_line_height_hide = []() -> bool { return GetGameSettings().game_creation.climate_threshold_mode != 1 && _game_mode == GM_MENU; };
			auto rainforest_line_height_hide = []() -> bool { return GetGameSettings().game_creation.climate_threshold_mode != 1; };
			genworld->Add(new ConditionallyHiddenSettingEntry("game_creation.snow_coverage", coverage_hide));
			genworld->Add(new ConditionallyHiddenSettingEntry("game_creation.snow_line_height", snow_line_height_hide));
			genworld->Add(new ConditionallyHiddenSettingEntry("game_creation.desert_coverage", coverage_hide));
			genworld->Add(new ConditionallyHiddenSettingEntry("game_creation.rainforest_line_height", rainforest_line_height_hide));
			genworld->Add(new SettingEntry("game_creation.amount_of_rocks"));
			genworld->Add(new SettingEntry("game_creation.height_affects_rocks"));
			genworld->Add(new SettingEntry("game_creation.build_public_roads"));
		}

		SettingsPage *environment = main->Add(new SettingsPage(STR_CONFIG_SETTING_ENVIRONMENT));
		{
			SettingsPage *time = environment->Add(new SettingsPage(STR_CONFIG_SETTING_ENVIRONMENT_TIME));
			{
				time->Add(new SettingEntry("economy.timekeeping_units"));
				time->Add(new SettingEntry("economy.minutes_per_calendar_year"));
				time->Add(new SettingEntry("game_creation.ending_year"));
				time->Add(new SettingEntry("gui.pause_on_newgame"));
				time->Add(new SettingEntry("gui.fast_forward_speed_limit"));
				time->Add(new SettingEntry("economy.day_length_factor"));
				time->Add(new SettingEntry("economy.tick_rate"));
			}

			SettingsPage *authorities = environment->Add(new SettingsPage(STR_CONFIG_SETTING_ENVIRONMENT_AUTHORITIES));
			{
				authorities->Add(new SettingEntry("difficulty.town_council_tolerance"));
				authorities->Add(new SettingEntry("economy.bribe"));
				authorities->Add(new SettingEntry("economy.exclusive_rights"));
				authorities->Add(new SettingEntry("economy.fund_roads"));
				authorities->Add(new SettingEntry("economy.fund_buildings"));
				authorities->Add(new SettingEntry("economy.station_noise_level"));
			}

			SettingsPage *towns = environment->Add(new SettingsPage(STR_CONFIG_SETTING_ENVIRONMENT_TOWNS));
			{
				SettingsPage *town_zone = towns->Add(new SettingsPage(STR_CONFIG_SETTING_TOWN_ZONES));
				{
					town_zone->hide_callback = []() -> bool {
						return !GetGameSettings().economy.town_zone_calc_mode;
					};
					town_zone->Add(new SettingEntry("economy.town_zone_0_mult"));
					town_zone->Add(new SettingEntry("economy.town_zone_1_mult"));
					town_zone->Add(new SettingEntry("economy.town_zone_2_mult"));
					town_zone->Add(new SettingEntry("economy.town_zone_3_mult"));
					town_zone->Add(new SettingEntry("economy.town_zone_4_mult"));
					town_zone->Add(new SettingEntry("economy.city_zone_0_mult"));
					town_zone->Add(new SettingEntry("economy.city_zone_1_mult"));
					town_zone->Add(new SettingEntry("economy.city_zone_2_mult"));
					town_zone->Add(new SettingEntry("economy.city_zone_3_mult"));
					town_zone->Add(new SettingEntry("economy.city_zone_4_mult"));
				}
				towns->Add(new SettingEntry("economy.town_cargo_scale"));
				towns->Add(new SettingEntry("economy.town_cargo_scale_mode"));
				towns->Add(new SettingEntry("economy.town_growth_rate"));
				towns->Add(new SettingEntry("economy.town_growth_cargo_transported"));
				towns->Add(new SettingEntry("economy.default_allow_town_growth"));
				towns->Add(new SettingEntry("economy.town_zone_calc_mode"));
				towns->Add(new SettingEntry("economy.allow_town_roads"));
				towns->Add(new SettingEntry("economy.allow_town_road_branch_non_build"));
				towns->Add(new SettingEntry("economy.allow_town_level_crossings"));
				towns->Add(new SettingEntry("economy.allow_town_bridges"));
				towns->Add(new SettingEntry("economy.town_build_tunnels"));
				towns->Add(new SettingEntry("economy.town_max_road_slope"));
				towns->Add(new SettingEntry("economy.found_town"));
				towns->Add(new SettingEntry("economy.place_houses"));
				towns->Add(new SettingEntry("economy.town_layout"));
				towns->Add(new SettingEntry("economy.larger_towns"));
				towns->Add(new SettingEntry("economy.initial_city_size"));
				towns->Add(new SettingEntry("economy.town_min_distance"));
				towns->Add(new SettingEntry("economy.max_town_heightlevel"));
				towns->Add(new SettingEntry("economy.min_town_land_area"));
				towns->Add(new SettingEntry("economy.min_city_land_area"));
				towns->Add(new SettingEntry("economy.town_cargogen_mode"));
				towns->Add(new SettingEntry("economy.random_road_reconstruction"));
			}

			SettingsPage *industries = environment->Add(new SettingsPage(STR_CONFIG_SETTING_ENVIRONMENT_INDUSTRIES));
			{
				industries->Add(new SettingEntry("economy.industry_cargo_scale"));
				industries->Add(new SettingEntry("economy.industry_cargo_scale_mode"));
				industries->Add(new SettingEntry("difficulty.industry_density"));
				industries->Add(new SettingEntry("construction.raw_industry_construction"));
				industries->Add(new SettingEntry("construction.industry_platform"));
				industries->Add(new SettingEntry("economy.multiple_industry_per_town"));
				industries->Add(new SettingEntry("game_creation.oil_refinery_limit"));
				industries->Add(new SettingEntry("economy.type"));
				industries->Add(new SettingEntry("station.serve_neutral_industries"));
				industries->Add(new SettingEntry("station.station_delivery_mode"));
				industries->Add(new SettingEntry("economy.spawn_primary_industry_only"));
				industries->Add(new SettingEntry("economy.industry_event_rate"));
			}

			SettingsPage *cdist = environment->Add(new SettingsPage(STR_CONFIG_SETTING_ENVIRONMENT_CARGODIST));
			{
				cdist->Add(new SettingEntry("linkgraph.recalc_time"));
				cdist->Add(new SettingEntry("linkgraph.recalc_interval"));
				cdist->Add(new SettingEntry("linkgraph.distribution_pax"));
				cdist->Add(new SettingEntry("linkgraph.distribution_mail"));
				cdist->Add(new SettingEntry("linkgraph.distribution_armoured"));
				cdist->Add(new SettingEntry("linkgraph.distribution_default"));
				SettingsPage *cdist_override = cdist->Add(new SettingsPage(STR_CONFIG_SETTING_ENVIRONMENT_CARGODIST_PER_CARGO_OVERRIDE));
				{
					const SettingTable &linkgraph_table = GetLinkGraphSettingTable();
					uint base_index = GetSettingIndexByFullName(linkgraph_table, "linkgraph.distribution_per_cargo[0]");
					assert(base_index != UINT32_MAX);
					for (CargoType c = 0; c < NUM_CARGO; c++) {
						cdist_override->Add(new CargoDestPerCargoSettingEntry(c, GetSettingDescription(linkgraph_table, base_index + c)->AsIntSetting()));
					}
				}
				cdist->Add(new SettingEntry("linkgraph.accuracy"));
				cdist->Add(new SettingEntry("linkgraph.demand_distance"));
				cdist->Add(new SettingEntry("linkgraph.demand_size"));
				cdist->Add(new SettingEntry("linkgraph.short_path_saturation"));
				cdist->Add(new SettingEntry("linkgraph.aircraft_link_scale"));
			}

			SettingsPage *trees = environment->Add(new SettingsPage(STR_CONFIG_SETTING_ENVIRONMENT_TREES));
			{
				trees->Add(new SettingEntry("game_creation.tree_placer"));
				trees->Add(new SettingEntry("construction.extra_tree_placement"));
				trees->Add(new SettingEntry("construction.trees_around_snow_line_enabled"));
				trees->Add(new SettingEntry("construction.trees_around_snow_line_range"));
				trees->Add(new SettingEntry("construction.trees_around_snow_line_dynamic_range"));
				trees->Add(new SettingEntry("construction.tree_growth_rate"));
			}

			environment->Add(new SettingEntry("construction.flood_from_edges"));
			environment->Add(new SettingEntry("construction.map_edge_mode"));
			environment->Add(new SettingEntry("station.cargo_class_rating_wait_time"));
			environment->Add(new SettingEntry("station.station_size_rating_cargo_amount"));
			environment->Add(new SettingEntry("construction.purchased_land_clear_ground"));
		}

		SettingsPage *ai = main->Add(new SettingsPage(STR_CONFIG_SETTING_AI));
		{
			SettingsPage *npc = ai->Add(new SettingsPage(STR_CONFIG_SETTING_AI_NPC));
			{
				npc->Add(new SettingEntry("script.script_max_opcode_till_suspend"));
				npc->Add(new SettingEntry("script.script_max_memory_megabytes"));
				npc->Add(new SettingEntry("difficulty.competitor_speed"));
				npc->Add(new SettingEntry("ai.ai_in_multiplayer"));
				npc->Add(new SettingEntry("ai.ai_disable_veh_train"));
				npc->Add(new SettingEntry("ai.ai_disable_veh_roadveh"));
				npc->Add(new SettingEntry("ai.ai_disable_veh_aircraft"));
				npc->Add(new SettingEntry("ai.ai_disable_veh_ship"));
			}

			SettingsPage *sharing = ai->Add(new SettingsPage(STR_CONFIG_SETTING_SHARING));
			{
				sharing->Add(new SettingEntry("economy.infrastructure_sharing[0]"));
				sharing->Add(new SettingEntry("economy.infrastructure_sharing[1]"));
				sharing->Add(new SettingEntry("economy.infrastructure_sharing[2]"));
				sharing->Add(new SettingEntry("economy.infrastructure_sharing[3]"));
				sharing->Add(new SettingEntry("economy.sharing_fee[0]"));
				sharing->Add(new SettingEntry("economy.sharing_fee[1]"));
				sharing->Add(new SettingEntry("economy.sharing_fee[2]"));
				sharing->Add(new SettingEntry("economy.sharing_fee[3]"));
				sharing->Add(new SettingEntry("economy.sharing_payment_in_debt"));
			}

			ai->Add(new SettingEntry("economy.give_money"));
			ai->Add(new SettingEntry("economy.allow_shares"));
			ai->Add(new ConditionallyHiddenSettingEntry("economy.min_years_for_shares", []() -> bool { return !GetGameSettings().economy.allow_shares; }));
			ai->Add(new SettingEntry("difficulty.money_cheat_in_multiplayer"));
			ai->Add(new SettingEntry("difficulty.rename_towns_in_multiplayer"));
			ai->Add(new SettingEntry("difficulty.override_town_settings_in_multiplayer"));
		}

		SettingsPage *network = main->Add(new SettingsPage(STR_CONFIG_SETTING_NETWORK));
		{
			network->Add(new SettingEntry("network.use_relay_service"));
		}

		main->Init();
	}
	return *main;
}

static const StringID _game_settings_restrict_dropdown[] = {
	STR_CONFIG_SETTING_RESTRICT_BASIC,                            // RM_BASIC
	STR_CONFIG_SETTING_RESTRICT_ADVANCED,                         // RM_ADVANCED
	STR_CONFIG_SETTING_RESTRICT_ALL,                              // RM_ALL
	STR_CONFIG_SETTING_RESTRICT_CHANGED_AGAINST_DEFAULT,          // RM_CHANGED_AGAINST_DEFAULT
	STR_CONFIG_SETTING_RESTRICT_CHANGED_AGAINST_NEW,              // RM_CHANGED_AGAINST_NEW
	STR_CONFIG_SETTING_RESTRICT_PATCH,                            // RM_PATCH
};
static_assert(lengthof(_game_settings_restrict_dropdown) == RM_END);

/** Warnings about hidden search results. */
enum WarnHiddenResult : uint8_t {
	WHR_NONE,          ///< Nothing was filtering matches away.
	WHR_CATEGORY,      ///< Category setting filtered matches away.
	WHR_TYPE,          ///< Type setting filtered matches away.
	WHR_CATEGORY_TYPE, ///< Both category and type settings filtered matches away.
};

/**
 * Callback function for the reset all settings button
 * @param w Window which is calling this callback
 * @param confirmed boolean value, true when yes was clicked, false otherwise
 */
static void ResetAllSettingsConfirmationCallback(Window *w, bool confirmed)
{
	if (confirmed) {
		GetSettingsTree().ResetAll();
		GetSettingsTree().FoldAll();
		w->InvalidateData();
	}
}

/** Window to edit settings of the game. */
struct GameSettingsWindow : Window {
	static GameSettings *settings_ptr; ///< Pointer to the game settings being displayed and modified.

	SettingEntry *valuewindow_entry;   ///< If non-nullptr, pointer to setting for which a value-entering window has been opened.
	SettingEntry *clicked_entry;       ///< If non-nullptr, pointer to a clicked numeric setting (with a depressed left or right button).
	SettingEntry *last_clicked;        ///< If non-nullptr, pointer to the last clicked setting.
	SettingEntry *valuedropdown_entry; ///< If non-nullptr, pointer to the value for which a dropdown window is currently opened.
	bool closing_dropdown;             ///< True, if the dropdown list is currently closing.

	SettingFilter filter;              ///< Filter for the list.
	QueryString filter_editbox;        ///< Filter editbox;
	bool manually_changed_folding;     ///< Whether the user expanded/collapsed something manually.
	WarnHiddenResult warn_missing;     ///< Whether and how to warn about missing search results.
	int warn_lines;                    ///< Number of lines used for warning about missing search results.

	Scrollbar *vscroll;

	GameSettingsWindow(WindowDesc &desc) : Window(desc), filter_editbox(50)
	{
		this->warn_missing = WHR_NONE;
		this->warn_lines = 0;
		this->filter.mode = (RestrictionMode)_settings_client.gui.settings_restriction_mode;
		this->filter.min_cat = RM_ALL;
		this->filter.type = ST_ALL;
		this->filter.type_hides = false;
		this->settings_ptr = &GetGameSettings();

		GetSettingsTree().FoldAll(); // Close all sub-pages

		this->valuewindow_entry = nullptr; // No setting entry for which a entry window is opened
		this->clicked_entry = nullptr; // No numeric setting buttons are depressed
		this->last_clicked = nullptr;
		this->valuedropdown_entry = nullptr;
		this->closing_dropdown = false;
		this->manually_changed_folding = false;

		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_GS_SCROLLBAR);
		this->FinishInitNested(WN_GAME_OPTIONS_GAME_SETTINGS);

		this->querystrings[WID_GS_FILTER] = &this->filter_editbox;
		this->filter_editbox.cancel_button = QueryString::ACTION_CLEAR;
		this->SetFocusedWidget(WID_GS_FILTER);

		this->InvalidateData();
	}

	void OnInit() override
	{
		_circle_size = maxdim(GetSpriteSize(SPR_CIRCLE_FOLDED), GetSpriteSize(SPR_CIRCLE_UNFOLDED));
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_GS_OPTIONSPANEL:
				resize.height = SETTING_HEIGHT = std::max({(int)_circle_size.height, SETTING_BUTTON_HEIGHT, GetCharacterHeight(FS_NORMAL)}) + WidgetDimensions::scaled.vsep_normal;
				resize.width = 1;

				size.height = 5 * resize.height + WidgetDimensions::scaled.framerect.Vertical();
				break;

			case WID_GS_HELP_TEXT: {
				static const StringID setting_types[] = {
					STR_CONFIG_SETTING_TYPE_CLIENT,
					STR_CONFIG_SETTING_TYPE_COMPANY_MENU, STR_CONFIG_SETTING_TYPE_COMPANY_INGAME,
					STR_CONFIG_SETTING_TYPE_GAME_MENU, STR_CONFIG_SETTING_TYPE_GAME_INGAME,
				};
				for (const auto &setting_type : setting_types) {
					SetDParam(0, setting_type);
					size.width = std::max(size.width, GetStringBoundingBox(STR_CONFIG_SETTING_TYPE).width + padding.width);
				}
				size.height = 2 * GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal +
						std::max(size.height, GetSettingsTree().GetMaxHelpHeight(size.width));
				break;
			}

			case WID_GS_RESTRICT_CATEGORY:
			case WID_GS_RESTRICT_TYPE:
				size.width = std::max(GetStringBoundingBox(STR_CONFIG_SETTING_RESTRICT_CATEGORY).width, GetStringBoundingBox(STR_CONFIG_SETTING_RESTRICT_TYPE).width);
				break;

			default:
				break;
		}
	}

	void OnPaint() override
	{
		if (this->closing_dropdown) {
			this->closing_dropdown = false;
			assert(this->valuedropdown_entry != nullptr);
			this->valuedropdown_entry->SetButtons(0);
			this->valuedropdown_entry = nullptr;
		}

		/* Reserve the correct number of lines for the 'some search results are hidden' notice in the central settings display panel. */
		const Rect panel = this->GetWidget<NWidgetBase>(WID_GS_OPTIONSPANEL)->GetCurrentRect().Shrink(WidgetDimensions::scaled.frametext);
		StringID warn_str = STR_CONFIG_SETTING_CATEGORY_HIDES - 1 + this->warn_missing;
		int new_warn_lines;
		if (this->warn_missing == WHR_NONE) {
			new_warn_lines = 0;
		} else {
			SetDParam(0, _game_settings_restrict_dropdown[this->filter.min_cat]);
			new_warn_lines = GetStringLineCount(warn_str, panel.Width());
		}
		if (this->warn_lines != new_warn_lines) {
			this->vscroll->SetCount(this->vscroll->GetCount() - this->warn_lines + new_warn_lines);
			this->warn_lines = new_warn_lines;
		}

		this->DrawWidgets();

		/* Draw the 'some search results are hidden' notice. */
		if (this->warn_missing != WHR_NONE) {
			SetDParam(0, _game_settings_restrict_dropdown[this->filter.min_cat]);
			DrawStringMultiLine(panel.WithHeight(this->warn_lines * GetCharacterHeight(FS_NORMAL)), warn_str, TC_FROMSTRING, SA_CENTER);
		}
	}

	void SetStringParameters(WidgetID widget) const override
	{
		switch (widget) {
			case WID_GS_RESTRICT_DROPDOWN:
				SetDParam(0, _game_settings_restrict_dropdown[this->filter.mode]);
				break;

			case WID_GS_TYPE_DROPDOWN:
				switch (this->filter.type) {
					case ST_GAME:    SetDParam(0, _game_mode == GM_MENU ? STR_CONFIG_SETTING_TYPE_DROPDOWN_GAME_MENU : STR_CONFIG_SETTING_TYPE_DROPDOWN_GAME_INGAME); break;
					case ST_COMPANY: SetDParam(0, _game_mode == GM_MENU ? STR_CONFIG_SETTING_TYPE_DROPDOWN_COMPANY_MENU : STR_CONFIG_SETTING_TYPE_DROPDOWN_COMPANY_INGAME); break;
					case ST_CLIENT:  SetDParam(0, STR_CONFIG_SETTING_TYPE_DROPDOWN_CLIENT); break;
					default:         SetDParam(0, STR_CONFIG_SETTING_TYPE_DROPDOWN_ALL); break;
				}
				break;
		}
	}

	DropDownList BuildDropDownList(WidgetID widget) const
	{
		DropDownList list;
		switch (widget) {
			case WID_GS_RESTRICT_DROPDOWN:
				for (int mode = 0; mode != RM_END; mode++) {
					/* If we are in adv. settings screen for the new game's settings,
					 * we don't want to allow comparing with new game's settings. */
					bool disabled = mode == RM_CHANGED_AGAINST_NEW && settings_ptr == &_settings_newgame;

					list.push_back(MakeDropDownListStringItem(_game_settings_restrict_dropdown[mode], mode, disabled));
				}
				break;

			case WID_GS_TYPE_DROPDOWN:
				list.push_back(MakeDropDownListStringItem(STR_CONFIG_SETTING_TYPE_DROPDOWN_ALL, ST_ALL));
				list.push_back(MakeDropDownListStringItem(_game_mode == GM_MENU ? STR_CONFIG_SETTING_TYPE_DROPDOWN_GAME_MENU : STR_CONFIG_SETTING_TYPE_DROPDOWN_GAME_INGAME, ST_GAME));
				list.push_back(MakeDropDownListStringItem(_game_mode == GM_MENU ? STR_CONFIG_SETTING_TYPE_DROPDOWN_COMPANY_MENU : STR_CONFIG_SETTING_TYPE_DROPDOWN_COMPANY_INGAME, ST_COMPANY));
				list.push_back(MakeDropDownListStringItem(STR_CONFIG_SETTING_TYPE_DROPDOWN_CLIENT, ST_CLIENT));
				break;
		}
		return list;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_GS_OPTIONSPANEL: {
				Rect tr = r.Shrink(WidgetDimensions::scaled.frametext, WidgetDimensions::scaled.framerect);
				tr.top += this->warn_lines * SETTING_HEIGHT;
				uint last_row = this->vscroll->GetPosition() + this->vscroll->GetCapacity() - this->warn_lines;
				int next_row = GetSettingsTree().Draw(settings_ptr, tr.left, tr.right, tr.top,
						this->vscroll->GetPosition(), last_row, this->last_clicked);
				if (next_row == 0) DrawString(tr, STR_CONFIG_SETTINGS_NONE);
				break;
			}

			case WID_GS_HELP_TEXT:
				if (this->last_clicked != nullptr) {
					const IntSettingDesc *sd = this->last_clicked->setting;

					Rect tr = r;
					switch (sd->GetType()) {
						case ST_COMPANY: SetDParam(0, _game_mode == GM_MENU ? STR_CONFIG_SETTING_TYPE_COMPANY_MENU : STR_CONFIG_SETTING_TYPE_COMPANY_INGAME); break;
						case ST_CLIENT:  SetDParam(0, STR_CONFIG_SETTING_TYPE_CLIENT); break;
						case ST_GAME:    SetDParam(0, _game_mode == GM_MENU ? STR_CONFIG_SETTING_TYPE_GAME_MENU : STR_CONFIG_SETTING_TYPE_GAME_INGAME); break;
						default: NOT_REACHED();
					}
					DrawString(tr, STR_CONFIG_SETTING_TYPE);
					tr.top += GetCharacterHeight(FS_NORMAL);

					auto [param1, param2] = sd->GetValueParams(sd->GetDefaultValue());
					DrawString(tr, GetString(STR_CONFIG_SETTING_DEFAULT_VALUE, param1, param2));
					tr.top += GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal;

					if (sd->guiproc != nullptr) {
						SettingOnGuiCtrlData data;
						data.type = SOGCT_GUI_WARNING_TEXT;
						data.text = STR_NULL;
						data.val = sd->Read(ResolveObject(settings_ptr, sd));
						if (sd->guiproc(data)) {
							const Dimension warning_dimensions = GetSpriteSize(SPR_WARNING_SIGN);
							const int step_height = std::max<int>(warning_dimensions.height, GetCharacterHeight(FS_NORMAL));
							const int text_offset_y = (step_height - GetCharacterHeight(FS_NORMAL)) / 2;
							const int warning_offset_y = (step_height - warning_dimensions.height) / 2;
							const bool rtl = _current_text_dir == TD_RTL;

							int left = tr.left;
							int right = tr.right;
							DrawSprite(SPR_WARNING_SIGN, 0, rtl ? right - warning_dimensions.width - 5 : left + 5, tr.top + warning_offset_y);
							if (rtl) {
								right -= (warning_dimensions.width + 10);
							} else {
								left += (warning_dimensions.width + 10);
							}
							DrawString(left, right, tr.top + text_offset_y, data.text, TC_RED);

							tr.top += step_height + WidgetDimensions::scaled.vsep_normal;
						}
					}

					DrawStringMultiLine(tr, sd->GetHelp(), TC_WHITE);
				}
				break;

			default:
				break;
		}
	}

	/**
	 * Set the entry that should have its help text displayed, and mark the window dirty so it gets repainted.
	 * @param pe Setting to display help text of, use \c nullptr to stop displaying help of the currently displayed setting.
	 */
	void SetDisplayedHelpText(SettingEntry *pe)
	{
		if (this->last_clicked != pe) this->SetDirty();
		this->last_clicked = pe;
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_GS_EXPAND_ALL:
				this->manually_changed_folding = true;
				GetSettingsTree().UnFoldAll();
				this->InvalidateData();
				break;

			case WID_GS_COLLAPSE_ALL:
				this->manually_changed_folding = true;
				GetSettingsTree().FoldAll();
				this->InvalidateData();
				break;

			case WID_GS_RESET_ALL:
				ShowQuery(
					STR_CONFIG_SETTING_RESET_ALL_CONFIRMATION_DIALOG_CAPTION,
					STR_CONFIG_SETTING_RESET_ALL_CONFIRMATION_DIALOG_TEXT,
					this,
					ResetAllSettingsConfirmationCallback
				);
				break;

			case WID_GS_RESTRICT_DROPDOWN: {
				DropDownList list = this->BuildDropDownList(widget);
				if (!list.empty()) {
					ShowDropDownList(this, std::move(list), this->filter.mode, widget);
				}
				break;
			}

			case WID_GS_TYPE_DROPDOWN: {
				DropDownList list = this->BuildDropDownList(widget);
				if (!list.empty()) {
					ShowDropDownList(this, std::move(list), this->filter.type, widget);
				}
				break;
			}
		}

		if (widget != WID_GS_OPTIONSPANEL) return;

		int32_t btn = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_GS_OPTIONSPANEL, WidgetDimensions::scaled.framerect.top);
		if (btn == INT32_MAX || btn < this->warn_lines) return;
		btn -= this->warn_lines;

		uint cur_row = 0;
		BaseSettingEntry *clicked_entry = GetSettingsTree().FindEntry(btn, &cur_row);

		if (clicked_entry == nullptr) return;  // Clicked below the last setting of the page

		int x = (_current_text_dir == TD_RTL ? this->width - 1 - pt.x : pt.x) - WidgetDimensions::scaled.frametext.left - (clicked_entry->level + 1) * WidgetDimensions::scaled.hsep_indent;  // Shift x coordinate
		if (x < 0) return;  // Clicked left of the entry

		SettingsPage *clicked_page = dynamic_cast<SettingsPage*>(clicked_entry);
		if (clicked_page != nullptr) {
			this->SetDisplayedHelpText(nullptr);
			clicked_page->folded = !clicked_page->folded; // Flip 'folded'-ness of the sub-page

			this->manually_changed_folding = true;

			this->InvalidateData();
			return;
		}

		SettingEntry *pe = dynamic_cast<SettingEntry*>(clicked_entry);
		assert(pe != nullptr);
		const IntSettingDesc *sd = pe->setting;

		/* return if action is only active in network, or only settable by server */
		if (!pe->IsGUIEditable()) {
			this->SetDisplayedHelpText(pe);
			return;
		}

		auto [min_val, max_val] = sd->GetRange();
		int32_t value = sd->Read(ResolveObject(settings_ptr, sd));

		/* clicked on the icon on the left side. Either scroller, bool on/off or dropdown */
		if (x < SETTING_BUTTON_WIDTH && sd->flags.Any({SettingFlag::GuiDropdown, SettingFlag::Enum})) {
			this->SetDisplayedHelpText(pe);

			if (this->valuedropdown_entry == pe) {
				/* unclick the dropdown */
				HideDropDownMenu(this);
				this->closing_dropdown = false;
				this->valuedropdown_entry->SetButtons(0);
				this->valuedropdown_entry = nullptr;
			} else {
				if (this->valuedropdown_entry != nullptr) this->valuedropdown_entry->SetButtons(0);
				this->closing_dropdown = false;

				const NWidgetBase *wid = this->GetWidget<NWidgetBase>(WID_GS_OPTIONSPANEL);
				int rel_y = (pt.y - wid->pos_y - WidgetDimensions::scaled.framerect.top) % wid->resize_y;

				Rect wi_rect;
				wi_rect.left = pt.x - (_current_text_dir == TD_RTL ? SETTING_BUTTON_WIDTH - 1 - x : x);
				wi_rect.right = wi_rect.left + SETTING_BUTTON_WIDTH - 1;
				wi_rect.top = pt.y - rel_y + (SETTING_HEIGHT - SETTING_BUTTON_HEIGHT) / 2;
				wi_rect.bottom = wi_rect.top + SETTING_BUTTON_HEIGHT - 1;

				/* For dropdowns we also have to check the y position thoroughly, the mouse may not above the just opening dropdown */
				if (pt.y >= wi_rect.top && pt.y <= wi_rect.bottom) {
					this->valuedropdown_entry = pe;
					this->valuedropdown_entry->SetButtons(SEF_LEFT_DEPRESSED);

					DropDownList list;
					if (sd->flags.Test(SettingFlag::GuiDropdown)) {
						for (int32_t i = min_val; i <= static_cast<int32_t>(max_val); i++) {
							int32_t val = i;
							if (sd->guiproc != nullptr) {
								SettingOnGuiCtrlData data;
								data.type = SOGCT_GUI_DROPDOWN_ORDER;
								data.val = i - sd->min;
								if (sd->guiproc(data)) {
									val = data.val;
								}
								assert_msg(val >= min_val && val <= static_cast<int32_t>(max_val), "min: {}, max: {}, val: {}", sd->min, sd->max, val);
							}
							auto [param1, param2] = sd->GetValueParams(val);
							list.push_back(MakeDropDownListStringItem(GetString(STR_JUST_STRING1, param1, param2), val, false));
						}
					} else if (sd->flags.Test(SettingFlag::Enum)) {
						for (const SettingDescEnumEntry *enumlist = sd->enumlist; enumlist != nullptr && enumlist->str != STR_NULL; enumlist++) {
							list.push_back(MakeDropDownListStringItem(enumlist->str, enumlist->val, false));
						}
					}

					ShowDropDownListAt(this, std::move(list), value, WID_GS_SETTING_DROPDOWN, wi_rect, COLOUR_ORANGE);
				}
			}
			this->SetDirty();
		} else if (x < SETTING_BUTTON_WIDTH) {
			this->SetDisplayedHelpText(pe);
			int32_t oldvalue = value;

			if (sd->IsBoolSetting()) {
				value ^= 1;
			} else {
				/* Add a dynamic step-size to the scroller. In a maximum of
				 * 50-steps you should be able to get from min to max,
				 * unless specified otherwise in the 'interval' variable
				 * of the current setting. */
				uint32_t step = (sd->interval == 0) ? ((max_val - min_val) / 50) : sd->interval;
				if (step == 0) step = 1;

				/* don't allow too fast scrolling */
				if (this->flags.Test(WindowFlag::Timeout) && this->timeout_timer > 1) {
					_left_button_clicked = false;
					return;
				}

				/* Increase or decrease the value and clamp it to extremes */
				if (x >= SETTING_BUTTON_WIDTH / 2) {
					value += step;
					if (min_val < 0) {
						assert(static_cast<int32_t>(max_val) >= 0);
						if (value > static_cast<int32_t>(max_val)) value = static_cast<int32_t>(max_val);
					} else {
						if (static_cast<uint32_t>(value) > max_val) value = static_cast<int32_t>(max_val);
					}
					if (value < min_val) value = min_val; // skip between "disabled" and minimum
				} else {
					value -= step;
					if (value < min_val) value = sd->flags.Test(SettingFlag::GuiZeroIsSpecial) ? 0 : min_val;
				}

				/* Set up scroller timeout for numeric values */
				if (value != oldvalue) {
					if (this->clicked_entry != nullptr) { // Release previous buttons if any
						this->clicked_entry->SetButtons(0);
					}
					this->clicked_entry = pe;
					this->clicked_entry->SetButtons((x >= SETTING_BUTTON_WIDTH / 2) != (_current_text_dir == TD_RTL) ? SEF_RIGHT_DEPRESSED : SEF_LEFT_DEPRESSED);
					this->SetTimeout();
					_left_button_clicked = false;
				}
			}

			if (value != oldvalue) {
				SetSettingValue(sd, value);
				this->SetDirty();
			}
		} else {
			/* Only open editbox if clicked for the second time, and only for types where it is sensible for. */
			if (this->last_clicked == pe && !sd->IsBoolSetting() && !sd->flags.Any({SettingFlag::GuiDropdown, SettingFlag::Enum})) {
				int64_t value64 = value;
				/* Show the correct currency or velocity translated value */
				if (sd->flags.Test(SettingFlag::GuiCurrency)) value64 *= GetCurrency().rate;
				if (sd->flags.Test(SettingFlag::GuiVelocity)) value64 = ConvertKmhishSpeedToDisplaySpeed((uint)value64, VEH_TRAIN);

				this->valuewindow_entry = pe;
				if (sd->flags.Test(SettingFlag::GuiVelocity) && _settings_game.locale.units_velocity == 3) {
					CharSetFilter charset_filter = CS_NUMERAL_DECIMAL; //default, only numeric input and decimal point allowed
					if (min_val < 0) charset_filter = CS_NUMERAL_DECIMAL_SIGNED; // special case, also allow '-' sign for negative input

					ShowQueryString(GetString(STR_JUST_DECIMAL1, value64), STR_CONFIG_SETTING_QUERY_CAPTION, 10, this, charset_filter, QSF_ENABLE_DEFAULT);
				} else {
					CharSetFilter charset_filter = CS_NUMERAL; //default, only numeric input allowed
					if (min_val < 0) charset_filter = CS_NUMERAL_SIGNED; // special case, also allow '-' sign for negative input

					/* Limit string length to 14 so that MAX_INT32 * max currency rate doesn't exceed MAX_INT64. */
					ShowQueryString(GetString(STR_JUST_INT, value64), STR_CONFIG_SETTING_QUERY_CAPTION, 15, this, charset_filter, QSF_ENABLE_DEFAULT);
				}
			}
			this->SetDisplayedHelpText(pe);
		}
	}

	void OnTimeout() override
	{
		if (this->clicked_entry != nullptr) { // On timeout, release any depressed buttons
			this->clicked_entry->SetButtons(0);
			this->clicked_entry = nullptr;
			this->SetDirty();
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		/* The user pressed cancel */
		if (!str.has_value()) return;

		assert(this->valuewindow_entry != nullptr);
		const IntSettingDesc *sd = this->valuewindow_entry->setting;

		int32_t value;
		if (!str->empty()) {
			long long llvalue;
			if (sd->flags.Test(SettingFlag::GuiVelocity) && _settings_game.locale.units_velocity == 3) {
				llvalue = atof(str->c_str()) * 10;
			} else {
				llvalue = atoll(str->c_str());
			}

			/* Save the correct currency-translated value */
			if (sd->flags.Test(SettingFlag::GuiCurrency)) llvalue /= GetCurrency().rate;

			value = ClampTo<int32_t>(llvalue);

			/* Save the correct velocity-translated value */
			if (sd->flags.Test(SettingFlag::GuiVelocity)) value = ConvertDisplaySpeedToKmhishSpeed(value, VEH_TRAIN);
		} else {
			value = sd->GetDefaultValue();
		}

		SetSettingValue(this->valuewindow_entry->setting, value);
		this->SetDirty();
	}

	void OnDropdownSelect(WidgetID widget, int index) override
	{
		switch (widget) {
			case WID_GS_RESTRICT_DROPDOWN:
				this->filter.mode = (RestrictionMode)index;
				if (this->filter.mode == RM_CHANGED_AGAINST_DEFAULT ||
						this->filter.mode == RM_CHANGED_AGAINST_NEW) {

					if (!this->manually_changed_folding) {
						/* Expand all when selecting 'changes'. Update the filter state first, in case it becomes less restrictive in some cases. */
						GetSettingsTree().UpdateFilterState(this->filter, false);
						GetSettingsTree().UnFoldAll();
					}
				} else {
					/* Non-'changes' filter. Save as default. */
					_settings_client.gui.settings_restriction_mode = this->filter.mode;
				}
				this->InvalidateData();
				break;

			case WID_GS_TYPE_DROPDOWN:
				this->filter.type = (SettingType)index;
				this->InvalidateData();
				break;

			case WID_GS_SETTING_DROPDOWN:
				/* Deal with drop down boxes on the panel. */
				assert(this->valuedropdown_entry != nullptr);
				const IntSettingDesc *sd = this->valuedropdown_entry->setting;
				assert(sd->flags.Any({SettingFlag::GuiDropdown, SettingFlag::Enum}));

				SetSettingValue(sd, index);
				this->SetDirty();
				break;
		}
	}

	void OnDropdownClose(Point pt, WidgetID widget, int index, bool instant_close) override
	{
		if (widget != WID_GS_SETTING_DROPDOWN) {
			/* Normally the default implementation of OnDropdownClose() takes care of
			 * a few things. We want that behaviour here too, but only for
			 * "normal" dropdown boxes. The special dropdown boxes added for every
			 * setting that needs one can't have this call. */
			Window::OnDropdownClose(pt, widget, index, instant_close);
		} else {
			/* We cannot raise the dropdown button just yet. OnClick needs some hint, whether
			 * the same dropdown button was clicked again, and then not open the dropdown again.
			 * So, we only remember that it was closed, and process it on the next OnPaint, which is
			 * after OnClick. */
			assert(this->valuedropdown_entry != nullptr);
			this->closing_dropdown = true;
			this->SetDirty();
		}
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;

		/* Update which settings are to be visible. */
		RestrictionMode min_level = (this->filter.mode <= RM_ALL || this->filter.mode == RM_PATCH) ? this->filter.mode : RM_BASIC;
		this->filter.min_cat = min_level;
		this->filter.type_hides = false;
		GetSettingsTree().UpdateFilterState(this->filter, false);

		if (this->filter.string.IsEmpty()) {
			this->warn_missing = WHR_NONE;
		} else if (min_level < this->filter.min_cat || (min_level == RM_PATCH && min_level != this->filter.min_cat)) {
			this->warn_missing = this->filter.type_hides ? WHR_CATEGORY_TYPE : WHR_CATEGORY;
		} else {
			this->warn_missing = this->filter.type_hides ? WHR_TYPE : WHR_NONE;
		}
		this->vscroll->SetCount(GetSettingsTree().Length() + this->warn_lines);

		if (this->last_clicked != nullptr && !GetSettingsTree().IsVisible(this->last_clicked)) {
			this->SetDisplayedHelpText(nullptr);
		}

		bool all_folded = true;
		bool all_unfolded = true;
		GetSettingsTree().GetFoldingState(all_folded, all_unfolded);
		this->SetWidgetDisabledState(WID_GS_EXPAND_ALL, all_unfolded);
		this->SetWidgetDisabledState(WID_GS_COLLAPSE_ALL, all_folded);
	}

	void OnEditboxChanged(WidgetID wid) override
	{
		if (wid == WID_GS_FILTER) {
			this->filter.string.SetFilterTerm(this->filter_editbox.text.GetText());
			if (!this->filter.string.IsEmpty() && !this->manually_changed_folding) {
				/* User never expanded/collapsed single pages and entered a filter term.
				 * Expand everything, to save weird expand clicks, */
				GetSettingsTree().UnFoldAll();
			}
			this->InvalidateData();
		}
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_GS_OPTIONSPANEL, WidgetDimensions::scaled.framerect.Vertical());
	}
};

GameSettings *GameSettingsWindow::settings_ptr = nullptr;

static constexpr NWidgetPart _nested_settings_selection_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_MAUVE),
		NWidget(WWT_CAPTION, COLOUR_MAUVE), SetStringTip(STR_CONFIG_SETTING_TREE_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_DEFSIZEBOX, COLOUR_MAUVE),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_MAUVE),
		NWidget(NWID_VERTICAL), SetPIP(WidgetDimensions::unscaled.frametext.top, WidgetDimensions::unscaled.vsep_normal, WidgetDimensions::unscaled.frametext.bottom),
			NWidget(NWID_HORIZONTAL), SetPIP(WidgetDimensions::unscaled.frametext.left, WidgetDimensions::unscaled.hsep_wide, WidgetDimensions::unscaled.frametext.right),
				NWidget(WWT_TEXT, INVALID_COLOUR, WID_GS_RESTRICT_CATEGORY), SetStringTip(STR_CONFIG_SETTING_RESTRICT_CATEGORY),
				NWidget(WWT_DROPDOWN, COLOUR_MAUVE, WID_GS_RESTRICT_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_JUST_STRING, STR_CONFIG_SETTING_RESTRICT_DROPDOWN_HELPTEXT), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(WidgetDimensions::unscaled.frametext.left, WidgetDimensions::unscaled.hsep_wide, WidgetDimensions::unscaled.frametext.right),
				NWidget(WWT_TEXT, INVALID_COLOUR, WID_GS_RESTRICT_TYPE), SetStringTip(STR_CONFIG_SETTING_RESTRICT_TYPE),
				NWidget(WWT_DROPDOWN, COLOUR_MAUVE, WID_GS_TYPE_DROPDOWN), SetMinimalSize(100, 12), SetStringTip(STR_JUST_STRING, STR_CONFIG_SETTING_TYPE_DROPDOWN_HELPTEXT), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(WidgetDimensions::unscaled.frametext.left, WidgetDimensions::unscaled.hsep_wide, WidgetDimensions::unscaled.frametext.right),
				NWidget(WWT_TEXT, INVALID_COLOUR), SetFill(0, 1), SetStringTip(STR_CONFIG_SETTING_FILTER_TITLE),
				NWidget(WWT_EDITBOX, COLOUR_MAUVE, WID_GS_FILTER), SetStringTip(STR_LIST_FILTER_OSKTITLE, STR_LIST_FILTER_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_MAUVE, WID_GS_OPTIONSPANEL), SetMinimalSize(400, 174), SetScrollbar(WID_GS_SCROLLBAR), EndContainer(),
		NWidget(NWID_VSCROLLBAR, COLOUR_MAUVE, WID_GS_SCROLLBAR),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_MAUVE),
		NWidget(WWT_EMPTY, INVALID_COLOUR, WID_GS_HELP_TEXT), SetMinimalSize(300, 25), SetFill(1, 1), SetResize(1, 0),
				SetPadding(WidgetDimensions::unscaled.frametext),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_MAUVE, WID_GS_EXPAND_ALL), SetStringTip(STR_CONFIG_SETTING_EXPAND_ALL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_MAUVE, WID_GS_COLLAPSE_ALL), SetStringTip(STR_CONFIG_SETTING_COLLAPSE_ALL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_MAUVE, WID_GS_RESET_ALL), SetStringTip(STR_CONFIG_SETTING_RESET_ALL),
		NWidget(WWT_PANEL, COLOUR_MAUVE), SetFill(1, 0), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_RESIZEBOX, COLOUR_MAUVE),
	EndContainer(),
};

static WindowDesc _settings_selection_desc(__FILE__, __LINE__,
	WDP_CENTER, "settings", 510, 450,
	WC_GAME_OPTIONS, WC_NONE,
	{},
	_nested_settings_selection_widgets
);

/** Open advanced settings window. */
void ShowGameSettings()
{
	CloseWindowByClass(WC_GAME_OPTIONS);
	new GameSettingsWindow(_settings_selection_desc);
}


/**
 * Draw [<][>] boxes.
 * @param x the x position to draw
 * @param y the y position to draw
 * @param button_colour the colour of the button
 * @param state 0 = none clicked, 1 = first clicked, 2 = second clicked
 * @param clickable_left is the left button clickable?
 * @param clickable_right is the right button clickable?
 */
void DrawArrowButtons(int x, int y, Colours button_colour, uint8_t state, bool clickable_left, bool clickable_right)
{
	int colour = GetColourGradient(button_colour, SHADE_DARKER);
	Dimension dim = NWidgetScrollbar::GetHorizontalDimension();

	Rect lr = {x,                  y, x + (int)dim.width     - 1, y + (int)dim.height - 1};
	Rect rr = {x + (int)dim.width, y, x + (int)dim.width * 2 - 1, y + (int)dim.height - 1};

	DrawFrameRect(lr, button_colour, (state == 1) ? FrameFlag::Lowered : FrameFlags{});
	DrawFrameRect(rr, button_colour, (state == 2) ? FrameFlag::Lowered : FrameFlags{});
	DrawSpriteIgnorePadding(SPR_ARROW_LEFT,  PAL_NONE, lr, SA_CENTER);
	DrawSpriteIgnorePadding(SPR_ARROW_RIGHT, PAL_NONE, rr, SA_CENTER);

	/* Grey out the buttons that aren't clickable */
	bool rtl = _current_text_dir == TD_RTL;
	if (rtl ? !clickable_right : !clickable_left) {
		GfxFillRect(lr.Shrink(WidgetDimensions::scaled.bevel), colour, FILLRECT_CHECKER);
	}
	if (rtl ? !clickable_left : !clickable_right) {
		GfxFillRect(rr.Shrink(WidgetDimensions::scaled.bevel), colour, FILLRECT_CHECKER);
	}
}

/**
 * Draw a dropdown button.
 * @param x the x position to draw
 * @param y the y position to draw
 * @param button_colour the colour of the button
 * @param state true = lowered
 * @param clickable is the button clickable?
 */
void DrawDropDownButton(int x, int y, Colours button_colour, bool state, bool clickable)
{
	int colour = GetColourGradient(button_colour, SHADE_DARKER);

	Rect r = {x, y, x + SETTING_BUTTON_WIDTH - 1, y + SETTING_BUTTON_HEIGHT - 1};

	DrawFrameRect(r, button_colour, state ? FrameFlag::Lowered : FrameFlags{});
	DrawSpriteIgnorePadding(SPR_ARROW_DOWN, PAL_NONE, r, SA_CENTER);

	if (!clickable) {
		GfxFillRect(r.Shrink(WidgetDimensions::scaled.bevel), colour, FILLRECT_CHECKER);
	}
}

/**
 * Draw a toggle button.
 * @param x the x position to draw
 * @param y the y position to draw
 * @param state true = lowered
 * @param clickable is the button clickable?
 */
void DrawBoolButton(int x, int y, bool state, bool clickable)
{
	static const Colours _bool_ctabs[2][2] = {{COLOUR_CREAM, COLOUR_RED}, {COLOUR_DARK_GREEN, COLOUR_GREEN}};

	Rect r = {x, y, x + SETTING_BUTTON_WIDTH - 1, y + SETTING_BUTTON_HEIGHT - 1};
	DrawFrameRect(r, _bool_ctabs[state][clickable], state ? FrameFlag::Lowered : FrameFlags{});
}

struct CustomCurrencyWindow : Window {
	int query_widget;

	CustomCurrencyWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested();

		SetButtonState();
	}

	void SetButtonState()
	{
		this->SetWidgetDisabledState(WID_CC_RATE_DOWN, GetCustomCurrency().rate == 1);
		this->SetWidgetDisabledState(WID_CC_RATE_UP, GetCustomCurrency().rate == UINT16_MAX);
		this->SetWidgetDisabledState(WID_CC_YEAR_DOWN, GetCustomCurrency().to_euro == CF_NOEURO);
		this->SetWidgetDisabledState(WID_CC_YEAR_UP, GetCustomCurrency().to_euro == CalTime::MAX_YEAR);
	}

	void SetStringParameters(WidgetID widget) const override
	{
		switch (widget) {
			case WID_CC_RATE:      SetDParam(0, 1); SetDParam(1, 1);            break;
			case WID_CC_SEPARATOR: SetDParamStr(0, GetCustomCurrency().separator); break;
			case WID_CC_PREFIX:    SetDParamStr(0, GetCustomCurrency().prefix);    break;
			case WID_CC_SUFFIX:    SetDParamStr(0, GetCustomCurrency().suffix);    break;
			case WID_CC_YEAR:
				SetDParam(0, (GetCustomCurrency().to_euro != CF_NOEURO) ? STR_CURRENCY_SWITCH_TO_EURO : STR_CURRENCY_SWITCH_TO_EURO_NEVER);
				SetDParam(1, GetCustomCurrency().to_euro);
				break;

			case WID_CC_PREVIEW:
				SetDParam(0, 10000);
				break;
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			/* Set the appropriate width for the up/down buttons. */
			case WID_CC_RATE_DOWN:
			case WID_CC_RATE_UP:
			case WID_CC_YEAR_DOWN:
			case WID_CC_YEAR_UP:
				size = maxdim(size, {(uint)SETTING_BUTTON_WIDTH / 2, (uint)SETTING_BUTTON_HEIGHT});
				break;

			/* Set the appropriate width for the edit buttons. */
			case WID_CC_SEPARATOR_EDIT:
			case WID_CC_PREFIX_EDIT:
			case WID_CC_SUFFIX_EDIT:
				size = maxdim(size, {(uint)SETTING_BUTTON_WIDTH, (uint)SETTING_BUTTON_HEIGHT});
				break;

			/* Make sure the window is wide enough for the widest exchange rate */
			case WID_CC_RATE:
				SetDParam(0, 1);
				SetDParam(1, INT32_MAX);
				size = GetStringBoundingBox(STR_CURRENCY_EXCHANGE_RATE);
				break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		int line = 0;
		int len = 0;
		std::string str;
		CharSetFilter afilter = CS_ALPHANUMERAL;

		switch (widget) {
			case WID_CC_RATE_DOWN:
				if (GetCustomCurrency().rate > 1) GetCustomCurrency().rate--;
				if (GetCustomCurrency().rate == 1) this->DisableWidget(WID_CC_RATE_DOWN);
				this->EnableWidget(WID_CC_RATE_UP);
				break;

			case WID_CC_RATE_UP:
				if (GetCustomCurrency().rate < UINT16_MAX) GetCustomCurrency().rate++;
				if (GetCustomCurrency().rate == UINT16_MAX) this->DisableWidget(WID_CC_RATE_UP);
				this->EnableWidget(WID_CC_RATE_DOWN);
				break;

			case WID_CC_RATE:
				str = GetString(STR_JUST_INT, GetCustomCurrency().rate);
				len = 5;
				line = WID_CC_RATE;
				afilter = CS_NUMERAL;
				break;

			case WID_CC_SEPARATOR_EDIT:
			case WID_CC_SEPARATOR:
				str = GetCustomCurrency().separator;
				len = 7;
				line = WID_CC_SEPARATOR;
				break;

			case WID_CC_PREFIX_EDIT:
			case WID_CC_PREFIX:
				str = GetCustomCurrency().prefix;
				len = 15;
				line = WID_CC_PREFIX;
				break;

			case WID_CC_SUFFIX_EDIT:
			case WID_CC_SUFFIX:
				str = GetCustomCurrency().suffix;
				len = 15;
				line = WID_CC_SUFFIX;
				break;

			case WID_CC_YEAR_DOWN:
				GetCustomCurrency().to_euro = (GetCustomCurrency().to_euro <= MIN_EURO_YEAR) ? CF_NOEURO : GetCustomCurrency().to_euro - 1;
				if (GetCustomCurrency().to_euro == CF_NOEURO) this->DisableWidget(WID_CC_YEAR_DOWN);
				this->EnableWidget(WID_CC_YEAR_UP);
				break;

			case WID_CC_YEAR_UP:
				GetCustomCurrency().to_euro = Clamp<CalTime::Year>(GetCustomCurrency().to_euro + 1, MIN_EURO_YEAR, CalTime::MAX_YEAR);
				if (GetCustomCurrency().to_euro == CalTime::MAX_YEAR) this->DisableWidget(WID_CC_YEAR_UP);
				this->EnableWidget(WID_CC_YEAR_DOWN);
				break;

			case WID_CC_YEAR:
				str = GetString(STR_JUST_INT, GetCustomCurrency().to_euro);
				len = 7;
				line = WID_CC_YEAR;
				afilter = CS_NUMERAL;
				break;
		}

		if (len != 0) {
			this->query_widget = line;
			ShowQueryString(str, STR_CURRENCY_CHANGE_PARAMETER, len + 1, this, afilter, QSF_NONE);
		}

		this->SetTimeout();
		this->SetDirty();
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value()) return;

		switch (this->query_widget) {
			case WID_CC_RATE:
				GetCustomCurrency().rate = Clamp(atoi(str->c_str()), 1, UINT16_MAX);
				break;

			case WID_CC_SEPARATOR: // Thousands separator
				GetCustomCurrency().separator = std::move(*str);
				break;

			case WID_CC_PREFIX:
				GetCustomCurrency().prefix = std::move(*str);
				break;

			case WID_CC_SUFFIX:
				GetCustomCurrency().suffix = std::move(*str);
				break;

			case WID_CC_YEAR: { // Year to switch to euro
				CalTime::Year val{atoi(str->c_str())};

				GetCustomCurrency().to_euro = (val < MIN_EURO_YEAR ? CF_NOEURO : std::min<CalTime::Year>(val, CalTime::MAX_YEAR));
				break;
			}
		}
		MarkWholeScreenDirty();
		SetButtonState();
	}

	void OnTimeout() override
	{
		this->SetDirty();
	}
};

static constexpr NWidgetPart _nested_cust_currency_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY), SetStringTip(STR_CURRENCY_WINDOW, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0), SetPadding(WidgetDimensions::unscaled.sparse),
			NWidget(NWID_VERTICAL, NWidContainerFlag::EqualSize), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
					NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
						NWidget(WWT_PUSHARROWBTN, COLOUR_YELLOW, WID_CC_RATE_DOWN), SetArrowWidgetTypeTip(AWV_DECREASE, STR_CURRENCY_DECREASE_EXCHANGE_RATE_TOOLTIP),
						NWidget(WWT_PUSHARROWBTN, COLOUR_YELLOW, WID_CC_RATE_UP), SetArrowWidgetTypeTip(AWV_INCREASE, STR_CURRENCY_INCREASE_EXCHANGE_RATE_TOOLTIP),
					EndContainer(),
					NWidget(WWT_TEXT, INVALID_COLOUR, WID_CC_RATE), SetStringTip(STR_CURRENCY_EXCHANGE_RATE, STR_CURRENCY_SET_EXCHANGE_RATE_TOOLTIP), SetFill(1, 0),
				EndContainer(),
				NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
					NWidget(WWT_PUSHBTN, COLOUR_DARK_BLUE, WID_CC_SEPARATOR_EDIT), SetToolTip(STR_CURRENCY_SET_CUSTOM_CURRENCY_SEPARATOR_TOOLTIP), SetFill(0, 1),
					NWidget(WWT_TEXT, INVALID_COLOUR, WID_CC_SEPARATOR), SetStringTip(STR_CURRENCY_SEPARATOR, STR_CURRENCY_SET_CUSTOM_CURRENCY_SEPARATOR_TOOLTIP), SetFill(1, 0),
				EndContainer(),
				NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
					NWidget(WWT_PUSHBTN, COLOUR_DARK_BLUE, WID_CC_PREFIX_EDIT), SetToolTip(STR_CURRENCY_SET_CUSTOM_CURRENCY_PREFIX_TOOLTIP), SetFill(0, 1),
					NWidget(WWT_TEXT, INVALID_COLOUR, WID_CC_PREFIX), SetStringTip(STR_CURRENCY_PREFIX, STR_CURRENCY_SET_CUSTOM_CURRENCY_PREFIX_TOOLTIP), SetFill(1, 0),
				EndContainer(),
				NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
					NWidget(WWT_PUSHBTN, COLOUR_DARK_BLUE, WID_CC_SUFFIX_EDIT), SetToolTip(STR_CURRENCY_SET_CUSTOM_CURRENCY_SUFFIX_TOOLTIP), SetFill(0, 1),
					NWidget(WWT_TEXT, INVALID_COLOUR, WID_CC_SUFFIX), SetStringTip(STR_CURRENCY_SUFFIX, STR_CURRENCY_SET_CUSTOM_CURRENCY_SUFFIX_TOOLTIP), SetFill(1, 0),
				EndContainer(),
				NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
					NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
						NWidget(WWT_PUSHARROWBTN, COLOUR_YELLOW, WID_CC_YEAR_DOWN), SetArrowWidgetTypeTip(AWV_DECREASE, STR_CURRENCY_DECREASE_CUSTOM_CURRENCY_TO_EURO_TOOLTIP),
						NWidget(WWT_PUSHARROWBTN, COLOUR_YELLOW, WID_CC_YEAR_UP), SetArrowWidgetTypeTip(AWV_INCREASE, STR_CURRENCY_INCREASE_CUSTOM_CURRENCY_TO_EURO_TOOLTIP),
					EndContainer(),
					NWidget(WWT_TEXT, INVALID_COLOUR, WID_CC_YEAR), SetStringTip(STR_JUST_STRING1, STR_CURRENCY_SET_CUSTOM_CURRENCY_TO_EURO_TOOLTIP), SetFill(1, 0),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_LABEL, INVALID_COLOUR, WID_CC_PREVIEW),
					SetStringTip(STR_CURRENCY_PREVIEW, STR_CURRENCY_CUSTOM_CURRENCY_PREVIEW_TOOLTIP),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _cust_currency_desc(__FILE__, __LINE__,
	WDP_CENTER, nullptr, 0, 0,
	WC_CUSTOM_CURRENCY, WC_NONE,
	{},
	_nested_cust_currency_widgets
);

/** Open custom currency window. */
static void ShowCustCurrency()
{
	CloseWindowById(WC_CUSTOM_CURRENCY, 0);
	new CustomCurrencyWindow(_cust_currency_desc);
}
