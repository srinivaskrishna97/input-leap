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

#include "SingleInstance.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QThread>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

// Every connection starts with this greeting from the running instance, followed by its process
// id. Requests are single lines.
const QByteArray kGreeting = "inputleap-gui";
const QByteArray kShow = "show";
const QByteArray kQuit = "quit";

// Runs the event loop until the condition holds or the time is up.  The requests are made before
// the application's event loop runs, and in tests the running instance is in the same process.
template <class Condition>
bool waitFor(Condition condition, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() >= timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QThread::msleep(5);
    }
    return true;
}

} // namespace

SingleInstance::SingleInstance(const QString& name, QObject* parent) :
    QObject(parent),
    m_name(name)
{
    // other users must not be able to make this instance quit
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
}

SingleInstance::~SingleInstance()
{
    delete m_other;
}

QString SingleInstance::defaultName()
{
    QString user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"));
    if (user.isEmpty()) {
        user = QDir::home().dirName();
    }
    QString name = QStringLiteral("InputLeap-gui-");
    for (QChar c : user) {
        name += c.isLetterOrNumber() ? c : QChar('_');
    }
    return name;
}

bool SingleInstance::otherInstanceRunning(int timeoutMs)
{
    delete m_other;
    m_other = new QLocalSocket;
    m_otherPid = 0;

    m_other->connectToServer(m_name);
    waitFor([this]() { return m_other->state() != QLocalSocket::ConnectingState; }, timeoutMs);
    bool greeted = m_other->state() == QLocalSocket::ConnectedState &&
                   waitFor([this]() {
                       return m_other->canReadLine() ||
                              m_other->state() != QLocalSocket::ConnectedState;
                   }, timeoutMs) &&
                   m_other->canReadLine();
    if (greeted) {
        // "inputleap-gui <pid>"
        QList<QByteArray> fields = m_other->readLine().trimmed().split(' ');
        greeted = fields.size() == 2 && fields[0] == kGreeting;
        if (greeted) {
            m_otherPid = fields[1].toLongLong();
        }
    }
    if (!greeted) {
        // nothing there, or something that is not a running instance
        delete m_other;
        m_other = nullptr;
    }
    return greeted;
}

bool SingleInstance::send(const QByteArray& request, int timeoutMs)
{
    if (m_other == nullptr || m_other->state() != QLocalSocket::ConnectedState) {
        return false;
    }
    m_other->write(request + "\n");
    return waitFor([this]() {
        return m_other->bytesToWrite() == 0 || m_other->state() != QLocalSocket::ConnectedState;
    }, timeoutMs) && m_other->bytesToWrite() == 0;
}

bool SingleInstance::requestShow(int timeoutMs)
{
#if defined(Q_OS_WIN)
    // only the application in the foreground may bring another one to the front
    if (m_otherPid > 0) {
        AllowSetForegroundWindow(static_cast<DWORD>(m_otherPid));
    }
#endif
    return send(kShow, timeoutMs);
}

bool SingleInstance::requestQuit(int timeoutMs)
{
    if (!send(kQuit, timeoutMs)) {
        return false;
    }
    // the connection closes when the running instance exits
    return waitFor([this]() { return m_other->state() == QLocalSocket::UnconnectedState; },
                   timeoutMs);
}

bool SingleInstance::listen()
{
    if (m_server.listen(m_name)) {
        return true;
    }
    if (m_server.serverError() == QAbstractSocket::AddressInUseError) {
        // nothing answered on the name, so it was left behind by an instance that crashed (only
        // on systems where the name is a file)
        QLocalServer::removeServer(m_name);
        return m_server.listen(m_name);
    }
    return false;
}

void SingleInstance::onNewConnection()
{
    while (QLocalSocket* socket = m_server.nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            while (socket->canReadLine()) {
                QByteArray request = socket->readLine().trimmed();
                if (request == kShow) {
                    emit showRequested();
                }
                else if (request == kQuit) {
                    emit quitRequested();
                }
            }
        });
        socket->write(kGreeting + " " + QByteArray::number(QCoreApplication::applicationPid()) +
                      "\n");
        socket->flush();
    }
}
