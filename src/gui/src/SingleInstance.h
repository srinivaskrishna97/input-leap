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

#include <QByteArray>
#include <QLocalServer>
#include <QObject>
#include <QString>

class QLocalSocket;

/*! Makes sure that a user runs only one InputLeap GUI at a time.

    The first instance listens on a local socket named after the user. An instance started later
    finds it there and asks it either to show its window or to quit, so that the new instance
    takes over, e.g. to restart InputLeap. A second GUI would otherwise start or command another
    copy of the InputLeap server or client.
*/
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(const QString& name, QObject* parent = nullptr);
    ~SingleInstance() override;

    /// The name used by the GUI of the current user.
    static QString defaultName();

    /// Returns true if another instance with this name is running and answers.
    bool otherInstanceRunning(int timeoutMs = 2000);

    /// Asks the running instance to show its window.
    bool requestShow(int timeoutMs = 2000);

    /// Asks the running instance to quit, and waits until it has.
    bool requestQuit(int timeoutMs);

    /// Starts taking requests from instances started later. Returns false if the name is in use.
    bool listen();

signals:
    void showRequested();
    void quitRequested();

private slots:
    void onNewConnection();

private:
    bool send(const QByteArray& request, int timeoutMs);

    QString m_name;
    QLocalServer m_server;
    // the connection to the running instance, and its process id
    QLocalSocket* m_other = nullptr;
    qint64 m_otherPid = 0;
};
