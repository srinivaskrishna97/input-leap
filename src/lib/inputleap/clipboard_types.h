/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdint>

//! Clipboard ID
/*!
Type to hold a clipboard identifier.
*/
typedef std::uint8_t ClipboardID;

//! @name Clipboard identifiers
//@{
// clipboard identifiers.  kClipboardClipboard is what is normally
// considered the clipboard (e.g. the cut/copy/paste menu items
// affect it).  kClipboardSelection is the selection on those
// platforms that can treat the selection as a clipboard (e.g. X
// windows).  clipboard identifiers must be sequential starting
// at zero.
static const ClipboardID    kClipboardClipboard = 0;
static const ClipboardID    kClipboardSelection = 1;

// the number of clipboards (i.e. one greater than the last clipboard id)
static const ClipboardID    kClipboardEnd       = 2;
//@}

//! @name Clipboard transfer capabilities
//@{
// Optional features of the clipboard protocol. The server announces the features that it
// supports with the kOptionClipboardCapabilities option and a client that supports any of them
// replies with kMsgCClipboardCapabilities. Only features supported by both sides are used, so
// older clients and servers keep using kMsgDClipboard.

// The contents of a clipboard are announced with kMsgDClipboardOffer and sent with
// kMsgDClipboardData only when they are requested with kMsgQClipboardData, e.g. when the user
// pastes. Contents of at most kClipboardEagerSize bytes are sent right after the offer.
static const std::uint32_t kClipboardCapOffers      = 1u << 0;

// kMsgDClipboardData may carry compressed contents.
static const std::uint32_t kClipboardCapCompression = 1u << 1;

// all capabilities implemented by this version
// The peer accepts the IClipboard::kFiles format.
static const std::uint32_t kClipboardCapFiles       = 1u << 2;

// The client tells how old its clipboard contents are with kMsgDClipboardAge when it connects,
// and the server keeps the younger of the two clipboards instead of replacing the client's.
static const std::uint32_t kClipboardCapAge         = 1u << 3;

static const std::uint32_t kClipboardCapsSupported  = kClipboardCapOffers |
                                                      kClipboardCapCompression |
                                                      kClipboardCapFiles |
                                                      kClipboardCapAge;

// kMsgDClipboardAge age of contents whose age is not known
static const std::uint32_t kClipboardAgeUnknown     = 0xffffffffu;

//! How old the contents of a client's clipboard are, see kMsgDClipboardAge
struct ClipboardAgeInfo {
    ClipboardID id = 0;
    std::uint32_t age_ms = kClipboardAgeUnknown;
};

//! The state of clipboard contents arriving from another screen, for showing their progress
struct ClipboardTransferProgress {
    enum class State {
        Receiving,
        Completed,
        Failed,
    };
    State state = State::Receiving;
    std::uint64_t received = 0;   //!< bytes received so far
    std::uint64_t total = 0;      //!< bytes of the whole transfer
    std::uint32_t formats = 0;    //!< a bit per IClipboard::EFormat, if known
};

//! The progress of a transfer together with the hash that identifies its contents
struct ClipboardTransferProgressInfo {
    std::uint64_t hash = 0;
    ClipboardTransferProgress progress;
};

// clipboard contents up to this size are sent along with the offer
static const std::uint32_t kClipboardEagerSize      = 1024 * 1024;

// kMsgDClipboardOffer flag: the contents follow in kMsgDClipboardData messages
static const std::uint32_t kClipboardOfferDataFollows = 1u << 0;
//@}
