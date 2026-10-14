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

#include "client/Client.h"
#include "inputleap/IClipboard.h"

#include "client/ServerProxy.h"
#include "inputleap/Screen.h"
#include "inputleap/FileChunk.h"
#include "inputleap/DropHelper.h"
#include "inputleap/PacketStreamFilter.h"
#include "inputleap/ProtocolUtil.h"
#include "inputleap/protocol_types.h"
#include "inputleap/Exceptions.h"
#include "inputleap/StreamChunker.h"
#include "inputleap/IPlatformScreen.h"
#include "mt/Thread.h"
#include "net/TCPSocket.h"
#include "net/IDataSocket.h"
#include "net/ISocketFactory.h"
#include "net/SecureSocket.h"
#include "arch/Arch.h"
#include "base/Log.h"
#include "base/EventQueueTimer.h"
#include "base/IEventQueue.h"
#include "base/Time.h"


#include <climits>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <fstream>

namespace inputleap {

Client::Client(IEventQueue* events, const std::string& name, const NetworkAddress& address,
               ISocketFactory* socketFactory,
               inputleap::Screen* screen,
               ClientArgs const& args) :
    m_mock(false),
    m_name(name),
    m_serverAddress(address),
    m_socketFactory(socketFactory),
    m_screen(screen),
    m_stream(nullptr),
    m_timer(nullptr),
    m_server(nullptr),
    m_ready(false),
    m_active(false),
    m_suspended(false),
    m_connectOnResume(false),
    m_events(events),
    m_sendFileThread(nullptr),
    m_writeToDropDirThread(nullptr),
    m_useSecureNetwork(args.m_enableCrypto),
    m_args(args),
    m_enableClipboard(true),
    m_maximumClipboardSize(INT_MAX)
{
    assert(m_socketFactory != nullptr);
    assert(m_screen != nullptr);

    // register suspend/resume event handlers
    m_events->add_handler(EventType::SCREEN_SUSPEND, get_event_target(),
                          [this](const auto& e){ handle_suspend(); });
    m_events->add_handler(EventType::SCREEN_RESUME, get_event_target(),
                          [this](const auto& e){ handle_resume(); });

    if (m_args.m_enableDragDrop) {
        m_events->add_handler(EventType::FILE_CHUNK_SENDING, this,
                              [this](const auto& e){ handle_file_chunk_sending(e); });
        m_events->add_handler(EventType::FILE_RECEIVE_COMPLETED, this,
                              [this](const auto& e){ handle_file_receive_completed(e); });
    }
}

Client::~Client()
{
    if (m_mock) {
        return;
    }

    m_events->remove_handler(EventType::SCREEN_SUSPEND, get_event_target());
    m_events->remove_handler(EventType::SCREEN_RESUME, get_event_target());

    cleanupTimer();
    cleanupScreen();
    cleanupConnecting();
    cleanupConnection();
    delete m_socketFactory;
}

void
Client::connect()
{
    if (m_stream != nullptr) {
        return;
    }
    if (m_suspended) {
        m_connectOnResume = true;
        return;
    }

    auto security_level = ConnectionSecurityLevel::PLAINTEXT;
    if (m_useSecureNetwork) {
        // client always authenticates server
        security_level = ConnectionSecurityLevel::ENCRYPTED_AUTHENTICATED;
    }

    try {
        // resolve the server hostname.  do this every time we connect
        // in case we couldn't resolve the address earlier or the address
        // has changed (which can happen frequently if this is a laptop
        // being shuttled between various networks).  patch by Brent
        // Priddy.
        m_serverAddress.resolve();

        // m_serverAddress will be null if the hostname address is not reolved
        if (m_serverAddress.getAddress() != nullptr) {
          // to help users troubleshoot, show server host name (issue: 60)
          LOG_NOTE("connecting to '%s': %s:%i",
          m_serverAddress.getHostname().c_str(),
          ARCH->addrToString(m_serverAddress.getAddress()).c_str(),
          m_serverAddress.getPort());
        }

        // create the socket
        auto socket = m_socketFactory->create(ARCH->getAddrFamily(m_serverAddress.getAddress()),
                                              security_level);
        // filter socket messages, including a packetizing filter
        auto socket_ptr = socket.get();
        m_stream = new PacketStreamFilter(m_events, std::move(socket));

        // connect
        LOG_DEBUG1("connecting to server");
        setupConnecting();
        setupTimer();
        socket_ptr->connect(m_serverAddress);
    }
    catch (XBase& e) {
        cleanupTimer();
        cleanupConnecting();
        cleanupStream();
        LOG_DEBUG1("connection failed");
        sendConnectionFailedEvent(e.what());
        return;
    }
}

void
Client::disconnect(const char* msg)
{
    m_connectOnResume = false;
    cleanupTimer();
    cleanupScreen();
    cleanupConnecting();
    cleanupConnection();
    if (msg != nullptr) {
        sendConnectionFailedEvent(msg);
    }
    else {
        send_event(EventType::CLIENT_DISCONNECTED);
    }
}

void
Client::handshakeComplete()
{
    m_ready = true;
    m_enteredSinceConnect = false;
    m_screen->enable();

    // The server used to be assumed to own the clipboards, which emptied this screen's.  A
    // server that compares ages is told how old the contents are instead, and the younger of
    // the two clipboards wins.
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        if (m_enableClipboard && m_maximumClipboardSize != 0 && serverComparesClipboardAges()) {
            m_announceClipboard[id] = true;
            announceClipboard(id);
        }
        else {
            grabClipboard(id);
        }
    }

