/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file saveload.cpp
 * All actions handling saving and loading goes on in this file. The general actions
 * are as follows for saving a game (loading is analogous):
 * <ol>
 * <li>initialize the writer by creating a temporary memory-buffer for it
 * <li>go through all to-be saved elements, each 'chunk' (#ChunkHandler) prefixed by a label
 * <li>use their description array (#SaveLoad) to know what elements to save and in what version
 *    of the game it was active (used when loading)
 * <li>write all data byte-by-byte to the temporary buffer so it is endian-safe
 * <li>when the buffer is full; flush it to the output (eg save to file) (_sl.buf, _sl.bufp, _sl.bufe)
 * <li>repeat this until everything is done, and flush any remaining output to file
 * </ol>
 */

#include "../stdafx.h"
#include "../debug.h"
#include "../station_base.h"
#include "../thread.h"
#include "../town.h"
#include "../network/network.h"
#include "../window_func.h"
#include "../strings_func.h"
#include "../core/bitmath_func.hpp"
#include "../core/endian_func.hpp"
#include "../vehicle_base.h"
#include "../company_func.h"
#include "../date_func.h"
#include "../autoreplace_base.h"
#include "../roadstop_base.h"
#include "../linkgraph/linkgraph.h"
#include "../linkgraph/linkgraphjob.h"
#include "../statusbar_gui.h"
#include "../fileio_func.h"
#include "../gamelog.h"
#include "../string_func.h"
#include "../fios.h"
#include "../load_check.h"
#include "../error.h"
#include "../scope.h"
#include "../newgrf_railtype.h"
#include "../newgrf_roadtype.h"
#include "../core/ring_buffer.hpp"
#include "../timer/timer_game_tick.h"
#include <atomic>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __EMSCRIPTEN__
#	include <emscripten.h>
#endif
#ifndef _WIN32
#	include <unistd.h>
#endif /* _WIN32 */

#include "../tbtr_template_vehicle.h"
#include "../3rdparty/cpp-btree/btree_map.h"

#include "table/strings.h"

#include "saveload_internal.h"
#include "saveload_filter.h"
#include "saveload_buffer.h"
#include "extended_ver_sl.h"

#include <vector>

#include "../thread.h"
#include <mutex>
#include <condition_variable>

#include "../safeguards.h"

extern const SaveLoadVersion SAVEGAME_VERSION = SLV_CUSTOM_SUBSIDY_DURATION; ///< Current savegame version of OpenTTD.
extern const SaveLoadVersion MAX_LOAD_SAVEGAME_VERSION = (SaveLoadVersion)(SL_MAX_VERSION - 1); ///< Max loadable savegame version of OpenTTD.

const SaveLoadVersion SAVEGAME_VERSION_EXT = (SaveLoadVersion)(0x8000); ///< Savegame extension indicator mask

SavegameType _savegame_type;      ///< type of savegame we are loading
FileToSaveLoad _file_to_saveload; ///< File to save or load in the openttd loop.

uint32_t _ttdp_version;       ///< version of TTDP savegame (if applicable)
SaveLoadVersion _sl_version;  ///< the major savegame version identifier
uint8_t _sl_minor_version;    ///< the minor savegame version, DO NOT USE!
std::string _savegame_format; ///< how to compress savegames
bool _do_autosave;            ///< are we doing an autosave at the moment?

extern bool _sl_is_ext_version;
extern bool _sl_maybe_springpp;
extern bool _sl_maybe_chillpp;
extern bool _sl_upstream_mode;

[[noreturn]] void SlErrorCorruptWithChunk(std::string_view msg);

namespace upstream_sl {
	void SlNullPointers();
	void SlNullPointerChunkByID(uint32_t);
	void SlLoadChunks();
	void SlLoadChunkByID(uint32_t id);
	void SlLoadCheckChunks();
	void SlLoadCheckChunkByID(uint32_t id);
	void SlFixPointers();
	void SlFixPointerChunkByID(uint32_t id);
	void SlSaveChunkChunkByID(uint32_t id);
	void SlResetLoadState();
	void FixSCCEncoded(std::string &str, bool fix_code);
	void FixSCCEncodedNegative(std::string &str);
}

/** What are we currently doing? */
enum SaveLoadAction {
	SLA_LOAD,        ///< loading
	SLA_SAVE,        ///< saving
	SLA_PTRS,        ///< fixing pointers
	SLA_NULL,        ///< null all pointers (on loading error)
	SLA_LOAD_CHECK,  ///< partial loading into #_load_check_data
};

enum NeedLength {
	NL_NONE = 0,       ///< not working in NeedLength mode
	NL_WANTLENGTH = 1, ///< writing length and data
};

void ReadBuffer::SkipBytesSlowPath(size_t bytes)
{
	bytes -= (this->bufe - this->bufp);
	while (true) {
		size_t len = this->reader->Read(this->buf, lengthof(this->buf));
		if (len == 0) SlErrorCorruptWithChunk("Unexpected end of chunk");
		this->read += len;
		if (len >= bytes) {
			this->bufp = this->buf + bytes;
			this->bufe = this->buf + len;
			return;
		} else {
			bytes -= len;
		}
	}
}

void ReadBuffer::AcquireBytes(size_t bytes)
{
	size_t remainder = this->bufe - this->bufp;
	if (remainder) {
		memmove(this->buf, this->bufp, remainder);
	}
	size_t total = remainder;
	size_t target = remainder + bytes;
	do {
		size_t len = this->reader->Read(this->buf + total, lengthof(this->buf) - total);
		if (len == 0) SlErrorCorruptWithChunk("Unexpected end of chunk");

		total += len;
	} while (total < target);

	this->read += total - remainder;
	this->bufp = this->buf;
	this->bufe = this->buf + total;
}

void MemoryDumper::FinaliseBlock()
{
	assert(this->saved_buf == nullptr);
	if (!this->blocks.empty()) {
		size_t s = MEMORY_CHUNK_SIZE - (this->bufe - this->buf);
		this->blocks.back().size = s;
		this->completed_block_bytes += s;
	}
	this->buf = this->bufe = nullptr;
}

void MemoryDumper::AllocateBuffer()
{
	if (this->saved_buf) {
		const size_t offset = this->buf - this->autolen_buf;
		const size_t size = (this->autolen_buf_end - this->autolen_buf) * 2;
		this->autolen_buf = ReallocT<uint8_t>(this->autolen_buf, size);
		this->autolen_buf_end = this->autolen_buf + size;
		this->buf = this->autolen_buf + offset;
		this->bufe = this->autolen_buf_end;
		return;
	}
	this->FinaliseBlock();
	this->buf = MallocT<uint8_t>(MEMORY_CHUNK_SIZE);
	this->blocks.emplace_back(this->buf);
	this->bufe = this->buf + MEMORY_CHUNK_SIZE;
}

/**
 * Flush this dumper into a writer.
 * @param writer The filter we want to use.
 */
void MemoryDumper::Flush(SaveFilter &writer)
{
	this->FinaliseBlock();

	size_t block_count = this->blocks.size();
	Debug(sl, 3, "About to serialise {} bytes in {} blocks", this->completed_block_bytes, block_count);
	for (size_t i = 0; i < block_count; i++) {
		writer.Write(this->blocks[i].data, this->blocks[i].size);
	}
	Debug(sl, 3, "Serialised {} bytes in {} blocks",  this->completed_block_bytes, block_count);

	writer.Finish();
}

void MemoryDumper::StartAutoLength()
{
	assert(this->saved_buf == nullptr);

	this->saved_buf = this->buf;
	this->saved_bufe = this->bufe;
	this->buf = this->autolen_buf;
	this->bufe = this->autolen_buf_end;
}

std::span<uint8_t> MemoryDumper::StopAutoLength()
{
	assert(this->saved_buf != nullptr);
	auto res = std::span(this->autolen_buf, this->buf - this->autolen_buf);

	this->buf = this->saved_buf;
	this->bufe = this->saved_bufe;
	this->saved_buf = this->saved_bufe = nullptr;
	return res;
}

/**
 * Get the size of the memory dump made so far.
 * @return The size.
 */
size_t MemoryDumper::GetSize() const
{
	assert(this->saved_buf == nullptr);
	return this->completed_block_bytes + (this->bufe ? (MEMORY_CHUNK_SIZE - (this->bufe - this->buf)) : 0);
}

/**
 * Get the size of the memory dump made so far.
 * @return The size.
 */
size_t MemoryDumper::GetWriteOffsetGeneric() const
{
	if (this->saved_buf != nullptr) {
		return this->buf - this->autolen_buf;
	} else {
		return this->GetSize();
	}
}

enum SaveLoadBlockFlags {
	SLBF_TABLE_ARRAY_LENGTH_PREFIX_MISSING, ///< Table chunk arrays were incorrectly saved without the length prefix, skip reading the length prefix on load
};

/** The saveload struct, containing reader-writer functions, buffer, version, etc. */
struct SaveLoadParams {
	SaveLoadAction action;               ///< are we doing a save or a load atm.
	NeedLength need_length;              ///< working in NeedLength (Autolength) mode?
	uint8_t block_mode;                  ///< ???
	uint8_t block_flags;                 ///< block flags: SaveLoadBlockFlags
	bool error;                          ///< did an error occur or not

	size_t obj_len;                      ///< the length of the current object we are busy with
	int array_index, last_array_index;   ///< in the case of an array, the current and last positions
	bool expect_table_header;            ///< In the case of a table, if the header is saved/loaded.

	uint32_t current_chunk_id;           ///< Current chunk ID

	btree::btree_map<uint32_t, uint8_t> chunk_block_modes; ///< Chunk block modes

	std::unique_ptr<MemoryDumper> dumper;///< Memory dumper to write the savegame to.
	std::shared_ptr<SaveFilter> sf;      ///< Filter to write the savegame to.

	std::unique_ptr<ReadBuffer> reader;  ///< Savegame reading buffer.
	std::shared_ptr<LoadFilter> lf;      ///< Filter to read the savegame from.

	StringID error_str;                  ///< the translatable error message to show
	std::string extra_msg;               ///< the error message

	bool saveinprogress;                 ///< Whether there is currently a save in progress.
	SaveModeFlags save_flags;            ///< Save mode flags
};

static SaveLoadParams _sl; ///< Parameters used for/at saveload.

ReadBuffer *ReadBuffer::GetCurrent()
{
	return _sl.reader.get();
}

MemoryDumper *MemoryDumper::GetCurrent()
{
	return _sl.dumper.get();
}

static const std::vector<ChunkHandler> &ChunkHandlers()
{
	/* These define the chunks */
	extern const ChunkHandlerTable _version_ext_chunk_handlers;
	extern const ChunkHandlerTable _gamelog_chunk_handlers;
	extern const ChunkHandlerTable _map_chunk_handlers;
	extern const ChunkHandlerTable _misc_chunk_handlers;
	extern const ChunkHandlerTable _name_chunk_handlers;
	extern const ChunkHandlerTable _cheat_chunk_handlers;
	extern const ChunkHandlerTable _setting_chunk_handlers;
	extern const ChunkHandlerTable _company_chunk_handlers;
	extern const ChunkHandlerTable _engine_chunk_handlers;
	extern const ChunkHandlerTable _veh_chunk_handlers;
	extern const ChunkHandlerTable _waypoint_chunk_handlers;
	extern const ChunkHandlerTable _depot_chunk_handlers;
	extern const ChunkHandlerTable _order_chunk_handlers;
	extern const ChunkHandlerTable _town_chunk_handlers;
	extern const ChunkHandlerTable _sign_chunk_handlers;
	extern const ChunkHandlerTable _station_chunk_handlers;
	extern const ChunkHandlerTable _industry_chunk_handlers;
	extern const ChunkHandlerTable _economy_chunk_handlers;
	extern const ChunkHandlerTable _subsidy_chunk_handlers;
	extern const ChunkHandlerTable _cargomonitor_chunk_handlers;
	extern const ChunkHandlerTable _goal_chunk_handlers;
	extern const ChunkHandlerTable _story_page_chunk_handlers;
	extern const ChunkHandlerTable _league_chunk_handlers;
	extern const ChunkHandlerTable _ai_chunk_handlers;
	extern const ChunkHandlerTable _game_chunk_handlers;
	extern const ChunkHandlerTable _animated_tile_chunk_handlers;
	extern const ChunkHandlerTable _newgrf_chunk_handlers;
	extern const ChunkHandlerTable _group_chunk_handlers;
	extern const ChunkHandlerTable _cargopacket_chunk_handlers;
	extern const ChunkHandlerTable _autoreplace_chunk_handlers;
	extern const ChunkHandlerTable _labelmaps_chunk_handlers;
	extern const ChunkHandlerTable _linkgraph_chunk_handlers;
	extern const ChunkHandlerTable _airport_chunk_handlers;
	extern const ChunkHandlerTable _object_chunk_handlers;
	extern const ChunkHandlerTable _persistent_storage_chunk_handlers;
	extern const ChunkHandlerTable _trace_restrict_chunk_handlers;
	extern const ChunkHandlerTable _signal_chunk_handlers;
	extern const ChunkHandlerTable _plan_chunk_handlers;
	extern const ChunkHandlerTable _template_replacement_chunk_handlers;
	extern const ChunkHandlerTable _template_vehicle_chunk_handlers;
	extern const ChunkHandlerTable _bridge_signal_chunk_handlers;
	extern const ChunkHandlerTable _tunnel_chunk_handlers;
	extern const ChunkHandlerTable _train_speed_adaptation_chunk_handlers;
	extern const ChunkHandlerTable _new_signal_chunk_handlers;
	extern const ChunkHandlerTable _debug_chunk_handlers;

	/** List of all chunks in a savegame. */
	static const ChunkHandlerTable _chunk_handler_tables[] = {
		_version_ext_chunk_handlers,
		_gamelog_chunk_handlers,
		_map_chunk_handlers,
		_misc_chunk_handlers,
		_name_chunk_handlers,
		_cheat_chunk_handlers,
		_setting_chunk_handlers,
		_veh_chunk_handlers,
		_waypoint_chunk_handlers,
		_depot_chunk_handlers,
		_order_chunk_handlers,
		_industry_chunk_handlers,
		_economy_chunk_handlers,
		_subsidy_chunk_handlers,
		_cargomonitor_chunk_handlers,
		_goal_chunk_handlers,
		_story_page_chunk_handlers,
		_league_chunk_handlers,
		_engine_chunk_handlers,
		_town_chunk_handlers,
		_sign_chunk_handlers,
		_station_chunk_handlers,
		_company_chunk_handlers,
		_ai_chunk_handlers,
		_game_chunk_handlers,
		_animated_tile_chunk_handlers,
		_newgrf_chunk_handlers,
		_group_chunk_handlers,
		_cargopacket_chunk_handlers,
		_autoreplace_chunk_handlers,
		_labelmaps_chunk_handlers,
		_linkgraph_chunk_handlers,
		_airport_chunk_handlers,
		_object_chunk_handlers,
		_persistent_storage_chunk_handlers,
		_trace_restrict_chunk_handlers,
		_signal_chunk_handlers,
		_plan_chunk_handlers,
		_template_replacement_chunk_handlers,
		_template_vehicle_chunk_handlers,
		_bridge_signal_chunk_handlers,
		_tunnel_chunk_handlers,
		_train_speed_adaptation_chunk_handlers,
		_new_signal_chunk_handlers,
		_debug_chunk_handlers,
	};

	static std::vector<ChunkHandler> _chunk_handlers;

	if (_chunk_handlers.empty()) {
		for (auto &chunk_handler_table : _chunk_handler_tables) {
			for (auto &chunk_handler : chunk_handler_table) {
				_chunk_handlers.push_back(chunk_handler);
			}
		}
	}

	return _chunk_handlers;
}

/** Null all pointers (convert index -> nullptr) */
static void SlNullPointers()
{
	if (_sl_upstream_mode) {
		upstream_sl::SlNullPointers();
		return;
	}

	_sl.action = SLA_NULL;

	/* Do upstream chunk tests before clearing version data */
	ring_buffer<uint32_t> upstream_null_chunks;
	for (auto &ch : ChunkHandlers()) {
		_sl.current_chunk_id = ch.id;
		if (ch.special_proc != nullptr && ch.special_proc(ch.id, CSLSO_PRE_NULL_PTRS) == CSLSOR_UPSTREAM_NULL_PTRS) {
			upstream_null_chunks.push_back(ch.id);
		}
	}

	/* We don't want any savegame conversion code to run
	 * during NULLing; especially those that try to get
	 * pointers from other pools. */
	_sl_version = SAVEGAME_VERSION;
	SlXvSetCurrentState();

	for (auto &ch : ChunkHandlers()) {
		_sl.current_chunk_id = ch.id;
		if (!upstream_null_chunks.empty() && upstream_null_chunks.front() == ch.id) {
			upstream_null_chunks.pop_front();
			SlExecWithSlVersion(MAX_LOAD_SAVEGAME_VERSION, [&]() {
				upstream_sl::SlNullPointerChunkByID(ch.id);
			});
			continue;
		}

		if (ch.ptrs_proc != nullptr) {
			Debug(sl, 3, "Nulling pointers for {}", ChunkIDDumper()(ch.id));
			ch.ptrs_proc();
		}
	}

	assert(_sl.action == SLA_NULL);
}

struct ThreadSlErrorException {
	StringID string;
	std::string extra_msg;
};

/**
 * Error handler. Sets everything up to show an error message and to clean
 * up the mess of a partial savegame load.
 * @param string The translatable error message to show.
 * @param extra_msg An extra error message coming from one of the APIs.
 * @note This function does never return as it throws an exception to
 *       break out of all the saveload code.
 */
[[noreturn]] void SlError(StringID string, std::string extra_msg)
{
	if (IsNonMainThread() && IsNonGameThread() && _sl.action != SLA_SAVE) {
		throw ThreadSlErrorException{ string, std::move(extra_msg) };
	}

	/* Distinguish between loading into _load_check_data vs. normal save/load. */
	if (_sl.action == SLA_LOAD_CHECK) {
		_load_check_data.error = string;
		_load_check_data.error_msg = std::move(extra_msg);
	} else {
		_sl.error_str = string;
		_sl.extra_msg = std::move(extra_msg);
	}

	/* We have to nullptr all pointers here; we might be in a state where
	 * the pointers are actually filled with indices, which means that
	 * when we access them during cleaning the pool dereferences of
	 * those indices will be made with segmentation faults as result. */
	if (_sl.action == SLA_LOAD || _sl.action == SLA_PTRS) SlNullPointers();

	/* Logging could be active. */
	GamelogStopAnyAction();

	throw std::exception();
}

/**
 * Error handler for corrupt savegames. Sets everything up to show the
 * error message and to clean up the mess of a partial savegame load.
 * @param msg Location the corruption has been spotted.
 * @note This function does never return as it throws an exception to
 *       break out of all the saveload code.
 */
[[noreturn]] void SlErrorCorrupt(std::string msg)
{
	SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_SAVEGAME, std::move(msg));
}

[[noreturn]] void SlErrorCorruptWithChunk(std::string_view msg)
{
	format_buffer out;
	out.append(msg);
	if (_sl.current_chunk_id != 0) out.format(" ({})", ChunkIDDumper()(_sl.current_chunk_id));
	SlErrorCorrupt(out.to_string());
}

typedef void (*AsyncSaveFinishProc)();                      ///< Callback for when the savegame loading is finished.

struct AsyncSaveThread {
	std::atomic<bool> exit_thread;                ///< Signal that the thread should exit early
	std::atomic<AsyncSaveFinishProc> finish_proc; ///< Callback to call when the savegame saving is finished.
	std::thread save_thread;                      ///< The thread we're using to compress and write a savegame

	void SetAsyncSaveFinish(AsyncSaveFinishProc proc)
	{
		if (_exit_game || this->exit_thread.load(std::memory_order_relaxed)) return;

		while (this->finish_proc.load(std::memory_order_acquire) != nullptr) {
			CSleep(10);
			if (_exit_game || this->exit_thread.load(std::memory_order_relaxed)) return;
		}

		this->finish_proc.store(proc, std::memory_order_release);
	}

	void ProcessAsyncSaveFinish()
	{
		AsyncSaveFinishProc proc = this->finish_proc.exchange(nullptr, std::memory_order_acq_rel);
		if (proc == nullptr) return;

		proc();

		if (this->save_thread.joinable()) {
			this->save_thread.join();
		}
	}

	void WaitTillSaved()
	{
		if (!this->save_thread.joinable()) return;

		this->save_thread.join();

		/* Make sure every other state is handled properly as well. */
		this->ProcessAsyncSaveFinish();
	}

	~AsyncSaveThread()
	{
		this->exit_thread.store(true, std::memory_order_relaxed);

		if (this->save_thread.joinable()) {
			this->save_thread.join();
		}
	}
};
static AsyncSaveThread _async_save_thread;

/**
 * Called by save thread to tell we finished saving.
 * @param proc The callback to call when saving is done.
 */
static void SetAsyncSaveFinish(AsyncSaveFinishProc proc)
{
	_async_save_thread.SetAsyncSaveFinish(proc);
}

/**
 * Handle async save finishes.
 */
void ProcessAsyncSaveFinish()
{
	_async_save_thread.ProcessAsyncSaveFinish();
}

/**
 * Wrapper for reading a uint8_t from the buffer.
 * @return The read uint8_t.
 */
uint8_t SlReadByte()
{
	return _sl.reader->ReadByte();
}

/**
 * Read in bytes from the file/data structure but don't do
 * anything with them, discarding them in effect
 * @param length The amount of bytes that is being treated this way
 */
void SlSkipBytes(size_t length)
{
	return _sl.reader->SkipBytes(length);
}

uint16_t SlReadUint16()
{
	return _sl.reader->ReadRawBytes(2).RawReadUint16();
}

uint32_t SlReadUint32()
{
	return _sl.reader->ReadRawBytes(4).RawReadUint32();
}

uint64_t SlReadUint64()
{
	return _sl.reader->ReadRawBytes(8).RawReadUint64();
}

/**
 * Wrapper for writing a uint8_t to the dumper.
 * @param b The uint8_t to write.
 */
void SlWriteByte(uint8_t b)
{
	_sl.dumper->WriteByte(b);
}

void SlWriteUint16(uint16_t v)
{
	_sl.dumper->RawWriteBytes(2).RawWriteUint16(v);
}

void SlWriteUint32(uint32_t v)
{
	_sl.dumper->RawWriteBytes(4).RawWriteUint32(v);
}

void SlWriteUint64(uint64_t v)
{
	_sl.dumper->RawWriteBytes(8).RawWriteUint64(v);
}

/**
 * Returns number of bytes read so far
 * May only be called during a load/load check action
 */
size_t SlGetBytesRead()
{
	assert(_sl.action == SLA_LOAD || _sl.action == SLA_LOAD_CHECK);
	return _sl.reader->GetSize();
}

/**
 * Returns number of bytes written so far
 * May only be called during a save action
 */
size_t SlGetBytesWritten()
{
	assert(_sl.action == SLA_SAVE);
	return _sl.dumper->GetSize();
}

/**
 * Read in the header descriptor of an object or an array.
 * If the highest bit is set (7), then the index is bigger than 127
 * elements, so use the next byte to read in the real value.
 * The actual value is then both bytes added with the first shifted
 * 8 bits to the left, and dropping the highest bit (which only indicated a big index).
 * x = ((x & 0x7F) << 8) + SlReadByte();
 * @return Return the value of the index
 */
uint SlReadSimpleGamma()
{
	return _sl.reader->ReadSimpleGamma();
}

uint ReadBuffer::ReadSimpleGamma()
{
	if (unlikely(this->bufp == this->bufe)) {
		this->AcquireBytes();
	}

	uint8_t first_byte = *this->bufp++;
	uint extra_bytes = std::countl_one<uint8_t>(first_byte);
	if (extra_bytes == 0) return first_byte;
	if (extra_bytes > 4) SlErrorCorruptWithChunk("Unsupported gamma");

	uint result = first_byte & (0x7F >> extra_bytes);

	this->CheckBytes(extra_bytes);
	uint8_t *b = this->bufp;
	this->bufp += extra_bytes;
	for (uint i = 0; i < extra_bytes; i++) {
		result <<= 8;
		result |= *b++;
	}
	return result;
}

/**
 * Write the header descriptor of an object or an array.
 * If the element is bigger than 127, use 2 bytes for saving
 * and use the highest byte of the first written one as a notice
 * that the length consists of 2 bytes, etc.. like this:
 * 0xxxxxxx
 * 10xxxxxx xxxxxxxx
 * 110xxxxx xxxxxxxx xxxxxxxx
 * 1110xxxx xxxxxxxx xxxxxxxx xxxxxxxx
 * 11110--- xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
 * We could extend the scheme ad infinum to support arbitrarily
 * large chunks, but as sizeof(size_t) == 4 is still very common
 * we don't support anything above 32 bits. That's why in the last
 * case the 3 most significant bits are unused.
 * @param i Index being written
 */

void SlWriteSimpleGamma(size_t i)
{
	MemoryDumper *dumper = MemoryDumper::GetCurrent();
	RawMemoryDumper raw_dumper = dumper->BorrowRawWriteBytes(SlGetMaxGammaLength());
	raw_dumper.RawWriteSimpleGamma(i);
	dumper->ReturnRawWriteBytes(raw_dumper);
}

void RawMemoryDumper::RawWriteSimpleGamma(size_t i)
{
	const uint8_t data_bits = FindLastBit(i);
	assert(data_bits < 32);

	uint8_t extra_bytes = data_bits / 7;
	this->buf += 1 + extra_bytes;
	uint8_t *b = this->buf;

	uint8_t first_byte = 0;
	while (extra_bytes > 0) {
		first_byte >>= 1;
		first_byte |= 0x80;
		extra_bytes--;
		b--;
		*b = (uint8_t)i;
		i >>= 8;
	}
	b--;
	*b = first_byte | (uint8_t)i;
}

/** Return how many bytes used to encode a gamma value */
uint SlGetGammaLength(size_t i)
{
	return 1 + (FindLastBit(i) / 7);
}

static inline uint SlReadSparseIndex()
{
	return SlReadSimpleGamma();
}

static inline void SlWriteSparseIndex(uint index)
{
	SlWriteSimpleGamma(index);
}

static inline uint SlReadArrayLength()
{
	return SlReadSimpleGamma();
}

static inline void SlWriteArrayLength(size_t length)
{
	SlWriteSimpleGamma(length);
}

static inline uint SlGetArrayLength(size_t length)
{
	return SlGetGammaLength(length);
}

/**
 * Return the size in bytes of a certain type of normal/atomic variable
 * as it appears in memory. See VarTypes
 * @param conv VarType type of variable that is used for calculating the size
 * @return Return the size of this type in bytes
 */
static inline uint SlCalcConvMemLen(VarType conv)
{
	static const uint8_t conv_mem_size[] = {1, 1, 1, 2, 2, 4, 4, 8, 8, 0};

	switch (GetVarMemType(conv)) {
		case SLE_VAR_STR:
		case SLE_VAR_STRQ:
			return SlReadArrayLength();

		default:
			uint8_t type = GetVarMemType(conv) >> 4;
			assert(type < lengthof(conv_mem_size));
			return conv_mem_size[type];
	}
}

/**
 * Return the size in bytes of a certain type of normal/atomic variable
 * as it appears in a saved game. See VarTypes
 * @param conv VarType type of variable that is used for calculating the size
 * @return Return the size of this type in bytes
 */
static inline uint8_t SlCalcConvFileLen(VarType conv)
{
	uint8_t type = GetVarFileType(conv);
	if (type == SLE_FILE_VEHORDERID) return SlXvIsFeaturePresent(XSLFI_MORE_VEHICLE_ORDERS) ? 2 : 1;
	static const uint8_t conv_file_size[] = {0, 1, 1, 2, 2, 4, 4, 8, 8, 2};
	assert(type < lengthof(conv_file_size));
	return conv_file_size[type];
}

/** Return the size in bytes of a reference (pointer) */
static inline size_t SlCalcRefLen()
{
	return IsSavegameVersionBefore(SLV_69) ? 2 : 4;
}

void SlSetArrayIndex(uint index)
{
	_sl.need_length = NL_WANTLENGTH;
	_sl.array_index = index;
}

static size_t _next_offs;

/**
 * Iterate through the elements of an array and read the whole thing
 * @return The index of the object, or -1 if we have reached the end of current block
 */
int SlIterateArray()
{
	int index;

	/* After reading in the whole array inside the loop
	 * we must have read in all the data, so we must be at end of current block. */
	if (_next_offs != 0 && _sl.reader->GetSize() != _next_offs) {
		Debug(sl, 1, "Invalid chunk size: {} != {}", _sl.reader->GetSize(), _next_offs);
		SlErrorCorruptFmt("Invalid chunk size iterating array - expected to be at position {}, actually at {}, ({})", _next_offs, _sl.reader->GetSize(), ChunkIDDumper()(_sl.current_chunk_id));
	}

	for (;;) {
		uint length = SlReadArrayLength();
		if (length == 0) {
			assert(!_sl.expect_table_header);
			_next_offs = 0;
			return -1;
		}

		_sl.obj_len = --length;
		_next_offs = _sl.reader->GetSize() + length;

		if (_sl.expect_table_header) {
			_sl.expect_table_header = false;
			return INT32_MAX;
		}

		switch (_sl.block_mode) {
			case CH_SPARSE_ARRAY:
			case CH_SPARSE_TABLE:
				index = (int)SlReadSparseIndex();
				break;
			case CH_ARRAY:
			case CH_TABLE:
				index = _sl.array_index++;
				break;
			default:
				Debug(sl, 0, "SlIterateArray error");
				return -1; // error
		}

		if (length != 0) return index;
	}
}

/**
 * Skip an array or sparse array
 */
void SlSkipArray()
{
	while (SlIterateArray() != -1) {
		SlSkipBytes(_next_offs - _sl.reader->GetSize());
	}
}

/**
 * Sets the length of either a RIFF object or the number of items in an array.
 * This lets us load an object or an array of arbitrary size
 * @param length The length of the sought object/array
 */
void SlSetLength(size_t length)
{
	assert(_sl.action == SLA_SAVE);

	switch (_sl.need_length) {
		case NL_WANTLENGTH:
			_sl.need_length = NL_NONE;
			if ((_sl.block_mode == CH_TABLE || _sl.block_mode == CH_SPARSE_TABLE) && _sl.expect_table_header) {
				_sl.expect_table_header = false;
				SlWriteArrayLength(length + 1);
				break;
			}

			switch (_sl.block_mode) {
				case CH_RIFF:
					/* Ugly encoding of >16M RIFF chunks
					 * The lower 24 bits are normal
					 * The uppermost 4 bits are bits 24:27
					 *
					 * If we have more than 28 bits, use an extra uint32_t and
					 * signal this using the extended chunk header */
#ifdef POINTER_IS_64BIT
					assert(length < (1LL << 32));
#endif
					if (length >= (1 << 28)) {
						/* write out extended chunk header */
						SlWriteByte(CH_EXT_HDR);
						SlWriteUint32(static_cast<uint32_t>(SLCEHF_BIG_RIFF));
					}
					SlWriteUint32(static_cast<uint32_t>((length & 0xFFFFFF) | ((length >> 24) << 28)));
					if (length >= (1 << 28)) {
						SlWriteUint32(static_cast<uint32_t>(length >> 28));
					}
					break;
				case CH_ARRAY:
				case CH_TABLE:
					assert(_sl.last_array_index <= _sl.array_index);
					while (++_sl.last_array_index <= _sl.array_index) {
						SlWriteArrayLength(1);
					}
					SlWriteArrayLength(length + 1);
					break;
				case CH_SPARSE_ARRAY:
				case CH_SPARSE_TABLE:
					SlWriteArrayLength(length + 1 + SlGetArrayLength(_sl.array_index)); // Also include length of sparse index.
					SlWriteSparseIndex(_sl.array_index);
					break;
				default: NOT_REACHED();
			}
			break;

		default: NOT_REACHED();
	}
}

void SlCopyBytesRead(void *p, size_t length)
{
	_sl.reader->CopyBytes((uint8_t *)p, length);
}

void SlCopyBytesWrite(void *p, size_t length)
{
	_sl.dumper->CopyBytes((uint8_t *)p, length);
}

/**
 * Save/Load bytes. These do not need to be converted to Little/Big Endian
 * so directly write them or read them to/from file
 * @param ptr The source or destination of the object being manipulated
 * @param length number of bytes this fast CopyBytes lasts
 */
static void SlCopyBytes(void *ptr, size_t length)
{
	switch (_sl.action) {
		case SLA_LOAD_CHECK:
		case SLA_LOAD:
			SlCopyBytesRead(ptr, length);
			break;
		case SLA_SAVE:
			SlCopyBytesWrite(ptr, length);
			break;
		default: NOT_REACHED();
	}
}

/**
 * Read the given amount of bytes from the buffer into the string.
 * @param str The string to write to.
 * @param length The amount of bytes to read into the string.
 * @note Does not perform any validation on validity of the string.
 */
void SlReadString(std::string &str, size_t length)
{
	str.resize(length);
	SlCopyBytesRead((uint8_t *)str.data(), length);
}

/** Get the length of the current object */
size_t SlGetFieldLength()
{
	return _sl.obj_len;
}

/**
 * Return a signed-long version of the value of a setting
 * @param ptr pointer to the variable
 * @param conv type of variable, can be a non-clean
 * type, eg one with other flags because it is parsed
 * @return returns the value of the pointer-setting
 */
int64_t ReadValue(const void *ptr, VarType conv)
{
	switch (GetVarMemType(conv)) {
		case SLE_VAR_BL:  return (*(const bool   *)ptr != 0);
		case SLE_VAR_I8:  return *(const int8_t  *)ptr;
		case SLE_VAR_U8:  return *(const uint8_t *)ptr;
		case SLE_VAR_I16: return *(const int16_t *)ptr;
		case SLE_VAR_U16: return *(const uint16_t*)ptr;
		case SLE_VAR_I32: return *(const int32_t *)ptr;
		case SLE_VAR_U32: return *(const uint32_t*)ptr;
		case SLE_VAR_I64: return *(const int64_t *)ptr;
		case SLE_VAR_U64: return *(const uint64_t*)ptr;
		case SLE_VAR_NULL:return 0;
		default: NOT_REACHED();
	}
}

/**
 * Write the value of a setting
 * @param ptr pointer to the variable
 * @param conv type of variable, can be a non-clean type, eg
 *             with other flags. It is parsed upon read
 * @param val the new value being given to the variable
 */
void WriteValue(void *ptr, VarType conv, int64_t val)
{
	switch (GetVarMemType(conv)) {
		case SLE_VAR_BL:  *(bool    *)ptr = (val != 0);  break;
		case SLE_VAR_I8:  *(int8_t  *)ptr = val; break;
		case SLE_VAR_U8:  *(uint8_t *)ptr = val; break;
		case SLE_VAR_I16: *(int16_t *)ptr = val; break;
		case SLE_VAR_U16: *(uint16_t*)ptr = val; break;
		case SLE_VAR_I32: *(int32_t *)ptr = val; break;
		case SLE_VAR_U32: *(uint32_t*)ptr = val; break;
		case SLE_VAR_I64: *(int64_t *)ptr = val; break;
		case SLE_VAR_U64: *(uint64_t*)ptr = val; break;
		case SLE_VAR_NAME: *reinterpret_cast<std::string *>(ptr) = CopyFromOldName(val); break;
		case SLE_VAR_CNAME: *(TinyString*)ptr = CopyFromOldName(val); break;
		case SLE_VAR_NULL: break;
		default: NOT_REACHED();
	}
}

void SlSaveValue(int64_t x, VarType conv)
{
	/* Write the value to the file and check if its value is in the desired range */
	switch (GetVarFileType(conv)) {
		case SLE_FILE_I8: assert(x >= -128 && x <= 127);     SlWriteByte(x);break;
		case SLE_FILE_U8: assert(x >= 0 && x <= 255);        SlWriteByte(x);break;
		case SLE_FILE_I16:assert(x >= -32768 && x <= 32767); SlWriteUint16(x);break;
		case SLE_FILE_STRINGID:
		case SLE_FILE_VEHORDERID:
		case SLE_FILE_U16:assert(x >= 0 && x <= 65535);      SlWriteUint16(x);break;
		case SLE_FILE_I32:
		case SLE_FILE_U32:                                   SlWriteUint32((uint32_t)x);break;
		case SLE_FILE_I64:
		case SLE_FILE_U64:                                   SlWriteUint64(x);break;
		default: NOT_REACHED();
	}
}

int64_t SlLoadValue(VarType conv)
{
	int64_t x;
	/* Read a value from the file */
	switch (GetVarFileType(conv)) {
		case SLE_FILE_I8:  x = (int8_t  )SlReadByte();   break;
		case SLE_FILE_U8:  x = (uint8_t )SlReadByte();   break;
		case SLE_FILE_I16: x = (int16_t )SlReadUint16(); break;
		case SLE_FILE_U16: x = (uint16_t)SlReadUint16(); break;
		case SLE_FILE_I32: x = (int32_t )SlReadUint32(); break;
		case SLE_FILE_U32: x = (uint32_t)SlReadUint32(); break;
		case SLE_FILE_I64: x = (int64_t )SlReadUint64(); break;
		case SLE_FILE_U64: x = (uint64_t)SlReadUint64(); break;
		case SLE_FILE_STRINGID: x = RemapOldStringID((uint16_t)SlReadUint16()); break;
		case SLE_FILE_VEHORDERID:
			if (SlXvIsFeaturePresent(XSLFI_MORE_VEHICLE_ORDERS)) {
				x = (uint16_t)SlReadUint16();
			} else {
				VehicleOrderID id = (uint8_t)SlReadByte();
				x = (id == 0xFF) ? INVALID_VEH_ORDER_ID : id;
			}
			break;
		default: NOT_REACHED();
	}

	return x;
}

/**
 * Handle all conversion and typechecking of variables here.
 * In the case of saving, read in the actual value from the struct
 * and then write them to file, endian safely. Loading a value
 * goes exactly the opposite way
 * @param ptr The object being filled/read
 * @param conv VarType type of the current element of the struct
 */
template <SaveLoadAction action>
static void SlSaveLoadConvGeneric(void *ptr, VarType conv)
{
	switch (action) {
		case SLA_SAVE: {
			SlSaveValue(ReadValue(ptr, conv), conv);
			break;
		}
		case SLA_LOAD_CHECK:
		case SLA_LOAD: {
			/* Write The value to the struct. These ARE endian safe. */
			WriteValue(ptr, conv, SlLoadValue(conv));
			break;
		}
		case SLA_PTRS: break;
		case SLA_NULL: break;
		default: NOT_REACHED();
	}
}

void SlSaveLoadConv(void *ptr, VarType conv)
{
	switch (_sl.action) {
		case SLA_SAVE:
			SlSaveLoadConvGeneric<SLA_SAVE>(ptr, conv);
			return;
		case SLA_LOAD_CHECK:
		case SLA_LOAD:
			SlSaveLoadConvGeneric<SLA_LOAD>(ptr, conv);
			return;
		case SLA_PTRS:
		case SLA_NULL:
			return;
		default: NOT_REACHED();
	}
}

/**
 * Calculate the net length of a string. This is in almost all cases
 * just strlen(), but if the string is not properly terminated, we'll
 * resort to the maximum length of the buffer.
 * @param ptr pointer to the stringbuffer
 * @param length maximum length of the string (buffer). If -1 we don't care
 * about a maximum length, but take string length as it is.
 * @return return the net length of the string
 */
static inline size_t SlCalcNetStringLen(const char *ptr, size_t length)
{
	if (ptr == nullptr) return 0;
	return std::min(strlen(ptr), length - 1);
}

/**
 * Calculate the gross length of the std::string that it
 * will occupy in the savegame. This includes the real length,
 * and the length that the index will occupy.
 * @param str reference to the std::string
 * @return return the gross length of the string
 */
static inline size_t SlCalcStdStrLen(const std::string &str)
{
	return str.size() + SlGetArrayLength(str.size()); // also include the length of the index
}

/**
 * Calculate the gross length of the string that it
 * will occupy in the savegame. This includes the real length, returned
 * by SlCalcNetStringLen and the length that the index will occupy.
 * @param ptr pointer to the stringbuffer
 * @param length maximum length of the string (buffer size, etc.)
 * @param conv type of data been used
 * @return return the gross length of the string
 */
static inline size_t SlCalcStringLen(const void *ptr, size_t length, VarType conv)
{
	size_t len;
	const char *str;

	switch (GetVarMemType(conv)) {
		default: NOT_REACHED();
		case SLE_VAR_STR:
		case SLE_VAR_STRQ:
			str = *(const char * const *)ptr;
			len = SIZE_MAX;
			break;
	}

	len = SlCalcNetStringLen(str, len);
	return len + SlGetArrayLength(len); // also include the length of the index
}

/**
 * Save/Load a string.
 * @param ptr the string being manipulated
 * @param length of the string (full length)
 * @param conv must be SLE_FILE_STRING
 */
template <SaveLoadAction action>
void SlString(void *ptr, size_t length, VarType conv)
{
	switch (action) {
		case SLA_SAVE: {
			size_t len;
			switch (GetVarMemType(conv)) {
				default: NOT_REACHED();
				case SLE_VAR_STR:
				case SLE_VAR_STRQ:
					ptr = *(char **)ptr;
					len = SlCalcNetStringLen((char *)ptr, SIZE_MAX);
					break;
			}

			SlWriteArrayLength(len);
			SlCopyBytesWrite(ptr, len);
			break;
		}
		case SLA_LOAD_CHECK:
		case SLA_LOAD: {
			if ((conv & SLF_ALLOW_CONTROL) != 0 && IsSavegameVersionBefore(SLV_ENCODED_STRING_FORMAT) && SlXvIsFeatureMissing(XSLFI_ENCODED_STRING_FORMAT) && GetVarMemType(conv) != SLE_VAR_NULL) {
				/* Use std::string load path */
				std::string buffer;
				SlStdString(&buffer, conv);
				free(*(char **)ptr);
				if (buffer.empty()) {
					*(char **)ptr = nullptr;
				} else {
					*(char **)ptr = stredup(buffer.data(), buffer.data() + buffer.size());
				}
				break;
			}

			size_t len = SlReadArrayLength();

			switch (GetVarMemType(conv)) {
				default: NOT_REACHED();
				case SLE_VAR_NULL:
					SlSkipBytes(len);
					return;
				case SLE_VAR_STR:
				case SLE_VAR_STRQ: // Malloc'd string, free previous incarnation, and allocate
					free(*(char **)ptr);
					if (len == 0) {
						*(char **)ptr = nullptr;
						return;
					} else {
						*(char **)ptr = MallocT<char>(len + 1); // terminating '\0'
						ptr = *(char **)ptr;
						SlCopyBytesRead(ptr, len);
					}
					break;
			}

			((char *)ptr)[len] = '\0'; // properly terminate the string
			StringValidationSettings settings = SVS_REPLACE_WITH_QUESTION_MARK;
			if ((conv & SLF_ALLOW_CONTROL) != 0) {
				settings = settings | SVS_ALLOW_CONTROL_CODE;
			}
			if ((conv & SLF_ALLOW_NEWLINE) != 0) {
				settings = settings | SVS_ALLOW_NEWLINE;
			}
			StrMakeValidInPlace((char *)ptr, (char *)ptr + len, settings);
			break;
		}
		case SLA_PTRS: break;
		case SLA_NULL: break;
		default: NOT_REACHED();
	}
}

/**
 * Save/Load a \c std::string.
 * @param ptr the string being manipulated
 * @param conv must be SLE_FILE_STRING
 */
template <SaveLoadAction action>
void SlStdStringGeneric(std::string *ptr, VarType conv)
{
	switch (action) {
		case SLA_SAVE: {
			dbg_assert(ptr != nullptr);
			std::string &str = *ptr;

			SlWriteArrayLength(str.size());
			SlCopyBytesWrite(str.data(), str.size());
			break;
		}
		case SLA_LOAD_CHECK:
		case SLA_LOAD: {
			size_t len = SlReadArrayLength();
			if (GetVarMemType(conv) == SLE_VAR_NULL) {
				SlSkipBytes(len);
				return;
			}

			dbg_assert(ptr != nullptr);
			std::string &str = *ptr;

			str.resize(len);
			SlCopyBytesRead(str.data(), len);

			StringValidationSettings settings = SVS_REPLACE_WITH_QUESTION_MARK;
			if ((conv & SLF_ALLOW_CONTROL) != 0) {
				settings = settings | SVS_ALLOW_CONTROL_CODE;
				if (IsSavegameVersionBefore(SLV_ENCODED_STRING_FORMAT) && SlXvIsFeatureMissing(XSLFI_ENCODED_STRING_FORMAT)) {
					upstream_sl::FixSCCEncoded(str, IsSavegameVersionBefore(SLV_169));
				}
				if (IsSavegameVersionBefore(SLV_FIX_SCC_ENCODED_NEGATIVE) && SlXvIsFeatureMissing(XSLFI_ENCODED_STRING_FORMAT, 2)) {
					upstream_sl::FixSCCEncodedNegative(str);
				}
			}
			if ((conv & SLF_ALLOW_NEWLINE) != 0) {
				settings = settings | SVS_ALLOW_NEWLINE;
			}
			StrMakeValidInPlace(str, settings);
			break;
		}
		case SLA_PTRS: break;
		case SLA_NULL: break;
		default: NOT_REACHED();
	}
}

/**
 * Save/Load a \c std::string.
 * @param ptr the string being manipulated
 * @param conv must be SLE_FILE_STRING
 */
void SlStdString(std::string *ptr, VarType conv)
{
	switch (_sl.action) {
		case SLA_SAVE:
			SlStdStringGeneric<SLA_SAVE>(ptr, conv);
			return;
		case SLA_LOAD_CHECK:
		case SLA_LOAD:
			SlStdStringGeneric<SLA_LOAD>(ptr, conv);
			return;
		case SLA_PTRS:
		case SLA_NULL:
			return;
		default: NOT_REACHED();
	}
}

/**
 * Return the size in bytes of a certain type of atomic array
 * @param length The length of the array counted in elements
 * @param conv VarType type of the variable that is used in calculating the size
 */
static inline size_t SlCalcArrayLen(size_t length, VarType conv)
{
	return SlCalcConvFileLen(conv) * length;
}

/**
 * Save/Load an array.
 * @param array The array being manipulated
 * @param length The length of the array in elements
 * @param conv VarType type of the atomic array (int, uint8_t, uint64_t, etc.)
 */
void SlArray(void *array, size_t length, VarType conv)
{
	if (_sl.action == SLA_PTRS || _sl.action == SLA_NULL) return;

	if (SlIsTableChunk()) {
		assert(_sl.need_length == NL_NONE);

		switch (_sl.action) {
			case SLA_SAVE:
				SlWriteArrayLength(length);
				break;

			case SLA_LOAD_CHECK:
			case SLA_LOAD: {
				if (!HasBit(_sl.block_flags, SLBF_TABLE_ARRAY_LENGTH_PREFIX_MISSING)) {
					size_t sv_length = SlReadArrayLength();
					if (GetVarMemType(conv) == SLE_VAR_NULL) {
						/* We don't know this field, so we assume the length in the savegame is correct. */
						length = sv_length;
					} else if (sv_length != length) {
						/* If the SLE_ARR changes size, a savegame bump is required
						 * and the developer should have written conversion lines.
						 * Error out to make this more visible. */
						SlErrorCorruptWithChunk("Fixed-length array is of wrong length");
					}
				}
				break;
			}

			default:
				break;
		}
	}

	/* Automatically calculate the length? */
	if (_sl.need_length != NL_NONE) {
		SlSetLength(SlCalcArrayLen(length, conv));
	}

	/* NOTICE - handle some buggy stuff, in really old versions everything was saved
	 * as a byte-type. So detect this, and adjust array size accordingly */
	if (_sl.action != SLA_SAVE && _sl_version == 0) {
		/* all arrays except difficulty settings */
		if (conv == SLE_INT16 || conv == SLE_UINT16 || conv == SLE_STRINGID ||
				conv == SLE_INT32 || conv == SLE_UINT32) {
			SlCopyBytesRead(array, length * SlCalcConvFileLen(conv));
			return;
		}
		/* used for conversion of Money 32bit->64bit */
		if (conv == (SLE_FILE_I32 | SLE_VAR_I64)) {
			for (uint i = 0; i < length; i++) {
				((int64_t*)array)[i] = (int32_t)std::byteswap<uint32_t>(SlReadUint32());
			}
			return;
		}
	}

	/* If the size of elements is 1 byte both in file and memory, no special
	 * conversion is needed, use specialized copy-copy function to speed up things */
	if (conv == SLE_INT8 || conv == SLE_UINT8) {
		SlCopyBytes(array, length);
	} else {
		uint8_t *a = (uint8_t*)array;
		uint8_t mem_size = SlCalcConvMemLen(conv);

		for (; length != 0; length --) {
			SlSaveLoadConv(a, conv);
			a += mem_size; // get size
		}
	}
}


/**
 * Pointers cannot be saved to a savegame, so this functions gets
 * the index of the item, and if not available, it hussles with
 * pointers (looks really bad :()
 * Remember that a nullptr item has value 0, and all
 * indices have +1, so vehicle 0 is saved as index 1.
 * @param obj The object that we want to get the index of
 * @param rt SLRefType type of the object the index is being sought of
 * @return Return the pointer converted to an index of the type pointed to
 */
static size_t ReferenceToInt(const void *obj, SLRefType rt)
{
	assert(_sl.action == SLA_SAVE);

	if (obj == nullptr) return 0;

	switch (rt) {
		case REF_VEHICLE_OLD: // Old vehicles we save as new ones
		case REF_VEHICLE:   return ((const  Vehicle*)obj)->index + 1;
		case REF_TEMPLATE_VEHICLE: return ((const TemplateVehicle*)obj)->index + 1;
		case REF_STATION:   return ((const  Station*)obj)->index + 1;
		case REF_TOWN:      return ((const     Town*)obj)->index + 1;
		case REF_ORDER:     return ((const OrderPoolItem*)obj)->index + 1;
		case REF_ROADSTOPS: return ((const RoadStop*)obj)->index + 1;
		case REF_ENGINE_RENEWS:  return ((const       EngineRenew*)obj)->index + 1;
		case REF_CARGO_PACKET:   return ((const       CargoPacket*)obj)->index + 1;
		case REF_ORDERLIST:      return ((const         OrderList*)obj)->index + 1;
		case REF_STORAGE:        return ((const PersistentStorage*)obj)->index + 1;
		case REF_LINK_GRAPH:     return ((const         LinkGraph*)obj)->index + 1;
		case REF_LINK_GRAPH_JOB: return ((const      LinkGraphJob*)obj)->index + 1;
		default: NOT_REACHED();
	}
}

/**
 * Pointers cannot be loaded from a savegame, so this function
 * gets the index from the savegame and returns the appropriate
 * pointer from the already loaded base.
 * Remember that an index of 0 is a nullptr pointer so all indices
 * are +1 so vehicle 0 is saved as 1.
 * @param index The index that is being converted to a pointer
 * @param rt SLRefType type of the object the pointer is sought of
 * @return Return the index converted to a pointer of any type
 */
void *IntToReference(size_t index, SLRefType rt)
{
	static_assert(sizeof(size_t) <= sizeof(void *));

	assert(_sl.action == SLA_PTRS);

	/* After version 4.3 REF_VEHICLE_OLD is saved as REF_VEHICLE,
	 * and should be loaded like that */
	if (rt == REF_VEHICLE_OLD && !IsSavegameVersionBefore(SLV_4, 4)) {
		rt = REF_VEHICLE;
	}

	/* No need to look up nullptr pointers, just return immediately */
	if (index == (rt == REF_VEHICLE_OLD ? 0xFFFF : 0)) return nullptr;

	/* Correct index. Old vehicles were saved differently:
	 * invalid vehicle was 0xFFFF, now we use 0x0000 for everything invalid. */
	if (rt != REF_VEHICLE_OLD) index--;

	switch (rt) {
		case REF_ORDERLIST:
			if (OrderList::IsValidID(index)) return OrderList::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid OrderList");

		case REF_ORDER:
			if (OrderPoolItem::IsValidID(index)) return OrderPoolItem::Get(index);
			/* in old versions, invalid order was used to mark end of order list */
			if (IsSavegameVersionBefore(SLV_5, 2)) return nullptr;
			SlErrorCorruptWithChunk("Referencing invalid Order");

		case REF_VEHICLE_OLD:
		case REF_VEHICLE:
			if (Vehicle::IsValidID(index)) return Vehicle::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid Vehicle");

		case REF_TEMPLATE_VEHICLE:
			if (TemplateVehicle::IsValidID(index)) return TemplateVehicle::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid TemplateVehicle");

		case REF_STATION:
			if (Station::IsValidID(index)) return Station::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid Station");

		case REF_TOWN:
			if (Town::IsValidID(index)) return Town::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid Town");

		case REF_ROADSTOPS:
			if (RoadStop::IsValidID(index)) return RoadStop::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid RoadStop");

		case REF_ENGINE_RENEWS:
			if (EngineRenew::IsValidID(index)) return EngineRenew::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid EngineRenew");

		case REF_CARGO_PACKET:
			if (CargoPacket::IsValidID(index)) return CargoPacket::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid CargoPacket");

		case REF_STORAGE:
			if (PersistentStorage::IsValidID(index)) return PersistentStorage::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid PersistentStorage");

		case REF_LINK_GRAPH:
			if (LinkGraph::IsValidID(index)) return LinkGraph::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid LinkGraph");

		case REF_LINK_GRAPH_JOB:
			if (LinkGraphJob::IsValidID(index)) return LinkGraphJob::Get(index);
			SlErrorCorruptWithChunk("Referencing invalid LinkGraphJob");

		default: NOT_REACHED();
	}
}

/**
 * Handle conversion for references.
 * @param ptr The object being filled/read.
 * @param conv VarType type of the current element of the struct.
 */
template <SaveLoadAction action>
void SlSaveLoadRef(void *ptr, VarType conv)
{
	switch (action) {
		case SLA_SAVE:
			SlWriteUint32((uint32_t)ReferenceToInt(*(void **)ptr, (SLRefType)conv));
			break;
		case SLA_LOAD_CHECK:
		case SLA_LOAD:
			*(size_t *)ptr = IsSavegameVersionBefore(SLV_69) ? (size_t)SlReadUint16() : SlReadUint32();
			break;
		case SLA_PTRS:
			*(void **)ptr = IntToReference(*(size_t *)ptr, (SLRefType)conv);
			break;
		case SLA_NULL:
			*(void **)ptr = nullptr;
			break;
		default: NOT_REACHED();
	}
}

static uint SlGetListTypeLengthSize(size_t size)
{
	if (SlIsTableChunk()) {
		return SlGetArrayLength(size);
	} else {
		return 4;
	}
}

static void SlWriteListLength(size_t size)
{
	if (SlIsTableChunk()) {
		SlWriteArrayLength(size);
	} else {
		SlWriteUint32(static_cast<uint32_t>(size));
	}
}

static size_t SlReadListLength()
{
	if (SlIsTableChunk()) {
		return SlReadArrayLength();
	} else {
		return IsSavegameVersionBefore(SLV_69) ? SlReadUint16() : SlReadUint32();
	}
}

/**
 * Template class to help with list-like types.
 */
template <template <typename> typename Tstorage, typename Tvar>
class SlStorageHelper {
	typedef Tstorage<Tvar> SlStorageT;
public:
	/**
	 * Internal templated helper to return the size in bytes of a list-like type.
	 * @param storage The storage to find the size of
	 * @param conv VarType type of variable that is used for calculating the size
	 * @param cmd The SaveLoadType ware are saving/loading.
	 */
	static size_t SlCalcLen(const void *storage, VarType conv, SaveLoadType cmd = SL_VAR)
	{
		assert(cmd == SL_VAR || cmd == SL_REF);

		const SlStorageT *list = static_cast<const SlStorageT *>(storage);

		uint type_size = SlGetListTypeLengthSize(list->size()); // Size of the length of the list.
		uint item_size = SlCalcConvFileLen(cmd == SL_VAR ? conv : (VarType)SLE_FILE_U32);
		return list->size() * item_size + type_size;
	}

	template <SaveLoadAction action>
	static void SlSaveLoadMember(SaveLoadType cmd, Tvar *item, VarType conv)
	{
		switch (cmd) {
			case SL_VAR: SlSaveLoadConvGeneric<action>(item, conv); break;
			case SL_REF: SlSaveLoadRef<action>(item, conv); break;
			default:
				NOT_REACHED();
		}
	}

	/**
	 * Internal templated helper to save/load a list-like type.
	 * @param storage The storage being manipulated.
	 * @param conv VarType type of variable that is used for calculating the size.
	 * @param cmd The SaveLoadType ware are saving/loading.
	 */
	template <SaveLoadAction action>
	static void SlSaveLoad(void *storage, VarType conv, SaveLoadType cmd = SL_VAR)
	{
		assert(cmd == SL_VAR || cmd == SL_REF);

		SlStorageT *list = static_cast<SlStorageT *>(storage);

		switch (action) {
			case SLA_SAVE:
				SlWriteListLength(list->size());

				for (auto &item : *list) {
					SlSaveLoadMember<SLA_SAVE>(cmd, &item, conv);
				}
				break;

			case SLA_LOAD_CHECK:
			case SLA_LOAD: {
				size_t length;
				switch (cmd) {
					case SL_VAR: length = SlReadListLength(); break;
					case SL_REF: length = SlReadListLength(); break;
					default: NOT_REACHED();
				}

				/* Load each value and push to the end of the storage. */
				for (size_t i = 0; i < length; i++) {
					Tvar &data = list->emplace_back();
					SlSaveLoadMember<SLA_LOAD>(cmd, &data, conv);
				}
				break;
			}

			case SLA_PTRS:
				for (auto &item : *list) {
					SlSaveLoadMember<SLA_PTRS>(cmd, &item, conv);
				}
				break;

			case SLA_NULL:
				list->clear();
				break;

			default: NOT_REACHED();
		}
	}
};

/**
 * Return the size in bytes of a list.
 * @param list The std::list to find the size of.
 * @param conv VarType type of variable that is used for calculating the size.
 */
 template <typename PtrList>
static inline size_t SlCalcRefListLen(const void *list)
{
	const PtrList *l = (const PtrList *) list;

	uint type_size = SlGetListTypeLengthSize(l->size());
	size_t item_size = SlCalcRefLen();
	/* Each entry is saved as item_size bytes, plus type_size bytes are used for the length
	 * of the list */
	return l->size() * item_size + type_size;
}

static size_t SlCalcVarListLenFromItemCount(size_t item_count, size_t item_size)
{
	uint type_size = SlGetListTypeLengthSize(item_count);
	/* Each entry is saved as item_size bytes, plus type_size bytes are used for the length
	 * of the list */
	return item_count * item_size + type_size;
}

/**
 * Return the size in bytes of a list
 * @param list The std::list to find the size of
 */
 template <typename PtrList>
static inline size_t SlCalcVarListLen(const void *list, size_t item_size)
{
	const PtrList *l = (const PtrList *) list;
	return SlCalcVarListLenFromItemCount(l->size(), item_size);
}

/**
 * Save/Load a list.
 * @param list The list being manipulated.
 * @param conv VarType type of variable that is used for calculating the size.
 */
template <SaveLoadAction action, typename PtrList>
static void SlRefList(void *list, SLRefType conv)
{
	PtrList *l = (PtrList *)list;

	switch (action) {
		case SLA_SAVE: {
			/* Automatically calculate the length? */
			if (_sl.need_length != NL_NONE) {
				SlSetLength(SlCalcRefListLen<PtrList>(list));
			}

			SlWriteListLength(l->size());

			for (auto iter = l->begin(); iter != l->end(); ++iter) {
				void *ptr = *iter;
				SlWriteUint32((uint32_t)ReferenceToInt(ptr, conv));
			}
			break;
		}
		case SLA_LOAD_CHECK:
		case SLA_LOAD: {
			size_t length = SlReadListLength();
			if constexpr (!std::is_same_v<PtrList, std::list<void *>>) {
				l->reserve(length);
			}

			/* Load each reference and push to the end of the list */
			for (size_t i = 0; i < length; i++) {
				size_t data = IsSavegameVersionBefore(SLV_69) ? SlReadUint16() : SlReadUint32();
				l->push_back((void *)data);
			}
			break;
		}
		case SLA_PTRS: {
			for (auto iter = l->begin(); iter != l->end(); ++iter) {
				*iter = IntToReference((size_t)*iter, conv);
			}
			break;
		}
		case SLA_NULL:
			l->clear();
			break;
		default: NOT_REACHED();
	}
}

/**
 * Save/Load a list.
 * @param list The list being manipulated
 * @param conv VarType type of the list
 */
template <SaveLoadAction action, typename PtrList>
static void SlVarList(void *list, VarType conv)
{
	PtrList *l = (PtrList *)list;

	switch (action) {
		case SLA_SAVE: {
			/* Automatically calculate the length? */
			if (_sl.need_length != NL_NONE) {
				SlSetLength(SlCalcVarListLen<PtrList>(list, SlCalcConvFileLen(conv)));
			}

			SlWriteListLength(l->size());

			typename PtrList::iterator iter;
			for (iter = l->begin(); iter != l->end(); ++iter) {
				SlSaveLoadConvGeneric<SLA_SAVE>(&(*iter), conv);
			}
			break;
		}
		case SLA_LOAD_CHECK:
		case SLA_LOAD: {
			size_t length = SlReadListLength();
			l->resize(length);

			typename PtrList::iterator iter;
			iter = l->begin();

			for (size_t i = 0; i < length; i++) {
				SlSaveLoadConvGeneric<SLA_LOAD>(&(*iter), conv);
				++iter;
			}
			break;
		}
		case SLA_PTRS: break;
		case SLA_NULL:
			l->clear();
			break;
		default: NOT_REACHED();
	}
}

/**
 * Return the size in bytes of a ring buffer.
 * @param ring The ring buffer to find the size of
 * @param conv VarType type of variable that is used for calculating the size
 */
static inline size_t SlCalcRingLen(const void *ring, VarType conv)
{
	switch (GetVarMemType(conv)) {
		case SLE_VAR_BL: return SlStorageHelper<ring_buffer, bool>::SlCalcLen(ring, conv);
		case SLE_VAR_I8: return SlStorageHelper<ring_buffer, int8_t>::SlCalcLen(ring, conv);
		case SLE_VAR_U8: return SlStorageHelper<ring_buffer, uint8_t>::SlCalcLen(ring, conv);
		case SLE_VAR_I16: return SlStorageHelper<ring_buffer, int16_t>::SlCalcLen(ring, conv);
		case SLE_VAR_U16: return SlStorageHelper<ring_buffer, uint16_t>::SlCalcLen(ring, conv);
		case SLE_VAR_I32: return SlStorageHelper<ring_buffer, int32_t>::SlCalcLen(ring, conv);
		case SLE_VAR_U32: return SlStorageHelper<ring_buffer, uint32_t>::SlCalcLen(ring, conv);
		case SLE_VAR_I64: return SlStorageHelper<ring_buffer, int64_t>::SlCalcLen(ring, conv);
		case SLE_VAR_U64: return SlStorageHelper<ring_buffer, uint64_t>::SlCalcLen(ring, conv);
		default: NOT_REACHED();
	}
}

/**
 * Save/load a ring buffer.
 * @param ring The ring buffer being manipulated
 * @param conv VarType type of variable that is used for calculating the size
 */
template <SaveLoadAction action>
static void SlRing(void *ring, VarType conv)
{
	switch (GetVarMemType(conv)) {
		case SLE_VAR_BL: SlStorageHelper<ring_buffer, bool>::SlSaveLoad<action>(ring, conv); break;
		case SLE_VAR_I8: SlStorageHelper<ring_buffer, int8_t>::SlSaveLoad<action>(ring, conv); break;
		case SLE_VAR_U8: SlStorageHelper<ring_buffer, uint8_t>::SlSaveLoad<action>(ring, conv); break;
		case SLE_VAR_I16: SlStorageHelper<ring_buffer, int16_t>::SlSaveLoad<action>(ring, conv); break;
		case SLE_VAR_U16: SlStorageHelper<ring_buffer, uint16_t>::SlSaveLoad<action>(ring, conv); break;
		case SLE_VAR_I32: SlStorageHelper<ring_buffer, int32_t>::SlSaveLoad<action>(ring, conv); break;
		case SLE_VAR_U32: SlStorageHelper<ring_buffer, uint32_t>::SlSaveLoad<action>(ring, conv); break;
		case SLE_VAR_I64: SlStorageHelper<ring_buffer, int64_t>::SlSaveLoad<action>(ring, conv); break;
		case SLE_VAR_U64: SlStorageHelper<ring_buffer, uint64_t>::SlSaveLoad<action>(ring, conv); break;
		default: NOT_REACHED();
	}
}

template <SaveLoadAction action>
static void SlCustomContainerVarList(void *list, const SaveLoad &sld)
{
	switch (action) {
		case SLA_SAVE: {
			const size_t item_count = static_cast<size_t>(sld.custom.container_functor(list, SaveLoadCustomContainerOp::GetLength, {}, 0));

			/* Automatically calculate the length? */
			if (_sl.need_length != NL_NONE) {
				SlSetLength(SlCalcVarListLenFromItemCount(item_count, SlCalcConvFileLen(sld.conv)));
			}

			SlWriteListLength(item_count);
			sld.custom.container_functor(list, SaveLoadCustomContainerOp::Save, sld.conv, 0);
			break;
		}
		case SLA_LOAD_CHECK:
		case SLA_LOAD: {
			sld.custom.container_functor(list, SaveLoadCustomContainerOp::Load, sld.conv, SlReadListLength());
			break;
		}
		case SLA_PTRS: break;
		case SLA_NULL:
			sld.custom.container_functor(list, SaveLoadCustomContainerOp::Load, {}, 0);
			break;
		default: NOT_REACHED();
	}
}

/** Are we going to save this object or not? */
static inline bool SlIsObjectValidInSavegame(const SaveLoad &sld)
{
	return sld.ext_feature_test.IsFeaturePresent(_sl_version, sld.version_from, sld.version_to);
}

/**
 * Calculate the size of an object.
 * @param object to be measured.
 * @param slt The SaveLoad table with objects to save/load.
 * @return size of given object.
 */
size_t SlCalcObjLength(const void *object, const SaveLoadTable &slt)
{
	size_t length = 0;

	/* Need to determine the length and write a length tag. */
	for (auto &sld : slt) {
		length += SlCalcObjMemberLength(object, sld);
	}
	return length;
}

size_t SlCalcObjMemberLength(const void *object, const SaveLoad &sld)
{
	assert(_sl.action == SLA_SAVE);

	switch (sld.cmd) {
		case SL_VAR:
		case SL_REF:
		case SL_ARR:
		case SL_STR:
		case SL_REFLIST:
		case SL_REFRING:
		case SL_REFVEC:
		case SL_RING:
		case SL_STDSTR:
		case SL_VARVEC:
		case SL_CUSTOMLIST:
			/* CONDITIONAL saveload types depend on the savegame version */
			if (!SlIsObjectValidInSavegame(sld)) break;

			switch (sld.cmd) {
				case SL_VAR: return SlCalcConvFileLen(sld.conv);
				case SL_REF: return SlCalcRefLen();
				case SL_ARR: return SlCalcArrayLen(sld.length, sld.conv);
				case SL_STR: return SlCalcStringLen(GetVariableAddress(object, sld), sld.length, sld.conv);
				case SL_REFLIST: return SlCalcRefListLen<std::list<void *>>(GetVariableAddress(object, sld));
				case SL_REFRING: return SlCalcRefListLen<ring_buffer<void *>>(GetVariableAddress(object, sld));
				case SL_REFVEC: return SlCalcRefListLen<std::vector<void *>>(GetVariableAddress(object, sld));
				case SL_RING: return SlCalcRingLen(GetVariableAddress(object, sld), sld.conv);
				case SL_VARVEC: {
					const size_t mem_len = SlCalcConvMemLen(sld.conv);
					const size_t file_len = SlCalcConvFileLen(sld.conv);
					switch (mem_len) {
						case 1: return SlCalcVarListLen<std::vector<uint8_t>>(GetVariableAddress(object, sld), file_len);
						case 2: return SlCalcVarListLen<std::vector<uint16_t>>(GetVariableAddress(object, sld), file_len);
						case 4: return SlCalcVarListLen<std::vector<uint32_t>>(GetVariableAddress(object, sld), file_len);
						case 8: return SlCalcVarListLen<std::vector<uint64_t>>(GetVariableAddress(object, sld), file_len);
						default: NOT_REACHED();
					}
				}
				case SL_STDSTR: return SlCalcStdStrLen(*static_cast<std::string *>(GetVariableAddress(object, sld)));
				case SL_CUSTOMLIST: return SlCalcVarListLenFromItemCount(sld.custom.container_functor(GetVariableAddress(object, sld), SaveLoadCustomContainerOp::GetLength, {}, 0), SlCalcConvFileLen(sld.conv));
				default: NOT_REACHED();
			}
			break;
		case SL_WRITEBYTE: return 1; // a uint8_t is logically of size 1

		case SL_STRUCT:
		case SL_STRUCTLIST:
			NOT_REACHED(); // SlAutolength or similar should be used for sub-structs

		default: NOT_REACHED();
	}
	return 0;
}

void SlFilterObject(const SaveLoadTable &slt, std::vector<SaveLoad> &save);

static void SlFilterObjectMember(const SaveLoad &sld, std::vector<SaveLoad> &save)
{
	switch (sld.cmd) {
		case SL_VAR:
		case SL_REF:
		case SL_ARR:
		case SL_STR:
		case SL_REFLIST:
		case SL_REFRING:
		case SL_REFVEC:
		case SL_RING:
		case SL_STDSTR:
		case SL_VARVEC:
		case SL_CUSTOMLIST:
		case SL_STRUCT:
		case SL_STRUCTLIST:
			/* CONDITIONAL saveload types depend on the savegame version */
			if (!SlIsObjectValidInSavegame(sld)) return;

			switch (_sl.action) {
				case SLA_SAVE:
				case SLA_LOAD_CHECK:
				case SLA_LOAD:
					break;
				case SLA_PTRS:
				case SLA_NULL:
					switch (sld.cmd) {
						case SL_REF:
						case SL_REFLIST:
						case SL_REFRING:
						case SL_REFVEC:
						case SL_STRUCT:
						case SL_STRUCTLIST:
							break;

						/* non-ptr types do not require SLA_PTRS or SLA_NULL actions */
						default:
							return;
					}
					break;
				default: NOT_REACHED();
			}

			save.push_back(sld);
			break;

		/* SL_WRITEBYTE writes a value to the savegame to identify the type of an object.
		 * When loading, the value is read explicitly with SlReadByte() to determine which
		 * object description to use. */
		case SL_WRITEBYTE:
			if (_sl.action == SLA_SAVE) save.push_back(sld);
			break;

		case SL_INCLUDE:
			sld.include_functor(save);
			break;

		default: NOT_REACHED();
	}
}

void SlFilterObject(const SaveLoadTable &slt, std::vector<SaveLoad> &save)
{
	for (auto &sld : slt) {
		SlFilterObjectMember(sld, save);
	}
}

std::vector<SaveLoad> SlFilterObject(const SaveLoadTable &slt)
{
	std::vector<SaveLoad> save;
	SlFilterObject(slt, save);
	return save;
}

void SlFilterNamedSaveLoadTable(const NamedSaveLoadTable &nslt, std::vector<SaveLoad> &save)
{
	for (auto &nsld : nslt) {
		if ((nsld.nsl_flags & NSLF_TABLE_ONLY) != 0) continue;
		SlFilterObjectMember(nsld.save_load, save);
	}
}

std::vector<SaveLoad> SlFilterNamedSaveLoadTable(const NamedSaveLoadTable &nslt)
{
	std::vector<SaveLoad> save;
	SlFilterNamedSaveLoadTable(nslt, save);
	return save;
}

template <SaveLoadAction action, bool check_version>
bool SlObjectMemberGeneric(void *object, const SaveLoad &sld)
{
	void *ptr = GetVariableAddress(object, sld);

	VarType conv = GB(sld.conv, 0, 8);
	switch (sld.cmd) {
		case SL_VAR:
		case SL_REF:
		case SL_ARR:
		case SL_STR:
		case SL_REFLIST:
		case SL_REFRING:
		case SL_REFVEC:
		case SL_RING:
		case SL_STDSTR:
		case SL_VARVEC:
		case SL_CUSTOMLIST:
			/* CONDITIONAL saveload types depend on the savegame version */
			if (check_version) {
				if (!SlIsObjectValidInSavegame(sld)) return false;
			}

			switch (sld.cmd) {
				case SL_VAR: SlSaveLoadConvGeneric<action>(ptr, conv); break;
				case SL_REF: // Reference variable, translate
					switch (action) {
						case SLA_SAVE:
							SlWriteUint32((uint32_t)ReferenceToInt(*(void **)ptr, (SLRefType)conv));
							break;
						case SLA_LOAD_CHECK:
						case SLA_LOAD:
							*(size_t *)ptr = IsSavegameVersionBefore(SLV_69) ? SlReadUint16() : SlReadUint32();
							break;
						case SLA_PTRS:
							*(void **)ptr = IntToReference(*(size_t *)ptr, (SLRefType)conv);
							break;
						case SLA_NULL:
							*(void **)ptr = nullptr;
							break;
						default: NOT_REACHED();
					}
					break;
				case SL_ARR: SlArray(ptr, sld.length, conv); break;
				case SL_STR: SlString<action>(ptr, sld.length, sld.conv); break;
				case SL_REFLIST: SlRefList<action, std::list<void *>>(ptr, (SLRefType)conv); break;
				case SL_REFRING: SlRefList<action, ring_buffer<void *>>(ptr, (SLRefType)conv); break;
				case SL_REFVEC: SlRefList<action, std::vector<void *>>(ptr, (SLRefType)conv); break;
				case SL_RING: SlRing<action>(ptr, conv); break;
				case SL_VARVEC: {
					const size_t size_len = SlCalcConvMemLen(sld.conv);
					switch (size_len) {
						case 1: SlVarList<action, std::vector<uint8_t>>(ptr, conv); break;
						case 2: SlVarList<action, std::vector<uint16_t>>(ptr, conv); break;
						case 4: SlVarList<action, std::vector<uint32_t>>(ptr, conv); break;
						case 8: SlVarList<action, std::vector<uint64_t>>(ptr, conv); break;
						default: NOT_REACHED();
					}
					break;
				}
				case SL_CUSTOMLIST: SlCustomContainerVarList<action>(ptr, sld); break;
				case SL_STDSTR: SlStdStringGeneric<action>(static_cast<std::string *>(ptr), sld.conv); break;
				default: NOT_REACHED();
			}
			break;

		case SL_STRUCT:
		case SL_STRUCTLIST:
			switch (action) {
				case SLA_SAVE: {
					if (sld.cmd == SL_STRUCT) {
						/* Number of structs written in the savegame: write a value of 1, change to zero later if nothing after this was written */
						_sl.dumper->WriteByte(1);
						size_t offset = _sl.dumper->GetWriteOffsetGeneric();
						sld.struct_handler->Save(object);
						if (offset == _sl.dumper->GetWriteOffsetGeneric()) {
							/* Nothing was actually written, so it's safe to change the 1 above to 0 */
							_sl.dumper->ReplaceLastWrittenByte(0); // This is fine iff nothing has been written since the WriteByte(1)
						}
					} else {
						sld.struct_handler->Save(object);
					}
					break;
				}

				case SLA_LOAD_CHECK: {
					if (sld.cmd == SL_STRUCT && SlIsTableChunk()) {
						if (SlGetStructListLength(1) == 0) break;
					}
					sld.struct_handler->LoadCheck(object);
					break;
				}

				case SLA_LOAD: {
					if (sld.cmd == SL_STRUCT && SlIsTableChunk()) {
						if (SlGetStructListLength(1) == 0) break;
					}
					sld.struct_handler->Load(object);
					break;
				}

				case SLA_PTRS:
					sld.struct_handler->FixPointers(object);
					break;

				case SLA_NULL: break;
				default: NOT_REACHED();
			}
			break;

		/* SL_WRITEBYTE writes a value to the savegame to identify the type of an object.
		 * When loading, the value is read explicitly with SlReadByte() to determine which
		 * object description to use. */
		case SL_WRITEBYTE:
			switch (action) {
				case SLA_SAVE: SlWriteByte(*(uint8_t *)ptr); break;
				case SLA_LOAD_CHECK:
				case SLA_LOAD:
				case SLA_PTRS:
				case SLA_NULL: break;
				default: NOT_REACHED();
			}
			break;

		default: NOT_REACHED();
	}
	return true;
}

bool SlObjectMember(void *object, const SaveLoad &sld)
{
	switch (_sl.action) {
		case SLA_SAVE:
			return SlObjectMemberGeneric<SLA_SAVE, true>(object, sld);
		case SLA_LOAD_CHECK:
		case SLA_LOAD:
			return SlObjectMemberGeneric<SLA_LOAD, true>(object, sld);
		case SLA_PTRS:
			return SlObjectMemberGeneric<SLA_PTRS, true>(object, sld);
		case SLA_NULL:
			return SlObjectMemberGeneric<SLA_NULL, true>(object, sld);
		default: NOT_REACHED();
	}
}

/**
 * Main SaveLoad function.
 * @param object The object that is being saved or loaded.
 * @param slt The SaveLoad table with objects to save/load.
 */
void SlObject(void *object, const SaveLoadTable &slt)
{
	/* Automatically calculate the length? */
	if (_sl.need_length != NL_NONE) {
		SlSetLength(SlCalcObjLength(object, slt));
	}

	for (auto &sld : slt) {
		SlObjectMember(object, sld);
	}
}

template <SaveLoadAction action, bool check_version>
void SlObjectIterateBase(void *object, const SaveLoadTable &slt)
{
	for (auto &sld : slt) {
		SlObjectMemberGeneric<action, check_version>(object, sld);
	}
}

void SlObjectSaveFiltered(void *object, const SaveLoadTable &slt)
{
	if (_sl.need_length != NL_NONE) {
		_sl.need_length = NL_NONE;
		_sl.dumper->StartAutoLength();
		SlObjectIterateBase<SLA_SAVE, false>(object, slt);
		auto result = _sl.dumper->StopAutoLength();
		_sl.need_length = NL_WANTLENGTH;
		SlSetLength(result.size());
		_sl.dumper->CopyBytes(result);
	} else {
		SlObjectIterateBase<SLA_SAVE, false>(object, slt);
	}
}

void SlObjectLoadFiltered(void *object, const SaveLoadTable &slt)
{
	SlObjectIterateBase<SLA_LOAD, false>(object, slt);
}

void SlObjectPtrOrNullFiltered(void *object, const SaveLoadTable &slt)
{
	switch (_sl.action) {
		case SLA_PTRS:
			SlObjectIterateBase<SLA_PTRS, false>(object, slt);
			return;
		case SLA_NULL:
			SlObjectIterateBase<SLA_NULL, false>(object, slt);
			return;
		default: NOT_REACHED();
	}
}

bool SlIsTableChunk()
{
	return (_sl.block_mode == CH_TABLE || _sl.block_mode == CH_SPARSE_TABLE);
}

void SlSkipTableHeader()
{
	uint sub_tables = 0;
	while (true) {
		uint8_t type = SlReadByte();
		if (type == SLE_FILE_END) break;

		if ((type & SLE_FILE_TYPE_MASK) == SLE_FILE_STRUCT) sub_tables++;

		SlString<SLA_LOAD>(nullptr, 0, SLE_FILE_STRING | SLE_VAR_NULL);
	}
	for (uint i = 0; i < sub_tables; i++) {
		SlSkipTableHeader();
	}
}

/**
 * Return the type as saved/loaded inside savegame tables.
 */
static uint8_t GetSavegameTableFileType(const SaveLoad &sld)
{
	switch (sld.cmd) {
		case SL_VAR: {
			VarType type = GetVarFileType(sld.conv);
			if (type == SLE_FILE_VEHORDERID) {
				return SlXvIsFeaturePresent(XSLFI_MORE_VEHICLE_ORDERS) ? SLE_FILE_U16 : SLE_FILE_U8;
			}
			return type;
		}

		case SL_STR:
		case SL_STDSTR:
		case SL_ARR:
		case SL_VARVEC:
		case SL_RING:
		case SL_CUSTOMLIST:
			return GetVarFileType(sld.conv) | SLE_FILE_HAS_LENGTH_FIELD; break;

		case SL_REF:
			return SLE_FILE_U32;

		case SL_REFLIST:
		case SL_REFRING:
		case SL_REFVEC:
			return SLE_FILE_U32 | SLE_FILE_HAS_LENGTH_FIELD;

		case SL_WRITEBYTE:
			return SLE_FILE_U8;

		case SL_STRUCT:
		case SL_STRUCTLIST:
			return SLE_FILE_STRUCT | SLE_FILE_HAS_LENGTH_FIELD;

		default: NOT_REACHED();
	}
}

/**
 * Handler that is assigned when there is a struct read in the savegame which
 * is not known to the code. This means we are going to skip it.
 */
class SaveLoadSkipStructHandler : public SaveLoadStructHandler {
	void Save(void *) const override
	{
		NOT_REACHED();
	}

	void Load(void *object) const override
	{
		size_t length = SlGetStructListLength(UINT32_MAX);
		for (; length > 0; length--) {
			SlObjectLoadFiltered(object, this->GetLoadDescription());
		}
	}

	void LoadCheck(void *object) const override
	{
		this->Load(object);
	}

	NamedSaveLoadTable GetDescription() const override
	{
		return {};
	}
};

/**
 * Save or Load a table header.
 * @note a table-header can never contain more than 65535 fields.
 * @param slt The NamedSaveLoad table with objects to save/load.
 * @return The ordered SaveLoad array to use.
 */
SaveLoadTableData SlTableHeader(const NamedSaveLoadTable &slt, TableHeaderSpecialHandler *special_handler)
{
	/* You can only use SlTableHeader if you are a CH_TABLE. */
	assert(_sl.block_mode == CH_TABLE || _sl.block_mode == CH_SPARSE_TABLE);

	SaveLoadTableData saveloads;

	switch (_sl.action) {
		case SLA_LOAD_CHECK:
		case SLA_LOAD: {
			/* Build a key lookup mapping based on the available fields. */
			struct key_item {
				std::string_view name;
				const SaveLoad *save_load;

				bool operator==(const key_item &other) const { return this->name == other.name; }
				bool operator<(const key_item &other) const { return this->name < other.name; }
				bool operator==(const std::string_view &other) const { return this->name == other; }
				bool operator<(const std::string_view &other) const { return this->name < other; }
			};
			std::vector<key_item> key_lookup;
			key_lookup.reserve(slt.size());
			for (auto &nsld : slt) {
				if (StrEmpty(nsld.name) || !SlIsObjectValidInSavegame(nsld.save_load)) continue;

				key_lookup.push_back({ nsld.name, &nsld.save_load });
			}

			std::sort(key_lookup.begin(), key_lookup.end());

			/* Check that there is only one active SaveLoad for a given name. */
			[[maybe_unused]] auto duplicate = std::adjacent_find(key_lookup.begin(), key_lookup.end());
			assert_str(duplicate == key_lookup.end(), duplicate->name.data());

			while (true) {
				uint8_t type = SlReadByte();
				if (type == SLE_FILE_END) break;

				if ((type & SLE_FILE_TYPE_MASK) >= SLE_FILE_TABLE_END || (type & SLE_FILE_TYPE_MASK) == SLE_FILE_END) {
					SlErrorCorruptFmt("Invalid table field type: 0x{:X} ({})", type, ChunkIDDumper()(_sl.current_chunk_id));
				}

				std::string key;
				SlStdStringGeneric<SLA_LOAD>(&key, SLE_STR);

				auto sld_it = std::lower_bound(key_lookup.begin(), key_lookup.end(), key);
				if (sld_it == key_lookup.end() || sld_it->name != key) {
					if (special_handler != nullptr && special_handler->MissingField(key, type, saveloads)) continue; // Special handler took responsibility for missing field

					/* SLA_LOADCHECK triggers this debug statement a lot and is perfectly normal. */
					Debug(sl, _sl.action == SLA_LOAD ? 2 : 6, "Field '{}' of type 0x{:02X} not found, skipping", key, type);

					SaveLoadType saveload_type;
					SaveLoadStructHandler *struct_handler = nullptr;
					switch (type & SLE_FILE_TYPE_MASK) {
						case SLE_FILE_STRING:
							/* Strings are always marked with SLE_FILE_HAS_LENGTH_FIELD, as they are a list of chars. */
							saveload_type = SL_STDSTR;
							break;

						case SLE_FILE_STRUCT: {
							saveload_type = SL_STRUCTLIST;
							auto handler = std::make_unique<SaveLoadSkipStructHandler>();
							struct_handler = handler.get();
							saveloads.struct_handlers.push_back(std::move(handler));
							break;
						}

						default:
							saveload_type = (type & SLE_FILE_HAS_LENGTH_FIELD) ? SL_ARR : SL_VAR;
							break;
					}

					/* We don't know this field, so read to nothing. */
					saveloads.push_back({ true, saveload_type, ((VarType)type & SLE_FILE_TYPE_MASK) | SLE_VAR_NULL, 1, SL_MIN_VERSION, SL_MAX_VERSION, SLTAG_TABLE_UNKNOWN, { nullptr }, { struct_handler }, SlXvFeatureTest() });
					continue;
				}

				/* Validate the type of the field. If it is changed, the
				 * savegame should have been bumped so we know how to do the
				 * conversion. If this error triggers, that clearly didn't
				 * happen and this is a friendly poke to the developer to bump
				 * the savegame version and add conversion code. */
				uint8_t correct_type = GetSavegameTableFileType(*sld_it->save_load);
				if (correct_type != type) {
					Debug(sl, 1, "Field type for '{}' was expected to be 0x{:02X} but 0x{:02X} was found", key, correct_type, type);
					SlErrorCorruptWithChunk("Field type is different than expected");
				}
				saveloads.push_back(*sld_it->save_load);

				if ((type & SLE_FILE_TYPE_MASK) == SLE_FILE_STRUCT) {
					std::unique_ptr<SaveLoadStructHandler> handler = saveloads.back().struct_handler_factory();
					saveloads.back().struct_handler = handler.get();
					saveloads.struct_handlers.push_back(std::move(handler));
				}
			}

			for (auto &sld : saveloads) {
				if (sld.cmd == SL_STRUCTLIST || sld.cmd == SL_STRUCT) {
					sld.struct_handler->table_data = SlTableHeader(sld.struct_handler->GetDescription());
					sld.struct_handler->LoadedTableDescription();
				}
			}

			break;
		}

		case SLA_SAVE: {
			const NeedLength orig_need_length = _sl.need_length;
			if (orig_need_length != NL_NONE) {
				_sl.need_length = NL_NONE;
				_sl.dumper->StartAutoLength();
			}

			for (auto &nsld : slt) {
				if (StrEmpty(nsld.name) || !SlIsObjectValidInSavegame(nsld.save_load)) continue;

				uint8_t type = GetSavegameTableFileType(nsld.save_load);
				assert(type != SLE_FILE_END);
				SlWriteByte(type);
				SlString<SLA_SAVE>(const_cast<char **>(&nsld.name), 0, SLE_STR);

				saveloads.push_back(nsld.save_load);
			}

			/* Add an end-of-header marker. */
			SlWriteByte(SLE_FILE_END);

			for (auto &sld : saveloads) {
				if (sld.cmd == SL_STRUCTLIST || sld.cmd == SL_STRUCT) {
					std::unique_ptr<SaveLoadStructHandler> handler = sld.struct_handler_factory();
					sld.struct_handler = handler.get();
					sld.struct_handler->table_data = SlTableHeader(sld.struct_handler->GetDescription());
					sld.struct_handler->SavedTableDescription();
					saveloads.struct_handlers.push_back(std::move(handler));
				}
			}

			if (orig_need_length != NL_NONE) {
				auto result = _sl.dumper->StopAutoLength();
				_sl.need_length = orig_need_length;
				SlSetLength(result.size());
				_sl.dumper->CopyBytes(result);
			}

			break;
		}

		default: NOT_REACHED();
	}

	return saveloads;
}

SaveLoadTableData SlTableHeaderOrRiff(const NamedSaveLoadTable &slt)
{
	if (SlIsTableChunk()) return SlTableHeader(slt);

	SaveLoadTableData saveloads;
	SlFilterNamedSaveLoadTable(slt, saveloads);
	return saveloads;
}

SaveLoadTableData SlPrepareNamedSaveLoadTableForPtrOrNull(const NamedSaveLoadTable &slt)
{
	const bool table_mode = (_sl.action == SLA_NULL) || SlIsTableChunk();
	SaveLoadTableData saveloads;
	for (auto &nsld : slt) {
		if (table_mode) {
			if (StrEmpty(nsld.name)) continue;
		} else {
			if ((nsld.nsl_flags & NSLF_TABLE_ONLY) != 0) continue;
		}
		SlFilterObjectMember(nsld.save_load, saveloads);
	}
	for (auto &sld : saveloads) {
		if (sld.cmd == SL_STRUCTLIST || sld.cmd == SL_STRUCT) {
			std::unique_ptr<SaveLoadStructHandler> handler = sld.struct_handler_factory();
			sld.struct_handler = handler.get();
			saveloads.struct_handlers.push_back(std::move(handler));
			sld.struct_handler->table_data = SlPrepareNamedSaveLoadTableForPtrOrNull(sld.struct_handler->GetDescription());
		}
	}
	return saveloads;
}

void SlSaveTableObjectChunk(const SaveLoadTable &slt, void *object)
{
	SlSetArrayIndex(0);
	SlObjectSaveFiltered(object, slt);
}

void SlLoadTableOrRiffFiltered(const SaveLoadTable &slt, void *object)
{
	if (SlIsTableChunk() && SlIterateArray() == -1) return;
	SlObjectLoadFiltered(object, slt);
	if (SlIsTableChunk() && SlIterateArray() != -1) {
		uint32_t id = _sl.current_chunk_id;
		SlErrorCorruptFmt("Too many {} entries", ChunkIDDumper()(id));
	}
}

void SlLoadTableWithArrayLengthPrefixesMissing()
{
	SetBit(_sl.block_flags, SLBF_TABLE_ARRAY_LENGTH_PREFIX_MISSING);
}

/**
 * Set the length of this list.
 * @param The length of the list.
 */
void SlSetStructListLength(size_t length)
{
	SlWriteArrayLength(length);
}

/**
 * Get the length of this list; if it exceeds the limit, error out.
 * @param limit The maximum size the list can be.
 * @return The length of the list.
 */
size_t SlGetStructListLength(size_t limit)
{
	size_t length = SlReadArrayLength();
	if (length > limit) SlErrorCorruptWithChunk("List exceeds storage size");

	return length;
}

void SlSkipChunkContents()
{
	if (SlIsTableChunk()) SlSkipTableHeader();

	if (_sl.block_mode == CH_RIFF) {
		SlSkipBytes(SlGetFieldLength());
	} else {
		SlSkipArray();
	}
}

/**
 * Save or Load (a list of) global variables.
 * @param slt The SaveLoad table with objects to save/load.
 */
void SlGlobList(const SaveLoadTable &slt)
{
	SlObject(nullptr, slt);
}

void SlAutolengthSetup()
{
	assert(_sl.action == SLA_SAVE);
	assert(_sl.need_length == NL_WANTLENGTH);

	_sl.need_length = NL_NONE;
	_sl.dumper->StartAutoLength();
}

void SlAutolengthCompletion()
{
	auto result = _sl.dumper->StopAutoLength();
	/* Setup length */
	_sl.need_length = NL_WANTLENGTH;
	SlSetLength(result.size());
	_sl.dumper->CopyBytes(result);
}

uint8_t SlSaveToTempBufferSetup()
{
	assert(_sl.action == SLA_SAVE);
	NeedLength orig_need_length = _sl.need_length;

	_sl.need_length = NL_NONE;
	_sl.dumper->StartAutoLength();

	return (uint8_t) orig_need_length;
}

std::span<uint8_t> SlSaveToTempBufferRestore(uint8_t state)
{
	NeedLength orig_need_length = (NeedLength)state;

	auto result = _sl.dumper->StopAutoLength();
	/* Setup length */
	_sl.need_length = orig_need_length;
	return result;
}

SlConditionallySaveState SlConditionallySaveSetup()
{
	assert(_sl.action == SLA_SAVE);
	if (_sl.dumper->IsAutoLengthActive()) {
		return { (size_t)(_sl.dumper->buf - _sl.dumper->autolen_buf), 0, true };
	} else {
		return { 0, SlSaveToTempBufferSetup(), false };
	}
}

extern void SlConditionallySaveCompletion(const SlConditionallySaveState &state, bool save)
{
	if (state.nested) {
		if (!save) _sl.dumper->buf = _sl.dumper->autolen_buf + state.current_len;
	} else {
		auto result = SlSaveToTempBufferRestore(state.need_length);
		if (save) _sl.dumper->CopyBytes(result);
	}
}

SlLoadFromBufferState SlLoadFromBufferSetup(const uint8_t *buffer, size_t length)
{
	assert(_sl.action == SLA_LOAD || _sl.action == SLA_LOAD_CHECK);

	SlLoadFromBufferState state;

	state.old_obj_len = _sl.obj_len;
	_sl.obj_len = length;

	ReadBuffer *reader = ReadBuffer::GetCurrent();
	state.old_bufp = reader->bufp;
	state.old_bufe = reader->bufe;
	reader->bufp = const_cast<uint8_t *>(buffer);
	reader->bufe = const_cast<uint8_t *>(buffer) + length;

	return state;
}

void SlLoadFromBufferRestore(const SlLoadFromBufferState &state, const uint8_t *buffer, size_t length)
{
	ReadBuffer *reader = ReadBuffer::GetCurrent();
	if (reader->bufp != reader->bufe || reader->bufe != buffer + length) {
		SlErrorCorruptWithChunk("SlLoadFromBuffer: Wrong number of bytes read");
	}

	_sl.obj_len = state.old_obj_len;
	reader->bufp = state.old_bufp;
	reader->bufe = state.old_bufe;
}

/*
 * Notes on extended chunk header:
 *
 * If the chunk type is CH_EXT_HDR (15), then a u32 flags field follows.
 * This flag field may define additional fields which follow the flags field in future.
 * The standard chunk header follows, though it my be modified by the flags field.
 * At present SLCEHF_BIG_RIFF increases the RIFF size limit to a theoretical 60 bits,
 * by adding a further u32 field for the high bits after the existing RIFF size field.
 */

inline void SlRIFFSpringPPCheck(size_t len)
{
	if (_sl_maybe_springpp) {
		_sl_maybe_springpp = false;
		if (len == 0) {
			extern void SlXvSpringPPSpecialSavegameVersions();
			SlXvSpringPPSpecialSavegameVersions();
		} else if (_sl_version > MAX_LOAD_SAVEGAME_VERSION) {
			SlError(STR_GAME_SAVELOAD_ERROR_TOO_NEW_SAVEGAME);
		} else if (_sl_version >= SLV_START_PATCHPACKS && _sl_version <= SLV_END_PATCHPACKS) {
			SlError(STR_GAME_SAVELOAD_ERROR_PATCHPACK);
		}
	}
}

/**
 * Load a chunk of data (eg vehicles, stations, etc.)
 * @param ch The chunkhandler that will be used for the operation
 */
static void SlLoadChunk(const ChunkHandler &ch)
{
	if (ch.special_proc != nullptr) {
		if (ch.special_proc(ch.id, CSLSO_PRE_LOAD) == CSLSOR_LOAD_CHUNK_CONSUMED) return;
	}

	Debug(sl, 2, "Loading chunk {}", ChunkIDDumper()(ch.id));

	uint8_t m = SlReadByte();
	size_t len;
	size_t endoffs;

	_sl.block_mode = m;
	_sl.block_flags = 0;
	_sl.obj_len = 0;

	SaveLoadChunkExtHeaderFlags ext_flags = static_cast<SaveLoadChunkExtHeaderFlags>(0);
	if ((m & 0xF) == CH_EXT_HDR) {
		ext_flags = static_cast<SaveLoadChunkExtHeaderFlags>(SlReadUint32());

		/* read in real header */
		m = SlReadByte();
		_sl.block_mode = m;
		_sl.chunk_block_modes[_sl.current_chunk_id] = m;
	}

	_sl.expect_table_header = (_sl.block_mode == CH_TABLE || _sl.block_mode == CH_SPARSE_TABLE);

	/* The header should always be at the start. Read the length; the
	 * LoadCheck() should as first action process the header. */
	if (_sl.expect_table_header) {
		SlIterateArray();
	}

	switch (m) {
		case CH_ARRAY:
		case CH_TABLE:
			_sl.array_index = 0;
			ch.load_proc();
			if (_next_offs != 0) SlErrorCorruptFmt("Invalid array length in {}", ChunkIDDumper()(ch.id));
			break;
		case CH_SPARSE_ARRAY:
		case CH_SPARSE_TABLE:
			ch.load_proc();
			if (_next_offs != 0) SlErrorCorruptFmt("Invalid array length in {}", ChunkIDDumper()(ch.id));
			break;
		default:
			if ((m & 0xF) == CH_RIFF) {
				/* Read length */
				len = (SlReadByte() << 16) | ((m >> 4) << 24);
				len += SlReadUint16();
				SlRIFFSpringPPCheck(len);
				if (SlXvIsFeaturePresent(XSLFI_RIFF_HEADER_60_BIT)) {
					if (len != 0) {
						SlErrorCorruptFmt("RIFF chunk too large: {}", ChunkIDDumper()(ch.id));
					}
					len = SlReadUint32();
				}
				if (ext_flags & SLCEHF_BIG_RIFF) {
					len |= SlReadUint32() << 28;
				}

				_sl.obj_len = len;
				endoffs = _sl.reader->GetSize() + len;
				ch.load_proc();
				if (_sl.reader->GetSize() != endoffs) {
					Debug(sl, 1, "Invalid chunk size: {} != {}, ({}) for {}", _sl.reader->GetSize(), endoffs, len, ChunkIDDumper()(ch.id));
					SlErrorCorruptFmt("Invalid chunk size - expected to be at position {}, actually at {}, length: {} for {}",
							endoffs, _sl.reader->GetSize(), len, ChunkIDDumper()(ch.id));
				}
			} else {
				SlErrorCorruptFmt("Invalid chunk type for {}", ChunkIDDumper()(ch.id));
			}
			break;
	}

	if (_sl.expect_table_header) SlErrorCorruptFmt("Table chunk without header: {}", ChunkIDDumper()(ch.id));
}

/**
 * Load a chunk of data for checking savegames.
 * If the chunkhandler is nullptr, the chunk is skipped.
 * @param ch The chunkhandler that will be used for the operation, this may be nullptr
 */
static void SlLoadCheckChunk(const ChunkHandler *ch, uint32_t chunk_id)
{
	if (ch != nullptr && ch->special_proc != nullptr) {
		if (ch->special_proc(ch->id, CSLSO_PRE_LOADCHECK) == CSLSOR_LOAD_CHUNK_CONSUMED) return;
	}

	if (ch == nullptr) {
		Debug(sl, 1, "Discarding chunk {}", ChunkIDDumper()(chunk_id));
	} else {
		Debug(sl, 2, "Loading chunk {}", ChunkIDDumper()(chunk_id));
	}

	uint8_t m = SlReadByte();
	size_t len;
	size_t endoffs;

	_sl.block_mode = m;
	_sl.block_flags = 0;
	_sl.obj_len = 0;

	SaveLoadChunkExtHeaderFlags ext_flags = static_cast<SaveLoadChunkExtHeaderFlags>(0);
	if ((m & 0xF) == CH_EXT_HDR) {
		ext_flags = static_cast<SaveLoadChunkExtHeaderFlags>(SlReadUint32());

		/* read in real header */
		m = SlReadByte();
		_sl.block_mode = m;
		_sl.chunk_block_modes[_sl.current_chunk_id] = m;
	}

	_sl.expect_table_header = (_sl.block_mode == CH_TABLE || _sl.block_mode == CH_SPARSE_TABLE);

	/* The header should always be at the start. Read the length; the
	 * LoadCheck() should as first action process the header. */
	if (_sl.expect_table_header) {
		SlIterateArray();
	}

	switch (m) {
		case CH_ARRAY:
		case CH_TABLE:
			_sl.array_index = 0;
			if (ext_flags) {
				SlErrorCorruptFmt("CH_ARRAY does not take chunk header extension flags: 0x{:X} in {}", ext_flags, ChunkIDDumper()(chunk_id));
			}
			if (ch != nullptr && ch->load_check_proc) {
				ch->load_check_proc();
			} else {
				if (m == CH_TABLE) SlSkipTableHeader();
				SlSkipArray();
			}
			break;
		case CH_SPARSE_ARRAY:
		case CH_SPARSE_TABLE:
			if (ext_flags) {
				SlErrorCorruptFmt("CH_SPARSE_ARRAY does not take chunk header extension flags: 0x{:X} in {}", ext_flags, ChunkIDDumper()(chunk_id));
			}
			if (ch != nullptr && ch->load_check_proc) {
				ch->load_check_proc();
			} else {
				if (m == CH_SPARSE_TABLE) SlSkipTableHeader();
				SlSkipArray();
			}
			break;
		default:
			if ((m & 0xF) == CH_RIFF) {
				if (ext_flags != (ext_flags & SLCEHF_BIG_RIFF)) {
					SlErrorCorruptFmt("Unknown chunk header extension flags for CH_RIFF: 0x{:X} in {}", ext_flags, ChunkIDDumper()(chunk_id));
				}
				/* Read length */
				len = (SlReadByte() << 16) | ((m >> 4) << 24);
				len += SlReadUint16();
				SlRIFFSpringPPCheck(len);
				if (SlXvIsFeaturePresent(XSLFI_RIFF_HEADER_60_BIT)) {
					if (len != 0) {
						SlErrorCorruptWithChunk("RIFF chunk too large");
					}
					len = SlReadUint32();
					if (ext_flags & SLCEHF_BIG_RIFF) SlErrorCorruptFmt("XSLFI_RIFF_HEADER_60_BIT and SLCEHF_BIG_RIFF both present in {}", ChunkIDDumper()(chunk_id));
				}
				if (ext_flags & SLCEHF_BIG_RIFF) {
					uint64_t full_len = len | (static_cast<uint64_t>(SlReadUint32()) << 28);
					if (full_len >= (1LL << 32)) {
						SlErrorCorruptFmt("Chunk size too large: {} in {}", full_len, ChunkIDDumper()(chunk_id));
					}
					len = static_cast<size_t>(full_len);
				}
				_sl.obj_len = len;
				endoffs = _sl.reader->GetSize() + len;
				if (ch != nullptr && ch->load_check_proc) {
					ch->load_check_proc();
				} else {
					SlSkipBytes(len);
				}
				if (_sl.reader->GetSize() != endoffs) {
					Debug(sl, 1, "Invalid chunk size: {} != {}, ({}) for {}", _sl.reader->GetSize(), endoffs, len, ChunkIDDumper()(chunk_id));
					SlErrorCorruptFmt("Invalid chunk size - expected to be at position {}, actually at {}, length: {} for {}",
							endoffs, _sl.reader->GetSize(), len, ChunkIDDumper()(chunk_id));
				}
			} else {
				SlErrorCorruptFmt("Invalid chunk type for: {}", ChunkIDDumper()(chunk_id));
			}
			break;
	}

	if (_sl.expect_table_header) SlErrorCorruptFmt("Table chunk without header: {}", ChunkIDDumper()(chunk_id));
}

/**
 * Save a chunk of data (eg. vehicles, stations, etc.). Each chunk is
 * prefixed by an ID identifying it, followed by data, and terminator where appropriate
 * @param ch The chunkhandler that will be used for the operation
 */
static void SlSaveChunk(const ChunkHandler &ch)
{
	if (ch.special_proc != nullptr) {
		ChunkSaveLoadSpecialOpResult result = ch.special_proc(ch.id, CSLSO_SHOULD_SAVE_CHUNK);
		if (result == CSLSOR_DONT_SAVE_CHUNK) return;
		if (result == CSLSOR_UPSTREAM_SAVE_CHUNK) {
			SaveLoadVersion old_ver = _sl_version;
			_sl_version = MAX_LOAD_SAVEGAME_VERSION;
			auto guard = scope_guard([&]() {
				_sl_version = old_ver;
			});
			upstream_sl::SlSaveChunkChunkByID(ch.id);
			return;
		}
	}

	ChunkSaveLoadProc *proc = ch.save_proc;

	/* Don't save any chunk information if there is no save handler. */
	if (proc == nullptr) return;

	_sl.current_chunk_id = ch.id;
	SlWriteUint32(ch.id);
	Debug(sl, 2, "Saving chunk {}", ChunkIDDumper()(ch.id));

	size_t written = 0;
	if (GetDebugLevel(DebugLevelID::sl) >= 3) written = SlGetBytesWritten();

	_sl.block_mode = ch.type;
	_sl.block_flags = 0;
	_sl.expect_table_header = (_sl.block_mode == CH_TABLE || _sl.block_mode == CH_SPARSE_TABLE);
	_sl.need_length = (_sl.expect_table_header || _sl.block_mode == CH_RIFF) ? NL_WANTLENGTH : NL_NONE;

	switch (ch.type) {
		case CH_RIFF:
			proc();
			break;
		case CH_ARRAY:
		case CH_TABLE:
			_sl.last_array_index = 0;
			SlWriteByte(ch.type);
			proc();
			SlWriteArrayLength(0); // Terminate arrays
			break;
		case CH_SPARSE_ARRAY:
		case CH_SPARSE_TABLE:
			SlWriteByte(ch.type);
			proc();
			SlWriteArrayLength(0); // Terminate arrays
			break;
		default: NOT_REACHED();
	}

	if (_sl.expect_table_header) SlErrorCorruptFmt("Table chunk without header: {}", ChunkIDDumper()(ch.id));

	Debug(sl, 3, "Saved chunk {} ({} bytes)", ChunkIDDumper()(ch.id), SlGetBytesWritten() - written);
}

/** Save all chunks */
static void SlSaveChunks()
{
	for (auto &ch : ChunkHandlers()) {
		SlSaveChunk(ch);
	}

	/* Terminator */
	SlWriteUint32(0);
}

/**
 * Find the ChunkHandler that will be used for processing the found
 * chunk in the savegame or in memory
 * @param id the chunk in question
 * @return returns the appropriate chunkhandler
 */
static const ChunkHandler *SlFindChunkHandler(uint32_t id)
{
	for (auto &ch : ChunkHandlers()) if (ch.id == id) return &ch;
	return nullptr;
}

/** Load all chunks */
static void SlLoadChunks()
{
	if (_sl_upstream_mode) {
		upstream_sl::SlLoadChunks();
		return;
	}

	for (uint32_t id = SlReadUint32(); id != 0; id = SlReadUint32()) {
		_sl.current_chunk_id = id;
		size_t read = 0;
		if (GetDebugLevel(DebugLevelID::sl) >= 3) read = SlGetBytesRead();

		_sl.chunk_block_modes[id] = ReadBuffer::GetCurrent()->PeekByte();

		if (SlXvIsChunkDiscardable(id)) {
			SlLoadCheckChunk(nullptr, id);
		} else {
			const ChunkHandler *ch = SlFindChunkHandler(id);
			if (ch == nullptr) {
				SlErrorCorruptFmt("Unknown chunk type: {}", ChunkIDDumper()(id));
			} else {
				SlLoadChunk(*ch);
			}
		}
		Debug(sl, 3, "Loaded chunk {} ({} bytes)", ChunkIDDumper()(id), SlGetBytesRead() - read);
	}
}

/** Load all chunks for savegame checking */
static void SlLoadCheckChunks()
{
	if (_sl_upstream_mode) {
		upstream_sl::SlLoadCheckChunks();
		return;
	}

	uint32_t id;
	const ChunkHandler *ch;

	for (id = SlReadUint32(); id != 0; id = SlReadUint32()) {
		_sl.current_chunk_id = id;
		size_t read = 0;
		if (GetDebugLevel(DebugLevelID::sl) >= 3) read = SlGetBytesRead();

		_sl.chunk_block_modes[id] = ReadBuffer::GetCurrent()->PeekByte();

		if (SlXvIsChunkDiscardable(id)) {
			ch = nullptr;
		} else {
			ch = SlFindChunkHandler(id);
			if (ch == nullptr) SlErrorCorruptFmt("Unknown chunk type: {}", ChunkIDDumper()(id));
		}
		SlLoadCheckChunk(ch, id);
		Debug(sl, 3, "Loaded chunk {} ({} bytes)", ChunkIDDumper()(id), SlGetBytesRead() - read);
	}
}

/** Fix all pointers (convert index -> pointer) */
static void SlFixPointers()
{
	extern void FixupOldOrderPoolItemReferences();

	if (_sl_upstream_mode) {
		upstream_sl::SlFixPointers();

		_sl.action = SLA_PTRS;
		FixupOldOrderPoolItemReferences();
		return;
	}

	_sl.action = SLA_PTRS;

	for (auto &ch : ChunkHandlers()) {
		_sl.current_chunk_id = ch.id;
		_sl.block_mode = _sl.chunk_block_modes[_sl.current_chunk_id];
		if (ch.special_proc != nullptr) {
			if (ch.special_proc(ch.id, CSLSO_PRE_PTRS) == CSLSOR_LOAD_CHUNK_CONSUMED) continue;
		}
		if (ch.ptrs_proc != nullptr) {
			Debug(sl, 3, "Fixing pointers for {}", ChunkIDDumper()(ch.id));
			ch.ptrs_proc();
		}
	}

	assert(_sl.action == SLA_PTRS);
	FixupOldOrderPoolItemReferences();
}


/** Yes, simply reading from a file. */
struct FileReader : LoadFilter {
	std::optional<FileHandle> file; ///< The file to read from.
	long begin; ///< The begin of the file.

	/**
	 * Create the file reader, so it reads from a specific file.
	 * @param file The file to read from.
	 */
	FileReader(FileHandle &&file) : LoadFilter(nullptr), file(std::move(file)), begin(ftell(*this->file))
	{
	}

	/** Make sure everything is cleaned up. */
	~FileReader()
	{
		if (this->file.has_value()) {
			_game_session_stats.savegame_size = ftell(*this->file) - this->begin;
		}
	}

	size_t Read(uint8_t *buf, size_t size) override
	{
		/* We're in the process of shutting down, i.e. in "failure" mode. */
		if (!this->file.has_value()) return 0;

		return fread(buf, 1, size, *this->file);
	}

	void Reset() override
	{
		clearerr(*this->file);
		if (fseek(*this->file, this->begin, SEEK_SET)) {
			Debug(sl, 1, "Could not reset the file reading");
		}
	}
};

/** Yes, simply writing to a file. */
struct FileWriter : SaveFilter {
	std::optional<FileHandle> file; ///< The file to write to.
	std::string temp_name;
	std::string target_name;

	/**
	 * Create the file writer, so it writes to a specific file.
	 * @param file The file to write to.
	 * @param temp_name The temporary name of the file being written to.
	 * @param target_name The target name of the file to rename to, on success.
	 */
	FileWriter(FileHandle &&file, std::string temp_name, std::string target_name) : SaveFilter(nullptr), file(std::move(file)), temp_name(std::move(temp_name)), target_name(std::move(target_name))
	{
	}

	/** Make sure everything is cleaned up. */
	~FileWriter()
	{
		this->CloseFile();
		if (!this->temp_name.empty()) FioRemove(this->temp_name);
	}

	void Write(uint8_t *buf, size_t size) override
	{
		/* We're in the process of shutting down, i.e. in "failure" mode. */
		if (!this->file.has_value()) return;

		if (fwrite(buf, 1, size, *this->file) != size) SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_WRITEABLE);
	}

	void Finish() override
	{
		this->CloseFile();

		size_t save_size = 0;
		if (_game_session_stats.savegame_size.has_value()) save_size = _game_session_stats.savegame_size.value();

		if (save_size <= 8) SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_WRITEABLE, "Insufficient bytes written");

#if defined(_WIN32)
		struct _stat st;
		int stat_result = _wstat(OTTD2FS(this->temp_name).c_str(), &st);
#else
		struct stat st;
		int stat_result = stat(OTTD2FS(this->temp_name).c_str(), &st);
#endif
		if (stat_result != 0) {
			SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_WRITEABLE, "Failed to stat temporary save file");
		}
		if ((size_t)st.st_size != save_size) {
			SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_WRITEABLE, fmt::format("Temporary save file does not have expected file size: {} != {}", st.st_size, save_size));
		}

		if (!FioRenameFile(this->temp_name, this->target_name)) SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_WRITEABLE, "Failed to rename temporary save file to target name");
		this->temp_name.clear(); // Now no need to unlink temporary name
	}

