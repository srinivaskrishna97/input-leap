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

#include "server/ClientProxy.h"
#include "IClientConnection.h"
#include "base/EventQueue.h"

namespace inputleap {

ClientProxy::ClientProxy(const std::string& name, std::unique_ptr<IClientConnection> backend) :
    BaseClientProxy(name),
    conn_{std::move(backend)}
{
}

ClientProxy::~ClientProxy() = default;

void
ClientProxy::close(const char* msg)
{
    // Tell the client why it is disconnected.  The message is sent in the background: waiting
    // for it here would block the server for as long as a client that has stopped reading
    // keeps its connection open.  The server drops the client after a timeout if it does not
    // disconnect by itself.
    get_conn().send_close_1_6(msg);
}

const EventTarget* ClientProxy::get_event_target() const
{
    return this;
}

} // namespace inputleap
