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

#include "server/ClientProxy1_6.h"
#include "inputleap/IClipboard.h"
#include "ClientConnectionByStream.h"

#include "inputleap/ProtocolUtil.h"
#include "inputleap/ClipboardChunk.h"
#include "inputleap/ClipboardStreamer.h"
#include "inputleap/Exceptions.h"
#include "inputleap/FileChunk.h"
#include "inputleap/StreamChunker.h"
#include "server/Server.h"
#include "io/IStream.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/EventQueueTimer.h"

#include <cstring>

namespace inputleap {

ClientProxy1_6::ClientProxy1_6(const std::string& name,
                               std::unique_ptr<IClientConnection> backend,
                               Server* server, IEventQueue* events) :
    ClientProxy(name, std::move(backend)),
    m_heartbeatTimer(nullptr),
    m_parser(&ClientProxy1_6::parseHandshakeMessage),
    m_events(events),
    m_keepAliveRate(kKeepAliveRate),
    m_keepAliveTimer(nullptr),
    m_server{server}
{
    // install event handlers
    m_events->add_handler(EventType::STREAM_INPUT_READY, get_conn().get_event_target(),
                          [this](const auto& e){ handle_data(); });
    m_events->add_handler(EventType::STREAM_OUTPUT_ERROR, get_conn().get_event_target(),
                          [this](const auto& e){ handle_write_error(); });
    m_events->add_handler(EventType::STREAM_INPUT_SHUTDOWN, get_conn().get_event_target(),
                          [this](const auto& e){ handle_disconnect(); });
    m_events->add_handler(EventType::STREAM_INPUT_FORMAT_ERROR, get_conn().get_event_target(),
                          [this](const auto& e){ handle_disconnect(); });
    m_events->add_handler(EventType::STREAM_OUTPUT_SHUTDOWN, get_conn().get_event_target(),
                          [this](const auto& e){ handle_write_error(); });
    m_events->add_handler(EventType::FILE_KEEPALIVE, this,
                          [this](const auto& e){ keepAlive(); });
    m_events->add_handler(EventType::TIMER, this,
                          [this](const auto& e){ handle_flatline(); });

    m_clipboardStreamer = std::make_unique<ClipboardStreamer>(
                m_events, get_conn().get_event_target(),
                [this](const ClipboardChunk& chunk, bool encoded) {
                    if (encoded) {
                        get_conn().send_clipboard_data_chunk(chunk);
                    } else {
                        get_conn().send_clipboard_chunk_1_6(chunk);
                    }
                });

    // pieces of shared files from the client are passed on as they arrive
    m_clipboardReceiver.set_file_sink(
        [this](const ClipboardTransferHeader& header, std::uint64_t offset,
               const std::string& chunk) {
            ClipboardFileChunkInfo info;
            info.hash = header.hash;
            info.index = header.file_index;
            info.offset = offset;
            info.data = chunk;
            m_events->add_event(EventType::CLIPBOARD_FILE_CHUNK, get_event_target(),
                                create_event_data<ClipboardFileChunkInfo>(info));
            return true;
        });

    setHeartbeatRate(kHeartRate, kHeartRate * kHeartBeatsUntilDeath);

    get_conn().send_query_info_1_6();

    setHeartbeatRate(kKeepAliveRate, kKeepAliveRate * kKeepAlivesUntilDeath);
}

ClientProxy1_6::~ClientProxy1_6()
{
    remove_handlers();
}

IStream* ClientProxy1_6::getStream() const
{
    return get_conn().get_stream();
}

void ClientProxy1_6::disconnect()
{
    remove_handlers();
    get_conn().close();
    m_events->add_event(EventType::CLIENT_PROXY_DISCONNECTED, get_event_target());
}

void ClientProxy1_6::remove_handlers()
{
    // uninstall event handlers
    m_events->remove_handler(EventType::STREAM_INPUT_READY, get_conn().get_event_target());
    m_events->remove_handler(EventType::STREAM_OUTPUT_ERROR, get_conn().get_event_target());
    m_events->remove_handler(EventType::STREAM_INPUT_SHUTDOWN, get_conn().get_event_target());
    m_events->remove_handler(EventType::STREAM_OUTPUT_SHUTDOWN, get_conn().get_event_target());
    m_events->remove_handler(EventType::STREAM_INPUT_FORMAT_ERROR, get_conn().get_event_target());
    m_events->remove_handler(EventType::FILE_KEEPALIVE, this);
    m_events->remove_handler(EventType::TIMER, this);

    // stop sending clipboard data
    if (m_clipboardStreamer) {
        m_clipboardStreamer->cancel();
    }

    // remove timer
    removeHeartbeatTimer();
}

void ClientProxy1_6::addHeartbeatTimer()
{
    if (m_keepAliveRate > 0.0) {
        m_keepAliveTimer = m_events->newTimer(m_keepAliveRate, nullptr);
        m_events->add_handler(EventType::TIMER, m_keepAliveTimer,
                              [this](const auto& e){ keepAlive(); });
    }

    if (m_heartbeatAlarm > 0.0) {
        m_heartbeatTimer = m_events->newOneShotTimer(m_heartbeatAlarm, this);
    }
}

void ClientProxy1_6::removeHeartbeatTimer()
{
    if (m_keepAliveTimer != nullptr) {
        m_events->remove_handler(EventType::TIMER, m_keepAliveTimer);
        m_events->deleteTimer(m_keepAliveTimer);
        m_keepAliveTimer = nullptr;
    }

    if (m_heartbeatTimer != nullptr) {
        m_events->deleteTimer(m_heartbeatTimer);
        m_heartbeatTimer = nullptr;
    }
}

void ClientProxy1_6::resetHeartbeatTimer()
{
    // reset the alarm but not the keep alive timer
    if (m_heartbeatTimer != nullptr) {
        m_events->deleteTimer(m_heartbeatTimer);
        m_heartbeatTimer = nullptr;
    }

    if (m_heartbeatAlarm > 0.0) {
        m_heartbeatTimer = m_events->newOneShotTimer(m_heartbeatAlarm, this);
    }
}

void ClientProxy1_6::resetHeartbeatRate()
{
    setHeartbeatRate(kKeepAliveRate, kKeepAliveRate * kKeepAlivesUntilDeath);
}

void ClientProxy1_6::setHeartbeatRate(double rate, double alarm)
{
    m_keepAliveRate = rate;
    m_heartbeatAlarm = alarm;
}

void ClientProxy1_6::handle_data()
{
    // handle messages until there are no more.  first read message code.
    std::uint8_t code[4];
    std::uint32_t n = getStream()->read(code, 4);
    while (n != 0) {
        // verify we got an entire code
        if (n != 4) {
            LOG_ERR("incomplete message from \"%s\": %d bytes", getName().c_str(), n);
            disconnect();
            return;
        }

        // parse message
        try {
            LOG_DEBUG2("msg from \"%s\": %c%c%c%c", getName().c_str(), code[0], code[1], code[2], code[3]);
            if (!(this->*m_parser)(code)) {
                LOG_ERR("invalid message from client \"%s\": %c%c%c%c", getName().c_str(), code[0], code[1], code[2], code[3]);
                disconnect();
                return;
            }
        } catch (const XBadClient& e) {
            // TODO: disconnect handling is currently dispersed across both parseMessage() and
            // handleData() functions, we should collect that to a single place

            LOG_ERR("protocol error from client: %s", e.what());
            disconnect();
            return;
        }

        // next message
        n = getStream()->read(code, 4);
    }

    // restart heartbeat timer
    resetHeartbeatTimer();
}

bool ClientProxy1_6::parseHandshakeMessage(const std::uint8_t* code)
{
    if (memcmp(code, kMsgCNoop, 4) == 0) {
        // discard no-ops
        LOG_DEBUG2("no-op from %s", getName().c_str());
        return true;
    }
    else if (memcmp(code, kMsgDInfo, 4) == 0) {
        // future messages get parsed by parseMessage
        // NOTE: we're taking address of virtual function here,
        // not ClientProxy1_3 implementation of it.
        m_parser = &ClientProxy1_6::parseMessage;
        if (recvInfo()) {
            m_events->add_event(EventType::CLIENT_PROXY_READY, get_event_target());
            addHeartbeatTimer();
            return true;
        }
    }
    return false;
}

bool ClientProxy1_6::parseMessage(const std::uint8_t* code)
{
    if (memcmp(code, kMsgDFileTransfer, 4) == 0) {
        fileChunkReceived();
        return true;
    } else if (memcmp(code, kMsgDDragInfo, 4) == 0) {
        dragInfoReceived();
        return true;
    } else if (memcmp(code, kMsgCKeepAlive, 4) == 0) {
        // reset alarm
        resetHeartbeatTimer();
        return true;
    } else if (memcmp(code, kMsgDInfo, 4) == 0) {
        if (recvInfo()) {
            m_events->add_event(EventType::SCREEN_SHAPE_CHANGED, get_event_target());
            return true;
        }
        return false;
    }
    else if (memcmp(code, kMsgCNoop, 4) == 0) {
        // discard no-ops
        LOG_DEBUG2("no-op from %s", getName().c_str());
        return true;
    }
    else if (memcmp(code, kMsgCClipboard, 4) == 0) {
        return recvGrabClipboard();
    }
    else if (memcmp(code, kMsgDClipboard, 4) == 0) {
        return recvClipboard();
    }
    else if (memcmp(code, kMsgCClipboardCapabilities, 4) == 0) {
        return recvClipboardCapabilities();
    }
    else if (memcmp(code, kMsgDClipboardOffer, 4) == 0) {
        return recvClipboardOffer();
    }
    else if (memcmp(code, kMsgQClipboardData, 4) == 0) {
        return recvClipboardDataRequest();
    }
    else if (memcmp(code, kMsgDClipboardData, 4) == 0) {
        return recvClipboardData();
    }
    else if (memcmp(code, kMsgDClipboardAge, 4) == 0) {
        return recvClipboardAge();
    }
    else if (memcmp(code, kMsgDMonitors, 4) == 0) {
        return recvMonitors();
    }
    else if (memcmp(code, kMsgQClipboardFile, 4) == 0) {
        return recvClipboardFileRequest();
    }
    return false;
}

void ClientProxy1_6::handle_disconnect()
{
    LOG_NOTE("client \"%s\" has disconnected", getName().c_str());
    disconnect();
}

void ClientProxy1_6::handle_write_error()
{
    LOG_WARN("error writing to client \"%s\"", getName().c_str());
    disconnect();
}

void ClientProxy1_6::handle_flatline()
{
    // didn't get a heartbeat fast enough.  assume client is dead.
    LOG_NOTE("client \"%s\" is dead", getName().c_str());
    disconnect();
}

bool ClientProxy1_6::getClipboard(ClipboardID id, IClipboard* clipboard) const
{
    IClipboard::unmarshall(clipboard, getClipboardData(id)->data(), 0);
    return true;
}

MarshalledClipboardPtr ClientProxy1_6::getClipboardData(ClipboardID id) const
{
    const auto& data = m_clipboard[id].m_data;
    return data ? data : empty_marshalled_clipboard();
}

void ClientProxy1_6::getShape(std::int32_t& x, std::int32_t& y, std::int32_t& w,
                              std::int32_t& h) const
{
    x = m_info.m_x;
    y = m_info.m_y;
    w = m_info.m_w;
    h = m_info.m_h;
}

void ClientProxy1_6::getCursorPos(std::int32_t& x, std::int32_t& y) const
{
    // note -- this returns the cursor pos from when we last got client info
    x = m_info.m_mx;
    y = m_info.m_my;
}

void ClientProxy1_6::enter(std::int32_t xAbs, std::int32_t yAbs, std::uint32_t seqNum,
                           KeyModifierMask mask, bool)
{
    get_conn().send_enter_1_6(xAbs, yAbs, seqNum, mask);
}

bool ClientProxy1_6::leave()
{
    get_conn().send_leave_1_6();
    // we can never prevent the user from leaving
    return true;
}

void ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
    // ignore if this clipboard is already clean
    if (m_clipboard[id].m_dirty) {
        setClipboardData(id, marshall_clipboard(*clipboard));
    }
}

