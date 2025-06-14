/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file script_list.hpp A list which can keep item/value pairs, which you can walk. */
/** @defgroup ScriptList Classes that create a list of items. */

#ifndef SCRIPT_LIST_HPP
#define SCRIPT_LIST_HPP

#include "script_object.hpp"
#include "script_controller.hpp"
#include "../../3rdparty/cpp-btree/safe_btree_set.h"
#include "../../3rdparty/cpp-btree/safe_btree_map.h"

/** Maximum number of operations allowed for valuating a list. */
static const int MAX_VALUATE_OPS = 1000000;

class ScriptListSorter;

/**
 * Class that creates a list which can keep item/value pairs, which you can walk.
 * @api ai game
 */
class ScriptList : public ScriptObject {
public:
	/** Type of sorter */
	enum SorterType {
		SORT_BY_VALUE, ///< Sort the list based on the value of the item.
		SORT_BY_ITEM,  ///< Sort the list based on the item itself.
	};

	/** Sort ascending */
	static const bool SORT_ASCENDING = true;
	/** Sort descending */
	static const bool SORT_DESCENDING = false;

	/**
	 * The safe btree variants ars used because these automatically manage refreshing iterators
	 * which have been invalidated by adding/removing items.
	 */
	typedef btree::safe_btree_map<SQInteger, SQInteger> ScriptListMap;                 ///< Key to value map
	typedef btree::safe_btree_set<std::pair<SQInteger, SQInteger>> ScriptListValueSet; ///< [Value, Key] set

private:
	std::unique_ptr<ScriptListSorter> sorter; ///< Sorting algorithm
	SorterType sorter_type;       ///< Sorting type
	bool sort_ascending;          ///< Whether to sort ascending or descending
	bool initialized;             ///< Whether an iteration has been started
	bool values_inited;           ///< Whether the 'values' field has been initialised
	int modifications;            ///< Number of modification that has been done. To prevent changing data while valuating.

	void InitValues();
	void InitSorter();
	void SetIterValue(ScriptListMap::iterator item_iter, SQInteger value);
	ScriptListMap::iterator RemoveIter(ScriptListMap::iterator item_iter);
	ScriptListValueSet::iterator RemoveValueIter(ScriptListValueSet::iterator value_iter);

	template <typename T>
	struct FillListHelper {
		using IterType = T;

		auto Iterate()
		{
			return T::Iterate();
		}

		int OpcodeCharge([[maybe_unused]] int item_count)
		{
			return (int)(T::GetNumItems() / 2);
		}
	};

protected:
	template <typename T, typename... Targs>
	static void FillList(Targs... args)
	{
		FillListT<FillListHelper<T>>(FillListHelper<T>{}, args...);
	}

	template <typename Thelper, class ItemValid, class ItemFilter>
	static void FillListT(Thelper helper, ScriptList *list, ItemValid item_valid, ItemFilter item_filter)
	{
		using IterType = typename Thelper::IterType;

		int opcode_charge = 0;
		int item_count = 0;
		for (const IterType *item : helper.Iterate()) {
			item_count++;
			if (!item_valid(item)) continue;
			if (!item_filter(item)) continue;
			list->AddItem(item->index);
			opcode_charge += 3;
		}
		ScriptController::DecreaseOps(opcode_charge + helper.OpcodeCharge(item_count));
	}

	template <typename Thelper, class ItemValid>
	static void FillListT(Thelper helper, ScriptList *list, ItemValid item_valid)
	{
		using IterType = typename Thelper::IterType;

		ScriptList::FillListT<Thelper>(helper, list, item_valid, [](const IterType *) { return true; });
	}

	template <typename Thelper>
	static void FillListT(Thelper helper, ScriptList *list)
	{
		using IterType = typename Thelper::IterType;

		ScriptList::FillListT<Thelper>(list, [](const IterType *) { return true; });
	}

