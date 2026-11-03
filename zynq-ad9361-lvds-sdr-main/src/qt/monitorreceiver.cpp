#include "monitorreceiver.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QFile>
#include <QMutexLocker>
#include <QTimer>
#include <algorithm>

MonitorReceiver::MonitorReceiver(QObject *parent) : QObject(parent) {}

MonitorSnapshot MonitorReceiver::snapshot(bool takeMessages)
{
    QMutexLocker lock(&snapshotMutex);
    MonitorSnapshot result = published;
    if (takeMessages) { published.error.clear(); published.hex.clear(); }
    return result;
}

void MonitorReceiver::ensureRuntime()
{
    if (server) return;
    server = new QTcpServer(this);
    server->setProxy(QNetworkProxy::NoProxy);
    recording = new QFile(this);
    connect(server, &QTcpServer::newConnection, this, [this] {
        while (server->hasPendingConnections()) {
            QTcpSocket *incoming = server->nextPendingConnection();
            if (socket) {
                emit notice(session, QStringLiteral("已存在连接，拒绝额外客户端 %1。").arg(incoming->peerAddress().toString()));
                incoming->abort(); incoming->deleteLater();
            } else { resetStream(); attachSocket(incoming); }
        }
    });
}

void MonitorReceiver::resetStream()
{
    parser.reset(); rawBuffer.clear(); rawSequence = 0; hasSequence = false;
    state.latest = MonitorData(); state.lastDemodResult = MonitorData();
    state.fftHistory.clear(); state.successfulFft = AcquisitionFftFrame();
    state.spreadReference = SpreadReference(); referenceAssembler.reset(); fftAssembler.reset();
    state.frozenWaveform = MonitorData();
    state.revision = ++revision;
    publish();
}

void MonitorReceiver::configure(const MonitorLayout &newLayout, bool newRaw, quint64 newSession)
{
    ensureRuntime(); stop();
    decodeLayout = newLayout; raw = newRaw; session = newSession;
    resetStream();
}

QString MonitorReceiver::startServer(const QHostAddress &address, quint16 port,
                                    const MonitorLayout &layout, bool raw, quint64 session)
{
    configure(layout, raw, session);
    if (!server->listen(address, port)) {
        if (server->serverError() == QAbstractSocket::SocketAddressNotAvailableError)
            return QStringLiteral("%1；监听地址需填写电脑本机 IP 或 0.0.0.0，不能填写板卡 IP。")
                .arg(server->errorString());
        return server->errorString();
    }
    return {};
}

void MonitorReceiver::startClient(const QString &address, quint16 port,
                                 const MonitorLayout &layout, bool raw, quint64 session)
{
    configure(layout, raw, session);
    attachSocket(new QTcpSocket(this));
    socket->connectToHost(address, port);
}

void MonitorReceiver::startDemo(const MonitorLayout &layout)
{ configure(layout, false, session); }

void MonitorReceiver::stop(bool closeRecording)
{
    if (server) server->close();
    if (socket) {
        QTcpSocket *old = socket; socket = nullptr;
        old->disconnect(this); old->abort(); old->deleteLater();
    }
    parser.reset(); rawBuffer.clear(); hasSequence = false;
    if (closeRecording && recording) recording->close();
    publish();
}

void MonitorReceiver::attachSocket(QTcpSocket *incoming)
{
    socket = incoming; incoming->setParent(this);
    // Board sockets are raw LAN TCP, irrespective of desktop proxy settings.
    incoming->setProxy(QNetworkProxy::NoProxy);
    incoming->setReadBufferSize(8 * 1024 * 1024);
    auto connected = [this, incoming] {
        if (socket != incoming) return;
        incoming->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        incoming->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 8 * 1024 * 1024);
        emit stateChanged(session, QStringLiteral("已连接 %1:%2").arg(incoming->peerAddress().toString()).arg(incoming->peerPort()), true, false);
    };
    connect(incoming, &QTcpSocket::connected, this, connected);
    connect(incoming, &QTcpSocket::readyRead, this, &MonitorReceiver::receiveBytes);
    connect(incoming, &QTcpSocket::bytesWritten, this, [this, incoming](qint64 count) {
        if (socket != incoming) return;
        state.txBytes += quint64(count); publish();
    });
    connect(incoming, &QTcpSocket::disconnected, this, [this, incoming] {
        if (socket != incoming) return;
        // Disconnection bounds the remaining bytes; drain all of them before reset.
        while (incoming->bytesAvailable()) ingest(incoming->read(128 * 1024));
        socket = nullptr; incoming->deleteLater();
        parser.reset(); rawBuffer.clear(); hasSequence = false; publish();
        emit notice(session, QStringLiteral("对端断开连接。"));
        const bool listening = server->isListening();
        emit stateChanged(session, listening ? QStringLiteral("监听中，等待板端重新连接") : QStringLiteral("未连接"), false, !listening);
    });
    connect(incoming, &QTcpSocket::errorOccurred, this, [this, incoming](QAbstractSocket::SocketError error) {
        if (socket != incoming || error == QAbstractSocket::RemoteHostClosedError) return;
        emit notice(session, QStringLiteral("TCP 错误：%1").arg(incoming->errorString()));
        if (!server->isListening()) emit stateChanged(session, QStringLiteral("未连接"), false, true);
    });
    if (incoming->state() == QAbstractSocket::ConnectedState) connected();
    if (incoming->bytesAvailable()) receiveBytes();
}

