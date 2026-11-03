#ifndef MONITORRECEIVER_H
#define MONITORRECEIVER_H

#include <QObject>
#include <QMutex>
#include <QHostAddress>
#include "monitorprotocol.h"

class QTcpServer;
class QTcpSocket;
class QFile;

struct MonitorSnapshot {
    MonitorData latest, lastDemodResult, frozenWaveform;
    SpreadReference spreadReference;
    QVector<AcquisitionFftFrame> fftHistory;
    AcquisitionFftFrame successfulFft;
    quint64 rxBytes = 0, txBytes = 0, frames = 0, rejected = 0;
    quint64 sequenceJumps = 0, nonzeroDemodSections = 0;
    quint64 validDemodFrames = 0;
    quint64 discardedBytes = 0, invalidHeaders = 0, revision = 0;
    int bufferedBytes = 0;
    QByteArray hex;
    QString error;
};

// All socket, parser and recording operations run on the receiver thread.
// The GUI polls one bounded snapshot, rather than queueing a signal per frame.
class MonitorReceiver : public QObject {
    Q_OBJECT
public:
    explicit MonitorReceiver(QObject *parent = nullptr);
    MonitorSnapshot snapshot(bool takeMessages = false);
    QString startServer(const QHostAddress &address, quint16 port,
                        const MonitorLayout &layout, bool raw, quint64 session);
    void startClient(const QString &address, quint16 port,
                     const MonitorLayout &layout, bool raw, quint64 session);
    void startDemo(const MonitorLayout &layout);
    void stop(bool closeRecording = false);
    void send(const QByteArray &bytes, quint64 request = 0);
    void ingest(const QByteArray &bytes);
    void clearCounters();
    void setHexPreview(bool enabled);
    void setFreezeWaveform(bool enabled);
    void resumeWaveform();
    QString startRecording(const QString &path);
    void stopRecording();

signals:
    void stateChanged(quint64 session, const QString &text, bool connected, bool failed);
    void notice(quint64 session, const QString &text);
    void recordingFailed(const QString &text);
    void sendResult(quint64 session, quint64 request, const QByteArray &preview, qint64 total, const QString &error);

private:
    void ensureRuntime();
    void configure(const MonitorLayout &layout, bool raw, quint64 session);
    void attachSocket(QTcpSocket *incoming);
    void receiveBytes();
    void resetStream();
    void publish();
    QTcpServer *server = nullptr;
    QTcpSocket *socket = nullptr;
    QFile *recording = nullptr;
    MonitorLayout decodeLayout;
    MonitorParser parser;
    SpreadReferenceAssembler referenceAssembler;
    AcquisitionFftAssembler fftAssembler;
    QByteArray rawBuffer;
    quint32 rawSequence = 0, lastSequence = 0;
    quint64 session = 0, revision = 0;
    bool raw = false, hasSequence = false, hexEnabled = false, readScheduled = false;
    bool freezeWaveform = true;
    MonitorSnapshot state, published;
    QMutex snapshotMutex;
    QString pendingError;
    QByteArray pendingHex;
};
#endif