	template <typename Thelper, class ItemValid>
	static void FillListT(Thelper helper, HSQUIRRELVM vm, ScriptList *list, ItemValid item_valid)
	{
		using IterType = typename Thelper::IterType;

		int nparam = sq_gettop(vm) - 1;
		if (nparam >= 1) {
			/* Make sure the filter function is really a function, and not any
			 * other type. It's parameter 2 for us, but for the user it's the
			 * first parameter they give. */
			SQObjectType valuator_type = sq_gettype(vm, 2);
			if (valuator_type != OT_CLOSURE && valuator_type != OT_NATIVECLOSURE) {
				throw sq_throwerror(vm, "parameter 1 has an invalid type (expected function)");
			}

			/* Push the function to call */
			sq_push(vm, 2);
		}

		/* Don't allow docommand from a Valuator, as we can't resume in
		 * mid C++-code. */
		bool backup_allow = ScriptObject::GetAllowDoCommand();
		ScriptObject::SetAllowDoCommand(false);


		if (nparam < 1) {
			ScriptList::FillListT<Thelper>(helper, list, item_valid);
		} else {
			/* Limit the total number of ops that can be consumed by a filter operation, if a filter function is present */
			SQOpsLimiter limiter(vm, MAX_VALUATE_OPS, "list filter function");

			ScriptList::FillListT<Thelper>(helper, list, item_valid,
				[vm, nparam, backup_allow](const IterType *item) {
					/* Push the root table as instance object, this is what squirrel does for meta-functions. */
					sq_pushroottable(vm);
					/* Push all arguments for the valuator function. */
					sq_pushinteger(vm, item->index);
					for (int i = 0; i < nparam - 1; i++) {
						sq_push(vm, i + 3);
					}

					/* Call the function. Squirrel pops all parameters and pushes the return value. */
					if (SQ_FAILED(sq_call(vm, nparam + 1, SQTrue, SQTrue))) {
						ScriptObject::SetAllowDoCommand(backup_allow);
						throw sq_throwerror(vm, "failed to run filter");
					}

					SQBool add = SQFalse;

					/* Retrieve the return value */
					switch (sq_gettype(vm, -1)) {
						case OT_BOOL:
							sq_getbool(vm, -1, &add);
							break;

						default:
							ScriptObject::SetAllowDoCommand(backup_allow);
							throw sq_throwerror(vm, "return value of filter is not valid (not bool)");
					}

					/* Pop the return value. */
					sq_poptop(vm);

					return add;
				}
			);

			/* Pop the filter function */
			sq_poptop(vm);
		}

		ScriptObject::SetAllowDoCommand(backup_allow);
	}

	template <typename Thelper>
	static void FillListT(Thelper helper, HSQUIRRELVM vm, ScriptList *list)
	{
		using IterType = typename Thelper::IterType;

		ScriptList::FillListT<Thelper>(helper, vm, list, [](const IterType *) { return true; });
	}

	inline size_t GetSize() const
	{
		return this->items.size();
	}

public:
	ScriptListMap items;       ///< The items in the list
	ScriptListValueSet values; ///< The items in the list, sorted by value

	ScriptList();
	~ScriptList();

#ifdef DOXYGEN_API
	/**
	 * Add a single item to the list.
	 * @param item the item to add. Should be unique, otherwise it is ignored.
	 * @param value the value to assign.
	 */
	void AddItem(SQInteger item, SQInteger value);
#else
	void AddItem(SQInteger item, SQInteger value = 0);
#endif /* DOXYGEN_API */

	/**
	 * @api -all
	 */
	void AddOrSetItem(SQInteger item, SQInteger value);

	/**
	 * @api -all
	 */
	void AddToItemValue(SQInteger item, SQInteger value_to_add);

	/**
	 * Remove a single item from the list.
	 * @param item the item to remove. If not existing, it is ignored.
	 */
	void RemoveItem(SQInteger item);

	/**
	 * Clear the list, making Count() returning 0 and IsEmpty() returning true.
	 */
	void Clear();

	/**
	 * Check if an item is in the list.
	 * @param item the item to check for.
	 * @return true if the item is in the list.
	 */
	bool HasItem(SQInteger item);

	/**
	 * Go to the beginning of the list and return the item. To get the value use list.GetValue(list.Begin()).
	 * @return the first item.
	 * @note returns 0 if beyond end-of-list. Use IsEnd() to check for end-of-list.
	 */
	SQInteger Begin();

	/**
	 * Go to the next item in the list and return the item. To get the value use list.GetValue(list.Next()).
	 * @return the next item.
	 * @note returns 0 if beyond end-of-list. Use IsEnd() to check for end-of-list.
	 */
	SQInteger Next();

	/**
	 * Check if a list is empty.
	 * @return true if the list is empty.
	 */
	bool IsEmpty();

	/**
	 * Check if there is a element left. In other words, if this is false,
	 * the last call to Begin() or Next() returned a valid item.
	 * @return true if the current item is beyond end-of-list.
	 */
	bool IsEnd();

	/**
	 * Returns the amount of items in the list.
	 * @return amount of items in the list.
	 */
	SQInteger Count();

	/**
	 * Get the value that belongs to this item.
	 * @param item the item to get the value from
	 * @return the value that belongs to this item.
	 */
	SQInteger GetValue(SQInteger item);

	/**
	 * Set a value of an item directly.
	 * @param item the item to set the value for.
	 * @param value the value to give to the item
	 * @return true if we could set the item to value, false otherwise.
	 * @note Changing values of items while looping through a list might cause
	 *  entries to be skipped. Be very careful with such operations.
	 */
	bool SetValue(SQInteger item, SQInteger value);