void MonitorReceiver::receiveBytes()
{
    int processed = 0;
    while (socket && socket->bytesAvailable() && processed < 1024 * 1024) {
        const QByteArray bytes = socket->read(128 * 1024);
        if (bytes.isEmpty()) break;
        processed += bytes.size(); ingest(bytes);
    }
    if (socket && socket->bytesAvailable() && !readScheduled) {
        readScheduled = true;
        QTimer::singleShot(0, this, [this] { readScheduled = false; receiveBytes(); });
    }
}

void MonitorReceiver::ingest(const QByteArray &bytes)
{
    state.rxBytes += quint64(bytes.size());
    if (recording && recording->isOpen() && recording->write(bytes) != bytes.size()) {
        const QString error = QStringLiteral("原始流写入失败：%1").arg(recording->errorString());
        recording->close(); emit recordingFailed(error);
    }
    if (hexEnabled) pendingHex = bytes.left(64);
    QVector<MonitorFrame> packets;
    if (!raw) packets = parser.feed(bytes);
    else {
        rawBuffer.append(bytes);
        const int length = decodeLayout.payloadBytes();
        int offset = 0;
        while (rawBuffer.size() - offset >= length) {
            MonitorFrame frame; frame.type = decodeLayout.fftPoints == 0 ? 5 : 4;
            frame.hasWireHeader = false;
            frame.sequence = rawSequence++; frame.payload = rawBuffer.mid(offset, length);
            packets.append(std::move(frame)); offset += length;
        }
        rawBuffer.remove(0, offset);
    }
    for (const MonitorFrame &frame : packets) {
        MonitorData decoded; QString error;
        if (!decodeMonitorFrame(frame, decodeLayout, &decoded, &error)) {
            ++state.rejected; pendingError = error; continue;
        }
        if (hasSequence && frame.sequence != quint32(lastSequence + 1)) ++state.sequenceJumps;
        lastSequence = frame.sequence; hasSequence = true; ++state.frames;
        referenceAssembler.accept(decoded.spreadReference, &state.spreadReference);
        AcquisitionFftFrame fftFrame;
        if (fftAssembler.accept(decoded.acquisitionFft, &fftFrame) && fftFrame.successful()) {
            // Only a whole, genuine successful snapshot replaces the held view.
            // Ordinary FFT/IQ packets never overwrite it; partial windows remain invisible.
            state.successfulFft = fftFrame;
            state.fftHistory.clear();
            state.fftHistory.append(fftFrame);
        }
        if (!decoded.demodAllZero) ++state.nonzeroDemodSections;
        if (decoded.demodFrameValid) ++state.validDemodFrames;
        if (decoded.retainDemod()) {
            state.lastDemodResult = decoded;
            // Capture the first result here: several packets may precede a GUI refresh.
            if (freezeWaveform && state.frozenWaveform.i.isEmpty())
                state.frozenWaveform = decoded;
        }
        state.latest = std::move(decoded); state.revision = ++revision;
    }
    publish();
}

void MonitorReceiver::publish()
{
    state.discardedBytes = parser.discardedBytes(); state.invalidHeaders = parser.invalidHeaders();
    state.bufferedBytes = raw ? rawBuffer.size() : parser.bufferedBytes();
    QMutexLocker lock(&snapshotMutex);
    // Retain one diagnostic message until the GUI's next statistics update.
    QString error = pendingError.isEmpty() ? published.error : pendingError;
    QByteArray hex = pendingHex.isEmpty() ? published.hex : pendingHex;
    published = state; published.error = std::move(error); published.hex = std::move(hex);
    pendingError.clear(); pendingHex.clear();
}

void MonitorReceiver::send(const QByteArray &bytes, quint64 request)
{
    auto failed = [this, request](const QString &error) {
        emit notice(session, error);
        emit sendResult(session, request, {}, 0, error);
    };
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        failed(QStringLiteral("发送失败：当前没有 TCP 连接。")); return;
    }
    if (socket->bytesToWrite() + bytes.size() > 1024 * 1024) {
        failed(QStringLiteral("发送队列繁忙，请稍后重试。")); return;
    }
    const qint64 accepted = socket->write(bytes);
    if (accepted != bytes.size()) {
        failed(QStringLiteral("发送失败：仅入队 %1 / %2 B，%3").arg(std::max(qint64(0), accepted)).arg(bytes.size()).arg(socket->errorString())); return;
    }
    emit sendResult(session, request, bytes.left(512), accepted, {});
    emit notice(session, QStringLiteral("TX %1 B：%2").arg(bytes.size()).arg(QString::fromLatin1(bytes.left(64).toHex(' '))));
}

void MonitorReceiver::clearCounters()
{
    // A live half-frame stays intact. Counters and displayed snapshots are cleared.
    if (!socket) { parser.reset(); rawBuffer.clear(); rawSequence = 0; }
    referenceAssembler.reset(); fftAssembler.reset();
    state = MonitorSnapshot(); state.revision = ++revision; hasSequence = false;
    pendingError.clear(); pendingHex.clear();
    { QMutexLocker lock(&snapshotMutex); published = MonitorSnapshot(); }
    publish();
}

void MonitorReceiver::setFreezeWaveform(bool enabled)
{
    freezeWaveform = enabled;
    resumeWaveform();
}

void MonitorReceiver::resumeWaveform()
{
    state.frozenWaveform = MonitorData();
    state.revision = ++revision;
    publish();
}

void MonitorReceiver::setHexPreview(bool enabled)
{
    hexEnabled = enabled;
    if (!enabled) {
        pendingHex.clear(); QMutexLocker lock(&snapshotMutex); published.hex.clear();
    }
}

QString MonitorReceiver::startRecording(const QString &path)
{
    ensureRuntime(); recording->setFileName(path);
    if (!recording->open(QIODevice::WriteOnly)) return recording->errorString();
    return {};
}

void MonitorReceiver::stopRecording()
{ if (recording) recording->close(); }
