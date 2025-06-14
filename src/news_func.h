/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file news_func.h Functions related to news. */

#ifndef NEWS_FUNC_H
#define NEWS_FUNC_H

#include "news_type.h"
#include "vehicle_type.h"
#include "station_type.h"
#include "industry_type.h"

void AddNewsItem(StringID string, NewsType type, NewsStyle style, NewsFlags flags, NewsReferenceType reftype1 = NewsReferenceType::None, uint32_t ref1 = UINT32_MAX, NewsReferenceType reftype2 = NewsReferenceType::None, uint32_t ref2 = UINT32_MAX, std::unique_ptr<NewsAllocatedData> data = nullptr, AdviceType advice_type = AdviceType::Invalid);

inline void AddCompanyNewsItem(StringID string, std::unique_ptr<CompanyNewsInformation> cni)
{
	AddNewsItem(string, NewsType::CompanyInfo, NewsStyle::Company, {}, NewsReferenceType::None, UINT32_MAX, NewsReferenceType::None, UINT32_MAX, std::move(cni));
}

/**
 * Adds a newsitem referencing a vehicle.
 *
 * @warning The DParams may not reference the vehicle due to autoreplace stuff. See AddVehicleAdviceNewsItem for how that can be done.
 */
inline void AddVehicleNewsItem(StringID string, NewsType type, VehicleID vehicle, StationID station = INVALID_STATION)
{
	AddNewsItem(string, type, NewsStyle::Thin, {NewsFlag::NoTransparency, NewsFlag::Shaded}, NewsReferenceType::Vehicle, vehicle, station == INVALID_STATION ? NewsReferenceType::None : NewsReferenceType::Station, station);
}

/**
 * Adds a vehicle-advice news item.
 *
 * @warning DParam 0 must reference the vehicle!
 */
inline void AddVehicleAdviceNewsItem(AdviceType advice_type, StringID string, VehicleID vehicle)
{
	AddNewsItem(string, NewsType::Advice, NewsStyle::Small, {NewsFlag::InColour, NewsFlag::VehicleParam0}, NewsReferenceType::Vehicle, vehicle, NewsReferenceType::None, {}, nullptr, advice_type);
}

inline void AddTileNewsItem(StringID string, NewsType type, TileIndex tile, std::unique_ptr<NewsAllocatedData> data = nullptr, StationID station = INVALID_STATION)
{
	AddNewsItem(string, type, NewsStyle::Thin, {NewsFlag::NoTransparency, NewsFlag::Shaded}, NewsReferenceType::Tile, tile.base(), station == INVALID_STATION ? NewsReferenceType::None : NewsReferenceType::Station, station, std::move(data));
}

inline void AddIndustryNewsItem(StringID string, NewsType type, IndustryID industry, std::unique_ptr<NewsAllocatedData> data = nullptr)
{
	AddNewsItem(string, type, NewsStyle::Thin, {NewsFlag::NoTransparency, NewsFlag::Shaded}, NewsReferenceType::Industry, industry, NewsReferenceType::None, UINT32_MAX, std::move(data));
}

void NewsLoop();
void InitNewsItemStructs();

const NewsItem *GetStatusbarNews();

void DeleteInvalidEngineNews();
void DeleteVehicleNews(VehicleID vid, AdviceType advice_type = AdviceType::Invalid);
void DeleteStationNews(StationID sid);
void DeleteIndustryNews(IndustryID iid);

#endif /* NEWS_FUNC_H */
