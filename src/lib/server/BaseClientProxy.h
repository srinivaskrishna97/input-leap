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

#include "base/EventTarget.h"
#include "inputleap/MonitorLayout.h"
#include "inputleap/Fwd.h"
#include "inputleap/IClient.h"
#include "inputleap/ClipboardData.h"

namespace inputleap {

class IClientConnection;
class IStream;

//! Generic proxy for client or primary
class BaseClientProxy : public IClient, public EventTarget {
public:
    /*!
    \c name is the name of the client.
    */
    BaseClientProxy(const std::string& name);
    ~BaseClientProxy();

    //! @name manipulators
    //@{

    //! Save cursor position
    /*!
    Save the position of the cursor when jumping from client.
    */
    void setJumpCursorPos(std::int32_t x, std::int32_t y);

    //@}
    //! @name accessors
    //@{

    //! Get cursor position
    /*!
    Get the position of the cursor when last jumping from client.
    */
    void getJumpCursorPos(std::int32_t& x, std::int32_t& y) const;

    //! Get cursor position
    /*!
    Return if this proxy is for client or primary.
    */
    virtual bool isPrimary() const { return false; }

    //! Get the monitors of the screen
    /*!
    Returns the monitors that the screen reported, or an empty layout if it did not report
    them, in which case the whole shape counts as one monitor.
    */
    virtual const MonitorLayout& getMonitors() const
    {
        static const MonitorLayout none;
        return none;
    }

    //! Get clipboard contents in the marshalled form
    /*!
    Returns nullptr if the contents are not available yet. In that case a
    \c CLIPBOARD_CHANGED event is sent once they are. The default implementation reads the
    clipboard using getClipboard().
    */
    virtual MarshalledClipboardPtr getClipboardData(ClipboardID id) const;

    //@}

    //! @name manipulators
    //@{

    //! Set clipboard contents in the marshalled form
    /*!
    Like setClipboard() but avoids copying the data. The default implementation converts the
    data and calls setClipboard().
    */
    virtual void setClipboardData(ClipboardID id, const MarshalledClipboardPtr& data);

    //! Test if the screen accepts clipboard offers
    /*!
    If true, the screen is sent offers of clipboard contents (see setClipboardOffer()) and asks
    for the data with \c CLIPBOARD_DATA_REQUESTED events when it needs it.
    */
    virtual bool supportsClipboardOffers() const { return false; }

    //! Offer clipboard contents to the screen
    /*!
    Announces the contents without their data. Like setClipboardData() this does nothing if
    the screen's clipboard is not dirty. Only called if supportsClipboardOffers() returns true.
    */
    virtual void setClipboardOffer(ClipboardID id, const ClipboardOffer& offer) { }

    //! Request clipboard contents from the screen
    /*!
    Asks the screen, which owns the clipboard and has offered its contents, to send them. The
    contents arrive with a \c CLIPBOARD_CHANGED event, or a \c CLIPBOARD_DATA_UNAVAILABLE event is
    sent if the screen cannot provide them.
    */
    virtual void requestClipboardData(ClipboardID id, const ClipboardOffer& offer) { }

    //! Send requested clipboard contents to the screen
    /*!
    Unlike setClipboardData() this sends the data even if the clipboard is not dirty.
    */
    virtual void provideClipboardData(ClipboardID id, const MarshalledClipboardPtr& data);

    //! Tell the screen that requested clipboard contents are not available
    virtual void provideClipboardUnavailable(ClipboardID id, const ClipboardOffer& offer) { }

    //@}

    // IClient overrides
    virtual void sendDragInfo(std::uint32_t fileCount, const char* info, size_t size) = 0;
    virtual void file_chunk_sending(const FileChunk& chunk) = 0;
    std::string getName() const override;
    virtual IClientConnection& get_conn() const = 0;

private:
    std::string m_name;
    std::int32_t m_x, m_y;
};

} // namespace inputleap