private:
	void CloseFile()
	{
		if (this->file.has_value()) {
			_game_session_stats.savegame_size = ftell(*this->file);
			int res = this->file->Close();
			this->file.reset();
			if (res != 0) {
				SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_WRITEABLE);
			}
		}
	}
};

/*******************************************
 ********** START OF LZO CODE **************
 *******************************************/

#ifdef WITH_LZO
#include <lzo/lzo1x.h>

/** Buffer size for the LZO compressor */
static const uint LZO_BUFFER_SIZE = 8192;

/** Filter using LZO compression. */
struct LZOLoadFilter : LoadFilter {
	/**
	 * Initialise this filter.
	 * @param chain The next filter in this chain.
	 */
	LZOLoadFilter(std::shared_ptr<LoadFilter> chain) : LoadFilter(std::move(chain))
	{
		if (lzo_init() != LZO_E_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "cannot initialize decompressor");
	}

	size_t Read(uint8_t *buf, size_t ssize) override
	{
		assert(ssize >= LZO_BUFFER_SIZE);

		/* Buffer size is from the LZO docs plus the chunk header size. */
		uint8_t out[LZO_BUFFER_SIZE + LZO_BUFFER_SIZE / 16 + 64 + 3 + sizeof(uint32_t) * 2];
		uint32_t tmp[2];
		uint32_t size;
		lzo_uint len = ssize;

		/* Read header*/
		if (this->chain->Read((uint8_t*)tmp, sizeof(tmp)) != sizeof(tmp)) SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_READABLE, "File read failed");