    send_event(EventType::CLIENT_CONNECTED);
}

MonitorLayout Client::getMonitors() const
{
    return m_screen->getMonitors();
}

bool Client::serverTakesFiles() const
{
    // files are listed without their contents, which are streamed when they are pasted; that
    // needs the contents to be offered rather than sent, so that the paste is noticed
    const std::uint32_t needed = kClipboardCapOffers | kClipboardCapFiles |
                                 kClipboardCapFileStreaming;
    return m_server != nullptr && (m_server->clipboardCapabilities() & needed) == needed;
}

bool Client::serverComparesClipboardAges() const
{
    return m_server != nullptr && (m_server->clipboardCapabilities() & kClipboardCapAge) != 0;
}

bool
Client::isConnected() const
{
    return (m_server != nullptr);
}

bool
Client::isConnecting() const
{
    return (m_timer != nullptr);
}

NetworkAddress
Client::getServerAddress() const
{
    return m_serverAddress;
}

const EventTarget* Client::get_event_target() const
{
    return m_screen->get_event_target();
}

bool
Client::getClipboard(ClipboardID id, IClipboard* clipboard) const
{
    return m_screen->getClipboard(id, clipboard);
}

void Client::getShape(std::int32_t& x, std::int32_t& y, std::int32_t& w, std::int32_t& h) const
{
    m_screen->getShape(x, y, w, h);
}

void Client::getCursorPos(std::int32_t& x, std::int32_t& y) const
{
    m_screen->getCursorPos(x, y);
}

void Client::enter(std::int32_t xAbs, std::int32_t yAbs, std::uint32_t, KeyModifierMask mask, bool)
{
    m_active = true;
    m_enteredSinceConnect = true;
    m_screen->mouseMove(xAbs, yAbs);
    m_screen->enter(mask);

    if (m_sendFileThread != nullptr) {
        StreamChunker::interruptFile();
        m_sendFileThread = nullptr;
    }
}

bool
Client::leave()
{
    m_active = false;

    m_screen->leave();

    if (m_enableClipboard) {
        // send clipboards that we own and that have changed
        for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
            if (m_ownClipboard[id]) {
                sendClipboard(id);
            }
        }
    }

    return true;
}

void
Client::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
     m_screen->setClipboard(id, clipboard);
    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
}

void Client::setClipboardData(ClipboardID id, const MarshalledClipboardPtr& data)
{
    m_screen->setClipboardData(id, data);
    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
    m_hasOfferedClipboard[id] = false;
}