void ClientProxy1_6::setClipboardData(ClipboardID id, const MarshalledClipboardPtr& data)
{
    MarshalledClipboardPtr contents = data ? data : empty_marshalled_clipboard();

    // files are only sent to clients that can put them on their clipboard
    if (!supportsClipboardFiles()) {
        contents = without_clipboard_formats(contents, 1u << IClipboard::kFiles);
    }

    if (supportsClipboardOffers()) {
        if (m_clipboard[id].m_dirty) {
            // the client gets the offer and requests the data when it needs it.  small contents
            // are sent right away so that pasting them does not have to wait.
            m_clipboard[id].m_dirty = false;
            bool push = contents->size() <= kClipboardEagerSize &&
                        !wasClipboardDataPushed(*contents);
            sendClipboardOffer(id, ClipboardOffer::describe(*contents),
                               push ? kClipboardOfferDataFollows : 0);
            if (push) {
                pushClipboardData(id, contents);
            }
        } else if (m_clipboardHasOffer[id] && m_clipboardOffered[id].describes(*contents) &&
                   contents->size() <= kClipboardEagerSize &&
                   !wasClipboardDataPushed(*contents)) {
            // the contents were offered before their data was known
            pushClipboardData(id, contents);
        }
        return;
    }

    // ignore if this clipboard is already clean
    if (!m_clipboard[id].m_dirty) {
        return;
    }

    // this clipboard is now clean
    m_clipboard[id].m_dirty = false;
    m_clipboard[id].m_data = contents;

    LOG_DEBUG("sending clipboard %d to \"%s\", size=%zu", id, getName().c_str(),
              contents->size());

    // the data is sent in the background so that it does not delay input
    m_clipboardStreamer->send(id, 0, contents);
}