		/* Check if size is bad */
		((uint32_t*)out)[0] = size = tmp[1];

		if (_sl_version != SL_MIN_VERSION) {
			tmp[0] = TO_BE32(tmp[0]);
			size = TO_BE32(size);
		}

		if (size >= sizeof(out)) SlErrorCorrupt("Inconsistent size");

		/* Read block */
		if (this->chain->Read(out + sizeof(uint32_t), size) != size) SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_READABLE);

		/* Verify checksum */
		if (tmp[0] != lzo_adler32(0, out, size + sizeof(uint32_t))) SlErrorCorrupt("Bad checksum");

		/* Decompress */
		int ret = lzo1x_decompress_safe(out + sizeof(uint32_t) * 1, size, buf, &len, nullptr);
		if (ret != LZO_E_OK) SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_READABLE);
		return len;
	}
};

/** Filter using LZO compression. */
struct LZOSaveFilter : SaveFilter {
	/**
	 * Initialise this filter.
	 * @param chain             The next filter in this chain.
	 * @param compression_level The requested level of compression.
	 */
	LZOSaveFilter(std::shared_ptr<SaveFilter> chain, uint8_t compression_level) : SaveFilter(std::move(chain))
	{
		if (lzo_init() != LZO_E_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "cannot initialize compressor");
	}

