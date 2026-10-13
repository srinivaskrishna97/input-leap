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

#include "../src/SingleInstance.h"
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <memory>

namespace {

// the local sockets need an application object for their events
void ensureApplication()
{
    static int argc = 1;
    static char name[] = "guiunittests";
    static char* argv[] = { name, nullptr };
    if (QCoreApplication::instance() == nullptr) {
        new QCoreApplication(argc, argv);
    }
}

// a name that no real GUI and no other test run uses
QString uniqueName(const char* test)
{
    return QStringLiteral("InputLeap-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(test);
}

void processEventsFor(int ms)
{
    for (int i = 0; i < ms / 5; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

} // namespace

TEST(SingleInstanceTests, first_instance_finds_no_other)
{
    ensureApplication();
    SingleInstance first(uniqueName("first"));
    EXPECT_FALSE(first.otherInstanceRunning(500));
    EXPECT_TRUE(first.listen());
}

TEST(SingleInstanceTests, later_instance_asks_running_one_to_show)
{
    ensureApplication();
    QString name = uniqueName("show");
    SingleInstance running(name);
    ASSERT_TRUE(running.listen());
    int shows = 0;
    int quits = 0;
    QObject::connect(&running, &SingleInstance::showRequested, [&shows]() { ++shows; });
    QObject::connect(&running, &SingleInstance::quitRequested, [&quits]() { ++quits; });

    SingleInstance later(name);
    ASSERT_TRUE(later.otherInstanceRunning());
    EXPECT_TRUE(later.requestShow());
    for (int i = 0; i < 200 && shows == 0; ++i) {
        processEventsFor(10);
    }
    EXPECT_EQ(shows, 1);
    EXPECT_EQ(quits, 0);
}

TEST(SingleInstanceTests, later_instance_takes_over_when_running_one_quits)
{
    ensureApplication();
    QString name = uniqueName("quit");
    auto running = std::make_unique<SingleInstance>(name);
    ASSERT_TRUE(running->listen());
    // the running instance exits when asked to quit, which closes its connections
    QObject::connect(running.get(), &SingleInstance::quitRequested, [&running]() {
        QTimer::singleShot(0, [&running]() { running.reset(); });
    });

    SingleInstance later(name);
    ASSERT_TRUE(later.otherInstanceRunning());
    EXPECT_TRUE(later.requestQuit(5000));
    EXPECT_TRUE(running == nullptr);

    // the name is free for the new instance now
    EXPECT_TRUE(later.listen());
    SingleInstance third(name);
    EXPECT_TRUE(third.otherInstanceRunning());
}

TEST(SingleInstanceTests, default_name_is_safe_for_a_socket_name)
{
    ensureApplication();
    QString name = SingleInstance::defaultName();
    EXPECT_TRUE(name.startsWith("InputLeap-gui-"));
    for (QChar c : name) {
        EXPECT_TRUE(c.isLetterOrNumber() || c == '-' || c == '_') << name.toStdString();
    }
}