bool ClientProxy1_6::supportsClipboardOffers() const
{
    return (m_clipboardCaps & kClipboardCapOffers) != 0;
}

void ClientProxy1_6::setClipboardOffer(ClipboardID id, const ClipboardOffer& offer)
{
    // ignore if this clipboard is already clean
    if (!supportsClipboardOffers() || !m_clipboard[id].m_dirty) {
        return;
    }
    m_clipboard[id].m_dirty = false;
    sendClipboardOffer(id, offer, 0);
}

void ClientProxy1_6::requestClipboardData(ClipboardID id, const ClipboardOffer& offer)
{
    if (!supportsClipboardOffers()) {
        return;
    }
    LOG_DEBUG("requesting clipboard %d from \"%s\", size=%u", id, getName().c_str(), offer.size);
    get_conn().send_clipboard_data_request(id, offer.hash);
}

void ClientProxy1_6::provideClipboardData(ClipboardID id, const MarshalledClipboardPtr& data)
{
    if (!supportsClipboardOffers()) {
        BaseClientProxy::provideClipboardData(id, data);
        return;
    }
    pushClipboardData(id, data ? data : empty_marshalled_clipboard());
}

void ClientProxy1_6::provideClipboardUnavailable(ClipboardID id, const ClipboardOffer& offer)
{
    if (!supportsClipboardOffers()) {
        return;
    }
    LOG_DEBUG("clipboard %d requested by \"%s\" is not available", id, getName().c_str());
    sendClipboardTransfer(id, make_unavailable_clipboard_transfer(offer.hash));
}