	void Write(uint8_t *buf, size_t size) override
	{
		const lzo_bytep in = buf;
		/* Buffer size is from the LZO docs plus the chunk header size. */
		uint8_t out[LZO_BUFFER_SIZE + LZO_BUFFER_SIZE / 16 + 64 + 3 + sizeof(uint32_t) * 2];
		uint8_t wrkmem[LZO1X_1_MEM_COMPRESS];
		lzo_uint outlen;

		do {
			/* Compress up to LZO_BUFFER_SIZE bytes at once. */
			lzo_uint len = size > LZO_BUFFER_SIZE ? LZO_BUFFER_SIZE : (lzo_uint)size;
			lzo1x_1_compress(in, len, out + sizeof(uint32_t) * 2, &outlen, wrkmem);
			((uint32_t*)out)[1] = TO_BE32((uint32_t)outlen);
			((uint32_t*)out)[0] = TO_BE32(lzo_adler32(0, out + sizeof(uint32_t), outlen + sizeof(uint32_t)));
			this->chain->Write(out, outlen + sizeof(uint32_t) * 2);

			/* Move to next data chunk. */
			size -= len;
			in += len;
		} while (size > 0);
	}
};

#endif /* WITH_LZO */

/*********************************************
 ******** START OF NOCOMP CODE (uncompressed)*
 *********************************************/

