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

#include "platform/MSWindowsClipboard.h"

#include "platform/MSWindowsClipboardTextConverter.h"
#include "platform/MSWindowsClipboardUTF16Converter.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"
#include "platform/MSWindowsClipboardHTMLConverter.h"
#include "platform/MSWindowsClipboardFileConverter.h"
#include "platform/MSWindowsClipboardFacade.h"
#include "arch/win32/ArchMiscWindows.h"
#include "base/Log.h"

#include <atomic>

namespace inputleap {

UINT                    MSWindowsClipboard::s_ownershipFormat = 0;

MSWindowsClipboard::MSWindowsClipboard(HWND window) :
    m_window(window),
    m_time(0),
    m_facade(new MSWindowsClipboardFacade()),
    m_deleteFacade(true)
{
    // add converters, most desired first
    m_converters.push_back(new MSWindowsClipboardUTF16Converter);
    m_converters.push_back(new MSWindowsClipboardBitmapConverter);
    m_converters.push_back(new MSWindowsClipboardHTMLConverter);
    m_converters.push_back(new MSWindowsClipboardFileConverter);
}

MSWindowsClipboard::~MSWindowsClipboard()
{
    clearConverters();

    // dependency injection causes confusion over ownership, so we need
    // logic to decide whether or not we delete the facade. there must
    // be a more elegant way of doing this.
    if (m_deleteFacade)
        delete m_facade;
}

void
MSWindowsClipboard::setFacade(IMSWindowsClipboardFacade& facade)
{
    delete m_facade;
    m_facade = &facade;
    m_deleteFacade = false;
}

bool
MSWindowsClipboard::emptyUnowned()
{
    LOG_DEBUG("empty clipboard");

    // empty the clipboard (and take ownership)
    if (!EmptyClipboard()) {
        // unable to cause this in integ tests, but this error has never
        // actually been reported by users.
        LOG_DEBUG("failed to grab clipboard");
        return false;
    }

    return true;
}

bool
MSWindowsClipboard::clear()
{
    if (!emptyUnowned()) {
        return false;
    }

    // mark clipboard as being owned by InputLeap
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 1);
    UINT format = getOwnershipFormat();
    if (nullptr == SetClipboardData(format, data)) {
        DWORD error = GetLastError();
        LOG_DEBUG("failed to mark the clipboard as InputLeap's, format %u (error %lu)", format,
                  static_cast<unsigned long>(error));
        GlobalFree(data);
        return false;
    }

    return true;
}

void
MSWindowsClipboard::add(EFormat format, const std::string& data)
{
    LOG_DEBUG("add %d bytes to clipboard format: %d", data.size(), format);

    // convert data to win32 form
    for (auto index = m_converters.begin(); index != m_converters.end(); ++index) {
        IMSWindowsClipboardConverter* converter = *index;

        // skip converters for other formats
        if (converter->getFormat() == format) {
            HANDLE win32Data = converter->fromIClipboard(data);
            if (win32Data != nullptr) {
                UINT win32Format = converter->getWin32Format();
                m_facade->write(win32Data, win32Format);
                if (format == kFiles) {
                    addPreferredDropEffect();
                }
            }
        }
    }
}

bool
MSWindowsClipboard::open(Time time) const
{
    LOG_DEBUG("open clipboard");

    if (!OpenClipboard(m_window)) {
        // unable to cause this in integ tests; but this can happen!
        // * http://symless.com/pm/issues/86
        // * http://symless.com/pm/issues/1256
        // logging improved to see if we can catch more info next time.
        LOG_WARN("failed to open clipboard: %d", GetLastError());
        return false;
    }

    m_time = time;

    return true;
}

void
MSWindowsClipboard::close() const
{
    LOG_DEBUG("close clipboard");
    CloseClipboard();
}

IClipboard::Time
MSWindowsClipboard::getTime() const
{
    return m_time;
}

bool
MSWindowsClipboard::has(EFormat format) const
{
    for (auto index = m_converters.begin(); index != m_converters.end(); ++index) {
        IMSWindowsClipboardConverter* converter = *index;
        if (converter->getFormat() == format) {
            if (IsClipboardFormatAvailable(converter->getWin32Format())) {
                return true;
            }
        }
    }
    return false;
}

std::string MSWindowsClipboard::get(EFormat format) const
{
    // find the converter for the first clipboard format we can handle
    IMSWindowsClipboardConverter* converter = nullptr;
    for (auto index = m_converters.begin(); index != m_converters.end(); ++index) {

        converter = *index;
        if (converter->getFormat() == format) {
            break;
        }
        converter = nullptr;
    }

    // if no converter then we don't recognize any formats
    if (converter == nullptr) {
        LOG_WARN("no converter for format %d", format);
        return {};
    }

    // get a handle to the clipboard data
    HANDLE win32Data = GetClipboardData(converter->getWin32Format());
    if (win32Data == nullptr) {
        // nb: can't cause this using integ tests; this is only caused when
        // the selected converter returns an invalid format -- which you
        // cannot cause using public functions.
        return {};
    }

    // convert
    return converter->toIClipboard(win32Data);
}

UINT MSWindowsClipboard::getWin32Format(EFormat format) const
{
    for (auto* converter : m_converters) {
        if (converter->getFormat() == format) {
            return converter->getWin32Format();
        }
    }
    return 0;
}

bool MSWindowsClipboard::getFormatForWin32(UINT win32Format, EFormat& format) const
{
    for (auto* converter : m_converters) {
        if (converter->getWin32Format() == win32Format) {
            format = converter->getFormat();
            return true;
        }
    }
    return false;
}

void MSWindowsClipboard::addDelayed(EFormat format)
{
    UINT win32Format = getWin32Format(format);
    if (win32Format != 0) {
        LOG_DEBUG("announce clipboard format: %d", format);
        SetClipboardData(win32Format, nullptr);
    }
}

UINT MSWindowsClipboard::preferredDropEffectFormat()
{
    static const UINT format = RegisterClipboardFormatW(L"Preferred DropEffect");
    return format;
}

void MSWindowsClipboard::addPreferredDropEffect()
{
    // tell Explorer to copy the files when they are pasted rather than to move them
    const DWORD kDropEffectCopy = 1;
    UINT format = preferredDropEffectFormat();
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, sizeof(DWORD));
    if (format == 0 || global == nullptr) {
        return;
    }
    DWORD* effect = static_cast<DWORD*>(GlobalLock(global));
    if (effect == nullptr) {
        GlobalFree(global);
        return;
    }
    *effect = kDropEffectCopy;
    GlobalUnlock(global);
    m_facade->write(global, format);
}

void
MSWindowsClipboard::clearConverters()
{
    for (auto index = m_converters.begin(); index != m_converters.end(); ++index) {
        delete *index;
    }
    m_converters.clear();
}

bool MSWindowsClipboard::is_owned_by_us()
{
    UINT format = getOwnershipFormat();
    return format != 0 && IsClipboardFormatAvailable(format) != 0;
}

UINT
MSWindowsClipboard::getOwnershipFormat()
{
    // create ownership format if we haven't yet
    if (s_ownershipFormat == 0) {
        UINT format = RegisterClipboardFormat(TEXT("InputLeapOwnership"));
        if (format == 0) {
            DWORD error = GetLastError();
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                LOG_WARN("cannot register the InputLeapOwnership clipboard format (error %lu)",
                         static_cast<unsigned long>(error));
            }
            return 0;
        }
        LOG_DEBUG("the InputLeapOwnership clipboard format is %u", format);
        s_ownershipFormat = format;
    }

    // return the format
    return s_ownershipFormat;
}

} // namespace inputleap