void ClientProxy1_6::sendClipboardOffer(ClipboardID id, const ClipboardOffer& offer,
                                        std::uint32_t flags)
{
    // data of older contents of this clipboard is outdated now
    m_clipboardStreamer->cancel(id);
    m_clipboardHasOffer[id] = true;
    m_clipboardOffered[id] = offer;
    LOG_DEBUG("offering clipboard %d to \"%s\", size=%u", id, getName().c_str(), offer.size);
    get_conn().send_clipboard_offer(id, flags, offer);
}

void ClientProxy1_6::sendClipboardTransfer(ClipboardID id, const ClipboardTransferPayload& payload)
{
    // the data is sent in the background so that it does not delay input
    m_clipboardStreamer->send_encoded(id, 0, payload.header.format(), payload.bytes);
}

void ClientProxy1_6::pushClipboardData(ClipboardID id, const MarshalledClipboardPtr& data)
{
    bool compress = (m_clipboardCaps & kClipboardCapCompression) != 0;
    auto payload = make_clipboard_transfer(data, compress);
    LOG_DEBUG("sending clipboard %d to \"%s\", size=%zu, sent=%llu", id, getName().c_str(),
              data->size(), static_cast<unsigned long long>(payload.header.encoded_size));
    sendClipboardTransfer(id, payload);
    m_clipboardPushed = true;
    m_clipboardPushedHash = data->hash();
}

bool ClientProxy1_6::wasClipboardDataPushed(const MarshalledClipboard& data) const
{
    return m_clipboardPushed && m_clipboardPushedHash == data.hash();
}