void Client::setClipboardOffer(ClipboardID id, const ClipboardOffer& offer, std::uint32_t flags)
{
    m_hasOfferedClipboard[id] = true;
    m_offeredClipboard[id] = offer;
    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;

    if (m_screen->supportsClipboardOffers()) {
        // the contents are put on the clipboard without their data, which is requested when
        // an application pastes them
        m_screen->setClipboardOffer(id, offer);
    } else if ((flags & kClipboardOfferDataFollows) == 0) {
        // the screen cannot wait with the data until it is pasted, so get it now
        m_server->requestClipboardData(id, offer);
    }
}

void Client::setOfferedClipboardData(ClipboardID id, const MarshalledClipboardPtr& data)
{
    // Ignore outdated data, e.g. data that was still on its way when newer contents were
    // offered. The contents may have been offered as either clipboard.
    bool wanted = false;
    for (ClipboardID other = 0; other < kClipboardEnd; ++other) {
        if (m_hasOfferedClipboard[other] && m_offeredClipboard[other].describes(*data)) {
            wanted = true;
        }
    }
    if (!wanted) {
        LOG_DEBUG("ignored outdated data of clipboard %d", id);
        return;
    }

    m_screen->setClipboardData(id, data);
}

void Client::clipboardDataUnavailable(ClipboardID id, const ClipboardOffer& offer)
{
    m_screen->clipboardDataUnavailable(id, offer);
}

void Client::clipboardDataRequested(ClipboardID id, const ClipboardOffer& offer)
{
    // the offered contents are the ones that were last sent for one of the clipboards, possibly
    // without the files if the server cannot take them
    for (ClipboardID other = 0; other < kClipboardEnd; ++other) {
        MarshalledClipboardPtr data = m_dataClipboard[other];
        if (data && !serverTakesFiles()) {
            data = without_clipboard_formats(data, 1u << IClipboard::kFiles);
        }
        if (data && data->hash() == offer.hash) {
            m_server->provideClipboardData(id, data);
            return;
        }
    }
    m_server->provideClipboardUnavailable(id, offer);
}

void Client::clipboardTransferProgress(std::uint64_t hash,
                                       const ClipboardTransferProgress& progress)
{
    // the offer tells what kind of contents are arriving
    ClipboardTransferProgress info = progress;
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        if (m_hasOfferedClipboard[id] && m_offeredClipboard[id].hash == hash) {
            info.formats = m_offeredClipboard[id].formats;
            break;
        }
    }
    m_screen->setClipboardTransferProgress(info);
}

void
Client::grabClipboard(ClipboardID id)
{
    m_screen->grabClipboard(id);
    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
}

void
Client::setClipboardDirty(ClipboardID, bool)
{
    assert(0 && "shouldn't be called");
}

void
Client::keyDown(KeyID id, KeyModifierMask mask, KeyButton button)
{
     m_screen->keyDown(id, mask, button);
}

void Client::keyRepeat(KeyID id, KeyModifierMask mask, std::int32_t count, KeyButton button)
{
     m_screen->keyRepeat(id, mask, count, button);
}

void
Client::keyUp(KeyID id, KeyModifierMask mask, KeyButton button)
{
     m_screen->keyUp(id, mask, button);
}

void
Client::mouseDown(ButtonID id)
{
     m_screen->mouseDown(id);
}

void
Client::mouseUp(ButtonID id)
{
     m_screen->mouseUp(id);
}

void Client::mouseMove(std::int32_t x, std::int32_t y)
{
    m_screen->mouseMove(x, y);
}

void Client::mouseRelativeMove(std::int32_t dx, std::int32_t dy)
{
    m_screen->mouseRelativeMove(dx, dy);
}

void Client::mouseWheel(std::int32_t xDelta, std::int32_t yDelta)
{
    m_screen->mouseWheel(xDelta, yDelta);
}

void
Client::screensaver(bool activate)
{
     m_screen->screensaver(activate);
}

void
Client::resetOptions()
{
    m_screen->resetOptions();
}