	/**
	 * Sort this list by the given sorter and direction.
	 * @param sorter    the type of sorter to use
	 * @param ascending if true, lowest value is on top, else at bottom.
	 * @note the current item stays at the same place.
	 * @see SORT_ASCENDING SORT_DESCENDING
	 */
	void Sort(SorterType sorter, bool ascending);

	/**
	 * Add one list to another one.
	 * @param list The list that will be added to the caller.
	 * @post The list to be added ('list') stays unmodified.
	 * @note All added items keep their value as it was in 'list'.
	 * @note If the item already exists inside the caller, the value of the
	 *  list that is added is set on the item.
	 */
	void AddList(ScriptList *list);

	/**
	 * Swap the contents of two lists.
	 * @param list The list that will be swapped with.
	 */
	void SwapList(ScriptList *list);

	/**
	 * Removes all items with a higher value than 'value'.
	 * @param value the value above which all items are removed.
	 */
	void RemoveAboveValue(SQInteger value);

	/**
	 * Removes all items with a lower value than 'value'.
	 * @param value the value below which all items are removed.
	 */
	void RemoveBelowValue(SQInteger value);

	/**
	 * Removes all items with a value above start and below end.
	 * @param start the lower bound of the to be removed values (exclusive).
	 * @param end   the upper bound of the to be removed values (exclusive).
	 */
	void RemoveBetweenValue(SQInteger start, SQInteger end);

	/**
	 * Remove all items with this value.
	 * @param value the value to remove.
	 */
	void RemoveValue(SQInteger value);

	/**
	 * Remove the first count items.
	 * @param count the amount of items to remove.
	 */
	void RemoveTop(SQInteger count);

	/**
	 * Remove the last count items.
	 * @param count the amount of items to remove.
	 */
	void RemoveBottom(SQInteger count);

	/**
	 * Remove everything that is in the given list from this list (same item index that is).
	 * @param list the list of items to remove.
	 * @pre list != null
	 */
	void RemoveList(ScriptList *list);

	/**
	 * Keep all items with a higher value than 'value'.
	 * @param value the value above which all items are kept.
	 */
	void KeepAboveValue(SQInteger value);

	/**
	 * Keep all items with a lower value than 'value'.
	 * @param value the value below which all items are kept.
	 */
	void KeepBelowValue(SQInteger value);

	/**
	 * Keep all items with a value above start and below end.
	 * @param start the lower bound of the to be kept values (exclusive).
	 * @param end   the upper bound of the to be kept values (exclusive).
	 */
	void KeepBetweenValue(SQInteger start, SQInteger end);

	/**
	 * Keep all items with this value.
	 * @param value the value to keep.
	 */
	void KeepValue(SQInteger value);

	/**
	 * Keep the first count items, i.e. remove everything except the first count items.
	 * @param count the amount of items to keep.
	 */
	void KeepTop(SQInteger count);

	/**
	 * Keep the last count items, i.e. remove everything except the last count items.
	 * @param count the amount of items to keep.
	 */
	void KeepBottom(SQInteger count);

	/**
	 * Keeps everything that is in the given list from this list (same item index that is).
	 * @param list the list of items to keep.
	 * @pre list != null
	 */
	void KeepList(ScriptList *list);

#ifndef DOXYGEN_API
	/**
	 * Used for 'foreach()' and [] get from Squirrel.
	 */
	SQInteger _get(HSQUIRRELVM vm);

	/**
	 * Used for [] set from Squirrel.
	 */
	SQInteger _set(HSQUIRRELVM vm);

	/**
	 * Used for 'foreach()' from Squirrel.
	 */
	SQInteger _nexti(HSQUIRRELVM vm);

	/**
	 * The Valuate() wrapper from Squirrel.
	 */
	SQInteger Valuate(HSQUIRRELVM vm);
#else
	/**
	 * Give all items a value defined by the valuator you give.
	 * @param valuator_function The function which will be doing the valuation.
	 * @param ... The params to give to the valuators (minus the first param,
	 *  which is always the index-value we are valuating).
	 * @note You may not add, remove or change (setting the value of) items while
	 *  valuating. You may also not (re)sort while valuating.
	 * @note You can write your own valuators and use them. Just remember that
	 *  the first parameter should be the index-value, and it should return
	 *  an integer.
	 * @note Example:
	 * @code
	 *  list.Valuate(ScriptBridge.GetPrice, 5);
	 *  list.Valuate(ScriptBridge.GetMaxLength);
	 *  function MyVal(bridge_id, myparam)
	 *  {
	 *    return myparam * bridge_id; // This is silly
	 *  }
	 *  list.Valuate(MyVal, 12);
	 * @endcode
	 */
	void Valuate(function valuator_function, ...);
#endif /* DOXYGEN_API */
};

#endif /* SCRIPT_LIST_HPP */
