/*
 * InputLeap -- mouse and keyboard sharing utility
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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace inputleap {

/*! Messages between the core and its clipboard helper on Windows.

    On Windows, a server or client that the service started runs with high privileges, and
    Windows withholds clipboard contents that applications hand over on request from such a
    process. The core therefore starts a helper program as the user, which captures the
    clipboard and sends the contents to the core. The messages are sent with WM_COPYDATA; the
    identifiers below go in dwData and the payloads encoded here in lpData.
*/
//@{

/// helper -> core: the helper has started. Payload: its window and process, see encode_clipboard_helper_hello().
constexpr std::uint32_t kClipboardHelperHelloMagic = 0x494C4348;   // "ILCH"
/// core -> helper: capture the clipboard now. No payload.
constexpr std::uint32_t kClipboardHelperRequestMagic = 0x494C4352; // "ILCR"
/// helper -> core: the captured contents. Payload: see encode_clipboard_capture().
constexpr std::uint32_t kClipboardHelperCaptureMagic = 0x494C4343; // "ILCC"
/// helper -> core, in answer to a request: the clipboard holds InputLeap's own contents, so there
/// is nothing to capture. Payload: the clipboard sequence number, see encode_clipboard_sequence().
constexpr std::uint32_t kClipboardHelperOwnedMagic = 0x494C434F;   // "ILCO"
/// helper -> core, before a capture that lists files: where the files are on this computer.
/// Payload: see encode_clipboard_file_paths().
constexpr std::uint32_t kClipboardHelperPathsMagic = 0x494C4350;   // "ILCP"

/// The clipboard sequence number (4 bytes, little endian) followed by the marshalled contents.
std::string encode_clipboard_capture(std::uint32_t sequence, const std::string& data);
bool decode_clipboard_capture(const void* bytes, std::size_t size, std::uint32_t& sequence,
                              std::string& data);

/*! The clipboard sequence number (4 bytes, little endian) and, per entry of the file list in
    order, a 4-byte length and the UTF-8 path of the file on this computer; directories have an
    empty path. */
std::string encode_clipboard_file_paths(std::uint32_t sequence,
                                        const std::vector<std::string>& paths);
bool decode_clipboard_file_paths(const void* bytes, std::size_t size, std::uint32_t& sequence,
                                 std::vector<std::string>& paths);

/// A clipboard sequence number: 4 bytes, little endian.
std::string encode_clipboard_sequence(std::uint32_t sequence);
bool decode_clipboard_sequence(const void* bytes, std::size_t size, std::uint32_t& sequence);

/// The helper's window handle (8 bytes) and process id (4 bytes), little endian.
std::string encode_clipboard_helper_hello(std::uint64_t window, std::uint32_t pid);
bool decode_clipboard_helper_hello(const void* bytes, std::size_t size, std::uint64_t& window,
                                   std::uint32_t& pid);

//@}

} // namespace inputleap