void
Client::setOptions(const OptionsList& options)
{
    for (auto index = options.begin(); index != options.end(); ++index) {
        const OptionID id       = *index;
        if (id == kOptionClipboardSharing) {
            index++;
            if (*index == static_cast<OptionValue>(false)) {
                LOG_NOTE("clipboard sharing is disabled");
            }
            m_enableClipboard = *index;

            break;
        } else if (id == kOptionClipboardSharingSize) {
            index++;
            if (index != options.end()) {
                m_maximumClipboardSize = *index;
            }
        }
    }

    if (m_enableClipboard && !m_maximumClipboardSize) {
        m_enableClipboard = false;
        LOG_NOTE("clipboard sharing is disabled because the server "
                       "set the maximum clipboard size to 0");
    }

    m_screen->setOptions(options);
}

std::string
Client::getName() const
{
    return m_name;
}

void
Client::sendClipboard(ClipboardID id)
{
    // note -- m_mutex must be locked on entry
    assert(m_screen != nullptr);
    assert(m_server != nullptr);

    if (m_screen->hasClipboardSnapshots()) {
        // the screen captures the clipboard in the background whenever it changes, so the
        // data is ready and leaving the screen does not have to wait for it
        MarshalledClipboardPtr data = m_screen->getClipboardSnapshot(id);
        if (!data) {
            // the screen sends CLIPBOARD_CHANGED once the capture completes
            LOG_DEBUG("clipboard %d is not available yet", id);
            return;
        }
        if (data->size() >= m_maximumClipboardSize) {
            LOG_NOTE("Skipping clipboard transfer because the clipboard contents exceed the "
                     "size limit of %zu bytes set by the server", m_maximumClipboardSize);
            return;
        }
        if (!m_sentClipboard[id] || !same_clipboard_contents(data, m_dataClipboard[id])) {
            m_sentClipboard[id] = true;
            m_dataClipboard[id] = data;
            sendClipboardContents(id, data);
        }
        return;
    }

    // get clipboard data.  set the clipboard time to the last
    // clipboard time before getting the data from the screen
    // as the screen may detect an unchanged clipboard and
    // avoid copying the data.
    Clipboard clipboard;
    if (clipboard.open(m_timeClipboard[id])) {
        clipboard.close();
    }
    m_screen->getClipboard(id, &clipboard);

    // check time
    if (m_timeClipboard[id] == 0 ||
        clipboard.getTime() != m_timeClipboard[id]) {
        // save new time
        m_timeClipboard[id] = clipboard.getTime();

        // marshall the data
        MarshalledClipboardPtr data = marshall_clipboard(clipboard);
        if (data->size() >= m_maximumClipboardSize) {
            LOG_NOTE("Skipping clipboard transfer because the clipboard contents exceed the "
                     "size limit of %zu bytes set by the server", m_maximumClipboardSize);
            return;
        }

        // save and send data if different or not yet sent
        if (!m_sentClipboard[id] || !same_clipboard_contents(data, m_dataClipboard[id])) {
            m_sentClipboard[id] = true;
            m_dataClipboard[id] = data;
            sendClipboardContents(id, data);
        }
    }
}

void Client::sendClipboardContents(ClipboardID id, const MarshalledClipboardPtr& contents)
{
    // files are only sent to servers that can put them on their clipboard
    MarshalledClipboardPtr data = contents;
    if (!serverTakesFiles()) {
        data = without_clipboard_formats(contents, 1u << IClipboard::kFiles);
    }

    if ((m_server->clipboardCapabilities() & kClipboardCapOffers) == 0) {
        m_server->onClipboardChanged(id, data);
        return;
    }

    // Offer the contents; the server asks for them when they are needed. Small contents are
    // sent right away, unless they were sent before for the other clipboard.
    bool push = data->size() <= kClipboardEagerSize &&
                !(m_pushedClipboard && m_pushedClipboardHash == data->hash());
    m_server->onClipboardOffered(id, ClipboardOffer::describe(*data),
                                 push ? kClipboardOfferDataFollows : 0);
    if (push) {
        m_server->onClipboardChanged(id, data);
        m_pushedClipboard = true;
        m_pushedClipboardHash = data->hash();
    }
}

void Client::handle_clipboard_file_requested(const Event& event)
{
    // an application pastes shared files, so get a file from the server
    const auto& info = event.get_data_as<ClipboardFileRequestInfo>();
    if (m_server == nullptr || !m_ready) {
        ClipboardFileChunkInfo chunk;
        chunk.hash = info.hash;
        chunk.index = info.index;
        chunk.unavailable = true;
        m_screen->clipboardFileChunk(chunk);
        return;
    }
    m_server->requestClipboardFile(info);
}