/** Filter without any compression. */
struct NoCompLoadFilter : LoadFilter {
	/**
	 * Initialise this filter.
	 * @param chain The next filter in this chain.
	 */
	NoCompLoadFilter(std::shared_ptr<LoadFilter> chain) : LoadFilter(std::move(chain))
	{
	}

	size_t Read(uint8_t *buf, size_t size) override
	{
		return this->chain->Read(buf, size);
	}
};

/** Filter without any compression. */
struct NoCompSaveFilter : SaveFilter {
	/**
	 * Initialise this filter.
	 * @param chain             The next filter in this chain.
	 * @param compression_level The requested level of compression.
	 */
	NoCompSaveFilter(std::shared_ptr<SaveFilter> chain, uint8_t compression_level) : SaveFilter(std::move(chain))
	{
	}

	void Write(uint8_t *buf, size_t size) override
	{
		this->chain->Write(buf, size);
	}
};

/********************************************
 ********** START OF ZLIB CODE **************
 ********************************************/

#if defined(WITH_ZLIB)
#include <zlib.h>

/** Filter using Zlib compression. */
struct ZlibLoadFilter : LoadFilter {
	z_stream z;                        ///< Stream state we are reading from.
	uint8_t fread_buf[MEMORY_CHUNK_SIZE]; ///< Buffer for reading from the file.