void ClientProxy1_6::grabClipboard(ClipboardID id)
{
    get_conn().send_grab_clipboard(id);

    // this clipboard is now dirty
    m_clipboard[id].m_dirty = true;
}

void ClientProxy1_6::setClipboardDirty(ClipboardID id, bool dirty)
{
    m_clipboard[id].m_dirty = dirty;
}

void ClientProxy1_6::keyDown(KeyID key, KeyModifierMask mask, KeyButton button)
{
    get_conn().send_key_down_1_6(key, mask, button);
}

void ClientProxy1_6::keyRepeat(KeyID key, KeyModifierMask mask, std::int32_t count,
                               KeyButton button)
{
    get_conn().send_key_repeat_1_6(key, mask, count, button);
}

void ClientProxy1_6::keyUp(KeyID key, KeyModifierMask mask, KeyButton button)
{
    get_conn().send_key_up_1_6(key, mask, button);
}

void ClientProxy1_6::mouseDown(ButtonID button)
{
    get_conn().send_mouse_down_1_6(button);
}

void ClientProxy1_6::mouseUp(ButtonID button)
{
    get_conn().send_mouse_up_1_6(button);
}

void ClientProxy1_6::mouseMove(std::int32_t xAbs, std::int32_t yAbs)
{
    get_conn().send_mouse_move_1_6(xAbs, yAbs);
}

void ClientProxy1_6::mouseRelativeMove(std::int32_t xRel, std::int32_t yRel)
{
    get_conn().send_mouse_relative_move_1_6(xRel, yRel);
}

void ClientProxy1_6::mouseWheel(std::int32_t xDelta, std::int32_t yDelta)
{
    get_conn().send_mouse_wheel_1_6(xDelta, yDelta);
}

void ClientProxy1_6::sendDragInfo(std::uint32_t fileCount, const char* info, size_t size)
{
    std::string data(info, size);
    get_conn().send_drag_info_1_6(fileCount, data);
}

void ClientProxy1_6::file_chunk_sending(const FileChunk& chunk)
{
    get_conn().send_file_chunk_1_6(chunk);
}

void ClientProxy1_6::screensaver(bool on)
{
    get_conn().send_screensaver_1_6(on);
}

void ClientProxy1_6::resetOptions()
{
    get_conn().send_reset_options_1_6();
    // reset heart rate and death
    resetHeartbeatRate();
    removeHeartbeatTimer();
    addHeartbeatTimer();
}

void ClientProxy1_6::setOptions(const OptionsList& options)
{
    get_conn().send_set_options_1_6(options);

    // check options
    for (std::uint32_t i = 0, n = static_cast<std::uint32_t>(options.size()); i < n; i += 2) {
        if (options[i] == kOptionHeartbeat) {
            double rate = 1.0e-3 * static_cast<double>(options[i + 1]);
            if (rate <= 0.0) {
                rate = -1.0;
            }
            setHeartbeatRate(rate, rate * kHeartBeatsUntilDeath);
            removeHeartbeatTimer();
            addHeartbeatTimer();
        }
    }
}

bool ClientProxy1_6::recvInfo()
{
    // parse the message
    std::int16_t x, y, w, h, dummy1, mx, my;
    if (!ProtocolUtil::readf(getStream(), kMsgDInfo + 4,
                            &x, &y, &w, &h, &dummy1, &mx, &my)) {
        return false;
    }
    LOG_DEBUG("received client \"%s\" info shape=%d,%d %dx%d at %d,%d", getName().c_str(), x, y, w, h, mx, my);

    // validate
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (mx < x || mx >= x + w || my < y || my >= y + h) {
        mx = x + w / 2;
        my = y + h / 2;
    }

    // save
    m_info.m_x  = x;
    m_info.m_y  = y;
    m_info.m_w  = w;
    m_info.m_h  = h;
    m_info.m_mx = mx;
    m_info.m_my = my;

    // acknowledge receipt
    get_conn().send_info_ack_1_6();
    return true;
}

