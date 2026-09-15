/****
DIAMOND protein sequence aligner
Copyright (C) 2012-2026 Benjamin J. Buchfink

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
****/
// SPDX-License-Identifier: GPL-3.0-or-later

/* Gzip output through Intel ISA-L (igzip). The stream written is a standard single-member
   gzip file, readable by zlib, gzip and every other gzip reader; only the compressor differs.
   ISA-L trades a slightly lower compression ratio than zlib's default level for a far higher
   throughput, which matters because DIAMOND compresses its whole result stream on one thread. */

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <isa-l/igzip_lib.h>
#include "compressor.h"

static uint32_t isal_level_buf_size(int level) {
	switch (level) {
	case 0:
		return 0;
	case 1:
		return ISAL_DEF_LVL1_DEFAULT;
	case 2:
		return ISAL_DEF_LVL2_DEFAULT;
	case 3:
		return ISAL_DEF_LVL3_DEFAULT;
	default:
		throw std::runtime_error("Invalid ISA-L compression level: " + std::to_string(level));
	}
}

IsalCompressor::IsalCompressor(int level) :
	strm_(new isal_zstream),
	level_buf_(isal_level_buf_size(level)),
	out_(chunk_size)
{
	isal_deflate_init(strm_.get());
	strm_->level = static_cast<uint32_t>(level);
	strm_->level_buf = level_buf_.empty() ? nullptr : level_buf_.data();
	strm_->level_buf_size = static_cast<uint32_t>(level_buf_.size());
	strm_->gzip_flag = IGZIP_GZIP;
	strm_->flush = NO_FLUSH;
	strm_->end_of_stream = 0;
}

/* Runs the compressor until all pending input has been consumed or, when finishing, until the
   gzip trailer has been written, and hands the compressed bytes to the FILE. */
void IsalCompressor::deflate_loop(FILE* stream, bool finish) {
	strm_->end_of_stream = finish ? 1 : 0;
	for (;;) {
		strm_->next_out = out_.data();
		strm_->avail_out = static_cast<uint32_t>(out_.size());
		const int ret = isal_deflate(strm_.get());
		if (ret != COMP_OK)
			throw std::runtime_error("Error during ISA-L compression (isal_deflate), code " + std::to_string(ret));
		const size_t produced = out_.size() - strm_->avail_out;
		if (produced != 0 && std::fwrite(out_.data(), 1, produced, stream) != produced)
			throw std::runtime_error(std::string("Error writing compressed file: ") + strerror(errno));
		if (finish) {
			if (strm_->internal_state.state == ZSTATE_END)
				return;
		}
		else if (strm_->avail_in == 0 && strm_->avail_out != 0)
			return;
	}
}

size_t IsalCompressor::fwrite(const void* buffer, size_t size, size_t count, FILE* stream) {
	if (size == 0 || count == 0)
		return 0;
	if (closed_)
		throw std::runtime_error("Cannot write to closed ISA-L compressor.");
	if (stream_ == nullptr)
		stream_ = stream;
	else if (stream_ != stream)
		throw std::runtime_error("Cannot write one ISA-L stream to multiple FILE handles.");
	if (count > std::numeric_limits<size_t>::max() / size)
		throw std::runtime_error("Compressed write size overflow.");

	size_t remaining = size * count;
	uint8_t* in = const_cast<uint8_t*>(static_cast<const uint8_t*>(buffer));
	while (remaining > 0) {
		const uint32_t chunk = static_cast<uint32_t>(std::min(remaining, (size_t)std::numeric_limits<uint32_t>::max()));
		strm_->next_in = in;
		strm_->avail_in = chunk;
		deflate_loop(stream, false);
		in += chunk;
		remaining -= chunk;
	}
	return count;
}

void IsalCompressor::close(FILE* stream) {
	if (closed_)
		return;
	if (stream_ == nullptr)
		stream_ = stream;
	else if (stream_ != stream)
		throw std::runtime_error("Cannot close ISA-L compressor with a different FILE handle.");
	closed_ = true;
	strm_->next_in = nullptr;
	strm_->avail_in = 0;
	deflate_loop(stream_, true);
}

IsalCompressor::~IsalCompressor() {
	if (closed_ || stream_ == nullptr)
		return;
	try {
		closed_ = true;
		strm_->next_in = nullptr;
		strm_->avail_in = 0;
		deflate_loop(stream_, true);
	}
	catch (...) {
	}
}
