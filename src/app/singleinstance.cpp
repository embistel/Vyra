#include "app/singleinstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QThread>

namespace {
constexpr int kQuietMs = 600;   // stop collecting after this long without a new path
constexpr int kMaximumMs = 8000;

QString channelName(const QString &verb)
{
    const QByteArray user = qgetenv("USERNAME") + qgetenv("USER");
    const QByteArray hash = QCryptographicHash::hash(user + verb.toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
    return QStringLiteral("Vyra-collect-") + QString::fromLatin1(hash);
}

bool sendToCollector(const QString &name, const QStringList &paths)
{
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(200)) return false;
    socket.write(paths.join(u'\n').toUtf8());
    socket.flush();
    socket.waitForBytesWritten(2000);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) socket.waitForDisconnected(1000);
    return true;
}
}

std::optional<QStringList> collectInvocations(const QString &verb, const QStringList &paths)
{
    const QString name = channelName(verb);
    if (sendToCollector(name, paths)) return std::nullopt;

    QLockFile lock(QDir::temp().filePath(name + QStringLiteral(".lock")));
    lock.setStaleLockTime(kMaximumMs + 2000);
    if (!lock.tryLock(0)) {
        // Another process is becoming the collector right now.
        for (int i = 0; i < 30; ++i) {
            QThread::msleep(100);
            if (sendToCollector(name, paths)) return std::nullopt;
        }
        return paths; // give up and act alone
    }
    QLocalServer::removeServer(name);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(name)) return paths;

    QStringList all = paths;
    QElapsedTimer total, quiet;
    total.start();
    quiet.start();
    while (quiet.elapsed() < kQuietMs && total.elapsed() < kMaximumMs) {
        if (!server.waitForNewConnection(50)) continue;
        while (QLocalSocket *socket = server.nextPendingConnection()) {
            QByteArray data;
            QElapsedTimer readTimer;
            readTimer.start();
            while (socket->state() == QLocalSocket::ConnectedState && readTimer.elapsed() < 2000) {
                if (socket->waitForReadyRead(100)) data += socket->readAll();
            }
            data += socket->readAll();
            for (const QString &p : QString::fromUtf8(data).split(u'\n', Qt::SkipEmptyParts)) all << p;
            delete socket;
        }
        quiet.restart();
    }
    server.close();
    all.removeDuplicates();
    return all;
}