bool ClientProxy1_6::recvClipboard()
{
    // parse message
    ClipboardID id;
    std::uint32_t seq;

    int r = ClipboardChunk::assemble(getStream(), m_clipboardRecvData,
                                     m_clipboardRecvExpectedSize, id, seq);

    if (r == kStart) {
        LOG_DEBUG("receiving clipboard %d size=%zd", id, m_clipboardRecvExpectedSize);
    } else if (r == kFinish) {
        LOG_DEBUG("received client \"%s\" clipboard %d seqnum=%d, size=%zd",
                getName().c_str(), id, seq, m_clipboardRecvData.size());
        // save clipboard
        m_clipboard[id].m_data = make_marshalled_clipboard(std::move(m_clipboardRecvData));
        m_clipboardRecvData.clear();
        m_clipboard[id].m_sequenceNumber = seq;

        // notify
        ClipboardInfo info;
        info.m_id = id;
        info.m_sequenceNumber = seq;
        m_events->add_event(EventType::CLIPBOARD_CHANGED, get_event_target(),
                            create_event_data<ClipboardInfo>(info));
    }

    return true;
}

bool ClientProxy1_6::recvClipboardCapabilities()
{
    std::uint32_t caps = 0;
    if (!ProtocolUtil::readf(getStream(), kMsgCClipboardCapabilities + 4, &caps)) {
        return false;
    }
    m_clipboardCaps = caps & kClipboardCapsSupported;
    LOG_DEBUG("client \"%s\" clipboard capabilities: 0x%x", getName().c_str(), m_clipboardCaps);
    return true;
}

bool ClientProxy1_6::recvClipboardOffer()
{
    if (!supportsClipboardOffers()) {
        return false;
    }

    ClipboardID id;
    std::uint32_t seq, flags, formats, size, hashHigh, hashLow;
    if (!ProtocolUtil::readf(getStream(), kMsgDClipboardOffer + 4, &id, &seq, &flags, &formats,
                             &size, &hashHigh, &hashLow)) {
        return false;
    }
    if (id >= kClipboardEnd) {
        return false;
    }

    ClipboardOfferInfo info;
    info.id = id;
    info.sequence = seq;
    info.flags = flags;
    info.offer.formats = formats;
    info.offer.size = size;
    info.offer.hash = (static_cast<std::uint64_t>(hashHigh) << 32) | hashLow;
    LOG_DEBUG("received client \"%s\" clipboard %d offer seqnum=%d size=%u",
              getName().c_str(), id, seq, size);

    m_events->add_event(EventType::CLIPBOARD_OFFERED, get_event_target(),
                        create_event_data<ClipboardOfferInfo>(info));
    return true;
}

bool ClientProxy1_6::recvClipboardDataRequest()
{
    if (!supportsClipboardOffers()) {
        return false;
    }

    ClipboardID id;
    std::uint32_t hashHigh, hashLow;
    if (!ProtocolUtil::readf(getStream(), kMsgQClipboardData + 4, &id, &hashHigh, &hashLow)) {
        return false;
    }
    if (id >= kClipboardEnd) {
        return false;
    }

    ClipboardOfferInfo info;
    info.id = id;
    info.offer.hash = (static_cast<std::uint64_t>(hashHigh) << 32) | hashLow;
    LOG_DEBUG("client \"%s\" requested clipboard %d", getName().c_str(), id);

    m_events->add_event(EventType::CLIPBOARD_DATA_REQUESTED, get_event_target(),
                        create_event_data<ClipboardOfferInfo>(info));
    return true;
}

const MonitorLayout& ClientProxy1_6::getMonitors() const
{
    return m_monitors;
}

bool ClientProxy1_6::recvMonitors()
{
    std::string data;
    if (!ProtocolUtil::readf(getStream(), kMsgDMonitors + 4, &data)) {
        return false;
    }
    MonitorLayout layout;
    if (!MonitorLayout::decode(data, layout)) {
        LOG_WARN("ignoring an invalid monitor layout from client \"%s\"", getName().c_str());
        return true;
    }
    m_monitors = layout;
    LOG_DEBUG("client \"%s\" monitors: %s", getName().c_str(), layout.describe().c_str());
    return true;
}

bool ClientProxy1_6::supportsClipboardFiles() const
{
    // files are listed without their contents, which are streamed when they are pasted; that
    // needs the contents to be offered rather than sent, so that the paste is noticed
    const std::uint32_t needed = kClipboardCapOffers | kClipboardCapFiles |
                                 kClipboardCapFileStreaming;
    return (m_clipboardCaps & needed) == needed;
}

