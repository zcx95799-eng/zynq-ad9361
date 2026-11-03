#include "monitorsender.h"
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QMutexLocker>
#include <QTimer>
#include <QtEndian>
#include "sendpayloadprotocol.h"

MonitorSender::MonitorSender(QObject *parent) : QObject(parent) {}

void MonitorSender::startClient(const QString &address, quint16 port, quint64 newSession)
{
    stop(); session = newSession; payloadMode = false; ackBuffer.clear();
    auto *connection = new QTcpSocket(this); socket = connection;
    connection->setProxy(QNetworkProxy::NoProxy);
    connect(connection, &QTcpSocket::connected, this, [this, connection] {
        if (socket != connection) return;
        connection->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        emit stateChanged(session, QStringLiteral("发送连接已连接 %1:%2")
            .arg(connection->peerAddress().toString()).arg(connection->peerPort()), true, false);
    });
    connect(connection, &QTcpSocket::bytesWritten, this, [this, connection](qint64 count) {
        if (socket != connection) return;
        QMutexLocker lock(&mutex); state.txBytes += quint64(count);
    });
    connect(connection, &QTcpSocket::readyRead, this, &MonitorSender::receiveReply);
    connect(connection, &QTcpSocket::disconnected, this, [this, connection] {
        if (socket != connection) return;
        while (connection->bytesAvailable()) receiveReply();
        failPending(QStringLiteral("发送连接已断开，未收到匹配的 TXA1，无法确认 PL 提交。"));
        socket = nullptr; connection->deleteLater();
        emit stateChanged(session, QStringLiteral("发送连接已断开，接收监听继续"), false, true);
    });
    connect(connection, &QTcpSocket::errorOccurred, this, [this, connection](QAbstractSocket::SocketError error) {
        if (socket != connection || error == QAbstractSocket::RemoteHostClosedError) return;
        emit stateChanged(session, QStringLiteral("发送连接失败：%1").arg(connection->errorString()), false, true);
    });
    connection->setReadBufferSize(1024 * 1024);
    connection->connectToHost(address, port);
}

void MonitorSender::stop()
{
    failPending(QStringLiteral("发送连接已停止，无法确认 PL 提交。"));
    ackBuffer.clear(); payloadMode = false;
    if (!socket) return;
    auto *old = socket; socket = nullptr;
    old->disconnect(this); old->abort(); old->deleteLater();
}

void MonitorSender::receiveReply()
{
    if (!socket) return;
    const QByteArray bytes = socket->read(128 * 1024);
    if (!bytes.isEmpty()) {
        QMutexLocker lock(&mutex);
        state.rxBytes += quint64(bytes.size()); state.reply = bytes.left(64);
    }
    if (payloadMode) {
        ackBuffer += bytes;
        while (ackBuffer.size() >= 12) {
            if (!ackBuffer.startsWith("TXA1")) {
                failPending(QStringLiteral("应答帧头无效，期待 TXA1；无法确认 PL 提交。"));
                emit stateChanged(session, QStringLiteral("发送应答协议错误，接收继续运行"), false, true);
                stop(); return;
            }
            const quint32 sequence = qFromBigEndian<quint32>(ackBuffer.constData() + 4);
            const quint32 result = qFromBigEndian<quint32>(ackBuffer.constData() + 8);
            ackBuffer.remove(0, 12);
            if (awaitingAck && sequence == pendingSequence) {
                awaitingAck = false; ackTimer->stop();
                emit payloadResult(session, pendingRequest, sequence, result, {});
            } else emit notice(session, QStringLiteral("忽略非当前帧 TXA1：seq=%1，result=%2").arg(sequence).arg(result));
        }
    }
    if (socket->bytesAvailable()) {
        QTcpSocket *source = socket;
        QTimer::singleShot(0, this, [this, source] { if (socket == source) receiveReply(); });
    }
}

void MonitorSender::send(const QByteArray &bytes, quint64 request)
{
    QString error;
    if (awaitingAck)
        error = QStringLiteral("正在等待上一帧 TXA1，请稍后重试。");
    else if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        error = QStringLiteral("发送失败：请先连接独立发送端口。");
    else if (socket->bytesToWrite() + bytes.size() > 1024 * 1024)
        error = QStringLiteral("发送队列繁忙，请稍后重试。");
    else {
        payloadMode = false; ackBuffer.clear();
        if (socket->write(bytes) != bytes.size()) error = QStringLiteral("发送失败：%1").arg(socket->errorString());
    }
    emit sendResult(session, request, error.isEmpty() ? bytes.left(512) : QByteArray(),
                    error.isEmpty() ? bytes.size() : 0, error);
}

void MonitorSender::failPending(const QString &error)
{
    if (!awaitingAck) return;
    awaitingAck = false;
    if (ackTimer) ackTimer->stop();
    emit payloadResult(session, pendingRequest, pendingSequence, 0, error);
}

void MonitorSender::sendPayload(const QByteArray &input, quint64 request)
{
    QByteArray payload; QString error;
    if (awaitingAck) error = QStringLiteral("正在等待上一帧 TXA1，请稍后重试。");
    else TxPayload::normalize(input, &payload, &error);
    if (error.isEmpty() && (!socket || socket->state() != QAbstractSocket::ConnectedState))
        error = QStringLiteral("发送失败：请先连接独立发送端口。");
    else if (error.isEmpty() && socket->bytesToWrite() > 1024 * 1024 - 1036)
        error = QStringLiteral("发送队列繁忙，请稍后重试。");
    if (!error.isEmpty()) { emit sendResult(session, request, {}, 0, error); return; }
    if (!ackTimer) {
        ackTimer = new QTimer(this); ackTimer->setSingleShot(true);
        connect(ackTimer, &QTimer::timeout, this, [this] {
            failPending(QStringLiteral("TXA1 回执等待超过 6 秒，无法确认 PL 提交；请重新连接发送端口。"));
            emit stateChanged(session, QStringLiteral("TXA1 回执超时，接收继续运行"), false, true);
            stop();
        });
    }
    pendingRequest = request; pendingSequence = quint32(request);
    const QByteArray packet = TxPayload::frame(payload, pendingSequence);
    if (!payloadMode) ackBuffer.clear();
    payloadMode = true;
    if (socket->write(packet) != packet.size()) {
        emit sendResult(session, request, {}, 0, QStringLiteral("TXD1 写入失败：%1").arg(socket->errorString()));
        emit stateChanged(session, QStringLiteral("TXD1 写入失败，接收继续运行"), false, true);
        stop(); return;
    }
    awaitingAck = true; ackTimer->start(6000);
    emit sendResult(session, request, packet.left(512), packet.size(), {});
}

SendSnapshot MonitorSender::snapshot(bool takeReply)
{
    QMutexLocker lock(&mutex); const SendSnapshot result = state;
    if (takeReply) state.reply.clear();
    return result;
}

void MonitorSender::clearCounters()
{
    QMutexLocker lock(&mutex); state = SendSnapshot();
}
