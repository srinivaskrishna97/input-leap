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

#include "inputleap/clipboard_types.h"
#include "inputleap/ClipboardData.h"
#include "inputleap/ClipboardTransfer.h"
#include "inputleap/key_types.h"
#include "inputleap/Fwd.h"
#include "base/Fwd.h"
#include "base/Event.h"
#include "base/EventTarget.h"

#include <limits>
#include <memory>
#include <string>

namespace inputleap {

class Client;
class ClientInfo;
class ClipboardStreamer;
class IStream;

//! Proxy for server
/*!
This class acts a proxy for the server, converting calls into messages
to the server and messages from the server to calls on the client.
*/
class ServerProxy : public EventTarget {
public:
    /*!
    Process messages from the server on \p stream and forward to
    \p client.
    */
    ServerProxy(Client* client, inputleap::IStream* stream, IEventQueue* events);
    ~ServerProxy();

    //! @name manipulators
    //@{

    void onInfoChanged();
    bool onGrabClipboard(ClipboardID);
    void onClipboardChanged(ClipboardID, const IClipboard*);

    //! Send clipboard contents to the server
    /*!
    The data is sent in the background without delaying input messages.
    */
    void onClipboardChanged(ClipboardID, const MarshalledClipboardPtr& data);

    //! Offer clipboard contents to the server without sending them
    /*!
    Only valid if the server supports kClipboardCapOffers. \p flags may contain
    kClipboardOfferDataFollows if the data is sent right after with onClipboardChanged().
    */
    void onClipboardOffered(ClipboardID, const ClipboardOffer& offer, std::uint32_t flags);

    //! Tell the server how old the contents of a clipboard are
    /*!
    Only valid if the server supports kClipboardCapAge, see kMsgDClipboardAge.
    */
    void onClipboardAge(ClipboardID, std::uint32_t age_ms);

    //! Ask the server for offered clipboard contents
    void requestClipboardData(ClipboardID, const ClipboardOffer& offer);

    //! Send clipboard contents that the server has requested
    void provideClipboardData(ClipboardID, const MarshalledClipboardPtr& data);

    //! Tell the server that requested clipboard contents are not available
    void provideClipboardUnavailable(ClipboardID, const ClipboardOffer& offer);

    //@}

    //! @name accessors
    //@{

    //! The optional clipboard features negotiated with the server (kClipboardCap*)
    std::uint32_t clipboardCapabilities() const { return m_clipboardCaps; }

    //@}

    // sending file chunk to server
    void file_chunk_sending(const FileChunk& chunk);

    // sending dragging information to server
    void sendDragInfo(std::uint32_t fileCount, const char* info, size_t size);

#ifdef INPUTLEAP_TEST_ENV
    void handleDataForTest() { handleData(Event(), nullptr); }
#endif

protected:
    enum EResult { kOkay, kUnknown, kDisconnect };
    EResult parseHandshakeMessage(const std::uint8_t* code);
    EResult parseMessage(const std::uint8_t* code);

private:
    // if compressing mouse motion then send the last motion now
    void flushCompressedMouse();

    void sendInfo(const ClientInfo&);

    void resetKeepAliveAlarm();
    void setKeepAliveRate(double);

    // modifier key translation
    KeyID translateKey(KeyID) const;
    KeyModifierMask translateModifierMask(KeyModifierMask) const;

    // event handlers
    void handle_data();
    void handle_keep_alive_alarm();

    // message handlers
    void enter();
    void leave();
    void setClipboard();
    void grabClipboard();
    void keyDown();
    void keyRepeat();
    void keyUp();
    void mouseDown();
    void mouseUp();
    void mouseMove();
    void mouseRelativeMove();
    void mouseWheel();
    void screensaver();
    void resetOptions();
    void setOptions();
    void queryInfo();
    void infoAcknowledgment();
    void fileChunkReceived();
    void dragInfoReceived();
    void clipboardOffer();
    void clipboardDataRequest();
    void clipboardData();

    void sendClipboardTransfer(ClipboardID, const ClipboardTransferPayload& payload);

private:
    typedef EResult (ServerProxy::*MessageParser)(const std::uint8_t*);

    Client* m_client;
    inputleap::IStream* m_stream;

    std::uint32_t m_seqNum;

    bool m_compressMouse;
    bool m_compressMouseRelative;
    std::int32_t m_xMouse, m_yMouse;
    std::int32_t m_dxMouse, m_dyMouse;

    bool m_ignoreMouse;

    KeyModifierID m_modifierTranslationTable[kKeyModifierIDLast];

    double m_keepAliveAlarm;
    EventQueueTimer* m_keepAliveAlarmTimer;

    MessageParser m_parser;
    IEventQueue* m_events;

    // sends clipboard contents without delaying other messages
    std::unique_ptr<ClipboardStreamer> m_clipboardStreamer;

    // clipboard data being received from the server
    std::string m_clipboardRecvData;
    std::size_t m_clipboardRecvExpectedSize = 0;

    // optional clipboard protocol features negotiated with the server (kClipboardCap*)
    std::uint32_t m_clipboardCaps = 0;

    // clipboard data being received from the server with kMsgDClipboardData
    ClipboardTransferReceiver m_clipboardReceiver{(std::numeric_limits<std::size_t>::max)()};
};

} // namespace inputleap