bool ClientProxy1_6::recvClipboardFileRequest()
{
    if (!supportsClipboardFiles()) {
        return false;
    }
    ClipboardID id;
    std::uint32_t hashHigh, hashLow, index, offsetHigh, offsetLow;
    if (!ProtocolUtil::readf(getStream(), kMsgQClipboardFile + 4, &id, &hashHigh, &hashLow,
                             &index, &offsetHigh, &offsetLow)) {
        return false;
    }
    if (id >= kClipboardEnd) {
        return false;
    }
    ClipboardFileRequestInfo request;
    request.id = id;
    request.hash = (static_cast<std::uint64_t>(hashHigh) << 32) | hashLow;
    request.index = index;
    request.offset = (static_cast<std::uint64_t>(offsetHigh) << 32) | offsetLow;
    LOG_DEBUG("client \"%s\" requested shared file %u from offset %llu", getName().c_str(),
              index, static_cast<unsigned long long>(request.offset));
    m_events->add_event(EventType::CLIPBOARD_FILE_REQUESTED, get_event_target(),
                        create_event_data<ClipboardFileRequestInfo>(request));
    return true;
}

void ClientProxy1_6::requestClipboardFile(const ClipboardFileRequestInfo& request)
{
    if (!supportsClipboardFiles()) {
        return;
    }
    get_conn().send_clipboard_file_request(request);
}

void ClientProxy1_6::provideClipboardFile(ClipboardID id, const ClipboardFileRequestInfo& request,
                                          const fs::path& path, std::uint64_t size)
{
    std::uint64_t remaining = size - request.offset;
    auto header = make_clipboard_file_header(request.hash, request.index, request.offset,
                                             remaining);
    LOG_DEBUG("sending shared file %u to \"%s\", %llu bytes", request.index,
              getName().c_str(), static_cast<unsigned long long>(remaining));
    m_clipboardStreamer->send_file(id, 0, header.format(), path, request.offset, remaining);
}

void ClientProxy1_6::provideClipboardFileUnavailable(ClipboardID id, std::uint64_t hash,
                                                     std::uint32_t index)
{
    LOG_DEBUG("shared file %u requested by \"%s\" is not available", index, getName().c_str());
    auto header = make_unavailable_clipboard_file_header(hash, index);
    m_clipboardStreamer->send_encoded(id, 0, header.format(),
                                      std::make_shared<const std::string>());
}

bool ClientProxy1_6::recvClipboardAge()
{
    if ((m_clipboardCaps & kClipboardCapAge) == 0) {
        return false;
    }

    ClipboardID id;
    std::uint32_t age;
    if (!ProtocolUtil::readf(getStream(), kMsgDClipboardAge + 4, &id, &age)) {
        return false;
    }
    if (id >= kClipboardEnd) {
        return false;
    }

    ClipboardAgeInfo info;
    info.id = id;
    info.age_ms = age;
    if (age == kClipboardAgeUnknown) {
        LOG_DEBUG("client \"%s\" clipboard %d is of unknown age", getName().c_str(), id);
    } else {
        LOG_DEBUG("client \"%s\" clipboard %d is %u ms old", getName().c_str(), id, age);
    }

    m_events->add_event(EventType::CLIPBOARD_AGE, get_event_target(),
                        create_event_data<ClipboardAgeInfo>(info));
    return true;
}

