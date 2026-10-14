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

#pragma once

#include "inputleap/ClipboardData.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace inputleap {

/// Encodings of clipboard contents in kMsgDClipboardData transfers.
enum class ClipboardEncoding : std::uint32_t {
    // the marshalled contents as they are
    None = 0,
    // the marshalled contents encoded with rle32_encode()
    Rle32 = 1,
    // the requested contents are not available; the transfer carries no data
    Unavailable = 255,
};

/*! Header of a kMsgDClipboardData transfer, sent as the payload of the start chunk.

The header is the text "1 <encoding> <encoded size> <size> <hash>", where 1 is the version of
the header, the sizes are decimal numbers of bytes and the hash is MarshalledClipboard::hash()
of the marshalled contents as 16 hexadecimal digits.

A transfer can also carry a piece of the contents of a shared file (see kMsgQClipboardFile):
its header is "2 <encoding> <encoded size> <size> <hash> <index> <offset>", where the hash
is that of the contents listing the files, index is the file's index in the list, offset is
where the piece starts within the file, and size is the length of the piece. Such pieces are
never compressed.
*/
struct ClipboardTransferHeader {
    enum class Kind {
        Contents,   // clipboard contents
        File,       // a piece of a shared file
    };

    Kind kind = Kind::Contents;
    ClipboardEncoding encoding = ClipboardEncoding::None;
    std::uint64_t encoded_size = 0;
    std::uint64_t size = 0;
    std::uint64_t hash = 0;
    std::uint32_t file_index = 0;
    std::uint64_t file_offset = 0;

    std::string format() const;
    static bool parse(const std::string& text, ClipboardTransferHeader& header);
};

/// The header of a transfer that carries \p size bytes of a shared file from \p offset on.
ClipboardTransferHeader make_clipboard_file_header(std::uint64_t hash, std::uint32_t index,
                                                   std::uint64_t offset, std::uint64_t size);

/// The header of a transfer that tells the receiver that a shared file is not available.
ClipboardTransferHeader make_unavailable_clipboard_file_header(std::uint64_t hash,
                                                               std::uint32_t index);

/// Clipboard contents prepared for a kMsgDClipboardData transfer.
struct ClipboardTransferPayload {
    ClipboardTransferHeader header;
    std::shared_ptr<const std::string> bytes;
};

/*! Prepares \p data for sending. If \p allow_compression is true and compression makes the data
    noticeably smaller, the data is sent run-length encoded. The encoded form is cached in
    \p data, so preparing the same contents again is cheap.
*/
ClipboardTransferPayload make_clipboard_transfer(const MarshalledClipboardPtr& data,
                                                 bool allow_compression);

/// Prepares a transfer that tells the receiver that the requested contents are not available.
ClipboardTransferPayload make_unavailable_clipboard_transfer(std::uint64_t hash);

/*! Reassembles kMsgDClipboardData transfers received on a connection.

The data is verified against the header before it is accepted, so corrupted or truncated
transfers are rejected.
*/
class ClipboardTransferReceiver {
public:
    enum class Status {
        // more chunks are expected
        InProgress,
        // the contents have been received; get them with take()
        Complete,
        // the sender does not have the requested contents
        Unavailable,
        // the transfer is malformed or exceeds the size limit
        Failed,
    };

    explicit ClipboardTransferReceiver(std::size_t max_size);

    /// Transfers of contents larger than \p max_size are rejected.
    void set_max_size(std::size_t max_size);

    /*! Receives the pieces of shared files, see ClipboardTransferHeader::Kind::File. Called
        with each chunk of such a transfer and the offset of the chunk within the file; the
        transfer fails if it returns false. Without a sink such transfers fail. */
    using FileSink = std::function<bool(const ClipboardTransferHeader& header,
                                        std::uint64_t offset, const std::string& chunk)>;
    void set_file_sink(FileSink sink) { file_sink_ = std::move(sink); }

    /// Adds a chunk with the given mark (kDataStart, kDataChunk or kDataEnd).
    Status add(std::uint8_t mark, const std::string& data);

    /// Returns the received contents after add() returned Status::Complete.
    MarshalledClipboardPtr take();

    /// The header of the current or last transfer.
    const ClipboardTransferHeader& header() const { return header_; }
    /// The bytes received so far of the current transfer, and the bytes it will have in total.
    std::uint64_t received() const { return header_.kind == ClipboardTransferHeader::Kind::File ? file_received_ : buffer_.size(); }
    std::uint64_t expected() const { return header_.encoded_size; }

private:
    std::size_t max_size_;
    bool started_ = false;
    ClipboardTransferHeader header_;
    std::string buffer_;
    MarshalledClipboardPtr result_;
    FileSink file_sink_;
    std::uint64_t file_received_ = 0;
};

} // namespace inputleap