	/**
	 * Initialise this filter.
	 * @param chain The next filter in this chain.
	 */
	ZlibLoadFilter(std::shared_ptr<LoadFilter> chain) : LoadFilter(std::move(chain))
	{
		memset(&this->z, 0, sizeof(this->z));
		if (inflateInit(&this->z) != Z_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "cannot initialize decompressor");
	}

	/** Clean everything up. */
	~ZlibLoadFilter()
	{
		inflateEnd(&this->z);
	}

	size_t Read(uint8_t *buf, size_t size) override
	{
		this->z.next_out  = buf;
		this->z.avail_out = (uint)size;

		do {
			/* read more bytes from the file? */
			if (this->z.avail_in == 0) {
				this->z.next_in = this->fread_buf;
				this->z.avail_in = (uint)this->chain->Read(this->fread_buf, sizeof(this->fread_buf));
			}

			/* inflate the data */
			int r = inflate(&this->z, 0);
			if (r == Z_STREAM_END) break;

			if (r != Z_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "inflate() failed");
		} while (this->z.avail_out != 0);

		return size - this->z.avail_out;
	}
};

/** Filter using Zlib compression. */
struct ZlibSaveFilter : SaveFilter {
	z_stream z; ///< Stream state we are writing to.
	uint8_t buf[MEMORY_CHUNK_SIZE]; ///< output buffer

	/**
	 * Initialise this filter.
	 * @param chain             The next filter in this chain.
	 * @param compression_level The requested level of compression.
	 */
	ZlibSaveFilter(std::shared_ptr<SaveFilter> chain, uint8_t compression_level) : SaveFilter(std::move(chain))
	{
		memset(&this->z, 0, sizeof(this->z));
		if (deflateInit(&this->z, compression_level) != Z_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "cannot initialize compressor");
	}

	/** Clean up what we allocated. */
	~ZlibSaveFilter()
	{
		deflateEnd(&this->z);
	}

	/**
	 * Helper loop for writing the data.
	 * @param p    The bytes to write.
	 * @param len  Amount of bytes to write.
	 * @param mode Mode for deflate.
	 */
	void WriteLoop(uint8_t *p, size_t len, int mode)
	{
		uint n;
		this->z.next_in = p;
		this->z.avail_in = (uInt)len;
		do {
			this->z.next_out = this->buf;
			this->z.avail_out = sizeof(this->buf);

			/**
			 * For the poor next soul who sees many valgrind warnings of the
			 * "Conditional jump or move depends on uninitialised value(s)" kind:
			 * According to the author of zlib it is not a bug and it won't be fixed.
			 * http://groups.google.com/group/comp.compression/browse_thread/thread/b154b8def8c2a3ef/cdf9b8729ce17ee2
			 * [Mark Adler, Feb 24 2004, 'zlib-1.2.1 valgrind warnings' in the newsgroup comp.compression]
			 */
			int r = deflate(&this->z, mode);

			/* bytes were emitted? */
			if ((n = sizeof(this->buf) - this->z.avail_out) != 0) {
				this->chain->Write(this->buf, n);
			}
			if (r == Z_STREAM_END) break;

			if (r != Z_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "zlib returned error code");
		} while (this->z.avail_in || !this->z.avail_out);
	}

	void Write(uint8_t *buf, size_t size) override
	{
		this->WriteLoop(buf, size, 0);
	}

	void Finish() override
	{
		this->WriteLoop(nullptr, 0, Z_FINISH);
		this->chain->Finish();
	}
};

#endif /* WITH_ZLIB */

/********************************************
 ********** START OF LZMA CODE **************
 ********************************************/

#if defined(WITH_LIBLZMA)
#include <lzma.h>

/**
 * Have a copy of an initialised LZMA stream. We need this as it's
 * impossible to "re"-assign LZMA_STREAM_INIT to a variable in some
 * compilers, i.e. LZMA_STREAM_INIT can't be used to set something.
 * This var has to be used instead.
 */
static const lzma_stream _lzma_init = LZMA_STREAM_INIT;

/** Filter without any compression. */
struct LZMALoadFilter : LoadFilter {
	lzma_stream lzma;                     ///< Stream state that we are reading from.
	uint8_t fread_buf[MEMORY_CHUNK_SIZE]; ///< Buffer for reading from the file.

	/**
	 * Initialise this filter.
	 * @param chain The next filter in this chain.
	 */
	LZMALoadFilter(std::shared_ptr<LoadFilter> chain) : LoadFilter(std::move(chain)), lzma(_lzma_init)
	{
		/* Allow saves up to 256 MB uncompressed */
		if (lzma_auto_decoder(&this->lzma, 1 << 28, 0) != LZMA_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "cannot initialize decompressor");
	}

	/** Clean everything up. */
	~LZMALoadFilter()
	{
		lzma_end(&this->lzma);
	}

	size_t Read(uint8_t *buf, size_t size) override
	{
		this->lzma.next_out  = buf;
		this->lzma.avail_out = size;

		do {
			/* read more bytes from the file? */
			if (this->lzma.avail_in == 0) {
				this->lzma.next_in  = this->fread_buf;
				this->lzma.avail_in = this->chain->Read(this->fread_buf, sizeof(this->fread_buf));
			}

			/* inflate the data */
			lzma_ret r = lzma_code(&this->lzma, LZMA_RUN);
			if (r == LZMA_STREAM_END) break;
			if (r != LZMA_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, fmt::format("liblzma returned error code: {}", r));
		} while (this->lzma.avail_out != 0);

		return size - this->lzma.avail_out;
	}
};

/** Filter using LZMA compression. */
struct LZMASaveFilter : SaveFilter {
	lzma_stream lzma; ///< Stream state that we are writing to.
	uint8_t buf[MEMORY_CHUNK_SIZE]; ///< output buffer

	/**
	 * Initialise this filter.
	 * @param chain             The next filter in this chain.
	 * @param compression_level The requested level of compression.
	 */
	LZMASaveFilter(std::shared_ptr<SaveFilter> chain, uint8_t compression_level) : SaveFilter(std::move(chain)), lzma(_lzma_init)
	{
		if (lzma_easy_encoder(&this->lzma, compression_level, LZMA_CHECK_CRC32) != LZMA_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "cannot initialize compressor");
	}

	/** Clean up what we allocated. */
	~LZMASaveFilter()
	{
		lzma_end(&this->lzma);
	}