void Client::clipboardFileRequested(ClipboardID id, const ClipboardFileRequestInfo& request)
{
    fs::path path;
    std::uint64_t size = 0;
    if (!m_screen->getClipboardFile(request.hash, request.index, path, size) ||
        request.offset > size) {
        LOG_DEBUG("the server requested shared file %u, which is not available", request.index);
        m_server->provideClipboardFileUnavailable(id, request.hash, request.index);
        return;
    }
    m_server->provideClipboardFile(id, request, path, size);
}

void Client::clipboardFileChunk(const ClipboardFileChunkInfo& chunk)
{
    m_screen->clipboardFileChunk(chunk);
}

void Client::send_event(EventType type)
{
    m_events->add_event(type, get_event_target());
}

void
Client::sendConnectionFailedEvent(const char* msg)
{
    FailInfo info{msg};
    info.m_retry = true;
    m_events->add_event(EventType::CLIENT_CONNECTION_FAILED, get_event_target(),
                        create_event_data<FailInfo>(info));
}

void Client::send_file_chunk(const FileChunk& chunk)
{
    LOG_DEBUG1("send file chunk");
    assert(m_server != nullptr);

    m_server->file_chunk_sending(chunk);
}

void
Client::setupConnecting()
{
    assert(m_stream != nullptr);

    if (m_args.m_enableCrypto) {
        m_events->add_handler(EventType::DATA_SOCKET_SECURE_CONNECTED, m_stream->get_event_target(),
                              [this](const auto& e){ handle_connected(); });
    }
    else {
        m_events->add_handler(EventType::DATA_SOCKET_CONNECTED, m_stream->get_event_target(),
                              [this](const auto& e){ handle_connected(); });
    }
    m_events->add_handler(EventType::DATA_SOCKET_CONNECTION_FAILED, m_stream->get_event_target(),
                          [this](const auto& e){ handle_connection_failed(e); });
}

void
Client::setupConnection()
{
    assert(m_stream != nullptr);

    m_events->add_handler(EventType::SOCKET_DISCONNECTED, m_stream->get_event_target(),
                          [this](const auto& e){ handle_disconnected(); });
    m_events->add_handler(EventType::STREAM_INPUT_READY, m_stream->get_event_target(),
                          [this](const auto& e){ handle_hello(); });
    m_events->add_handler(EventType::STREAM_OUTPUT_ERROR, m_stream->get_event_target(),
                          [this](const auto& e){ handle_output_error(); });
    m_events->add_handler(EventType::STREAM_INPUT_SHUTDOWN, m_stream->get_event_target(),
                          [this](const auto& e){ handle_disconnected(); });
    m_events->add_handler(EventType::STREAM_OUTPUT_SHUTDOWN, m_stream->get_event_target(),
                          [this](const auto& e){ handle_disconnected(); });
    m_events->add_handler(EventType::SOCKET_STOP_RETRY, m_stream->get_event_target(),
                          [this](const auto& e){ handle_stop_retry(); });
}

void
Client::setupScreen()
{
    assert(m_server == nullptr);

    m_ready  = false;
    m_server = new ServerProxy(this, m_stream, m_events);
    m_events->add_handler(EventType::SCREEN_SHAPE_CHANGED, get_event_target(),
                          [this](const auto& e){ handle_shape_changed(); });
    m_events->add_handler(EventType::CLIPBOARD_GRABBED, get_event_target(),
                          [this](const auto& e){ handle_clipboard_grabbed(e); });
    m_events->add_handler(EventType::CLIPBOARD_CHANGED, get_event_target(),
                          [this](const auto& e){ handle_clipboard_changed(e); });
    m_events->add_handler(EventType::CLIPBOARD_DATA_REQUESTED, get_event_target(),
                          [this](const auto& e){ handle_clipboard_data_requested(e); });
    m_events->add_handler(EventType::CLIPBOARD_FILE_REQUESTED, get_event_target(),
                          [this](const auto& e){ handle_clipboard_file_requested(e); });
}

