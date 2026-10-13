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

#include "server/ClientProxy.h"
#include "base/Fwd.h"
#include "inputleap/Clipboard.h"
#include "inputleap/ClipboardData.h"
#include "inputleap/ClipboardTransfer.h"
#include "inputleap/protocol_types.h"

#include <limits>
#include <memory>

namespace inputleap {

class Server;
class IStream;
class ClipboardStreamer;

//! Proxy for client implementing protocol version 1.0
class ClientProxy1_6 : public ClientProxy {
public:
    ClientProxy1_6(const std::string& name, std::unique_ptr<IClientConnection> backend,
                   Server* server, IEventQueue* events);
    ~ClientProxy1_6() override;

    Server* getServer() { return m_server; }

    IStream* getStream() const;

    // IScreen
    bool getClipboard(ClipboardID id, IClipboard*) const override;
    void getShape(std::int32_t& x, std::int32_t& y, std::int32_t& width,
                  std::int32_t& height) const override;
    void getCursorPos(std::int32_t& x, std::int32_t& y) const override;
    const MonitorLayout& getMonitors() const override;

    // IClient overrides
    void enter(std::int32_t xAbs, std::int32_t yAbs, std::uint32_t seqNum, KeyModifierMask mask,
               bool forScreensaver) override;
    bool leave() override;
    void setClipboard(ClipboardID, const IClipboard*) override;
    void grabClipboard(ClipboardID) override;
    void setClipboardDirty(ClipboardID, bool) override;
    void keyDown(KeyID, KeyModifierMask, KeyButton) override;
    void keyRepeat(KeyID, KeyModifierMask, std::int32_t count, KeyButton) override;
    void keyUp(KeyID, KeyModifierMask, KeyButton) override;
    void mouseDown(ButtonID) override;
    void mouseUp(ButtonID) override;
    void mouseMove(std::int32_t xAbs, std::int32_t yAbs) override;
    void mouseRelativeMove(std::int32_t xRel, std::int32_t yRel) override;
    void mouseWheel(std::int32_t xDelta, std::int32_t yDelta) override;
    void screensaver(bool activate) override;
    void resetOptions() override;
    void setOptions(const OptionsList& options) override;
    void sendDragInfo(std::uint32_t fileCount, const char* info, size_t size) override;
    void file_chunk_sending(const FileChunk& chunk) override;

    // BaseClientProxy overrides
    MarshalledClipboardPtr getClipboardData(ClipboardID id) const override;
    void setClipboardData(ClipboardID id, const MarshalledClipboardPtr& data) override;
    bool supportsClipboardOffers() const override;
    void setClipboardOffer(ClipboardID id, const ClipboardOffer& offer) override;
    void requestClipboardData(ClipboardID id, const ClipboardOffer& offer) override;
    void provideClipboardData(ClipboardID id, const MarshalledClipboardPtr& data) override;
    void provideClipboardUnavailable(ClipboardID id, const ClipboardOffer& offer) override;

protected:
    virtual bool parseHandshakeMessage(const std::uint8_t* code);
    virtual bool parseMessage(const std::uint8_t* code);

    virtual void resetHeartbeatRate();
    virtual void setHeartbeatRate(double rate, double alarm);
    virtual void resetHeartbeatTimer();
    virtual void addHeartbeatTimer();
    virtual void removeHeartbeatTimer();
    virtual bool recvClipboard();
    virtual void keepAlive();

    void fileChunkReceived();
    void dragInfoReceived();

private:
    void disconnect();
    void remove_handlers();

    void handle_data();
    void handle_disconnect();
    void handle_write_error();
    void handle_flatline();

    bool recvInfo();
    bool recvGrabClipboard();
    bool recvClipboardCapabilities();
    bool recvClipboardOffer();
    bool recvClipboardDataRequest();
    bool recvClipboardData();
    bool recvClipboardAge();
    bool recvMonitors();

    void sendClipboardOffer(ClipboardID id, const ClipboardOffer& offer, std::uint32_t flags);
    void sendClipboardTransfer(ClipboardID id, const ClipboardTransferPayload& payload);
    void pushClipboardData(ClipboardID id, const MarshalledClipboardPtr& data);
    bool wasClipboardDataPushed(const MarshalledClipboard& data) const;

protected:
    struct ClientClipboard {
    public:
        ClientClipboard();

    public:
        MarshalledClipboardPtr m_data;
        std::uint32_t m_sequenceNumber;
        bool m_dirty;
    };

    ClientClipboard m_clipboard[kClipboardEnd];

    // sends clipboard contents without delaying input messages
    std::unique_ptr<ClipboardStreamer> m_clipboardStreamer;

    // clipboard data being received from the client
    std::string m_clipboardRecvData;
    std::size_t m_clipboardRecvExpectedSize = 0;

    // optional clipboard protocol features supported by the client (kClipboardCap*)
    std::uint32_t m_clipboardCaps = 0;

    // clipboard data being received from the client with kMsgDClipboardData.  the server
    // enforces its configured size limit once the contents have arrived.
    ClipboardTransferReceiver m_clipboardReceiver{(std::numeric_limits<std::size_t>::max)()};

    // the last contents offered to the client for each clipboard
    bool m_clipboardHasOffer[kClipboardEnd] = {};
    ClipboardOffer m_clipboardOffered[kClipboardEnd];

    // the hash of the last contents whose data was sent to the client.  the client has a
    // single system clipboard for both clipboards on most platforms, so the same data does not
    // need to be sent twice.
    bool m_clipboardPushed = false;
    std::uint64_t m_clipboardPushedHash = 0;

protected:
    typedef bool (ClientProxy1_6::*MessageParser)(const std::uint8_t*);

    ClientInfo m_info;
    // the monitors the client reported, see kMsgDMonitors
    MonitorLayout m_monitors;
    double m_heartbeatAlarm;
    EventQueueTimer* m_heartbeatTimer;
    MessageParser m_parser;
    IEventQueue* m_events;

    double m_keepAliveRate;
    EventQueueTimer* m_keepAliveTimer;
    Server* m_server;
};

} // namespace inputleap