bool ClientProxy1_6::recvClipboardData()
{
    if (!supportsClipboardOffers()) {
        return false;
    }

    ClipboardID id;
    std::uint32_t seq;
    std::uint8_t mark;
    std::string data;
    if (!ProtocolUtil::readf(getStream(), kMsgDClipboardData + 4, &id, &seq, &mark, &data)) {
        return false;
    }
    if (id >= kClipboardEnd) {
        return false;
    }

    auto status = m_clipboardReceiver.add(mark, data);

    if (m_clipboardReceiver.header().kind == ClipboardTransferHeader::Kind::File) {
        // the pieces were passed on through the sink; tell how the file ended
        if (status == ClipboardTransferReceiver::Status::InProgress) {
            return true;
        }
        ClipboardFileChunkInfo info;
        info.hash = m_clipboardReceiver.header().hash;
        info.index = m_clipboardReceiver.header().file_index;
        info.offset = m_clipboardReceiver.header().file_offset + m_clipboardReceiver.received();
        info.complete = status == ClipboardTransferReceiver::Status::Complete;
        info.unavailable = !info.complete;
        m_events->add_event(EventType::CLIPBOARD_FILE_CHUNK, get_event_target(),
                            create_event_data<ClipboardFileChunkInfo>(info));
        return true;
    }

    ClipboardTransferProgressInfo progress;
    progress.hash = m_clipboardReceiver.header().hash;
    progress.progress.received = m_clipboardReceiver.received();
    progress.progress.total = m_clipboardReceiver.expected();
    if (status == ClipboardTransferReceiver::Status::InProgress) {
        m_events->add_event(EventType::CLIPBOARD_TRANSFER_PROGRESS, get_event_target(),
                            create_event_data<ClipboardTransferProgressInfo>(progress));
        return true;
    }
    progress.progress.state = status == ClipboardTransferReceiver::Status::Complete
                                  ? ClipboardTransferProgress::State::Completed
                                  : ClipboardTransferProgress::State::Failed;
    m_events->add_event(EventType::CLIPBOARD_TRANSFER_PROGRESS, get_event_target(),
                        create_event_data<ClipboardTransferProgressInfo>(progress));

    if (status == ClipboardTransferReceiver::Status::Complete) {
        auto contents = m_clipboardReceiver.take();
        LOG_DEBUG("received client \"%s\" clipboard %d seqnum=%d, size=%zu",
                  getName().c_str(), id, seq, contents->size());
        m_clipboard[id].m_data = std::move(contents);
        m_clipboard[id].m_sequenceNumber = seq;

        ClipboardInfo info;
        info.m_id = id;
        info.m_sequenceNumber = seq;
        m_events->add_event(EventType::CLIPBOARD_CHANGED, get_event_target(),
                            create_event_data<ClipboardInfo>(info));
        return true;
    }

    // the client cannot provide the requested contents or sent a broken transfer
    ClipboardOfferInfo info;
    info.id = id;
    info.sequence = seq;
    info.offer.hash = m_clipboardReceiver.header().hash;
    m_events->add_event(EventType::CLIPBOARD_DATA_UNAVAILABLE, get_event_target(),
                        create_event_data<ClipboardOfferInfo>(info));
    return true;
}

bool ClientProxy1_6::recvGrabClipboard()
{
    // parse message
    ClipboardID id;
    std::uint32_t seqNum;
    if (!ProtocolUtil::readf(getStream(), kMsgCClipboard + 4, &id, &seqNum)) {
        return false;
    }
    LOG_DEBUG("received client \"%s\" grabbed clipboard %d seqnum=%d", getName().c_str(), id, seqNum);

    // validate
    if (id >= kClipboardEnd) {
        return false;
    }

    // the client's clipboard has changed, so data sent before is not there anymore
    m_clipboardPushed = false;

    // notify
    ClipboardInfo info;
    info.m_id = id;
    info.m_sequenceNumber = seqNum;
    m_events->add_event(EventType::CLIPBOARD_GRABBED, get_event_target(),
                        create_event_data<ClipboardInfo>(info));

    return true;
}

void ClientProxy1_6::keepAlive()
{
    get_conn().send_keep_alive_1_6();
}

void ClientProxy1_6::fileChunkReceived()
{
    Server* server = getServer();
    int result = FileChunk::assemble(getStream(), server->getReceivedFileData(),
                                     server->getExpectedFileSize());

    if (result == kFinish) {
        m_events->add_event(EventType::FILE_RECEIVE_COMPLETED, server);
    } else if (result == kStart) {
        if (server->getFakeDragFileList().size() > 0) {
            std::string filename = server->getFakeDragFileList().at(0).getFilename();
            LOG_DEBUG("start receiving %s", filename.c_str());
        }
    }
}

void ClientProxy1_6::dragInfoReceived()
{
    // parse
    std::uint32_t fileNum = 0;
    std::string content;
    ProtocolUtil::readf(getStream(), kMsgDDragInfo + 4, &fileNum, &content);

    m_server->dragInfoReceived(fileNum, content);
}

ClientProxy1_6::ClientClipboard::ClientClipboard() :
    m_data(empty_marshalled_clipboard()),
    m_sequenceNumber(0),
    m_dirty(true)
{
    // do nothing
}

} // namespace inputleap