void
Client::setupTimer()
{
    assert(m_timer == nullptr);

    m_timer = m_events->newOneShotTimer(15.0, nullptr);
    m_events->add_handler(EventType::TIMER, m_timer,
                          [this](const auto& e){ handle_connect_timeout(); });
}

void
Client::cleanupConnecting()
{
    if (m_stream != nullptr) {
        m_events->remove_handler(EventType::DATA_SOCKET_CONNECTED, m_stream->get_event_target());
        m_events->remove_handler(EventType::DATA_SOCKET_CONNECTION_FAILED,
                                m_stream->get_event_target());
    }
}

void
Client::cleanupConnection()
{
    if (m_stream != nullptr) {
        m_events->remove_handler(EventType::STREAM_INPUT_READY, m_stream->get_event_target());
        m_events->remove_handler(EventType::STREAM_OUTPUT_ERROR, m_stream->get_event_target());
        m_events->remove_handler(EventType::STREAM_INPUT_SHUTDOWN, m_stream->get_event_target());
        m_events->remove_handler(EventType::STREAM_OUTPUT_SHUTDOWN, m_stream->get_event_target());
        m_events->remove_handler(EventType::SOCKET_DISCONNECTED, m_stream->get_event_target());
        m_events->remove_handler(EventType::SOCKET_STOP_RETRY, m_stream->get_event_target());
        cleanupStream();
    }
}

void
Client::cleanupScreen()
{
    if (m_server != nullptr) {
        if (m_ready) {
            m_screen->disable();
            m_ready = false;
        }
        m_events->remove_handler(EventType::SCREEN_SHAPE_CHANGED, get_event_target());
        m_events->remove_handler(EventType::CLIPBOARD_GRABBED, get_event_target());
        m_events->remove_handler(EventType::CLIPBOARD_CHANGED, get_event_target());
        m_events->remove_handler(EventType::CLIPBOARD_DATA_REQUESTED, get_event_target());
        m_events->remove_handler(EventType::CLIPBOARD_FILE_REQUESTED, get_event_target());
        delete m_server;
        m_server = nullptr;
    }
}

void
Client::cleanupTimer()
{
    if (m_timer != nullptr) {
        m_events->remove_handler(EventType::TIMER, m_timer);
        m_events->deleteTimer(m_timer);
        m_timer = nullptr;
    }
}

void
Client::cleanupStream()
{
    delete m_stream;
    m_stream = nullptr;
}

void
Client::handle_connected()
{
    LOG_DEBUG1("connected;  wait for hello");
    cleanupConnecting();
    setupConnection();

    // reset clipboard state
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        m_ownClipboard[id]  = false;
        m_sentClipboard[id] = false;
        m_timeClipboard[id] = 0;
    }
}

void Client::handle_connection_failed(const Event& event)
{
    const auto& info = event.get_data_as<IDataSocket::ConnectionFailedInfo>();

    cleanupTimer();
    cleanupConnecting();
    cleanupStream();
    LOG_DEBUG1("connection failed");
    sendConnectionFailedEvent(info.m_what.c_str());
}

void Client::handle_connect_timeout()
{
    cleanupTimer();
    cleanupConnecting();
    cleanupConnection();
    cleanupStream();
    LOG_DEBUG1("connection timed out");
    sendConnectionFailedEvent("Timed out");
}

void Client::handle_output_error()
{
    cleanupTimer();
    cleanupScreen();
    cleanupConnection();
    LOG_WARN("error sending to server");
    send_event(EventType::CLIENT_DISCONNECTED);
}

void Client::handle_disconnected()
{
    cleanupTimer();
    cleanupScreen();
    cleanupConnection();
    LOG_DEBUG1("disconnected");
    send_event(EventType::CLIENT_DISCONNECTED);
}

void Client::handle_shape_changed()
{
    LOG_DEBUG("resolution changed");
    m_server->onInfoChanged();
}