	/**
	 * Helper loop for writing the data.
	 * @param p      The bytes to write.
	 * @param len    Amount of bytes to write.
	 * @param action Action for lzma_code.
	 */
	void WriteLoop(uint8_t *p, size_t len, lzma_action action)
	{
		size_t n;
		this->lzma.next_in = p;
		this->lzma.avail_in = len;
		do {
			this->lzma.next_out = this->buf;
			this->lzma.avail_out = sizeof(this->buf);

			lzma_ret r = lzma_code(&this->lzma, action);

			/* bytes were emitted? */
			if ((n = sizeof(this->buf) - this->lzma.avail_out) != 0) {
				this->chain->Write(this->buf, n);
			}
			if (r == LZMA_STREAM_END) break;
			if (r != LZMA_OK) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, fmt::format("liblzma returned error code: {}", r));
		} while (this->lzma.avail_in || !this->lzma.avail_out);
	}

	void Write(uint8_t *buf, size_t size) override
	{
		this->WriteLoop(buf, size, LZMA_RUN);
	}

	void Finish() override
	{
		this->WriteLoop(nullptr, 0, LZMA_FINISH);
		this->chain->Finish();
	}
};

#endif /* WITH_LIBLZMA */

/********************************************
 ********** START OF ZSTD CODE **************
 ********************************************/

#if defined(WITH_ZSTD)
#include <zstd.h>


/** Filter using ZSTD compression. */
struct ZSTDLoadFilter : LoadFilter {
	ZSTD_DCtx *zstd;  ///< ZSTD decompression context
	uint8_t fread_buf[MEMORY_CHUNK_SIZE];  ///< Buffer for reading from the file
	ZSTD_inBuffer input;  ///< ZSTD input buffer for fread_buf

	/**
	 * Initialise this filter.
	 * @param chain The next filter in this chain.
	 */
	ZSTDLoadFilter(std::shared_ptr<LoadFilter> chain) : LoadFilter(std::move(chain))
	{
		this->zstd = ZSTD_createDCtx();
		if (!this->zstd) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "cannot initialize compressor");
		this->input = {this->fread_buf, 0, 0};
	}

	/** Clean everything up. */
	~ZSTDLoadFilter()
	{
		ZSTD_freeDCtx(this->zstd);
	}

	size_t Read(uint8_t *buf, size_t size) override
	{
		ZSTD_outBuffer output{buf, size, 0};

		do {
			/* read more bytes from the file? */
			if (this->input.pos == this->input.size) {
				this->input.size = this->chain->Read(this->fread_buf, sizeof(this->fread_buf));
				this->input.pos = 0;
				if (this->input.size == 0) break;
			}

			size_t ret = ZSTD_decompressStream(this->zstd, &output, &this->input);
			if (ZSTD_isError(ret)) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "libzstd returned error code");
			if (ret == 0) break;
		} while (output.pos < output.size);

		return output.pos;
	}
};

/** Filter using ZSTD compression. */
struct ZSTDSaveFilter : SaveFilter {
	ZSTD_CCtx *zstd;  ///< ZSTD compression context
	uint8_t buf[MEMORY_CHUNK_SIZE]; ///< output buffer

	/**
	 * Initialise this filter.
	 * @param chain             The next filter in this chain.
	 * @param compression_level The requested level of compression.
	 */
	ZSTDSaveFilter(std::shared_ptr<SaveFilter> chain, uint8_t compression_level) : SaveFilter(std::move(chain))
	{
		this->zstd = ZSTD_createCCtx();
		if (!this->zstd) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "cannot initialize compressor");
		if (ZSTD_isError(ZSTD_CCtx_setParameter(this->zstd, ZSTD_c_compressionLevel, (int)compression_level - 100))) {
			ZSTD_freeCCtx(this->zstd);
			SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "invalid compresison level");
		}
	}

	/** Clean up what we allocated. */
	~ZSTDSaveFilter()
	{
		ZSTD_freeCCtx(this->zstd);
	}

	/**
	 * Helper loop for writing the data.
	 * @param p      The bytes to write.
	 * @param len    Amount of bytes to write.
	 * @param mode   Mode for ZSTD_compressStream2.
	 */
	void WriteLoop(uint8_t *p, size_t len, ZSTD_EndDirective mode)
	{
		ZSTD_inBuffer input{p, len, 0};

		bool finished;
		do {
			ZSTD_outBuffer output{this->buf, sizeof(this->buf), 0};
			size_t remaining = ZSTD_compressStream2(this->zstd, &output, &input, mode);
			if (ZSTD_isError(remaining)) SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, "libzstd returned error code");

			if (output.pos != 0) this->chain->Write(this->buf, output.pos);

			finished = (mode == ZSTD_e_end ? (remaining == 0) : (input.pos == input.size));
		} while (!finished);
	}

	void Write(uint8_t *buf, size_t size) override
	{
		this->WriteLoop(buf, size, ZSTD_e_continue);
	}

	void Finish() override
	{
		this->WriteLoop(nullptr, 0, ZSTD_e_end);
		this->chain->Finish();
	}
};

#endif /* WITH_LIBZSTD */

/*******************************************
 ************* END OF CODE *****************
 *******************************************/

enum SaveLoadFormatFlags : uint8_t {
	SLF_NONE             = 0,
	SLF_NO_THREADED_LOAD = 1 << 0, ///< Unsuitable for threaded loading
	SLF_REQUIRES_ZSTD    = 1 << 1, ///< Automatic selection requires the zstd flag
};
DECLARE_ENUM_AS_BIT_SET(SaveLoadFormatFlags);

/** The format for a reader/writer type of a savegame */
struct SaveLoadFormat {
	const char *name;                     ///< name of the compressor/decompressor (debug-only)
	uint32_t tag;                         ///< the 4-letter tag by which it is identified in the savegame

	std::shared_ptr<LoadFilter> (*init_load)(std::shared_ptr<LoadFilter> chain);                       ///< Constructor for the load filter.
	std::shared_ptr<SaveFilter> (*init_write)(std::shared_ptr<SaveFilter> chain, uint8_t compression); ///< Constructor for the save filter.

	uint8_t min_compression;              ///< the minimum compression level of this format
	uint8_t default_compression;          ///< the default compression level of this format
	uint8_t max_compression;              ///< the maximum compression level of this format
	SaveLoadFormatFlags flags;            ///< flags
};

/** The different saveload formats known/understood by OpenTTD. */
static const SaveLoadFormat _saveload_formats[] = {
#if defined(WITH_LZO)
	/* Roughly 75% larger than zlib level 6 at only ~7% of the CPU usage. */
	{"lzo",    TO_BE32('OTTD'), CreateLoadFilter<LZOLoadFilter>,    CreateSaveFilter<LZOSaveFilter>,    0, 0, 0, SLF_NO_THREADED_LOAD},
#else
	{"lzo",    TO_BE32('OTTD'), nullptr,                            nullptr,                            0, 0, 0, SLF_NO_THREADED_LOAD},
#endif
	/* Roughly 5 times larger at only 1% of the CPU usage over zlib level 6. */
	{"none",   TO_BE32('OTTN'), CreateLoadFilter<NoCompLoadFilter>, CreateSaveFilter<NoCompSaveFilter>, 0, 0, 0, SLF_NONE},
#if defined(WITH_ZLIB)
	/* After level 6 the speed reduction is significant (1.5x to 2.5x slower per level), but the reduction in filesize is
	 * fairly insignificant (~1% for each step). Lower levels become ~5-10% bigger by each level than level 6 while level
	 * 1 is "only" 3 times as fast. Level 0 results in uncompressed savegames at about 8 times the cost of "none". */
	{"zlib",   TO_BE32('OTTZ'), CreateLoadFilter<ZlibLoadFilter>,   CreateSaveFilter<ZlibSaveFilter>,   0, 6, 9, SLF_NONE},
#else
	{"zlib",   TO_BE32('OTTZ'), nullptr,                            nullptr,                            0, 0, 0, SLF_NONE},
#endif
#if defined(WITH_LIBLZMA)
	/* Level 2 compression is speed wise as fast as zlib level 6 compression (old default), but results in ~10% smaller saves.
	 * Higher compression levels are possible, and might improve savegame size by up to 25%, but are also up to 10 times slower.
	 * The next significant reduction in file size is at level 4, but that is already 4 times slower. Level 3 is primarily 50%
	 * slower while not improving the filesize, while level 0 and 1 are faster, but don't reduce savegame size much.
	 * It's OTTX and not e.g. OTTL because liblzma is part of xz-utils and .tar.xz is preferred over .tar.lzma. */
	{"lzma",   TO_BE32('OTTX'), CreateLoadFilter<LZMALoadFilter>,   CreateSaveFilter<LZMASaveFilter>,   0, 2, 9, SLF_NONE},
#else
	{"lzma",   TO_BE32('OTTX'), nullptr,                            nullptr,                            0, 0, 0, SLF_NONE},
#endif
#if defined(WITH_ZSTD)
	/* Zstd provides a decent compression rate at a very high compression/decompression speed. Compared to lzma level 2
	 * zstd saves are about 40% larger (on level 1) but it has about 30x faster compression and 5x decompression making it
	 * a good choice for multiplayer servers. And zstd level 1 seems to be the optimal one for client connection speed
	 * (compress + 10 MB/s download + decompress time), about 3x faster than lzma:2 and 1.5x than zlib:2 and lzo.
	 * As zstd has negative compression levels the values were increased by 100 moving zstd level range -100..22 into
	 * openttd 0..122. Also note that value 100 matches zstd level 0 which is a special value for default level 3 (openttd 103) */
	{"zstd",   TO_BE32('OTTS'), CreateLoadFilter<ZSTDLoadFilter>,   CreateSaveFilter<ZSTDSaveFilter>,   0, 101, 122, SLF_REQUIRES_ZSTD},
#else
	{"zstd",   TO_BE32('OTTS'), nullptr,                            nullptr,                            0, 0, 0, SLF_REQUIRES_ZSTD},
#endif
};

/**
 * Return the savegameformat of the game. Whether it was created with ZLIB compression
 * uncompressed, or another type
 * @param full_name Name of the savegame format. If empty it picks the first available one
 * @param compression_level Output for telling what compression level we want.
 * @return Pointer to SaveLoadFormat struct giving all characteristics of this type of savegame
 */
static const SaveLoadFormat *GetSavegameFormat(const std::string &full_name, uint8_t *compression_level, SaveModeFlags flags)
{
	const SaveLoadFormat *def = lastof(_saveload_formats);

	/* find default savegame format, the highest one with which files can be written */
	while (!def->init_write || ((def->flags & SLF_REQUIRES_ZSTD) && !(flags & SMF_ZSTD_OK))) def--;

	if (!full_name.empty()) {
		/* Get the ":..." of the compression level out of the way */
		size_t separator = full_name.find(':');
		bool has_comp_level = separator != std::string::npos;
		const std::string name(full_name, 0, has_comp_level ? separator : full_name.size());

		for (const SaveLoadFormat *slf = &_saveload_formats[0]; slf != endof(_saveload_formats); slf++) {
			if (slf->init_write != nullptr && name.compare(slf->name) == 0) {
				*compression_level = slf->default_compression;
				if (has_comp_level) {
					const std::string complevel(full_name, separator + 1);

					/* Get the level and determine whether all went fine. */
					size_t processed;
					long level = std::stol(complevel, &processed, 10);
					if (processed == 0 || level != Clamp(level, slf->min_compression, slf->max_compression)) {
						SetDParamStr(0, complevel);
						ShowErrorMessage(STR_CONFIG_ERROR, STR_CONFIG_ERROR_INVALID_SAVEGAME_COMPRESSION_LEVEL, WL_CRITICAL);
					} else {
						*compression_level = level;
					}
				}
				return slf;
			}
		}

		SetDParamStr(0, name);
		SetDParamStr(1, def->name);
		ShowErrorMessage(STR_CONFIG_ERROR, STR_CONFIG_ERROR_INVALID_SAVEGAME_COMPRESSION_ALGORITHM, WL_CRITICAL);
	}
	*compression_level = def->default_compression;
	return def;
}

/* actual loader/saver function */
void InitializeGame(uint size_x, uint size_y, bool reset_date, bool reset_settings);
extern bool AfterLoadGame();
extern bool LoadOldSaveGame(const std::string &file);

/**
 * Clear temporary data that is passed between various saveload phases.
 */
static void ResetSaveloadData()
{
	ResetTempEngineData();
	ClearRailTypeLabelList();
	ClearRoadTypeLabelList();
	ResetOldWaypoints();

	extern void ClearOrderPoolLoadState();
	ClearOrderPoolLoadState();

	extern void ClearVehicleOldOrderLoadState();
	ClearVehicleOldOrderLoadState();
}

/**
 * Clear/free saveload state.
 */
static inline void ClearSaveLoadState()
{
	_sl.dumper = nullptr;
	_sl.sf = nullptr;
	_sl.reader = nullptr;
	_sl.lf = nullptr;
	_sl.save_flags = SMF_NONE;
	_sl.current_chunk_id = 0;
	_sl.chunk_block_modes.clear();

	GamelogStopAnyAction();
}

/** Update the gui accordingly when starting saving and set locks on saveload. */
static void SaveFileStart()
{
	SetMouseCursorBusy(true);

	InvalidateWindowData(WC_STATUS_BAR, 0, SBI_SAVELOAD_START);
	_sl.saveinprogress = true;
}

/** Update the gui accordingly when saving is done and release locks on saveload. */
static void SaveFileDone()
{
	SetMouseCursorBusy(false);

	InvalidateWindowData(WC_STATUS_BAR, 0, SBI_SAVELOAD_FINISH);
	_sl.saveinprogress = false;

#ifdef __EMSCRIPTEN__
	EM_ASM(if (window["openttd_syncfs"]) openttd_syncfs());
#endif
}

/** Set the error message from outside of the actual loading/saving of the game (AfterLoadGame and friends) */
void SetSaveLoadError(StringID str)
{
	_sl.error_str = str;
}

/** Return the appropriate initial string for an error depending on whether we are saving or loading. */
StringID GetSaveLoadErrorType()
{
	return _sl.action == SLA_SAVE ? STR_ERROR_GAME_SAVE_FAILED : STR_ERROR_GAME_LOAD_FAILED;
}

/** Return the description of the error. **/
StringID GetSaveLoadErrorMessage()
{
	SetDParamStr(0, _sl.extra_msg);
	return _sl.error_str;
}

/** Show a gui message when saving has failed */
static void SaveFileError()
{
	ShowErrorMessage(GetSaveLoadErrorType(), GetSaveLoadErrorMessage(), WL_ERROR);
	SaveFileDone();
}

/**
 * We have written the whole game into memory, _memory_savegame, now find
 * and appropriate compressor and start writing to file.
 */
static SaveOrLoadResult SaveFileToDisk(bool threaded)
{
	try {
		uint8_t compression;
		const SaveLoadFormat *fmt = GetSavegameFormat(_savegame_format, &compression, _sl.save_flags);

		Debug(sl, 3, "Using compression format: {}, level: {}", fmt->name, compression);

		/* We have written our stuff to memory, now write it to file! */
		uint32_t hdr[2] = { fmt->tag, TO_BE32((uint32_t) (SAVEGAME_VERSION | SAVEGAME_VERSION_EXT) << 16) };
		_sl.sf->Write((uint8_t*)hdr, sizeof(hdr));

		_sl.sf = fmt->init_write(_sl.sf, compression);
		_sl.dumper->Flush(*(_sl.sf));

		ClearSaveLoadState();

		if (threaded) SetAsyncSaveFinish(SaveFileDone);

		return SL_OK;
	} catch (...) {
		ClearSaveLoadState();

		AsyncSaveFinishProc asfp = SaveFileDone;

		/* We don't want to shout when saving is just
		 * cancelled due to a client disconnecting. */
		if (_sl.error_str != STR_NETWORK_ERROR_LOSTCONNECTION) {
			/* Skip the "colour" character */
			Debug(sl, 0, "{}{}", strip_leading_colours(GetString(GetSaveLoadErrorType())), GetString(GetSaveLoadErrorMessage()));
			asfp = SaveFileError;
		}

		if (threaded) {
			SetAsyncSaveFinish(asfp);
		} else {
			asfp();
		}
		return SL_ERROR;
	}
}

void WaitTillSaved()
{
	_async_save_thread.WaitTillSaved();
}

/**
 * Actually perform the saving of the savegame.
 * General tactics is to first save the game to memory, then write it to file
 * using the writer, either in threaded mode if possible, or single-threaded.
 * @param writer   The filter to write the savegame to.
 * @param threaded Whether to try to perform the saving asynchronously.
 * @return Return the result of the action. #SL_OK or #SL_ERROR
 */
static SaveOrLoadResult DoSave(std::shared_ptr<SaveFilter> writer, bool threaded)
{
	assert(!_sl.saveinprogress);

	_sl.dumper = std::make_unique<MemoryDumper>();
	_sl.sf = std::move(writer);

	_sl_version = SAVEGAME_VERSION;
	SlXvSetCurrentState();

	SaveViewportBeforeSaveGame();
	SlSaveChunks();

	SaveFileStart();

	if (!threaded || !StartNewThread(&_async_save_thread.save_thread, "ottd:savegame", &SaveFileToDisk, true)) {
		if (threaded) Debug(sl, 1, "Cannot create savegame thread, reverting to single-threaded mode...");

		SaveOrLoadResult result = SaveFileToDisk(false);
		SaveFileDone();

		return result;
	}

	return SL_OK;
}

/**
 * Save the game using a (writer) filter.
 * @param writer   The filter to write the savegame to.
 * @param threaded Whether to try to perform the saving asynchronously.
 * @param flags Save mode flags.
 * @return Return the result of the action. #SL_OK or #SL_ERROR
 */
SaveOrLoadResult SaveWithFilter(std::shared_ptr<SaveFilter> writer, bool threaded, SaveModeFlags flags)
{
	try {
		_sl.action = SLA_SAVE;
		_sl.save_flags = flags;
		return DoSave(std::move(writer), threaded);
	} catch (...) {
		ClearSaveLoadState();
		return SL_ERROR;
	}
}

bool IsNetworkServerSave()
{
	return _sl.save_flags & SMF_NET_SERVER;
}

bool IsScenarioSave()
{
	return _sl.save_flags & SMF_SCENARIO;
}

struct ThreadedLoadFilter : LoadFilter {
	static const size_t BUFFER_COUNT = 4;

	std::mutex mutex;
	std::condition_variable full_cv;
	std::condition_variable empty_cv;
	uint first_ready = 0;
	uint count_ready = 0;
	size_t read_offsets[BUFFER_COUNT];
	size_t read_counts[BUFFER_COUNT];
	uint8_t read_buf[MEMORY_CHUNK_SIZE * BUFFER_COUNT]; ///< Buffers for reading from source.
	bool no_thread = false;

	bool have_exception = false;
	ThreadSlErrorException caught_exception;

	std::thread read_thread;

	/**
	 * Initialise this filter.
	 * @param chain The next filter in this chain.
	 */
	ThreadedLoadFilter(std::shared_ptr<LoadFilter> chain) : LoadFilter(std::move(chain))
	{
		std::unique_lock<std::mutex> lk(this->mutex);
		if (!StartNewThread(&this->read_thread, "ottd:loadgame", &ThreadedLoadFilter::RunThread, this)) {
			Debug(sl, 1, "Failed to start load read thread, reading non-threaded");
			this->no_thread = true;
		} else {
			Debug(sl, 2, "Started load read thread");
		}
	}

	/** Clean everything up. */
	~ThreadedLoadFilter()
	{
		std::unique_lock<std::mutex> lk(this->mutex);
		this->no_thread = true;
		lk.unlock();
		this->empty_cv.notify_all();
		this->full_cv.notify_all();
		if (this->read_thread.joinable()) {
			this->read_thread.join();
			Debug(sl, 2, "Joined load read thread");
		}
	}

	static void RunThread(ThreadedLoadFilter *self)
	{
		try {
			std::unique_lock<std::mutex> lk(self->mutex);
			while (!self->no_thread) {
				if (self->count_ready == BUFFER_COUNT) {
					self->full_cv.wait(lk);
					continue;
				}

				uint buf = (self->first_ready + self->count_ready) % BUFFER_COUNT;
				lk.unlock();
				size_t read = self->chain->Read(self->read_buf + (buf * MEMORY_CHUNK_SIZE), MEMORY_CHUNK_SIZE);
				lk.lock();
				self->read_offsets[buf] = 0;
				self->read_counts[buf] = read;
				self->count_ready++;
				if (self->count_ready == 1) self->empty_cv.notify_one();
			}
		} catch (const ThreadSlErrorException &ex) {
			std::unique_lock<std::mutex> lk(self->mutex);
			self->caught_exception = std::move(ex);
			self->have_exception = true;
			self->empty_cv.notify_one();
		}
	}

	size_t Read(uint8_t *buf, size_t size) override
	{
		if (this->no_thread) return this->chain->Read(buf, size);

		size_t read = 0;
		std::unique_lock<std::mutex> lk(this->mutex);
		while (read < size || this->have_exception) {
			if (this->have_exception) {
				this->have_exception = false;
				SlError(this->caught_exception.string, this->caught_exception.extra_msg);
			}
			if (this->count_ready == 0) {
				this->empty_cv.wait(lk);
				continue;
			}

			size_t to_read = std::min<size_t>(size - read, read_counts[this->first_ready]);
			if (to_read == 0) break;
			memcpy(buf + read, this->read_buf + (this->first_ready * MEMORY_CHUNK_SIZE) + read_offsets[this->first_ready], to_read);
			read += to_read;
			read_offsets[this->first_ready] += to_read;
			read_counts[this->first_ready] -= to_read;
			if (read_counts[this->first_ready] == 0) {
				this->first_ready = (this->first_ready + 1) % BUFFER_COUNT;
				this->count_ready--;
				if (this->count_ready == BUFFER_COUNT - 1) this->full_cv.notify_one();
			}
		}
		return read;
	}
};

