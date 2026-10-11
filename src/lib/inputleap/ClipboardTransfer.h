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
*/
struct ClipboardTransferHeader {
    ClipboardEncoding encoding = ClipboardEncoding::None;
    std::uint64_t encoded_size = 0;
    std::uint64_t size = 0;
    std::uint64_t hash = 0;

    std::string format() const;
    static bool parse(const std::string& text, ClipboardTransferHeader& header);
};

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

    /// Adds a chunk with the given mark (kDataStart, kDataChunk or kDataEnd).
    Status add(std::uint8_t mark, const std::string& data);

    /// Returns the received contents after add() returned Status::Complete.
    MarshalledClipboardPtr take();

    /// The header of the current or last transfer.
    const ClipboardTransferHeader& header() const { return header_; }

private:
    std::size_t max_size_;
    bool started_ = false;
    ClipboardTransferHeader header_;
    std::string buffer_;
    MarshalledClipboardPtr result_;
};

} // namespace inputleap