void Client::handle_clipboard_grabbed(const Event& event)
{
    if (!m_enableClipboard || (m_maximumClipboardSize == 0)) {
        return;
    }

    const auto& info = event.get_data_as<IScreen::ClipboardInfo>();

    if (!m_enteredSinceConnect && serverComparesClipboardAges()) {
        // the server has not entered this screen since it connected, so a grab would carry
        // a sequence number that the server regards as old.  announce the contents with their
        // age instead, once the screen has captured them.
        m_announceClipboard[info.m_id] = true;
        m_ownClipboard[info.m_id] = true;
        m_sentClipboard[info.m_id] = false;
        m_timeClipboard[info.m_id] = 0;
        m_hasOfferedClipboard[info.m_id] = false;
        announceClipboard(info.m_id);
        return;
    }

    // grab ownership
    m_server->onGrabClipboard(info.m_id);

    // we now own the clipboard and it has not been sent to the server
    m_ownClipboard[info.m_id]  = true;
    m_sentClipboard[info.m_id] = false;
    m_timeClipboard[info.m_id] = 0;
    m_hasOfferedClipboard[info.m_id] = false;

    // if we're not the active screen then send the clipboard now,
    // otherwise we'll wait until we leave.
    if (!m_active) {
        sendClipboard(info.m_id);
    }
}

void Client::handle_clipboard_changed(const Event& event)
{
    if (!m_enableClipboard || (m_maximumClipboardSize == 0) || m_server == nullptr) {
        return;
    }

    const auto& info = event.get_data_as<IScreen::ClipboardInfo>();
    if (info.m_id >= kClipboardEnd) {
        return;
    }

    if (m_announceClipboard[info.m_id] ||
        (!m_enteredSinceConnect && serverComparesClipboardAges())) {
        // contents to be announced with their age, see handle_clipboard_grabbed()
        m_announceClipboard[info.m_id] = true;
        announceClipboard(info.m_id);
        return;
    }

    // the screen finished capturing a clipboard that we own.  if we're not the active screen
    // then send it now, otherwise it is sent when we leave.
    if (m_ownClipboard[info.m_id] && !m_active) {
        sendClipboard(info.m_id);
    }
}

void Client::announceClipboard(ClipboardID id)
{
    // note -- only with a server that supports kClipboardCapAge
    MarshalledClipboardPtr data;
    std::uint32_t age = kClipboardAgeUnknown;
    if (m_screen->hasClipboardSnapshots()) {
        data = m_screen->getClipboardSnapshot(id);
        if (!data) {
            // the screen sends CLIPBOARD_CHANGED once the capture completes
            return;
        }
        age = m_screen->getClipboardSnapshotAge(id);
    }
    else {
        Clipboard clipboard;
        if (clipboard.open(0)) {
            clipboard.close();
        }
        m_screen->getClipboard(id, &clipboard);
        data = marshall_clipboard(clipboard);
    }
    m_announceClipboard[id] = false;

    if (marshalled_clipboard_formats(data->data()) == 0 ||
        data->size() >= m_maximumClipboardSize) {
        // nothing worth keeping, so the server's contents may replace it, as they always did
        LOG_DEBUG("clipboard %d has nothing to keep", id);
        grabClipboard(id);
        return;
    }

    if (age == kClipboardAgeUnknown) {
        LOG_DEBUG("announcing clipboard %d of unknown age", id);
    } else {
        LOG_DEBUG("announcing clipboard %d, %u ms old", id, age);
    }
    m_server->onClipboardAge(id, age);

    // the server takes this screen as the owner if the contents are younger than its own.
    // behave as the owner until the server sends other contents.
    m_ownClipboard[id] = true;
    m_sentClipboard[id] = true;
    m_dataClipboard[id] = data;
    m_hasOfferedClipboard[id] = false;
    sendClipboardContents(id, data);
}

void Client::handle_clipboard_data_requested(const Event& event)
{
    const auto& info = event.get_data_as<ClipboardOfferInfo>();
    if (m_server == nullptr || !m_ready) {
        // the contents cannot be obtained, so don't let the paste wait for them
        m_screen->clipboardDataUnavailable(info.id, info.offer);
        return;
    }

    // an application pastes offered contents, so get them from the server.  screens with a
    // single system clipboard request them as any clipboard that offered them.
    ClipboardID id = info.id;
    for (ClipboardID other = 0; other < kClipboardEnd; ++other) {
        if (m_hasOfferedClipboard[other] && m_offeredClipboard[other].hash == info.offer.hash) {
            id = other;
            break;
        }
    }
    if (id >= kClipboardEnd) {
        return;
    }
    m_server->requestClipboardData(id, info.offer);
}

