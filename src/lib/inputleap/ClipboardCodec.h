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

#include <cstddef>
#include <string>

namespace inputleap {

/*! Run-length encoding of 4-byte words, used to compress clipboard contents for transfer.

Clipboard images are transferred as uncompressed bitmaps with 4 bytes per pixel. Screenshots
contain long runs of identical pixels (window backgrounds, toolbars, empty areas of documents),
which this simple encoding compresses well while being fast and easy to validate. A repeated
pattern of 4 bytes forms runs at any alignment, so the encoding also works for the bitmap that
follows the headers in the marshalled clipboard data.

The encoded data is a sequence of packets. A header byte h < 128 is followed by h + 1 words
that are copied as is. A header byte h >= 128 is followed by one word that is repeated h - 125
times (3 to 130 times). The final size % 4 bytes of the data are stored as is at the end.
*/
std::string rle32_encode(const std::string& data);

/*! Decodes data produced by rle32_encode(). \p decoded_size is the size of the original data.
    Returns false if \p encoded is malformed or does not decode to exactly \p decoded_size bytes.
    The caller must limit \p decoded_size to a sensible value since it is allocated up front.
*/
bool rle32_decode(const std::string& encoded, std::size_t decoded_size, std::string& decoded);

/// The largest size that rle32_encode() can produce for data of \p size bytes.
std::size_t rle32_max_encoded_size(std::size_t size);

} // namespace inputleap
