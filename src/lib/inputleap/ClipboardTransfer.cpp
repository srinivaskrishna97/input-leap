/*  InputLeap -- mouse and keyboard sharing utility
    Copyright (C) InputLeap contributors

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "inputleap/ClipboardTransfer.h"

#include "inputleap/ClipboardCodec.h"
#include "inputleap/protocol_types.h"
#include "base/Log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <new>
#include <sstream>

namespace inputleap {

namespace {

constexpr int kClipboardTransferHeaderVersion = 1;
constexpr int kClipboardFileTransferHeaderVersion = 2;

// A piece of a file is never larger than this, so a bogus header cannot make the receiver
// wait for more than that.
constexpr std::uint64_t kClipboardFilePieceMaxSize = 1ull << 40;

// Compression pays off only for larger contents, and only if it saves a noticeable amount.
constexpr std::size_t kClipboardMinCompressSize = 16 * 1024;

// Upper bound for received contents regardless of the configured limit, and the most memory
// reserved before the data has actually arrived.
constexpr std::size_t kClipboardTransferAbsoluteMaxSize = 512 * 1024 * 1024;
constexpr std::size_t kClipboardTransferMaxReserve = 4 * 1024 * 1024;

bool clipboard_transfer_parse_hash(const std::string& text, std::uint64_t& hash)
{
    if (text.size() != 16) {
        return false;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<std::uint64_t>(c - 'a' + 10);
        } else {
            return false;
        }
    }
    hash = value;
    return true;
}

} // namespace

std::string ClipboardTransferHeader::format() const
{
    char hash_text[17];
    std::snprintf(hash_text, sizeof(hash_text), "%016" PRIx64, hash);

    std::ostringstream out;
    if (kind == Kind::File) {
        out << kClipboardFileTransferHeaderVersion << ' ' << static_cast<std::uint32_t>(encoding)
            << ' ' << encoded_size << ' ' << size << ' ' << hash_text << ' ' << file_index << ' '
            << file_offset;
        return out.str();
    }
    out << kClipboardTransferHeaderVersion << ' ' << static_cast<std::uint32_t>(encoding) << ' '
        << encoded_size << ' ' << size << ' ' << hash_text;
    return out.str();
}

ClipboardTransferHeader make_clipboard_file_header(std::uint64_t hash, std::uint32_t index,
                                                   std::uint64_t offset, std::uint64_t size)
{
    ClipboardTransferHeader header;
    header.kind = ClipboardTransferHeader::Kind::File;
    header.encoding = ClipboardEncoding::None;
    header.encoded_size = size;
    header.size = size;
    header.hash = hash;
    header.file_index = index;
    header.file_offset = offset;
    return header;
}

ClipboardTransferHeader make_unavailable_clipboard_file_header(std::uint64_t hash,
                                                               std::uint32_t index)
{
    ClipboardTransferHeader header;
    header.kind = ClipboardTransferHeader::Kind::File;
    header.encoding = ClipboardEncoding::Unavailable;
    header.hash = hash;
    header.file_index = index;
    return header;
}

bool ClipboardTransferHeader::parse(const std::string& text, ClipboardTransferHeader& header)
{
    std::istringstream in(text);
    int version = 0;
    std::uint32_t encoding = 0;
    std::uint64_t encoded_size = 0;
    std::uint64_t size = 0;
    std::string hash_text;
    std::uint32_t file_index = 0;
    std::uint64_t file_offset = 0;
    if (!(in >> version >> encoding >> encoded_size >> size >> hash_text)) {
        return false;
    }
    if (version == kClipboardFileTransferHeaderVersion) {
        if (!(in >> file_index >> file_offset)) {
            return false;
        }
        // pieces of files are not compressed
        if (encoding == static_cast<std::uint32_t>(ClipboardEncoding::Rle32) ||
            (encoding == static_cast<std::uint32_t>(ClipboardEncoding::None) &&
             (encoded_size != size || size > kClipboardFilePieceMaxSize))) {
            return false;
        }
    }
    else if (version != kClipboardTransferHeaderVersion) {
        return false;
    }

    std::uint64_t hash = 0;
    if (!clipboard_transfer_parse_hash(hash_text, hash)) {
        return false;
    }

    switch (static_cast<ClipboardEncoding>(encoding)) {
    case ClipboardEncoding::None:
    case ClipboardEncoding::Rle32:
    case ClipboardEncoding::Unavailable:
        break;
    default:
        return false;
    }

    header.kind = version == kClipboardFileTransferHeaderVersion
                      ? ClipboardTransferHeader::Kind::File
                      : ClipboardTransferHeader::Kind::Contents;
    header.encoding = static_cast<ClipboardEncoding>(encoding);
    header.encoded_size = encoded_size;
    header.size = size;
    header.hash = hash;
    header.file_index = file_index;
    header.file_offset = file_offset;
    return true;
}

ClipboardTransferPayload make_clipboard_transfer(const MarshalledClipboardPtr& data,
                                                 bool allow_compression)
{
    const auto& contents = data ? data : empty_marshalled_clipboard();

    ClipboardTransferPayload payload;
    payload.header.size = contents->size();
    payload.header.hash = contents->hash();

    if (allow_compression && contents->size() >= kClipboardMinCompressSize) {
        auto encoded = contents->rle32();
        // only worth it if it saves at least a tenth
        if (encoded->size() < contents->size() - contents->size() / 10) {
            payload.header.encoding = ClipboardEncoding::Rle32;
            payload.header.encoded_size = encoded->size();
            payload.bytes = std::move(encoded);
            return payload;
        }
    }

    payload.header.encoding = ClipboardEncoding::None;
    payload.header.encoded_size = contents->size();
    // shares ownership with the contents instead of copying them
    payload.bytes = std::shared_ptr<const std::string>(contents, &contents->data());
    return payload;
}

ClipboardTransferPayload make_unavailable_clipboard_transfer(std::uint64_t hash)
{
    ClipboardTransferPayload payload;
    payload.header.encoding = ClipboardEncoding::Unavailable;
    payload.header.hash = hash;
    payload.bytes = std::make_shared<const std::string>();
    return payload;
}

ClipboardTransferReceiver::ClipboardTransferReceiver(std::size_t max_size)
{
    set_max_size(max_size);
}

void ClipboardTransferReceiver::set_max_size(std::size_t max_size)
{
    max_size_ = (std::min)(max_size, kClipboardTransferAbsoluteMaxSize);
}

ClipboardTransferReceiver::Status ClipboardTransferReceiver::add(std::uint8_t mark,
                                                                  const std::string& data)
{
    if (mark == kDataStart) {
        // a new transfer replaces an incomplete one
        buffer_.clear();
        result_.reset();
        started_ = false;
        file_received_ = 0;

        ClipboardTransferHeader header;
        if (!ClipboardTransferHeader::parse(data, header)) {
            LOG_WARN("invalid clipboard transfer header");
            return Status::Failed;
        }

        if (header.kind == ClipboardTransferHeader::Kind::File) {
            // the pieces go to the sink as they arrive
            header_ = header;
            if (header.encoding == ClipboardEncoding::Unavailable) {
                started_ = true;
                return Status::InProgress;
            }
            if (!file_sink_) {
                LOG_WARN("rejected a piece of a shared file, there is nowhere to put it");
                return Status::Failed;
            }
            started_ = true;
            return Status::InProgress;
        }

        std::uint64_t max_encoded_size = header.size;
        if (header.encoding == ClipboardEncoding::Rle32) {
            max_encoded_size = rle32_max_encoded_size(static_cast<std::size_t>(header.size));
        } else if (header.encoding == ClipboardEncoding::Unavailable) {
            max_encoded_size = 0;
        }
        if (header.size > max_size_ || header.encoded_size > max_encoded_size ||
            (header.encoding == ClipboardEncoding::None && header.encoded_size != header.size)) {
            LOG_WARN("rejected clipboard transfer of %" PRIu64 " bytes", header.size);
            header_ = header;
            return Status::Failed;
        }

        header_ = header;
        buffer_.reserve(static_cast<std::size_t>(
                (std::min)(header.encoded_size, static_cast<std::uint64_t>(kClipboardTransferMaxReserve))));
        started_ = true;
        return Status::InProgress;
    }

    if (!started_) {
        return Status::Failed;
    }

    if (mark == kDataChunk) {
        if (header_.kind == ClipboardTransferHeader::Kind::File) {
            if (header_.encoding == ClipboardEncoding::Unavailable ||
                data.size() > header_.encoded_size - file_received_ ||
                !file_sink_(header_, header_.file_offset + file_received_, data)) {
                started_ = false;
                return Status::Failed;
            }
            file_received_ += data.size();
            return Status::InProgress;
        }
        if (data.size() > header_.encoded_size - buffer_.size()) {
            started_ = false;
            buffer_.clear();
            return Status::Failed;
        }
        buffer_.append(data);
        return Status::InProgress;
    }

    if (mark != kDataEnd) {
        started_ = false;
        buffer_.clear();
        return Status::Failed;
    }

    started_ = false;
    if (header_.encoding == ClipboardEncoding::Unavailable) {
        return Status::Unavailable;
    }
    if (header_.kind == ClipboardTransferHeader::Kind::File) {
        return file_received_ == header_.encoded_size ? Status::Complete : Status::Failed;
    }
    if (buffer_.size() != header_.encoded_size) {
        buffer_.clear();
        return Status::Failed;
    }

    MarshalledClipboardPtr result;
    try {
        if (header_.encoding == ClipboardEncoding::Rle32) {
            std::string decoded;
            if (!rle32_decode(buffer_, static_cast<std::size_t>(header_.size), decoded)) {
                LOG_WARN("corrupted clipboard transfer");
                buffer_.clear();
                return Status::Failed;
            }
            // keep the encoded form so that relaying the contents does not encode them again
            auto encoded = std::make_shared<const std::string>(std::move(buffer_));
            result = std::make_shared<const MarshalledClipboard>(std::move(decoded),
                                                                 std::move(encoded));
        } else {
            result = make_marshalled_clipboard(std::move(buffer_));
        }
    } catch (const std::bad_alloc&) {
        LOG_WARN("not enough memory for clipboard transfer of %" PRIu64 " bytes", header_.size);
        buffer_.clear();
        return Status::Failed;
    }
    buffer_.clear();

    if (result->hash() != header_.hash) {
        LOG_WARN("clipboard transfer does not match its hash");
        return Status::Failed;
    }

    result_ = std::move(result);
    return Status::Complete;
}

MarshalledClipboardPtr ClipboardTransferReceiver::take()
{
    return std::move(result_);
}

} // namespace inputleap