void Client::handle_hello()
{
    std::int16_t major, minor;
    if (!ProtocolUtil::readf(m_stream, kMsgHello, &major, &minor)) {
        sendConnectionFailedEvent("Protocol error from server, check encryption settings");
        cleanupTimer();
        cleanupConnection();
        return;
    }

    // check versions
    LOG_DEBUG1("got hello version %d.%d", major, minor);
    if (major < kProtocolMajorVersion ||
        (major == kProtocolMajorVersion && minor < kProtocolMinorVersion)) {
        sendConnectionFailedEvent(XIncompatibleClient(major, minor).what());
        cleanupTimer();
        cleanupConnection();
        return;
    }

    // say hello back
    LOG_DEBUG1("say hello version %d.%d", kProtocolMajorVersion, kProtocolMinorVersion);
    ProtocolUtil::writef(m_stream, kMsgHelloBack,
                            kProtocolMajorVersion,
                            kProtocolMinorVersion, &m_name);

    // now connected but waiting to complete handshake
    setupScreen();
    cleanupTimer();

    // make sure we process any remaining messages later.  we won't
    // receive another event for already pending messages so we fake
    // one.
    if (m_stream->isReady()) {
        m_events->add_event(EventType::STREAM_INPUT_READY, m_stream->get_event_target());
    }
}

void Client::handle_suspend()
{
    LOG_INFO("suspend");
    m_suspended       = true;
    bool wasConnected = isConnected();
    disconnect(nullptr);
    m_connectOnResume = wasConnected;
}

void Client::handle_resume()
{
    LOG_INFO("resume");
    m_suspended = false;
    if (m_connectOnResume) {
        m_connectOnResume = false;
        connect();
    }
}

void Client::handle_file_chunk_sending(const Event& event)
{
    send_file_chunk(event.get_data_as<FileChunk>());
}

void Client::handle_file_receive_completed(const Event& event)
{
    (void) event;

    onFileReceiveCompleted();
}

void
Client::onFileReceiveCompleted()
{
    if (isReceivedFileSizeValid()) {
        m_writeToDropDirThread = new Thread([this](){ write_to_drop_dir_thread(); });
    }
}

void Client::handle_stop_retry()
{
    m_args.m_restartable = false;
}

void Client::write_to_drop_dir_thread()
{
    LOG_DEBUG("starting write to drop dir thread");

    while (m_screen->isFakeDraggingStarted()) {
        inputleap::this_thread_sleep(.1f);
    }

    DropHelper::writeToDir(m_screen->getDropTarget(), m_dragFileList,
                    m_receivedFileData);
}

void Client::dragInfoReceived(std::uint32_t fileNum, std::string data)
{
    // TODO: fix duplicate function from CServer
    if (!m_args.m_enableDragDrop) {
        LOG_DEBUG("drag drop not enabled, ignoring drag info.");
        return;
    }

    DragInformation::parseDragInfo(m_dragFileList, fileNum, data);

    m_screen->startDraggingFiles(m_dragFileList);
}

bool
Client::isReceivedFileSizeValid()
{
    return m_expectedFileSize == m_receivedFileData.size();
}

void
Client::sendFileToServer(const char* filename)
{
    if (m_sendFileThread != nullptr) {
        StreamChunker::interruptFile();
    }

    m_sendFileThread = new Thread([this, filename]() { send_file_thread(filename); });
}

void Client::send_file_thread(const char* filename)
{
    try {
        StreamChunker::sendFile(filename, m_events, this);
    }
    catch (std::runtime_error& error) {
        LOG_ERR("failed sending file chunks: %s", error.what());
    }

    m_sendFileThread = nullptr;
}

void Client::sendDragInfo(std::uint32_t fileCount, std::string& info, size_t size)
{
    m_server->sendDragInfo(fileCount, info.c_str(), size);
}

} // namespace inputleap
