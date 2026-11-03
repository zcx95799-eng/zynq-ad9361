#ifndef MONITORSENDER_H
#define MONITORSENDER_H

#include <QObject>
#include <QMutex>

class QTcpSocket;
class QTimer;

struct SendSnapshot {
    quint64 txBytes = 0, rxBytes = 0;
    QByteArray reply;
};

// An independent TCP client: connecting/stopping TX never reconfigures RX.
class MonitorSender : public QObject {
    Q_OBJECT
public:
    explicit MonitorSender(QObject *parent = nullptr);
    void startClient(const QString &address, quint16 port, quint64 session);
    void stop();
    void send(const QByteArray &bytes, quint64 request);
    void sendPayload(const QByteArray &payload, quint64 request);
    void clearCounters();
    SendSnapshot snapshot(bool takeReply = false);

signals:
    void stateChanged(quint64 session, const QString &text, bool connected, bool failed);
    void sendResult(quint64 session, quint64 request, const QByteArray &preview, qint64 total, const QString &error);
    void payloadResult(quint64 session, quint64 request, quint32 sequence, quint32 result, const QString &error);
    void notice(quint64 session, const QString &text);

private:
    void receiveReply();
    void failPending(const QString &error);
    QTimer *ackTimer = nullptr;
    QByteArray ackBuffer;
    bool awaitingAck = false, payloadMode = false;
    quint64 pendingRequest = 0;
    quint32 pendingSequence = 0;
    QTcpSocket *socket = nullptr;
    quint64 session = 0;
    SendSnapshot state;
    QMutex mutex;
};

#endif
