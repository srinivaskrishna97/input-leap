# InputLeap -- mouse and keyboard sharing utility
#
# This package is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# found in the file LICENSE that should have accompanied this file.
#
# This package is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

# The clipboard helper runs as the user next to a server or client that the Windows service
# started with high privileges, and captures the clipboard contents that Windows withholds from
# such a process.
set(sources
    input-leap-clipboard.cpp
    input-leap-clipboard.exe.manifest
)

add_executable(input-leap-clipboard WIN32 ${sources})
target_link_libraries(input-leap-clipboard
    arch base client common io mt net ipc platform server synlib ${libs} OpenSSL::SSL OpenSSL::Crypto)

install(TARGETS input-leap-clipboard DESTINATION .)