/**
 * Actually perform the loading of a "non-old" savegame.
 * @param reader     The filter to read the savegame from.
 * @param load_check Whether to perform the checking ("preview") or actually load the game.
 * @return Return the result of the action. #SL_OK or #SL_REINIT ("unload" the game)
 */
static SaveOrLoadResult DoLoad(std::shared_ptr<LoadFilter> reader, bool load_check)
{
	_sl.lf = std::move(reader);

	if (load_check) {
		/* Clear previous check data */
		_load_check_data.Clear();
		/* Mark SL_LOAD_CHECK as supported for this savegame. */
		_load_check_data.checkable = true;
	}

	SlXvResetState();
	SlResetVENC();
	SlResetTNNC();
	SlResetERNC();
	auto guard = scope_guard([&]() {
		SlResetVENC();
		SlResetTNNC();
	});

	uint32_t hdr[2];
	if (_sl.lf->Read((uint8_t*)hdr, sizeof(hdr)) != sizeof(hdr)) SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_READABLE);

	SaveLoadVersion original_sl_version = SL_MIN_VERSION;

	/* see if we have any loader for this type. */
	const SaveLoadFormat *fmt = _saveload_formats;
	for (;;) {
		/* No loader found, treat as version 0 and use LZO format */
		if (fmt == endof(_saveload_formats)) {
			Debug(sl, 0, "Unknown savegame type, trying to load it as the buggy format");
			_sl.lf->Reset();
			_sl_version = SL_MIN_VERSION;
			_sl_minor_version = 0;
			SlXvResetState();

			/* Try to find the LZO savegame format; it uses 'OTTD' as tag. */
			fmt = _saveload_formats;
			for (;;) {
				if (fmt == endof(_saveload_formats)) {
					/* Who removed LZO support? */
					NOT_REACHED();
				}
				if (fmt->tag == TO_BE32('OTTD')) break;
				fmt++;
			}
			break;
		}

		if (fmt->tag == hdr[0]) {
			/* check version number */
			_sl_version = (SaveLoadVersion)(TO_BE32(hdr[1]) >> 16);
			/* Minor is not used anymore from version 18.0, but it is still needed
			 * in versions before that (4 cases) which can't be removed easy.
			 * Therefore it is loaded, but never saved (or, it saves a 0 in any scenario). */
			_sl_minor_version = (TO_BE32(hdr[1]) >> 8) & 0xFF;

			bool special_version = false;
			if (_sl_version & SAVEGAME_VERSION_EXT) {
				_sl_version = (SaveLoadVersion)(_sl_version & ~SAVEGAME_VERSION_EXT);
				_sl_is_ext_version = true;
			} else {
				special_version = SlXvCheckSpecialSavegameVersions();
			}

			original_sl_version = _sl_version;

			if (_sl_version >= SLV_SAVELOAD_LIST_LENGTH) {
				if (_sl_is_ext_version) {
					Debug(sl, 0, "Got an extended savegame version with a base version in the upstream mode range, giving up");
					SlError(STR_GAME_SAVELOAD_ERROR_TOO_NEW_SAVEGAME);
				} else {
					_sl_upstream_mode = true;
				}
			}

			Debug(sl, 1, "Loading savegame version {}{}{}{}{}", _sl_version, _sl_is_ext_version ? " (extended)" : "",
					_sl_maybe_springpp ? " which might be SpringPP" : "", _sl_maybe_chillpp ? " which might be ChillPP" : "", _sl_upstream_mode ? " (upstream mode)" : "");

			/* Is the version higher than the current? */
			if (_sl_version > MAX_LOAD_SAVEGAME_VERSION && !special_version) SlError(STR_GAME_SAVELOAD_ERROR_TOO_NEW_SAVEGAME);
			if (_sl_version >= SLV_START_PATCHPACKS && _sl_version <= SLV_END_PATCHPACKS && !special_version) SlError(STR_GAME_SAVELOAD_ERROR_PATCHPACK);
			break;
		}

		fmt++;
	}

	/* loader for this savegame type is not implemented? */
	if (fmt->init_load == nullptr) {
		SlError(STR_GAME_SAVELOAD_ERROR_BROKEN_INTERNAL_ERROR, fmt::format("Loader for '{}' is not available.", fmt->name));
	}

	_sl.lf = fmt->init_load(std::move(_sl.lf));
	if (!(fmt->flags & SLF_NO_THREADED_LOAD)) {
		_sl.lf = std::make_shared<ThreadedLoadFilter>(std::move(_sl.lf));
	}
	_sl.reader = std::make_unique<ReadBuffer>(_sl.lf);
	_next_offs = 0;

	upstream_sl::SlResetLoadState();

	if (!load_check) {
		ResetSaveloadData();

		/* Old maps were hardcoded to 256x256 and thus did not contain
		 * any mapsize information. Pre-initialize to 256x256 to not to
		 * confuse old games */
		InitializeGame(256, 256, true, true);

		GamelogReset();

		if (IsSavegameVersionBefore(SLV_4)) {
			/*
			 * NewGRFs were introduced between 0.3,4 and 0.3.5, which both
			 * shared savegame version 4. Anything before that 'obviously'
			 * does not have any NewGRFs. Between the introduction and
			 * savegame version 41 (just before 0.5) the NewGRF settings
			 * were not stored in the savegame and they were loaded by
			 * using the settings from the main menu.
			 * So, to recap:
			 * - savegame version  <  4:  do not load any NewGRFs.
			 * - savegame version >= 41:  load NewGRFs from savegame, which is
			 *                            already done at this stage by
			 *                            overwriting the main menu settings.
			 * - other savegame versions: use main menu settings.
			 *
			 * This means that users *can* crash savegame version 4..40
			 * savegames if they set incompatible NewGRFs in the main menu,
			 * but can't crash anymore for savegame version < 4 savegames.
			 *
			 * Note: this is done here because AfterLoadGame is also called
			 * for TTO/TTD/TTDP savegames which have their own NewGRF logic.
			 */
			ClearGRFConfigList(_grfconfig);
		}
	}

	if (load_check) {
		/* Load chunks into _load_check_data.
		 * No pools are loaded. References are not possible, and thus do not need resolving. */
		SlLoadCheckChunks();
	} else {
		ResetSettingsToDefaultForLoad();

		/* Load chunks and resolve references */
		SlLoadChunks();
		SlFixPointers();
	}

	ClearSaveLoadState();

	_savegame_type = SGT_OTTD;

	if (load_check) {
		/* The only part from AfterLoadGame() we need */
		if (_load_check_data.want_grf_compatibility) _load_check_data.grf_compatibility = IsGoodGRFConfigList(_load_check_data.grfconfig);
		_load_check_data.sl_is_ext_version = _sl_is_ext_version;

		if (GetDebugLevel(DebugLevelID::sl) > 0) {
			_load_check_data.version_name = fmt::format("Version {}{}{}", original_sl_version, _sl_is_ext_version ? ", extended" : "", _sl_upstream_mode ? ", upstream mode" : "");
			if (_sl_version != original_sl_version) {
				_load_check_data.version_name += fmt::format(" as {}", _sl_version);
			}
			if (_sl_xv_feature_versions[XSLFI_CHILLPP] >= SL_CHILLPP_232) {
				_load_check_data.version_name += ", ChillPP v14.7";
			} else if (_sl_xv_feature_versions[XSLFI_CHILLPP] > 0) {
				_load_check_data.version_name += ", ChillPP v8";
			}
			if (_sl_xv_feature_versions[XSLFI_SPRINGPP] > 0) {
				_load_check_data.version_name += ", SpringPP 2013 ";
				switch (_sl_xv_feature_versions[XSLFI_SPRINGPP]) {
					case 1:
						_load_check_data.version_name += "v2.0.102";
						break;
					case 2:
						_load_check_data.version_name += "v2.0.108";
						break;
					case 3:
						_load_check_data.version_name += "v2.3.xxx"; // Note that this break in numbering is deliberate
						break;
					case 4:
						_load_check_data.version_name += "v2.1.147"; // Note that this break in numbering is deliberate
						break;
					case 5:
						_load_check_data.version_name += "v2.3.b3";
						break;
					case 6:
						_load_check_data.version_name += "v2.3.b4";
						break;
					case 7:
						_load_check_data.version_name += "v2.3.b5";
						break;
					case 8:
						_load_check_data.version_name += "v2.4";
						break;
					default:
						_load_check_data.version_name += "???";
						break;
				}
			}
			if (_sl_xv_feature_versions[XSLFI_JOKERPP] > 0) {
				_load_check_data.version_name += ", JokerPP";
			}

			extern std::string _sl_xv_version_label;
			extern SaveLoadVersion _sl_xv_upstream_version;
			if (!_sl_xv_version_label.empty()) {
				_load_check_data.version_name += fmt::format(", labelled: {}", _sl_xv_version_label);
			}
			if (_sl_xv_upstream_version > 0) {
				_load_check_data.version_name += fmt::format(", upstream version: {}", _sl_xv_upstream_version);
			}
		}
	} else {
		GamelogStartAction(GLAT_LOAD);

		/* After loading fix up savegame for any internal changes that
		 * might have occurred since then. If it fails, load back the old game. */
		if (!AfterLoadGame()) {
			GamelogStopAction();
			return SL_REINIT;
		}

		GamelogStopAction();
		SlXvSetCurrentState();
	}

	return SL_OK;
}

/**
 * Load the game using a (reader) filter.
 * @param reader   The filter to read the savegame from.
 * @return Return the result of the action. #SL_OK or #SL_REINIT ("unload" the game)
 */
SaveOrLoadResult LoadWithFilter(std::shared_ptr<LoadFilter> reader)
{
	try {
		_sl.action = SLA_LOAD;
		return DoLoad(std::move(reader), false);
	} catch (...) {
		ClearSaveLoadState();

		/* Skip the "colour" character */
		Debug(sl, 0, "{}{}", strip_leading_colours(GetString(GetSaveLoadErrorType())), GetString(GetSaveLoadErrorMessage()));

		return SL_REINIT;
	}
}

/**
 * Main Save or Load function where the high-level saveload functions are
 * handled. It opens the savegame, selects format and checks versions
 * @param filename The name of the savegame being created/loaded
 * @param fop Save or load mode. Load can also be a TTD(Patch) game.
 * @param sb The sub directory to save the savegame in
 * @param threaded True when threaded saving is allowed
 * @return Return the result of the action. #SL_OK, #SL_ERROR, or #SL_REINIT ("unload" the game)
 */
SaveOrLoadResult SaveOrLoad(const std::string &filename, SaveLoadOperation fop, DetailedFileType dft, Subdirectory sb, bool threaded, SaveModeFlags save_flags)
{
	/* An instance of saving is already active, so don't go saving again */
	if (_sl.saveinprogress && fop == SLO_SAVE && dft == DFT_GAME_FILE && threaded) {
		/* if not an autosave, but a user action, show error message */
		if (!_do_autosave) ShowErrorMessage(STR_ERROR_SAVE_STILL_IN_PROGRESS, INVALID_STRING_ID, WL_ERROR);
		return SL_OK;
	}
	WaitTillSaved();

	try {
		/* Load a TTDLX or TTDPatch game */
		if (fop == SLO_LOAD && dft == DFT_OLD_GAME_FILE) {
			ResetSaveloadData();

			InitializeGame(256, 256, true, true); // set a mapsize of 256x256 for TTDPatch games or it might get confused

			ResetSettingsToDefaultForLoad();

			/* TTD/TTO savegames have no NewGRFs, TTDP savegame have them
			 * and if so a new NewGRF list will be made in LoadOldSaveGame.
			 * Note: this is done here because AfterLoadGame is also called
			 * for OTTD savegames which have their own NewGRF logic. */
			ClearGRFConfigList(_grfconfig);
			GamelogReset();
			if (!LoadOldSaveGame(filename)) return SL_REINIT;
			_sl_version = SL_MIN_VERSION;
			_sl_minor_version = 0;
			SlXvResetState();
			GamelogStartAction(GLAT_LOAD);
			if (!AfterLoadGame()) {
				GamelogStopAction();
				return SL_REINIT;
			}
			GamelogStopAction();
			SlXvSetCurrentState();
			return SL_OK;
		}

		assert(dft == DFT_GAME_FILE);
		switch (fop) {
			case SLO_CHECK:
				_sl.action = SLA_LOAD_CHECK;
				break;

			case SLO_LOAD:
				_sl.action = SLA_LOAD;
				break;

			case SLO_SAVE:
				_sl.action = SLA_SAVE;
				break;

			default: NOT_REACHED();
		}
		_sl.save_flags = save_flags;

		std::optional<FileHandle> fh;
		std::string temp_save_filename;
		std::string temp_save_filename_suffix;

		if (fop == SLO_SAVE) {
			temp_save_filename_suffix = fmt::format(".tmp-{:08x}", InteractiveRandom());
			fh = FioFOpenFile(filename + temp_save_filename_suffix, "wb", sb, nullptr, &temp_save_filename);
		} else {
			fh = FioFOpenFile(filename, "rb", sb);

			/* Make it a little easier to load savegames from the console */
			if (!fh.has_value()) fh = FioFOpenFile(filename, "rb", SAVE_DIR);
			if (!fh.has_value()) fh = FioFOpenFile(filename, "rb", BASE_DIR);
			if (!fh.has_value()) fh = FioFOpenFile(filename, "rb", SCENARIO_DIR);
		}

		if (!fh.has_value()) {
			SlError(fop == SLO_SAVE ? STR_GAME_SAVELOAD_ERROR_FILE_NOT_WRITEABLE : STR_GAME_SAVELOAD_ERROR_FILE_NOT_READABLE);
		}

		if (fop == SLO_SAVE) { // SAVE game
			if (temp_save_filename.size() <= temp_save_filename_suffix.size()) SlError(STR_GAME_SAVELOAD_ERROR_FILE_NOT_WRITEABLE, "Failed to get temporary file name");
			Debug(desync, 1, "save: {}; {}", debug_date_dumper().HexDate(), filename);
			if (!_settings_client.gui.threaded_saves) threaded = false;

			return DoSave(std::make_shared<FileWriter>(std::move(*fh), temp_save_filename, temp_save_filename.substr(0, temp_save_filename.size() - temp_save_filename_suffix.size())), threaded);
		}

		/* LOAD game */
		assert(fop == SLO_LOAD || fop == SLO_CHECK);
		Debug(desync, 1, "load: {}", filename);
		return DoLoad(std::make_shared<FileReader>(std::move(*fh)), fop == SLO_CHECK);
	} catch (...) {
		/* This code may be executed both for old and new save games. */
		ClearSaveLoadState();

		/* Skip the "colour" character */
		if (fop != SLO_CHECK) Debug(sl, 0, "{}{}", strip_leading_colours(GetString(GetSaveLoadErrorType())), GetString(GetSaveLoadErrorMessage()));

		/* A saver/loader exception!! reinitialize all variables to prevent crash! */
		return (fop == SLO_LOAD) ? SL_REINIT : SL_ERROR;
	}
}

/**
 * Create an autosave or netsave.
 * @param counter A reference to the counter variable to be used for rotating the file name.
 * @param netsave Indicates if this is a regular autosave or a netsave.
 */
void DoAutoOrNetsave(FiosNumberedSaveName &counter, bool threaded, FiosNumberedSaveName *lt_counter)
{
	std::string filename;

	if (_settings_client.gui.keep_all_autosave) {
		filename = GenerateDefaultSaveName() + counter.Extension();
	} else {
		filename = counter.Filename();
		if (lt_counter != nullptr && counter.GetLastNumber() == 0) {
			std::string lt_path = lt_counter->FilenameUsingMaxSaves(_settings_client.gui.max_num_lt_autosaves);
			Debug(sl, 2, "Renaming autosave '{}' to long-term file '{}'", filename, lt_path);
			std::string dir = FioFindDirectory(AUTOSAVE_DIR);
			FioRenameFile(dir + filename, dir + lt_path);
		}
	}

	Debug(sl, 2, "Autosaving to '{}'", filename);
	if (SaveOrLoad(filename, SLO_SAVE, DFT_GAME_FILE, AUTOSAVE_DIR, threaded, SMF_ZSTD_OK) != SL_OK) {
		ShowErrorMessage(STR_ERROR_AUTOSAVE_FAILED, INVALID_STRING_ID, WL_ERROR);
	}
}


/** Do a save when exiting the game (_settings_client.gui.autosave_on_exit) */
void DoExitSave()
{
	SaveOrLoad("exit.sav", SLO_SAVE, DFT_GAME_FILE, AUTOSAVE_DIR, true, SMF_ZSTD_OK);
}

/**
 * Get the default name for a savegame *or* screenshot.
 */
std::string GenerateDefaultSaveName()
{
	/* Check if we have a name for this map, which is the name of the first
	 * available company. When there's no company available we'll use
	 * 'Spectator' as "company" name. */
	CompanyID cid = _local_company;
	if (!Company::IsValidID(cid)) {
		for (const Company *c : Company::Iterate()) {
			cid = c->index;
			break;
		}
	}

	SetDParam(0, cid);

	/* We show the current game time differently depending on the timekeeping units used by this game. */
	if (EconTime::UsingWallclockUnits() && CalTime::IsCalendarFrozen()) {
		/* Insert time played. */
		const auto play_time = _scaled_tick_counter / TICKS_PER_SECOND;
		SetDParam(1, STR_SAVEGAME_DURATION_REALTIME);
		SetDParam(2, play_time / 60 / 60);
		SetDParam(3, (play_time / 60) % 60);
	} else {
		/* Insert current date */
		switch (_settings_client.gui.date_format_in_default_names) {
			case 0: SetDParam(1, STR_JUST_DATE_LONG); break;
			case 1: SetDParam(1, STR_JUST_DATE_TINY); break;
			case 2: SetDParam(1, STR_JUST_DATE_ISO); break;
			default: NOT_REACHED();
		}
		SetDParam(2, CalTime::CurDate());
	}

	/* Get the correct string (special string for when there's not company) */
	std::string filename = GetString(!Company::IsValidID(cid) ? STR_SAVEGAME_NAME_SPECTATOR : STR_SAVEGAME_NAME_DEFAULT);
	SanitizeFilename(filename);
	return filename;
}

/**
 * Set the mode and file type of the file to save or load based on the type of file entry at the file system.
 * @param ft Type of file entry of the file system.
 */
void FileToSaveLoad::SetMode(FiosType ft)
{
	this->SetMode(SLO_LOAD, GetAbstractFileType(ft), GetDetailedFileType(ft));
}

/**
 * Set the mode and file type of the file to save or load.
 * @param fop File operation being performed.
 * @param aft Abstract file type.
 * @param dft Detailed file type.
 */
void FileToSaveLoad::SetMode(SaveLoadOperation fop, AbstractFileType aft, DetailedFileType dft)
{
	if (aft == FT_INVALID || aft == FT_NONE) {
		this->file_op = SLO_INVALID;
		this->detail_ftype = DFT_INVALID;
		this->abstract_ftype = FT_INVALID;
		return;
	}

	this->file_op = fop;
	this->detail_ftype = dft;
	this->abstract_ftype = aft;
}

/**
 * Set the title of the file.
 * @param title Title of the file.
 */
void FileToSaveLoad::Set(const FiosItem &item)
{
	this->SetMode(item.type);
	this->name = item.name;
	this->title = item.title;
}

bool SaveLoadFileTypeIsScenario()
{
	return _file_to_saveload.abstract_ftype == FT_SCENARIO;
}

void SlUnreachablePlaceholder()
{
	NOT_REACHED();
}

SaveLoadVersion SlExecWithSlVersionStart(SaveLoadVersion use_version)
{
	Debug(sl, 4, "SlExecWithSlVersion start: {}", use_version);
	SaveLoadVersion old_ver = _sl_version;
	_sl_version = use_version;
	return old_ver;
}

void SlExecWithSlVersionEnd(SaveLoadVersion old_version)
{
	Debug(sl, 4, "SlExecWithSlVersion end");
	_sl_version = old_version;
}

SaveLoadVersion GeneralUpstreamChunkLoadInfo::GetLoadVersion()
{
	extern SaveLoadVersion _sl_xv_upstream_version;

	uint8_t block_mode = _sl.chunk_block_modes[_sl.current_chunk_id];
	return (block_mode == CH_TABLE || block_mode == CH_SPARSE_TABLE) ? _sl_xv_upstream_version : _sl_version;
}

const char *ChunkIDDumper::operator()(uint32_t id)
{
	format_to_fixed_z::format_to(this->buffer, lastof(this->buffer), "{:c}{:c}{:c}{:c}", id >> 24, id >> 16, id >> 8, id);
	return this->buffer;
}
