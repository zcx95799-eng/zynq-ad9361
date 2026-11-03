#include <QtTest>
#include <QtEndian>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QNetworkInterface>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QPushButton>
#include <QLabel>
#include <QTableWidget>
#include <QTableView>
#include <QThread>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QTemporaryDir>
#include <QTimer>
#include <QApplication>
#include <QClipboard>
#include "monitorprotocol.h"
#include "mainwindow.h"
#include "plotwidget.h"
#include "spectrum.h"
#include "monitorsender.h"
#include "sendpayloadprotocol.h"
#include <cmath>
#include <complex>
#include <limits>
#include <thread>

// Independent fixture: exercise sign boundaries, section offsets and byte order.
static QByteArray fixture(quint32 sequence)
{
    QByteArray payload(6144, '\0');
    qToLittleEndian(quint32(0x07ff0800U), payload.data()); // I=-2048, Q=2047
    qToLittleEndian(quint32(0xf800ffffU), payload.data() + 4); // I=-1, Q=-2048
    qToLittleEndian(quint32(0x89abcdefU), payload.data() + 4096);
    qToLittleEndian(quint32(3), payload.data() + 5120);
    qToLittleEndian(quint32(0xfffffffcU), payload.data() + 5124); // FFT: 3-j4
    QByteArray header(16, '\0');
    header.replace(0, 4, "MONI"); header[4] = 1; header[5] = 4;
    qToBigEndian(quint16(0x1234), header.data() + 6);
    qToBigEndian(sequence, header.data() + 8);
    qToBigEndian(quint32(payload.size()), header.data() + 12);
    return header + payload;
}

static QByteArray downsampleFixture(quint32 sequence, bool zeroDemod)
{
    QByteArray payload(5120, '\0');
    qToLittleEndian(quint32(0x7fff8000U), payload.data());
    qToLittleEndian(quint32(0xffff2ee0U), payload.data() + 4); // I=12000,Q=-1
    if (!zeroDemod) qToLittleEndian(quint32(0x800000e4U), payload.data() + 4096);
    QByteArray header(16, '\0');
    header.replace(0, 4, "MONI"); header[4] = 1; header[5] = 5;
    qToBigEndian(quint16(zeroDemod ? 2 : 3), header.data() + 6);
    qToBigEndian(sequence, header.data() + 8);
    qToBigEndian(quint32(payload.size()), header.data() + 12);
    return header + payload;
}

class MonitorTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs); }
    void bytewiseAndCoalesced();
    void garbageAndInvalidHeaders();
    void decodeBoundariesAndLayouts();
    void rejectMismatchedFrames();
    void serverReceivesSendsAndReconnects();
    void clientConnectsAndDisconnects();
    void clientFailureRestoresControls();
    void recordingAndCsvExport();
    void downsampleAndPackedDemod();
    void sparseDemodSurvivesZeroPackets();
    void rawPayloadFragmentsAndCoalesces();
    void type5StreamAndCompatibility();
    void receiveWhileGuiIsBlocked();
    void pauseRefreshKeepsReceiving();
    void type5DemodValidityFlags();
    void validZeroDemodSurvivesFillers();
    void firstDemodWaveformCapture();
    void waveformFreezesUntilManualResume();
    void demodPrintSnapshotAndOrdering();
    void waveformWindowNavigationWhilePaused();
    void spectrumMatchesReferenceTransform();
    void spectrumTonesAndBoundaries();
    void spectrumFollowsDisplayedFrame();
    void spectrumAmplitudeTracksSignalStrength();
    void sendPreviewValidation();
    void sendConfiguredOverTcp();
    void independentSendAndReceiveStayConnected();
    void sameConnectionDuplexWhileSending();
    void tcpBoardConnectionsIgnoreApplicationProxy();
    void payloadAcknowledgementsAndTimeout();
    void payloadFilesAndDuplex();
    void demodSamplesPreserveRawWords();
    void sineDemodPayloadRoundTrip();
};

void MonitorTests::bytewiseAndCoalesced()
{
    MonitorParser parser;
    QVector<MonitorFrame> frames;
    const auto wire = fixture(0xffffffffU);
    for (char byte : wire) frames += parser.feed(QByteArray(1, byte));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames[0].sequence, 0xffffffffU);
    QCOMPARE(frames[0].flags, quint16(0x1234));
    QCOMPARE(parser.bufferedBytes(), 0);
    auto combined = parser.feed(fixture(0) + fixture(1) + fixture(2).left(23));
    QCOMPARE(combined.size(), 2);
    QCOMPARE(combined[1].sequence, quint32(1));
    QCOMPARE(parser.bufferedBytes(), 23);
    combined = parser.feed(fixture(2).mid(23));
    QCOMPARE(combined.size(), 1);
    QCOMPARE(combined[0].sequence, quint32(2));
    parser.feed(wire.left(500)); parser.reset();
    QCOMPARE(parser.feed(fixture(3)).size(), 1);
}

void MonitorTests::garbageAndInvalidHeaders()
{
    MonitorParser parser;
    auto badVersion = fixture(0).left(16); badVersion[4] = 2;
    auto oversized = fixture(0).left(16);
    qToBigEndian(quint32(MonitorParser::MaxPayloadBytes + 1), oversized.data() + 12);
    auto empty = fixture(0).left(16); qToBigEndian(quint32(0), empty.data() + 12);
    const auto frames = parser.feed(QByteArray(1000000, 'x') + badVersion + oversized + empty + fixture(8));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames[0].sequence, quint32(8));
    QCOMPARE(parser.invalidHeaders(), quint64(3));
    QCOMPARE(parser.discardedBytes(), quint64(1000048));
    QCOMPARE(parser.bufferedBytes(), 0);
    QVERIFY(parser.feed("junkMO").isEmpty());
    QCOMPARE(parser.feed(fixture(9).mid(2)).size(), 1);
}

void MonitorTests::decodeBoundariesAndLayouts()
{
    MonitorParser parser;
    auto frame = parser.feed(fixture(12))[0];
    MonitorData data; QString error; MonitorLayout layout;
    layout.fftPoints = 128; layout.iqEncoding = IqEncoding::I12Q12In16;
    QVERIFY2(decodeMonitorFrame(frame, layout, &data, &error), qPrintable(error));
    QCOMPARE(data.sequence, quint32(12));
    QCOMPARE(data.i.size(), 1024); QCOMPARE(data.q.size(), 1024);
    QCOMPARE(data.i[0], -2048.0); QCOMPARE(data.q[0], 2047.0);
    QCOMPARE(data.i[1], -1.0); QCOMPARE(data.q[1], -2048.0);
    QCOMPARE(data.demodWords[0], quint32(0x89abcdefU));
    QCOMPARE(data.fftReal[0], 3.0); QCOMPARE(data.fftImag[0], -4.0);
    QCOMPARE(data.fftMagnitude[0], 5.0); QCOMPARE(data.fftDb[0], 0.0);
    QCOMPARE(data.fftDb[1], -120.0);
    layout.iqEncoding = IqEncoding::Q12I12In16;
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    QCOMPARE(data.i[0], 2047.0); QCOMPARE(data.q[0], -2048.0);
    layout.iqEncoding = IqEncoding::PackedI12Q12;
    qToLittleEndian(quint32(0x007ff800U), frame.payload.data());
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    QCOMPARE(data.i[0], -2048.0); QCOMPARE(data.q[0], 2047.0);
    layout.iqEncoding = IqEncoding::SignedI16Q16;
    qToLittleEndian(quint32(0x7fff8000U), frame.payload.data());
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    QCOMPARE(data.i[0], -32768.0); QCOMPARE(data.q[0], 32767.0);
    qToLittleEndian(quint32(0x80000000U), frame.payload.data() + 5120);
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    QCOMPARE(data.fftReal[0], -2147483648.0);
    frame.payload.fill('\0');
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    for (double db : data.fftDb) { QVERIFY(std::isfinite(db)); QCOMPARE(db, -120.0); }
}

void MonitorTests::rejectMismatchedFrames()
{
    MonitorParser parser; auto frame = parser.feed(fixture(4))[0];
    MonitorData data; QString error; MonitorLayout layout;
    layout.fftPoints = 128; layout.iqEncoding = IqEncoding::I12Q12In16;
    frame.type = 2; QVERIFY(!decodeMonitorFrame(frame, layout, &data, &error)); QVERIFY(!error.isEmpty());
    frame.type = 4; frame.payload.chop(1); QVERIFY(!decodeMonitorFrame(frame, layout, &data, &error));
    frame = parser.feed(fixture(4))[0]; layout.demodBytes = 1023;
    QVERIFY(!decodeMonitorFrame(frame, layout, &data, &error));
    layout = MonitorLayout(); layout.iqSamples = 0;
    QVERIFY(!decodeMonitorFrame(frame, layout, &data, &error));
}

void MonitorTests::serverReceivesSendsAndReconnects()
{
    QTcpServer portReservation; QVERIFY(portReservation.listen(QHostAddress::LocalHost, 0));
    const quint16 port = portReservation.serverPort(); portReservation.close();
    MainWindow window;
    window.findChild<QComboBox *>("sendConnectionBox")->setCurrentIndex(0);
    window.findChild<QComboBox *>("profileBox")->setCurrentIndex(1);
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    auto *network = window.findChild<QPushButton *>("networkButton"); network->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    auto *state = window.findChild<QLabel *>("stateLabel");
    QTRY_VERIFY(state->text().contains(QStringLiteral("已连接")));
    const auto packet = fixture(40);
    peer.write(packet.left(7)); peer.flush(); QTest::qWait(30);
    peer.write(packet.mid(7) + fixture(41) + fixture(42)); peer.flush();
    auto *frame = window.findChild<QLabel *>("frameLabel");
    QTRY_VERIFY(frame->text().contains(QStringLiteral("序号 42")));
    auto *table = window.findChild<QTableWidget *>("demodTable");
    QCOMPARE(table->rowCount(), 256); QCOMPARE(table->item(0, 1)->text(), QString("0x89abcdef"));
    QTRY_VERIFY(window.findChild<QLabel *>("statsLabel")->text().contains(QStringLiteral("有效帧 3")));
    window.findChild<QPushButton *>("captureButton")->click();
    QTRY_VERIFY(peer.bytesAvailable() == 1); QCOMPARE(peer.readAll(), QByteArray("I"));
    window.findChild<QComboBox *>("sendFormat")->setCurrentIndex(1);
    window.findChild<QLineEdit *>("sendEdit")->setText("01 49");
    window.findChild<QPushButton *>("sendButton")->click();
    QTRY_VERIFY(peer.bytesAvailable() == 2); QCOMPARE(peer.readAll(), QByteArray::fromHex("0149"));
    window.findChild<QLineEdit *>("sendEdit")->setText("0Z");
    window.findChild<QPushButton *>("sendButton")->click();
    QVERIFY(window.findChild<QPlainTextEdit *>("logEdit")->toPlainText().contains(QStringLiteral("无效字符")));
    // A disconnect in mid-frame must never leak bytes into the next session.
    peer.write(fixture(43).left(100)); peer.flush(); QTest::qWait(30);
    peer.disconnectFromHost();
    QTRY_VERIFY(state->text().contains(QStringLiteral("等待板端重新连接")));
    QTcpSocket second; second.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(second.state(), QAbstractSocket::ConnectedState);
    second.write(fixture(99)); second.flush();
    QTRY_VERIFY(frame->text().contains(QStringLiteral("序号 99")));
    network->click(); QVERIFY(window.findChild<QComboBox *>("modeBox")->isEnabled());
}

void MonitorTests::clientConnectsAndDisconnects()
{
    QTcpServer remote; QVERIFY(remote.listen(QHostAddress::LocalHost, 0));
    MainWindow window; window.findChild<QComboBox *>("modeBox")->setCurrentIndex(1);
    window.findChild<QComboBox *>("profileBox")->setCurrentIndex(1);
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(remote.serverPort());
    window.findChild<QPushButton *>("networkButton")->click();
    QTRY_VERIFY(remote.hasPendingConnections());
    QTcpSocket *peer = remote.nextPendingConnection(); QVERIFY(peer);
    peer->write(fixture(123)); peer->flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 123")));
    peer->disconnectFromHost();
    QTRY_VERIFY(window.findChild<QComboBox *>("modeBox")->isEnabled());
    QCOMPARE(window.findChild<QLabel *>("stateLabel")->text(), QStringLiteral("未连接"));
}

void MonitorTests::clientFailureRestoresControls()
{
    QTcpServer remote; QVERIFY(remote.listen(QHostAddress::LocalHost, 0));
    const auto port = remote.serverPort(); remote.close();
    MainWindow window; auto *mode = window.findChild<QComboBox *>("modeBox"); mode->setCurrentIndex(1);
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTRY_VERIFY(mode->isEnabled());
    QCOMPARE(window.findChild<QLabel *>("stateLabel")->text(), QStringLiteral("未连接"));
}

void MonitorTests::recordingAndCsvExport()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    MainWindow window;
    window.findChild<QComboBox *>("profileBox")->setCurrentIndex(1);
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);

    // Select a path in Qt's non-native save dialog while its nested event loop runs.
    auto chooseFile = [&window](const QString &path, QPushButton *button) {
        QTimer chooser; chooser.setInterval(20);
        bool chosen = false;
        QObject::connect(&chooser, &QTimer::timeout, &window, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (auto *dialog = qobject_cast<QFileDialog *>(widget); dialog && dialog->isVisible()) {
                    dialog->selectFile(path); chosen = true; chooser.stop();
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                    break;
                }
            }
        });
        chooser.start(); button->click();
        return chosen;
    };
    const QString rawPath = directory.filePath("capture.bin");
    auto *record = window.findChild<QPushButton *>("recordButton");
    QVERIFY(chooseFile(rawPath, record));
    const QByteArray wire = fixture(17);
    peer.write(wire.left(11)); peer.flush(); QTest::qWait(30);
    peer.write(wire.mid(11)); peer.flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 17")));
    record->click();
    QFile raw(rawPath); QVERIFY(raw.open(QIODevice::ReadOnly)); QCOMPARE(raw.readAll(), wire);
    const QString csvPath = directory.filePath("frame.csv");
    QVERIFY(chooseFile(csvPath, window.findChild<QPushButton *>("exportButton")));
    QFile csv(csvPath); QVERIFY(csv.open(QIODevice::ReadOnly));
    const auto rows = QString::fromUtf8(csv.readAll()).remove('\r').split('\n', Qt::SkipEmptyParts);
    QCOMPARE(rows.size(), 5507);
    QCOMPARE(rows[1], QString("IQ,17,0,-2048,2047,,,"));
    QCOMPARE(rows[1025], QString("DEMOD,17,0,,,,,2309737967"));
    QCOMPARE(rows[1281], QString("FFT,17,0,3,-4,5,0,"));
    QCOMPARE(rows[1409], QString("DEMOD_SYMBOL,17,0,1,1,,,"));
    QCOMPARE(rows[5505], QString("DEMOD_STATUS,17,0,0,,,,"));
    QCOMPARE(rows[5506], QString("DEMOD_SYMBOL_STATUS,17,0,0,,,,"));
    for (const auto &row : rows) QCOMPARE(row.split(',').size(), 8);
    QByteArray filler = fixture(18);
    filler.replace(16 + 4096, 1024, QByteArray(1024, '\0'));
    peer.write(filler); peer.flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 18")));
    const QString heldPath = directory.filePath("held.csv");
    QVERIFY(chooseFile(heldPath, window.findChild<QPushButton *>("exportButton")));
    QFile held(heldPath); QVERIFY(held.open(QIODevice::ReadOnly));
    const auto heldRows = QString::fromUtf8(held.readAll()).remove('\r').split('\n', Qt::SkipEmptyParts);
    QCOMPARE(heldRows[1025], QString("DEMOD,18,0,,,,,0"));
    QCOMPARE(heldRows[1409], QString("DEMOD_SYMBOL,17,0,1,1,,,"));
}

void MonitorTests::downsampleAndPackedDemod()
{
    MonitorParser parser; MonitorData data; QString error; MonitorLayout layout;
    QCOMPARE(layout.payloadBytes(), 5120);
    auto frame = parser.feed(downsampleFixture(5, false))[0];
    QVERIFY2(decodeMonitorFrame(frame, layout, &data, &error), qPrintable(error));
    QCOMPARE(data.i[0], -32768.0); QCOMPARE(data.q[0], 32767.0);
    QCOMPARE(data.i[1], 12000.0); QCOMPARE(data.q[1], -1.0);
    QVERIFY(data.fftDb.isEmpty()); QVERIFY(!data.demodAllZero);
    QCOMPARE(data.demodWords[0], quint32(0x800000e4U));
    QVector<double> iBits, qBits;
    unpackDemodWords(data.demodWords, false, &iBits, &qBits);
    QCOMPARE(iBits.size(), 4096); QCOMPARE(qBits.size(), 4096);
    QCOMPARE(iBits.mid(0, 4), QVector<double>({0, 1, 0, 1}));
    QCOMPARE(qBits.mid(0, 4), QVector<double>({0, 0, 1, 1}));
    QCOMPARE(iBits[15], 0.0); QCOMPARE(qBits[15], 1.0);
    unpackDemodWords(data.demodWords, true, &iBits, &qBits);
    QCOMPARE(iBits[0], 0.0); QCOMPARE(qBits[0], 1.0);
    QCOMPARE(iBits[15], 0.0); QCOMPARE(qBits[15], 0.0);
    frame = parser.feed(downsampleFixture(6, true))[0];
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error)); QVERIFY(data.demodAllZero);
    frame.payload.chop(4); QVERIFY(!decodeMonitorFrame(frame, layout, &data, &error));
}

void MonitorTests::sparseDemodSurvivesZeroPackets()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    MainWindow window;
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    // A rare result and its following filler arrive before the GUI update.
    peer.write(downsampleFixture(201, false) + downsampleFixture(202, true)); peer.flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 202")));
    auto *status = window.findChild<QLabel *>("demodStatus");
    QVERIFY(status->text().contains(QStringLiteral("填零")));
    QVERIFY(status->text().contains(QStringLiteral("来源序号 201")));
    auto *raw = window.findChild<QTableWidget *>("demodTable");
    QCOMPARE(raw->item(0, 1)->text(), QString("0x800000e4"));
    auto *bits = window.findChild<QTableView *>("symbolTable");
    QCOMPARE(bits->model()->rowCount(), 4096);
    QCOMPARE(bits->model()->data(bits->model()->index(1, 3)).toString(), QString("1"));
    window.findChild<QComboBox *>("pairOrderBox")->setCurrentIndex(1);
    QTRY_COMPARE(bits->model()->data(bits->model()->index(0, 4)).toString(), QString("1"));
    window.findChild<QCheckBox *>("holdDemod")->setChecked(false);
    QTRY_COMPARE(raw->item(0, 1)->text(), QString("0x00000000"));
    QVERIFY(status->text().contains(QStringLiteral("来源序号 202")));
    peer.disconnectFromHost();
    QTRY_COMPARE(peer.state(), QAbstractSocket::UnconnectedState);
    QTcpSocket second; second.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(second.state(), QAbstractSocket::ConnectedState);
    window.findChild<QCheckBox *>("holdDemod")->setChecked(true);
    second.write(downsampleFixture(1, true)); second.flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 1 ")));
    QCOMPARE(raw->item(0, 1)->text(), QString("0x00000000"));
}

void MonitorTests::rawPayloadFragmentsAndCoalesces()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    MainWindow window; window.findChild<QComboBox *>("wireBox")->setCurrentIndex(1);
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    const auto payload = downsampleFixture(0, false).mid(16);
    peer.write(payload.left(3)); peer.flush(); QTest::qWait(30);
    QVERIFY(!window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 0")));
    peer.write(payload.mid(3) + downsampleFixture(0, true).mid(16)); peer.flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 1")));
    QCOMPARE(window.findChild<QTableWidget *>("demodTable")->item(0, 1)->text(), QString("0x800000e4"));
}

void MonitorTests::type5StreamAndCompatibility()
{
    MonitorParser parser; QVector<MonitorFrame> frames;
    const auto wire = downsampleFixture(7, false);
    for (char byte : wire) frames += parser.feed(QByteArray(1, byte));
    QCOMPARE(frames.size(), 1); QCOMPARE(frames[0].type, quint8(5));
    MonitorLayout layout; MonitorData type5, type4; QString error;
    QVERIFY2(decodeMonitorFrame(frames[0], layout, &type5, &error), qPrintable(error));
    QCOMPARE(type5.type, quint8(5)); QCOMPARE(type5.demodWords[0], quint32(0x800000e4U));
    auto compatible = frames[0]; compatible.type = 4;
    QVERIFY(decodeMonitorFrame(compatible, layout, &type4, &error));
    QCOMPARE(type4.type, quint8(4)); QCOMPARE(type4.i, type5.i);
    QCOMPARE(type4.q, type5.q); QCOMPARE(type4.demodWords, type5.demodWords);
    auto malformed = frames[0]; malformed.payload.chop(1);
    QVERIFY(!decodeMonitorFrame(malformed, layout, &type5, &error));
    QVERIFY(error.contains(QStringLiteral("载荷长度")));
    layout.fftPoints = 128; malformed = frames[0]; malformed.payload.resize(layout.payloadBytes());
    QVERIFY(!decodeMonitorFrame(malformed, layout, &type5, &error));
    QVERIFY(error.contains(QStringLiteral("FFT")));
    layout.fftPoints = 0; malformed.type = 6;
    QVERIFY(!decodeMonitorFrame(malformed, layout, &type5, &error));
    QVERIFY(error.contains("type=4 / type=5"));
    MonitorParser demoParser;
    QCOMPARE(demoParser.feed(makeDemoFrame(0, layout))[0].type, quint8(5));
    layout.fftPoints = 128;
    QCOMPARE(demoParser.feed(makeDemoFrame(0, layout))[0].type, quint8(4));
}

void MonitorTests::receiveWhileGuiIsBlocked()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    MainWindow window;
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QByteArray burst;
    constexpr int count = 2048;
    for (int n = 0; n < count; ++n) burst.append(downsampleFixture(n, n != 777));
    bool producerOk = false;
    std::thread producer([&] {
        QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
        if (!peer.waitForConnected(5000)) return;
        if (peer.write(burst) != burst.size()) return;
        while (peer.bytesToWrite()) if (!peer.waitForBytesWritten(5000)) return;
        producerOk = true;
        // Keep the session alive until reception is complete.
        QThread::msleep(200);
    });
    QElapsedTimer blocked; blocked.start();
    // Deliberately service NO GUI events: the old implementation cannot receive.
    while (window.receptionSnapshot().frames < count && blocked.elapsed() < 5000)
        QThread::msleep(2);
    const MonitorSnapshot received = window.receptionSnapshot();
    const qint64 receiveMs = blocked.elapsed();
    producer.join();
    QVERIFY(producerOk); QCOMPARE(received.frames, quint64(count));
    QCOMPARE(received.rxBytes, quint64(burst.size())); QCOMPARE(received.rejected, quint64(0));
    QCOMPARE(received.sequenceJumps, quint64(0)); QCOMPARE(received.latest.sequence, quint32(count - 1));
    QCOMPARE(received.lastDemodResult.sequence, quint32(777));
    QCOMPARE(received.validDemodFrames, quint64(1));
    QCOMPARE(received.nonzeroDemodSections, quint64(1)); QVERIFY(received.latest.demodAllZero);
    qInfo("GUI blocked: %d frames, %.2f MiB received in %lld ms", count,
          burst.size() / (1024.0 * 1024.0), static_cast<long long>(receiveMs));
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 2047")));
    QVERIFY(window.findChild<QLabel *>("demodStatus")->text().contains(QStringLiteral("来源序号 777")));
}

void MonitorTests::pauseRefreshKeepsReceiving()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    MainWindow window; window.show();
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    peer.write(downsampleFixture(1, false)); peer.flush();
    auto *label = window.findChild<QLabel *>("frameLabel");
    QTRY_VERIFY(label->text().contains(QStringLiteral("序号 1 ")));
    auto *pause = window.findChild<QCheckBox *>("pausePlots"); pause->setChecked(true);
    peer.write(downsampleFixture(2, false) + downsampleFixture(3, true)); peer.flush();
    QTRY_COMPARE(window.receptionSnapshot().frames, quint64(3));
    QTest::qWait(150); QVERIFY(label->text().contains(QStringLiteral("序号 1 ")));
    pause->setChecked(false);
    QTRY_VERIFY(label->text().contains(QStringLiteral("序号 3 ")));
    QVERIFY(window.findChild<QLabel *>("demodStatus")->text().contains(QStringLiteral("来源序号 2")));
}

void MonitorTests::type5DemodValidityFlags()
{
    MonitorParser parser; MonitorLayout layout; MonitorData data; QString error;
    auto zero = downsampleFixture(1, true);
    qToBigEndian(quint16(3), zero.data() + 6); // A complete frame can be all zero.
    auto frame = parser.feed(zero)[0];
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    QVERIFY(data.demodValidityKnown); QVERIFY(data.demodFrameValid);
    QVERIFY(data.demodAllZero); QVERIFY(data.retainDemod());
    frame = parser.feed(downsampleFixture(2, true))[0];
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    QVERIFY(data.demodValidityKnown); QVERIFY(!data.demodFrameValid);
    QVERIFY(data.demodAllZero); QVERIFY(!data.retainDemod());
    frame = parser.feed(downsampleFixture(3, false))[0]; frame.flags = 2;
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    QVERIFY(!data.demodAllZero); QVERIFY(!data.demodFrameValid);
    QVERIFY(!data.retainDemod()); // Nonzero bytes cannot override the validity bit.
    frame.type = 4;
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    QVERIFY(!data.demodValidityKnown); QVERIFY(!data.demodFrameValid);
    QVERIFY(data.retainDemod()); // Legacy compatibility is explicitly heuristic.
    frame.type = 5; frame.hasWireHeader = false;
    QVERIFY(decodeMonitorFrame(frame, layout, &data, &error));
    QVERIFY(!data.demodValidityKnown); QVERIFY(data.retainDemod());
    MonitorReceiver rawReceiver; rawReceiver.startClient("127.0.0.1", 1, layout, true, 1);
    rawReceiver.ingest(downsampleFixture(0, false).mid(16) + downsampleFixture(0, true).mid(16));
    const auto rawSnapshot = rawReceiver.snapshot();
    QCOMPARE(rawSnapshot.frames, quint64(2)); QCOMPARE(rawSnapshot.validDemodFrames, quint64(0));
    QVERIFY(!rawSnapshot.latest.demodValidityKnown);
    QVERIFY(!rawSnapshot.lastDemodResult.demodValidityKnown);
    QCOMPARE(rawSnapshot.lastDemodResult.sequence, quint32(0)); rawReceiver.stop();
    const auto demoFrames = parser.feed(makeDemoFrame(10, layout) + makeDemoFrame(11, layout));
    QCOMPARE(demoFrames.size(), 2); QCOMPARE(demoFrames[0].flags, quint16(3));
    QCOMPARE(demoFrames[1].flags, quint16(2));
}

void MonitorTests::validZeroDemodSurvivesFillers()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    MainWindow window;
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    auto validZero = downsampleFixture(11, true);
    qToBigEndian(quint16(3), validZero.data() + 6);
    auto invalidNonzero = downsampleFixture(12, false);
    qToBigEndian(quint16(2), invalidNonzero.data() + 6);
    peer.write(downsampleFixture(10, false) + validZero + invalidNonzero + downsampleFixture(13, true));
    peer.flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 13")));
    const auto snapshot = window.receptionSnapshot();
    QCOMPARE(snapshot.frames, quint64(4)); QCOMPARE(snapshot.validDemodFrames, quint64(2));
    QCOMPARE(snapshot.lastDemodResult.sequence, quint32(11));
    QVERIFY(snapshot.lastDemodResult.demodFrameValid); QVERIFY(snapshot.lastDemodResult.demodAllZero);
    QVERIFY(!snapshot.latest.demodFrameValid);
    auto *status = window.findChild<QLabel *>("demodStatus");
    QVERIFY(status->text().contains(QStringLiteral("填零")));
    QVERIFY(status->text().contains(QStringLiteral("来源序号 11")));
    QVERIFY(status->text().contains(QStringLiteral("完整解调帧累计 2")));
    QCOMPARE(window.findChild<QTableWidget *>("demodTable")->item(0, 1)->text(), QString("0x00000000"));
    auto *hold = window.findChild<QCheckBox *>("holdDemod"); hold->setChecked(false);
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 13")));
    hold->setChecked(true);
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 11")));
    peer.write(validZero.left(8)); peer.flush(); QTest::qWait(30);
    peer.write(validZero.mid(8)); peer.flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 11")));
    QVERIFY(status->text().contains(QStringLiteral("完整解调帧（全零数据）")));
}

void MonitorTests::firstDemodWaveformCapture()
{
    MonitorReceiver receiver; MonitorLayout layout; receiver.startDemo(layout);
    auto invalidNonzero = downsampleFixture(1, false);
    qToBigEndian(quint16(2), invalidNonzero.data() + 6);
    auto validZero = downsampleFixture(2, true);
    qToBigEndian(quint16(3), validZero.data() + 6);
    qToLittleEndian(quint32(0x00020001), validZero.data() + 16);
    receiver.ingest(downsampleFixture(0, true) + invalidNonzero);
    QVERIFY(receiver.snapshot().frozenWaveform.i.isEmpty());
    // First complete frame wins even when a newer result arrives in the same batch.
    receiver.ingest(validZero + downsampleFixture(3, false) + downsampleFixture(4, true));
    auto snapshot = receiver.snapshot();
    QCOMPARE(snapshot.latest.sequence, quint32(4));
    QCOMPARE(snapshot.lastDemodResult.sequence, quint32(3));
    QCOMPARE(snapshot.frozenWaveform.sequence, quint32(2));
    QCOMPARE(snapshot.frozenWaveform.i[0], 1.0); QCOMPARE(snapshot.frozenWaveform.q[0], 2.0);
    QVERIFY(snapshot.frozenWaveform.demodAllZero);
    receiver.resumeWaveform(); QVERIFY(receiver.snapshot().frozenWaveform.i.isEmpty());
    receiver.ingest(downsampleFixture(5, true)); QVERIFY(receiver.snapshot().frozenWaveform.i.isEmpty());
    receiver.ingest(downsampleFixture(6, false));
    QCOMPARE(receiver.snapshot().frozenWaveform.sequence, quint32(6));
    receiver.setFreezeWaveform(false); receiver.ingest(downsampleFixture(7, false));
    QVERIFY(receiver.snapshot().frozenWaveform.i.isEmpty());
    receiver.setFreezeWaveform(true); receiver.ingest(downsampleFixture(8, false));
    QCOMPARE(receiver.snapshot().frozenWaveform.sequence, quint32(8));
    receiver.clearCounters(); QVERIFY(receiver.snapshot().frozenWaveform.i.isEmpty());
    receiver.ingest(downsampleFixture(9, false));
    QCOMPARE(receiver.snapshot().frozenWaveform.sequence, quint32(9));
    receiver.startDemo(layout); QVERIFY(receiver.snapshot().frozenWaveform.i.isEmpty());
    auto legacy = downsampleFixture(10, false); legacy[5] = 4;
    receiver.ingest(legacy);
    QCOMPARE(receiver.snapshot().frozenWaveform.sequence, quint32(10));
    QVERIFY(!receiver.snapshot().frozenWaveform.demodValidityKnown);
}

void MonitorTests::waveformFreezesUntilManualResume()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    MainWindow window; window.show();
    auto *wave = window.findChild<QWidget *>("wavePlot");
    auto *qWave = window.findChild<QWidget *>("qWavePlot");
    auto *constellation = window.findChild<QWidget *>("constellationPlot");
    wave->setFixedSize(400, 240); constellation->setFixedSize(400, 240);
    qWave->setFixedSize(400, 240);
    auto *status = window.findChild<QLabel *>("waveformStatus");
    auto *resume = window.findChild<QPushButton *>("resumeWaveformButton");
    auto *freeze = window.findChild<QCheckBox *>("freezeWaveform");
    QVERIFY(freeze->isChecked()); QVERIFY(!resume->isEnabled());
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    auto changedIq = [](quint32 sequence, bool zeroDemod) {
        auto wire = downsampleFixture(sequence, zeroDemod);
        wire.replace(16, 4096, QByteArray(4096, '\0'));
        return wire;
    };
    peer.write(downsampleFixture(1, false) + changedIq(2, false) + changedIq(3, true)); peer.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 1")));
    QVERIFY(resume->isEnabled());
    const auto frozenWave = wave->grab().toImage();
    const auto frozenQWave = qWave->grab().toImage();
    const auto frozenConstellation = constellation->grab().toImage();
    peer.write(changedIq(4, false)); peer.flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 4 ")));
    QVERIFY(status->text().contains(QStringLiteral("来源序号 1")));
    QCOMPARE(wave->grab().toImage(), frozenWave);
    QCOMPARE(qWave->grab().toImage(), frozenQWave);
    QCOMPARE(constellation->grab().toImage(), frozenConstellation);
    resume->click();
    QTRY_VERIFY(status->text().contains(QStringLiteral("等待首次有效解调")));
    QVERIFY(!resume->isEnabled());
    QVERIFY(wave->grab().toImage() != frozenWave);
    QVERIFY(qWave->grab().toImage() != frozenQWave);
    QVERIFY(constellation->grab().toImage() != frozenConstellation);
    peer.write(changedIq(5, true)); peer.flush();
    QTRY_COMPARE(window.receptionSnapshot().latest.sequence, quint32(5));
    QVERIFY(window.receptionSnapshot().frozenWaveform.i.isEmpty());
    peer.write(downsampleFixture(6, false)); peer.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 6")));
    freeze->setChecked(false);
    QTRY_VERIFY(status->text().contains(QStringLiteral("自动冻结已关闭")));
    peer.write(changedIq(7, false)); peer.flush();
    QTRY_VERIFY(window.findChild<QLabel *>("frameLabel")->text().contains(QStringLiteral("序号 7 ")));
    QVERIFY(wave->grab().toImage() != frozenWave);
    freeze->setChecked(true);
    QTRY_VERIFY(status->text().contains(QStringLiteral("等待首次有效解调")));
    QVERIFY(window.receptionSnapshot().frozenWaveform.i.isEmpty());
    peer.write(downsampleFixture(8, false)); peer.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 8")));
    peer.disconnectFromHost(); QTRY_COMPARE(peer.state(), QAbstractSocket::UnconnectedState);
    QTcpSocket second; second.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(second.state(), QAbstractSocket::ConnectedState);
    second.write(changedIq(0, true)); second.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("等待首次有效解调")));
    QVERIFY(!resume->isEnabled());
}

void MonitorTests::demodPrintSnapshotAndOrdering()
{
    MainWindow window;
    auto *print = window.findChild<QPushButton *>("printDemodButton");
    auto *report = window.findChild<QPlainTextEdit *>("demodPrintEdit");
    auto *copy = window.findChild<QPushButton *>("copyDemodPrintButton");
    auto *save = window.findChild<QPushButton *>("saveDemodPrintButton");
    print->click(); QVERIFY(report->toPlainText().isEmpty());
    QVERIFY(!copy->isEnabled()); QVERIFY(!save->isEnabled());
    QVERIFY(window.findChild<QPlainTextEdit *>("logEdit")->toPlainText().contains(QStringLiteral("暂无可打印")));
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    peer.write(downsampleFixture(201, false) + downsampleFixture(202, true)); peer.flush();
    auto *status = window.findChild<QLabel *>("demodStatus");
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 201")));
    print->click(); const QString first = report->toPlainText();
    QVERIFY(first.contains(QStringLiteral("来源序号：201  type=5  flags=0x0003")));
    QVERIFY(first.contains(QStringLiteral("网络接收数据")));
    QVERIFY(first.contains(QStringLiteral("字数：256  字节数：1024  I/Q 符号数：4096")));
    QVERIFY(first.contains("WORD[0000] BYTE+0000 HEX=0x800000e4 DEC=2147483876 LE=e4 00 00 80 BIN=10000000000000000000000011100100"));
    QVERIFY(first.contains("WORD[0255] BYTE+1020 HEX=0x00000000"));
    QVERIFY(first.contains("I[0000..0063] = 0101000000000000" + QString(48, '0')));
    QVERIFY(first.contains("Q[0000..0063] = 0011000000000001" + QString(48, '0')));
    QVERIFY(first.contains("I[4032..4095] = " + QString(64, '0')));
    QVERIFY(first.contains("Q[4032..4095] = " + QString(64, '0')));
    copy->click(); QCOMPARE(QApplication::clipboard()->text(), first);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString path = directory.filePath("demod_print.txt");
    bool chosen = false; QTimer chooser;
    connect(&chooser, &QTimer::timeout, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QFileDialog *>(widget); dialog && dialog->isVisible()) {
                dialog->selectFile(path); chosen = true; chooser.stop();
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); break;
            }
        }
    });
    chooser.start(20); save->click(); QVERIFY(chosen);
    QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(file.readAll()), first);
    peer.write(downsampleFixture(203, false)); peer.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 203")));
    QCOMPARE(report->toPlainText(), first); // Printing stays fixed while reception continues.
    window.findChild<QComboBox *>("pairOrderBox")->setCurrentIndex(1);
    auto *table = window.findChild<QTableView *>("symbolTable");
    QTRY_COMPARE(table->model()->data(table->model()->index(0, 4)).toInt(), 1);
    print->click(); const QString high = report->toPlainText();
    QVERIFY(high.contains(QStringLiteral("高位符号先")));
    QVERIFY(high.contains("I[0000..0063] = 0000000000001010" + QString(48, '0')));
    QVERIFY(high.contains("Q[0000..0063] = 1000000000001100" + QString(48, '0')));
    auto *pause = window.findChild<QCheckBox *>("pausePlots"); pause->setChecked(true);
    peer.write(downsampleFixture(204, false)); peer.flush();
    QTRY_COMPARE(window.receptionSnapshot().latest.sequence, quint32(204));
    QTest::qWait(150); print->click();
    QVERIFY(report->toPlainText().contains(QStringLiteral("来源序号：203")));
    pause->setChecked(false);
    window.findChild<QCheckBox *>("holdDemod")->setChecked(false);
    peer.write(downsampleFixture(205, true)); peer.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 205")));
    print->click(); QVERIFY(report->toPlainText().contains(QStringLiteral("本包无完整解调帧")));
    auto validZero = downsampleFixture(206, true); qToBigEndian(quint16(3), validZero.data() + 6);
    peer.write(validZero); peer.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 206")));
    print->click(); QVERIFY(report->toPlainText().contains(QStringLiteral("完整解调帧；数据全零")));
    auto legacy = downsampleFixture(207, false); legacy[5] = 4;
    peer.write(legacy); peer.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 207")));
    print->click(); QVERIFY(report->toPlainText().contains(QStringLiteral("有效性未知")));
}

void MonitorTests::waveformWindowNavigationWhilePaused()
{
    MainWindow window; window.show(); window.startPreview();
    auto *wave = static_cast<PlotWidget *>(window.findChild<QWidget *>("wavePlot"));
    auto *qWave = static_cast<PlotWidget *>(window.findChild<QWidget *>("qWavePlot"));
    auto *size = window.findChild<QComboBox *>("waveWindowBox");
    auto *start = window.findChild<QSpinBox *>("waveStartSpin");
    auto *next = window.findChild<QPushButton *>("waveNextButton");
    auto *previous = window.findChild<QPushButton *>("wavePreviousButton");
    QTRY_COMPARE(wave->sampleCount(), 1024);
    QTRY_VERIFY(!window.receptionSnapshot().frozenWaveform.i.isEmpty());
    QCOMPARE(wave->sampleWindowSize(), 128);
    window.findChild<QCheckBox *>("pausePlots")->setChecked(true);
    const auto frozen = window.receptionSnapshot().frozenWaveform;
    auto *smooth = window.findChild<QCheckBox *>("smoothWaveform");
    QVERIFY(smooth->isChecked());
    const auto smoothImage = wave->grab().toImage();
    const auto bitImage = window.findChild<QWidget *>("bitPlot")->grab().toImage();
    smooth->setChecked(false);
    QVERIFY(wave->grab().toImage() != smoothImage);
    QCOMPARE(window.findChild<QWidget *>("bitPlot")->grab().toImage(), bitImage);
    smooth->setChecked(true);
    QCOMPARE(wave->grab().toImage(), smoothImage);
    next->click(); QCOMPARE(start->value(), 128);
    QCOMPARE(wave->sampleWindowStart(), 128); QCOMPARE(qWave->sampleWindowStart(), 128);
    previous->click(); QCOMPARE(start->value(), 0); QVERIFY(!previous->isEnabled());
    start->setValue(9999); QCOMPARE(start->value(), 896); QVERIFY(!next->isEnabled());
    QCOMPARE(wave->sampleWindowSize(), 128); QCOMPARE(qWave->sampleWindowStart(), 896);
    size->setCurrentIndex(size->findData(512)); QCOMPARE(start->value(), 512);
    QCOMPARE(wave->sampleWindowSize(), 512); QCOMPARE(qWave->sampleWindowSize(), 512);
    size->setCurrentIndex(size->findData(0)); QCOMPARE(start->value(), 0);
    QCOMPARE(wave->sampleWindowSize(), 1024); QVERIFY(!start->isEnabled());
    QVERIFY(!next->isEnabled()); QVERIFY(!previous->isEnabled());
    QCOMPARE(window.receptionSnapshot().frozenWaveform.i, frozen.i);
    QCOMPARE(window.receptionSnapshot().frozenWaveform.q, frozen.q);
    QCOMPARE(window.receptionSnapshot().frozenWaveform.sequence, frozen.sequence);

    // A requested window larger than the data and a one-sample frame remain drawable.
    PlotWidget shortWave(PlotWidget::IWaveform); shortWave.resize(400, 240);
    shortWave.setData({-3, 4, -5}); shortWave.setSampleWindow(896, 128);
    QCOMPARE(shortWave.sampleWindowStart(), 0); QCOMPARE(shortWave.sampleWindowSize(), 3);
    QVERIFY(!shortWave.grab().isNull());
    shortWave.setData({7}); QCOMPARE(shortWave.sampleWindowSize(), 1);
    QVERIFY(!shortWave.grab().isNull());
    shortWave.clear(); QCOMPARE(shortWave.sampleWindowSize(), 0);
    QVERIFY(!shortWave.grab().isNull());
}

void MonitorTests::spectrumMatchesReferenceTransform()
{
    const QVector<double> i{2, -3, 1, 5, -2, 4, 0, -1};
    const QVector<double> q{-1, 2, 0, -4, 3, 1, -2, 5};
    const double pi = std::acos(-1.0);
    for (auto input : {SpectrumInput::ComplexIq, SpectrumInput::IChannel, SpectrumInput::QChannel}) {
        for (bool hann : {false, true}) {
            const auto actual = calculateIqSpectrum(i, q, 8, input, hann);
            QVector<double> expected(8);
            double peak = 0;
            // Independent direct DFT verifies all FFT bins, window weights, and shift.
            for (int index = 0; index < 8; ++index) {
                std::complex<double> sum(0, 0);
                const int bin = index - 4;
                for (int n = 0; n < 8; ++n) {
                    const double weight = hann ? 0.5 - 0.5 * std::cos(2 * pi * n / 7) : 1;
                    const std::complex<double> sample(input == SpectrumInput::QChannel ? q[n] : i[n],
                        input == SpectrumInput::ComplexIq ? q[n] : 0);
                    sum += weight * sample * std::polar(1.0, -2 * pi * bin * n / 8);
                }
                expected[index] = std::abs(sum); peak = std::max(peak, expected[index]);
            }
            QCOMPARE(actual.fftSize, 8); QCOMPARE(actual.binWidthMHz, 1.0);
            for (int index = 0; index < 8; ++index) {
                QCOMPARE(actual.frequencyMHz[index], double(index - 4));
                const double db = 20 * std::log10(std::max(expected[index] / peak, 1e-6));
                QVERIFY(std::abs(actual.relativeDb[index] - db) < 1e-8);
                const double fullScaleDb = 20 * std::log10(std::max(expected[index] / ((hann ? 3.5 : 8) * 32768), 1e-6));
                QVERIFY(std::abs(actual.dbfs[index] - fullScaleDb) < 1e-8);
            }
        }
    }
}

void MonitorTests::spectrumTonesAndBoundaries()
{
    const double pi = std::acos(-1.0);
    QVector<double> i(64), q(64);
    for (int bin : {7, -9, 0, -32}) {
        for (int n = 0; n < 64; ++n) {
            i[n] = 100 * std::cos(2 * pi * bin * n / 64);
            q[n] = 100 * std::sin(2 * pi * bin * n / 64);
        }
        for (bool hann : {false, true}) {
            const auto result = calculateIqSpectrum(i, q, 8, SpectrumInput::ComplexIq, hann);
            QVERIFY(result.hasSignal); QCOMPARE(result.peakFrequencyMHz, bin * 0.125);
            QCOMPARE(result.relativeDb[bin + 32], 0.0);
        }
    }
    i.fill(0); q.fill(0);
    const auto zero = calculateIqSpectrum(i, q, 8, SpectrumInput::ComplexIq, true);
    QVERIFY(!zero.hasSignal);
    for (double db : zero.relativeDb) QCOMPARE(db, -120.0);
    for (double db : zero.dbfs) QCOMPARE(db, -120.0);
    i.resize(70); q.resize(70);
    auto prefix = calculateIqSpectrum(i, q, 8, SpectrumInput::ComplexIq, true);
    QCOMPARE(prefix.fftSize, 64); QCOMPARE(prefix.inputSamples, 70);
    q.resize(10);
    QCOMPARE(calculateIqSpectrum(i, q, 8, SpectrumInput::ComplexIq, false).fftSize, 8);
    QCOMPARE(calculateIqSpectrum(i, {}, 8, SpectrumInput::IChannel, false).fftSize, 64);
    QCOMPARE(calculateIqSpectrum({}, q, 8, SpectrumInput::QChannel, false).fftSize, 8);
    QCOMPARE(calculateIqSpectrum({}, {}, 8, SpectrumInput::ComplexIq, true).fftSize, 0);
    QCOMPARE(calculateIqSpectrum({1}, {0}, 8, SpectrumInput::ComplexIq, true).fftSize, 0);
    const auto two = calculateIqSpectrum({1, 1}, {0, 0}, 8, SpectrumInput::ComplexIq, true);
    QVERIFY(two.hasSignal); QCOMPARE(two.peakFrequencyMHz, 0.0);
    QCOMPARE(calculateIqSpectrum(i, q, 0, SpectrumInput::ComplexIq, true).fftSize, 0);
    i[0] = std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(calculateIqSpectrum(i, q, 8, SpectrumInput::ComplexIq, true).fftSize, 0);
}

void MonitorTests::spectrumFollowsDisplayedFrame()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort(); reservation.close();
    MainWindow window; window.show();
    window.findChild<QTabWidget *>()->setCurrentWidget(window.findChild<QWidget *>("spectrumPage"));
    auto *status = window.findChild<QLabel *>("spectrumStatus");
    auto *plot = static_cast<PlotWidget *>(window.findChild<QWidget *>("spectrumPlot"));
    auto *rate = window.findChild<QDoubleSpinBox *>("sampleRateSpin");
    QCOMPARE(rate->value(), 20.0);
    auto *bandwidth = window.findChild<QLabel *>("spectrumBandwidthLabel");
    QVERIFY(bandwidth->text().contains("20 MHz"));
    QVERIFY(bandwidth->text().contains("-10")); QVERIFY(bandwidth->text().contains("+10"));
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    auto tone = [](quint32 sequence, int bin, bool filler) {
        auto wire = downsampleFixture(sequence, filler);
        const double pi = std::acos(-1.0);
        for (int n = 0; n < 1024; ++n) {
            const quint32 i = quint32(qint32(std::lround(12000 * std::cos(2 * pi * bin * n / 1024)))) & 0xffffU;
            const quint32 q = quint32(qint32(std::lround(12000 * std::sin(2 * pi * bin * n / 1024)))) & 0xffffU;
            qToLittleEndian(i | (q << 16), wire.data() + 16 + 4 * n);
        }
        return wire;
    };
    peer.write(tone(101, 32, false) + tone(102, -128, true)); peer.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 101")));
    QVERIFY(status->text().contains("0.625000 MHz")); QCOMPARE(plot->sampleCount(), 1024);
    QVERIFY(status->text().contains("19.531 kHz"));
    auto *amplitude = window.findChild<QComboBox *>("spectrumAmplitudeBox");
    QCOMPARE(amplitude->currentIndex(), 0); QVERIFY(status->text().contains(" dB"));
    QVERIFY(!status->text().contains("dBFS"));
    QVERIFY(amplitude->currentText().contains("dB")); QVERIFY(!amplitude->currentText().contains("dBFS"));
    QVERIFY(window.findChild<QLabel *>("spectrumReferenceLabel")->text().contains(QStringLiteral("固定参考 dB")));
    auto *level = window.findChild<QDoubleSpinBox *>("spectrumLevelSpin");
    auto *division = window.findChild<QComboBox *>("spectrumDivisionBox");
    QCOMPARE(level->value(), 0.0); QCOMPARE(division->currentData().toInt(), 10);
    const auto axisImage = plot->grab().toImage();
    const auto originalStatus = status->text();
    level->setValue(-10); QVERIFY(plot->grab().toImage() != axisImage);
    QCOMPARE(status->text(), originalStatus); QCOMPARE(plot->sampleCount(), 1024);
    level->setValue(0); QCOMPARE(plot->grab().toImage(), axisImage);
    division->setCurrentIndex(0); QVERIFY(plot->grab().toImage() != axisImage);
    QCOMPARE(status->text(), originalStatus);
    division->setCurrentIndex(1); QCOMPARE(plot->grab().toImage(), axisImage);
    const auto fixedImage = plot->grab().toImage();
    amplitude->setCurrentIndex(1); QVERIFY(status->text().contains("0.00 dB"));
    QVERIFY(plot->grab().toImage() != fixedImage);
    amplitude->setCurrentIndex(0); QCOMPARE(plot->grab().toImage(), fixedImage);
    const auto spectrumImage = plot->grab().toImage();
    window.findChild<QComboBox *>("waveWindowBox")->setCurrentIndex(0);
    window.findChild<QSpinBox *>("waveStartSpin")->setValue(128);
    QCOMPARE(plot->grab().toImage(), spectrumImage);
    window.findChild<QCheckBox *>("smoothWaveform")->setChecked(false);
    QCOMPARE(plot->grab().toImage(), spectrumImage);
    window.findChild<QCheckBox *>("pausePlots")->setChecked(true);
    peer.write(tone(103, -128, false)); peer.flush();
    QTRY_COMPARE(window.receptionSnapshot().latest.sequence, quint32(103));
    rate->setValue(6.4);
    QVERIFY(bandwidth->text().contains("6.4 MHz"));
    QVERIFY(status->text().contains(QStringLiteral("来源序号 101")));
    QVERIFY(status->text().contains("0.200000 MHz"));
    window.findChild<QComboBox *>("spectrumInputBox")->setCurrentIndex(1);
    QVERIFY(status->text().contains(QStringLiteral("I 通道")));
    window.findChild<QComboBox *>("spectrumWindowBox")->setCurrentIndex(1);
    QVERIFY(status->text().contains(QStringLiteral("来源序号 101")));
    window.findChild<QCheckBox *>("pausePlots")->setChecked(false);
    window.findChild<QCheckBox *>("freezeWaveform")->setChecked(false);
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 103")));
    window.findChild<QComboBox *>("spectrumInputBox")->setCurrentIndex(0);
    QVERIFY(status->text().contains("-0.800000 MHz"));
    window.findChild<QPushButton *>("clearButton")->click();
    QCOMPARE(plot->sampleCount(), 0); QVERIFY(status->text().contains(QStringLiteral("等待")));
}

void MonitorTests::spectrumAmplitudeTracksSignalStrength()
{
    const double pi = std::acos(-1.0);
    // Known digital amplitudes verify a fixed reference and window/length gain.
    for (int count : {64, 128}) {
        for (bool hann : {false, true}) {
            QVector<double> i(count), q(count);
            for (int n = 0; n < count; ++n) {
                i[n] = 16384 * std::cos(2 * pi * 7 * n / count);
                q[n] = 16384 * std::sin(2 * pi * 7 * n / count);
            }
            const auto strong = calculateIqSpectrum(i, q, 20, SpectrumInput::ComplexIq, hann);
            for (int n = 0; n < count; ++n) { i[n] /= 2; q[n] /= 2; }
            const auto weak = calculateIqSpectrum(i, q, 20, SpectrumInput::ComplexIq, hann);
            QVERIFY(std::abs(strong.peakDbfs - (-6.020599913)) < 1e-8);
            QVERIFY(std::abs(weak.peakDbfs - (-12.041199827)) < 1e-8);
            QVERIFY(std::abs(strong.peakDbfs - weak.peakDbfs - 6.020599913) < 1e-8);
            QCOMPARE(strong.relativeDb, weak.relativeDb);
            QCOMPARE(strong.peakFrequencyMHz, weak.peakFrequencyMHz);
            const auto legacyReference = calculateIqSpectrum(i, q, 20, SpectrumInput::ComplexIq, hann, 2048);
            QVERIFY(std::abs(legacyReference.peakDbfs - weak.peakDbfs - 24.082399653) < 1e-8);
        }
    }
    // A real half-scale sine has two sidebands, each at quarter scale (-12 dBFS).
    QVector<double> cosine(64);
    for (int n = 0; n < 64; ++n) cosine[n] = 16384 * std::cos(2 * pi * 7 * n / 64);
    const auto real = calculateIqSpectrum(cosine, {}, 20, SpectrumInput::IChannel, false);
    QVERIFY(std::abs(real.dbfs[32 + 7] - (-12.041199827)) < 1e-8);
    QVERIFY(std::abs(real.dbfs[32 - 7] - (-12.041199827)) < 1e-8);
    QCOMPARE(calculateIqSpectrum(cosine, {}, 20, SpectrumInput::IChannel, false, 0).fftSize, 0);
}

void MonitorTests::sendPreviewValidation()
{
    MainWindow window;
    window.findChild<QComboBox *>("sendConnectionBox")->setCurrentIndex(0);
    auto *format = window.findChild<QComboBox *>("sendPageFormat");
    auto *input = window.findChild<QPlainTextEdit *>("sendPayloadEdit");
    auto *table = window.findChild<QTableWidget *>("sendByteTable");
    auto *status = window.findChild<QLabel *>("sendPreviewStatus");
    format->setCurrentIndex(1); input->setPlainText("00 7F 80 FF\n49");
    QCOMPARE(table->rowCount(), 5);
    QCOMPARE(table->item(3, 1)->text(), QString("0xff"));
    QCOMPARE(table->item(3, 2)->text(), QString("255"));
    QCOMPARE(table->item(2, 3)->text(), QString("10000000"));
    QCOMPARE(table->item(4, 4)->text(), QString("I"));
    QCOMPARE(static_cast<PlotWidget *>(window.findChild<QWidget *>("sendBytePlot"))->sampleCount(), 5);
    input->setPlainText("01 ZZ"); QVERIFY(status->text().contains(QStringLiteral("无效字符")));
    QCOMPARE(table->rowCount(), 0);
    input->setPlainText("F"); QVERIFY(status->text().contains(QStringLiteral("偶数")));
    format->setCurrentIndex(0); input->setPlainText(QStringLiteral("中文"));
    QCOMPARE(table->rowCount(), 6);
    window.findChild<QCheckBox *>("sendAppendLine")->setChecked(true);
    QCOMPARE(table->rowCount(), 8);
    input->setPlainText("I");
    window.findChild<QCheckBox *>("sendCyclic")->setChecked(true);
    window.findChild<QPushButton *>("sendConfiguredButton")->click();
    auto *history = window.findChild<QPlainTextEdit *>("sendHistoryEdit");
    QTRY_VERIFY(history->toPlainText().contains(QStringLiteral("没有 TCP 连接")));
    QVERIFY(!history->toPlainText().contains(QStringLiteral("已入队")));
    QVERIFY(!window.findChild<QTimer *>("sendTimer")->isActive());
}

void MonitorTests::sendConfiguredOverTcp()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 port = reservation.serverPort(); reservation.close();
    MainWindow window; window.show();
    window.findChild<QComboBox *>("sendConnectionBox")->setCurrentIndex(0);
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    QTRY_VERIFY(window.findChild<QLabel *>("stateLabel")->text().contains(QStringLiteral("已连接")));
    auto *input = window.findChild<QPlainTextEdit *>("sendPayloadEdit");
    auto *button = window.findChild<QPushButton *>("sendConfiguredButton");
    input->setPlainText(QStringLiteral("中文"));
    window.findChild<QCheckBox *>("sendAppendLine")->setChecked(true);
    window.findChild<QSpinBox *>("sendRepeatSpin")->setValue(2);
    button->click();
    const QByteArray expected = (QStringLiteral("中文").toUtf8() + "\r\n").repeated(2);
    QTRY_COMPARE(peer.bytesAvailable(), qint64(expected.size())); QCOMPARE(peer.readAll(), expected);
    window.findChild<QComboBox *>("sendPageFormat")->setCurrentIndex(1);
    window.findChild<QSpinBox *>("sendRepeatSpin")->setValue(1);
    input->setPlainText("00 7F 80 FF 49 01 22 44 AA 55");
    button->click(); QTRY_COMPARE(peer.bytesAvailable(), qint64(10));
    QCOMPARE(peer.readAll(), QByteArray::fromHex("007f80ff49012244aa55"));
    auto *history = window.findChild<QPlainTextEdit *>("sendHistoryEdit");
    QTRY_VERIFY(history->toPlainText().contains(QStringLiteral("已入队 10 B")));
    window.findChild<QTabWidget *>()->setCurrentWidget(window.findChild<QWidget *>("sendPage"));
    QTest::qWait(100); QVERIFY(window.grab().save("D:/QTXIANGMU/tcp/build/send-config-preview.png"));
    window.findChild<QCheckBox *>("sendCyclic")->setChecked(true);
    window.findChild<QSpinBox *>("sendIntervalSpin")->setValue(100);
    button->click(); QTRY_VERIFY(peer.bytesAvailable() >= 20);
    window.findChild<QPushButton *>("sendStopButton")->click();
    QVERIFY(!window.findChild<QTimer *>("sendTimer")->isActive());
    peer.readAll(); QTest::qWait(200); peer.readAll(); QTest::qWait(200); QCOMPARE(peer.bytesAvailable(), qint64(0));
    window.findChild<QPushButton *>("networkButton")->click();
}

void MonitorTests::independentSendAndReceiveStayConnected()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 rxPort = reservation.serverPort(); reservation.close();
    QTcpServer transmitTarget; QVERIFY(transmitTarget.listen(QHostAddress::LocalHost, 0));
    MainWindow window; window.show();
    window.findChild<QComboBox *>("sendProtocolBox")->setCurrentIndex(1);
    window.findChild<QCheckBox *>("freezeWaveform")->setChecked(false);
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(rxPort);
    window.findChild<QLineEdit *>("sendAddressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("sendPortSpin")->setValue(transmitTarget.serverPort());
    auto *rxButton = window.findChild<QPushButton *>("networkButton");
    auto *txButton = window.findChild<QPushButton *>("sendNetworkButton");
    rxButton->click();
    QTcpSocket source; source.connectToHost(QHostAddress::LocalHost, rxPort);
    QTRY_COMPARE(source.state(), QAbstractSocket::ConnectedState);
    txButton->click(); QTRY_VERIFY(transmitTarget.hasPendingConnections());
    QTcpSocket *destination = transmitTarget.nextPendingConnection(); QVERIFY(destination);
    QTRY_VERIFY(window.findChild<QLabel *>("sendStateLabel")->text().contains(QStringLiteral("已连接")));
    QCOMPARE(source.state(), QAbstractSocket::ConnectedState);

    // Independent source sends sine frames while the PC cyclically writes TX.
    quint32 sequence = 0;
    QTimer stream;
    connect(&stream, &QTimer::timeout, &window, [&] {
        QByteArray frame = downsampleFixture(++sequence, true);
        for (int n = 0; n < 1024; ++n) {
            const qint16 value = qint16(std::round(12000 * std::sin(2 * 3.141592653589793 * n / 64)));
            qToLittleEndian(quint32(quint16(value)), frame.data() + 16 + n * 4);
        }
        source.write(frame); source.flush();
    });
    stream.start(20);
    window.findChild<QComboBox *>("sendPageFormat")->setCurrentIndex(1);
    window.findChild<QPlainTextEdit *>("sendPayloadEdit")->setPlainText("98 04 00 00 25 09 00 00");
    window.findChild<QCheckBox *>("sendCyclic")->setChecked(true);
    window.findChild<QSpinBox *>("sendIntervalSpin")->setValue(100);
    window.findChild<QPushButton *>("sendConfiguredButton")->click();
    const QByteArray payload = QByteArray::fromHex("9804000025090000");
    QTRY_VERIFY(destination->bytesAvailable() >= payload.size() * 3);
    const QByteArray sent = destination->readAll();
    QVERIFY(sent.size() % payload.size() == 0);
    QCOMPARE(sent, payload.repeated(sent.size() / payload.size()));
    QTRY_VERIFY(window.receptionSnapshot().frames >= 10);
    const quint64 initialFrames = window.receptionSnapshot().frames;
    destination->write("OK"); destination->flush();
    window.findChild<QPushButton *>("captureButton")->click();
    QTRY_COMPARE(source.bytesAvailable(), qint64(1)); QCOMPARE(source.readAll(), QByteArray("I"));
    QTRY_VERIFY(window.receptionSnapshot().frames > initialFrames);
    QCOMPARE(window.receptionSnapshot().latest.i[16], 12000.0);
    QCOMPARE(window.receptionSnapshot().latest.i[48], -12000.0);
    auto *plot = static_cast<PlotWidget *>(window.findChild<QWidget *>("duplexReceivePlot"));
    QTRY_COMPARE(plot->sampleCount(), 1024);
    window.findChild<QTabWidget *>()->setCurrentWidget(window.findChild<QWidget *>("sendPage"));
    QTest::qWait(1100);
    QVERIFY(window.findChild<QLabel *>("duplexStatusLabel")->text().contains(QStringLiteral("应答 2 B")));
    QVERIFY(window.grab().save("D:/QTXIANGMU/tcp/build/duplex-preview.png"));

    // TX disconnect must not stop RX or require a new RX connection.
    destination->disconnectFromHost();
    QTRY_VERIFY(window.findChild<QLabel *>("sendStateLabel")->text().contains(QStringLiteral("已断开")));
    QCOMPARE(source.state(), QAbstractSocket::ConnectedState);
    const quint64 beforeTxStop = window.receptionSnapshot().frames;
    QTRY_VERIFY(window.receptionSnapshot().frames > beforeTxStop);
    QVERIFY(!window.findChild<QTimer *>("sendTimer")->isActive());
    window.findChild<QPushButton *>("sendConfiguredButton")->click();
    QVERIFY(window.findChild<QLabel *>("sendPreviewStatus")->text().contains(QStringLiteral("请先连接")));
    QVERIFY(!window.findChild<QTimer *>("sendTimer")->isActive());

    // Reconnect TX, then stop RX; the second connection must keep transmitting.
    txButton->click(); QTRY_VERIFY(transmitTarget.hasPendingConnections());
    QTcpSocket *secondDestination = transmitTarget.nextPendingConnection(); QVERIFY(secondDestination);
    QTRY_VERIFY(window.findChild<QLabel *>("sendStateLabel")->text().contains(QStringLiteral("已连接")));
    window.findChild<QPushButton *>("sendConfiguredButton")->click();
    QTRY_VERIFY(secondDestination->bytesAvailable() >= payload.size()); secondDestination->readAll();
    stream.stop(); rxButton->click();
    QCOMPARE(secondDestination->state(), QAbstractSocket::ConnectedState);
    QTRY_VERIFY(secondDestination->bytesAvailable() >= payload.size());
    QVERIFY(window.findChild<QTimer *>("sendTimer")->isActive());
    window.findChild<QPushButton *>("captureButton")->click();
    QVERIFY(window.findChild<QTimer *>("sendTimer")->isActive());
    txButton->click(); QVERIFY(!window.findChild<QTimer *>("sendTimer")->isActive());
}

void MonitorTests::sameConnectionDuplexWhileSending()
{
    QTcpServer remote; QVERIFY(remote.listen(QHostAddress::LocalHost, 0));
    MainWindow window;
    window.findChild<QComboBox *>("sendConnectionBox")->setCurrentIndex(0);
    window.findChild<QComboBox *>("modeBox")->setCurrentIndex(1);
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(remote.serverPort());
    window.findChild<QPushButton *>("networkButton")->click(); QTRY_VERIFY(remote.hasPendingConnections());
    QTcpSocket *peer = remote.nextPendingConnection(); QVERIFY(peer);
    QTRY_VERIFY(window.findChild<QLabel *>("stateLabel")->text().contains(QStringLiteral("已连接")));
    peer->write(downsampleFixture(10, true)); peer->flush();
    QTRY_COMPARE(window.receptionSnapshot().frames, quint64(1));
    window.findChild<QPlainTextEdit *>("sendPayloadEdit")->setPlainText("command");
    window.findChild<QPushButton *>("sendConfiguredButton")->click();
    QTRY_COMPARE(peer->bytesAvailable(), qint64(7)); QCOMPARE(peer->readAll(), QByteArray("command"));
    peer->write(downsampleFixture(11, true)); peer->flush();
    QTRY_COMPARE(window.receptionSnapshot().frames, quint64(2));
    QCOMPARE(window.receptionSnapshot().latest.sequence, quint32(11));
    QCOMPARE(peer->state(), QAbstractSocket::ConnectedState);
}

void MonitorTests::demodSamplesPreserveRawWords()
{
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 port = reservation.serverPort(); reservation.close();
    MainWindow window; window.show();
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(port);
    window.findChild<QComboBox *>("pairOrderBox")->setCurrentIndex(1);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, port);
    QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
    QByteArray frame = downsampleFixture(77, false);
    for (int n = 0; n < 256; ++n) qToLittleEndian(quint32(n == 255 ? 0xef0000ffU : quint32(n)), frame.data() + 16 + 4096 + n * 4);
    peer.write(frame); peer.flush();
    auto *plot = static_cast<PlotWidget *>(window.findChild<QWidget *>("demodPlot"));
    auto *status = window.findChild<QLabel *>("demodViewStatus");
    auto *format = window.findChild<QComboBox *>("demodDisplayBox");
    auto *start = window.findChild<QSpinBox *>("demodStartSpin");
    auto *count = window.findChild<QComboBox *>("demodWindowBox");
    auto *tabs = window.findChild<QTabWidget *>(); tabs->setCurrentWidget(window.findChild<QWidget *>("demodPage"));
    QTRY_COMPARE(plot->sampleCount(), 256); QCOMPARE(format->currentIndex(), 0);
    QVERIFY(status->text().contains(QStringLiteral("疑似递增计数")));
    QVERIFY(status->text().contains("ef0000ff"));
    QCOMPARE(window.receptionSnapshot().latest.demodWords.last(), quint32(0xef0000ff));
    QCOMPARE(window.findChild<QTableWidget *>("demodTable")->item(255, 1)->text(), QString("0xef0000ff"));
    window.findChild<QCheckBox *>("pausePlots")->setChecked(true);
    count->setCurrentIndex(0); start->setValue(192);
    QCOMPARE(plot->sampleWindowStart(), 192); QCOMPARE(plot->sampleWindowSize(), 64);
    // The final raw word remains present as I=255, Q=-4352 in I16/Q16 mode.
    QVector<double> i, q(256, 0);
    for (int n = 0; n < 256; ++n) i.append(n);
    q[255] = -4352;
    PlotWidget reference(PlotWidget::DemodSamples); reference.setFont(plot->font()); reference.resize(plot->size());
    reference.setSmoothDisplay(true); reference.setData(i, q); reference.setSampleWindow(192, 64);
    QCOMPARE(plot->grab().toImage().convertToFormat(QImage::Format_RGB32), reference.grab().toImage().convertToFormat(QImage::Format_RGB32));
    window.findChild<QPushButton *>("demodPreviousButton")->click(); QCOMPARE(start->value(), 128);
    window.findChild<QPushButton *>("demodNextButton")->click(); QCOMPARE(start->value(), 192);
    window.findChild<QComboBox *>("pairOrderBox")->setCurrentIndex(0);
    // Symbol order changes must not change the payload's byte/sample format.
    QCOMPARE(plot->grab().toImage().convertToFormat(QImage::Format_RGB32), reference.grab().toImage().convertToFormat(QImage::Format_RGB32));
    format->setCurrentIndex(1); QCOMPARE(plot->sampleCount(), 256); QCOMPARE(start->value(), 0);
    start->setValue(192); QVERIFY(status->text().contains(QStringLiteral("32位数值")));
    window.findChild<QCheckBox *>("pausePlots")->setChecked(true);
    QVector<double> signedWords;
    for (int n = 0; n < 255; ++n) signedWords.append(n);
    signedWords.append(-285212417.0);
    reference.setMode(PlotWidget::Demod); reference.setData(signedWords); reference.setSampleWindow(192, 64);
    QCOMPARE(plot->grab().toImage().convertToFormat(QImage::Format_RGB32), reference.grab().toImage().convertToFormat(QImage::Format_RGB32));
    format->setCurrentIndex(2); start->setValue(192); signedWords[255] = 4009754879.0;
    reference.setMode(PlotWidget::DemodUnsigned); reference.setData(signedWords); reference.setSampleWindow(192, 64);
    QCOMPARE(plot->grab().toImage().convertToFormat(QImage::Format_RGB32), reference.grab().toImage().convertToFormat(QImage::Format_RGB32));
    format->setCurrentIndex(3); QCOMPARE(plot->sampleCount(), 512); start->setValue(448);
    QVector<double> interleaved;
    for (int n = 0; n < 256; ++n) { interleaved.append(i[n]); interleaved.append(q[n]); }
    reference.setMode(PlotWidget::DemodSamples); reference.setData(interleaved); reference.setSampleWindow(448, 64);
    QCOMPARE(plot->grab().toImage().convertToFormat(QImage::Format_RGB32), reference.grab().toImage().convertToFormat(QImage::Format_RGB32));
    format->setCurrentIndex(0); count->setCurrentIndex(1); start->setValue(128);
    QTest::qWait(50); QVERIFY(window.grab().save("D:/QTXIANGMU/tcp/build/demod-numeric-preserved-preview.png"));
}

static QByteArray payloadAck(quint32 sequence, quint32 result)
{
    QByteArray ack(12, '\0'); ack.replace(0, 4, "TXA1");
    qToBigEndian(sequence, ack.data() + 4); qToBigEndian(result, ack.data() + 8);
    return ack;
}

void MonitorTests::sineDemodPayloadRoundTrip()
{
    QTcpServer board; QVERIFY(board.listen(QHostAddress::LocalHost, 0));
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 rxPort = reservation.serverPort(); reservation.close();
    MainWindow window; window.show();
    window.findChild<QLineEdit *>("sendAddressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("sendPortSpin")->setValue(board.serverPort());
    window.findChild<QPushButton *>("sendNetworkButton")->click();
    QTRY_VERIFY(board.hasPendingConnections()); auto *upload = board.nextPendingConnection();
    QTRY_VERIFY(window.findChild<QLabel *>("sendStateLabel")->text().contains(QStringLiteral("已连接")));
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(rxPort);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket source; source.connectToHost(QHostAddress::LocalHost, rxPort);
    QTRY_COMPARE(source.state(), QAbstractSocket::ConnectedState);
    window.findChild<QPushButton *>("sendSineButton")->click();
    window.findChild<QPushButton *>("sendConfiguredButton")->click();
    QTRY_COMPARE(upload->bytesAvailable(), qint64(1036)); const QByteArray request = upload->readAll();
    QCOMPARE(request.left(4), QByteArray("TXD1"));
    upload->write(payloadAck(qFromBigEndian<quint32>(request.constData() + 4), 0)); upload->flush();
    // Mock radio decoding returns the transmitted payload in DEMOD, not the ADC IQ section.
    QByteArray received = downsampleFixture(900, false);
    received.replace(16 + 4096, 1024, request.mid(12));
    source.write(received.left(7)); source.flush(); QTest::qWait(20);
    QCOMPARE(window.receptionSnapshot().frames, quint64(0));
    source.write(received.mid(7)); source.flush();
    auto *plot = static_cast<PlotWidget *>(window.findChild<QWidget *>("demodPlot"));
    auto *status = window.findChild<QLabel *>("demodViewStatus");
    auto *tabs = window.findChild<QTabWidget *>(); tabs->setCurrentWidget(window.findChild<QWidget *>("demodPage"));
    QTRY_COMPARE(plot->sampleCount(), 256); QVERIFY(status->text().contains(QStringLiteral("来源序号 900")));
    QVERIFY(status->text().contains(QStringLiteral("数据取自接收解调区")));
    QVector<double> i, q(256, 0);
    for (int n = 0; n < 256; ++n) i.append(std::round(12000 * std::sin(2 * 3.14159265358979323846 * n / 64)));
    QCOMPARE(i[16], 12000.0); QCOMPARE(i[48], -12000.0);
    PlotWidget reference(PlotWidget::DemodSamples); reference.setFont(plot->font()); reference.resize(plot->size());
    reference.setSmoothDisplay(true); reference.setData(i, q); reference.setSampleWindow(0, 256);
    QCOMPARE(plot->grab().toImage().convertToFormat(QImage::Format_RGB32), reference.grab().toImage().convertToFormat(QImage::Format_RGB32));
    QTest::qWait(50); QVERIFY(window.grab().save("D:/QTXIANGMU/tcp/build/demod-sine-samples-preview.png"));
    const auto sineImage = plot->grab().toImage();
    source.write(downsampleFixture(901, true)); source.flush();
    QTRY_COMPARE(window.receptionSnapshot().latest.sequence, quint32(901));
    QVERIFY(status->text().contains(QStringLiteral("来源序号 900"))); QCOMPARE(plot->grab().toImage(), sineImage);
    // Return a deliberately changed recovered sample. The graph must follow RX,
    // while the editor still holds the original sine and TX never sends again.
    qToBigEndian(quint32(902), received.data() + 8);
    qToLittleEndian(qint16(-500), received.data() + 16 + 4096 + 16 * 4);
    qToLittleEndian(qint16(8000), received.data() + 16 + 4096 + 16 * 4 + 2);
    source.write(received); source.flush();
    QTRY_VERIFY(status->text().contains(QStringLiteral("来源序号 902")));
    i[16] = -500; q[16] = 8000; reference.setData(i, q);
    QCOMPARE(plot->grab().toImage().convertToFormat(QImage::Format_RGB32), reference.grab().toImage().convertToFormat(QImage::Format_RGB32));
    QVERIFY(plot->grab().toImage() != sineImage); QCOMPARE(upload->bytesAvailable(), qint64(0));
    window.findChild<QCheckBox *>("pausePlots")->setChecked(true);
    window.findChild<QComboBox *>("demodWindowBox")->setCurrentIndex(0);
    window.findChild<QSpinBox *>("demodStartSpin")->setValue(128);
    reference.setSampleWindow(128, 64);
    QCOMPARE(plot->grab().toImage().convertToFormat(QImage::Format_RGB32), reference.grab().toImage().convertToFormat(QImage::Format_RGB32));
    window.findChild<QCheckBox *>("demodSmooth")->setChecked(false); reference.setSmoothDisplay(false);
    QCOMPARE(plot->grab().toImage().convertToFormat(QImage::Format_RGB32), reference.grab().toImage().convertToFormat(QImage::Format_RGB32));
}

void MonitorTests::payloadAcknowledgementsAndTimeout()
{
    QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    MonitorSender sender;
    QSignalSpy results(&sender, &MonitorSender::payloadResult), writes(&sender, &MonitorSender::sendResult);
    QSignalSpy states(&sender, &MonitorSender::stateChanged), notices(&sender, &MonitorSender::notice);
    sender.startClient("127.0.0.1", server.serverPort(), 1);
    QTRY_VERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection();
    QTRY_VERIFY(!states.isEmpty() && states.last()[2].toBool());
    sender.sendPayload(QByteArray(4096, '\0'), 20);
    QVERIFY(writes.last()[4].toString().contains("1024")); QCOMPARE(peer->bytesAvailable(), qint64(0));
    const QByteArray payload = TxPayload::sine();
    for (quint32 result = 0; result <= 5; ++result) {
        const quint32 sequence = 100 + result;
        sender.sendPayload(payload, sequence);
        QTRY_COMPARE(peer->bytesAvailable(), qint64(1036));
        const QByteArray packet = peer->readAll();
        QCOMPARE(packet.left(4), QByteArray("TXD1"));
        QCOMPARE(qFromBigEndian<quint32>(packet.constData() + 4), sequence);
        QCOMPARE(qFromBigEndian<quint32>(packet.constData() + 8), quint32(1024));
        QCOMPARE(packet.mid(12), payload);
        sender.sendPayload(payload, 200); QVERIFY(writes.last()[4].toString().contains("TXA1"));
        peer->write(payloadAck(sequence - 1, 0)); peer->flush();
        QTRY_COMPARE(notices.size(), int(result * 2 + 1)); QCOMPARE(results.size(), int(result));
        const QByteArray ack = payloadAck(sequence, result);
        for (int n = 0; n < 11; ++n) { peer->write(ack.mid(n, 1)); peer->flush(); QTest::qWait(2); }
        QCOMPARE(results.size(), int(result));
        peer->write(ack.right(1) + payloadAck(sequence, 0)); peer->flush();
        QTRY_COMPARE(results.size(), int(result + 1));
        QCOMPARE(results.last()[2].toUInt(), sequence); QCOMPARE(results.last()[3].toUInt(), result);
        QVERIFY(results.last()[4].toString().isEmpty());
        QTRY_COMPARE(notices.size(), int(2 * (result + 1)));
    }
    sender.sendPayload(payload, 300); QTRY_COMPARE(peer->bytesAvailable(), qint64(1036)); peer->readAll();
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 7, 7500);
    QVERIFY(results.last()[4].toString().contains(QStringLiteral("6 秒")));
    QTRY_COMPARE(peer->state(), QAbstractSocket::UnconnectedState);
    sender.startClient("127.0.0.1", server.serverPort(), 2);
    QTRY_VERIFY(server.hasPendingConnections()); peer = server.nextPendingConnection();
    QTRY_VERIFY(states.last()[2].toBool());
    sender.sendPayload(payload, 301); QTRY_COMPARE(peer->bytesAvailable(), qint64(1036)); peer->readAll();
    peer->write(payloadAck(301, 0).left(7)); peer->flush(); QTest::qWait(20);
    peer->disconnectFromHost(); QTRY_COMPARE(results.size(), 8);
    QVERIFY(results.last()[4].toString().contains(QStringLiteral("未收到")));
    sender.startClient("127.0.0.1", server.serverPort(), 3);
    QTRY_VERIFY(server.hasPendingConnections()); peer = server.nextPendingConnection();
    QTRY_VERIFY(states.last()[2].toBool());
    sender.sendPayload(payload, 302); QTRY_COMPARE(peer->bytesAvailable(), qint64(1036)); peer->readAll();
    peer->write(payloadAck(302, 0)); peer->flush(); QTRY_COMPARE(results.size(), 9);
    QVERIFY(results.last()[4].toString().isEmpty());
    sender.sendPayload(payload, 303); QTRY_COMPARE(peer->bytesAvailable(), qint64(1036)); peer->readAll();
    peer->write("BAD!12345678"); peer->flush(); QTRY_COMPARE(results.size(), 10);
    QVERIFY(results.last()[4].toString().contains(QStringLiteral("帧头无效")));
    QTRY_COMPARE(peer->state(), QAbstractSocket::UnconnectedState);
}

void MonitorTests::payloadFilesAndDuplex()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QByteArray payload(1024, '\0');
    // Independent sign-boundary fixture rather than a copy of the sine generator.
    qToLittleEndian(qint16(-32768), payload.data()); qToLittleEndian(qint16(32767), payload.data() + 2);
    QByteArray table("index\tI_int16\tQ_int16\tI_LE_Q_LE\n");
    for (int n = 0; n < 256; ++n) table += QByteArray::number(n) + '\t'
        + QByteArray::number(n ? 0 : -32768) + '\t' + QByteArray::number(n ? 0 : 32767)
        + '\t' + payload.mid(n * 4, 4).toHex(' ') + '\n';
    const QList<QPair<QString, QByteArray>> files = {{"iq.bin", payload}, {"iq.hex", payload.toHex(' ')}, {"iq.txt", table},
        {"frame.bin", TxPayload::frame(payload, 99)}};
    QTcpServer tx; QVERIFY(tx.listen(QHostAddress::LocalHost, 0));
    QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const quint16 rxPort = reservation.serverPort(); reservation.close();
    MainWindow window; window.show();
    window.findChild<QLineEdit *>("sendAddressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("sendPortSpin")->setValue(tx.serverPort());
    window.findChild<QPushButton *>("sendNetworkButton")->click();
    QTRY_VERIFY(tx.hasPendingConnections()); auto *peer = tx.nextPendingConnection();
    QTRY_VERIFY(window.findChild<QLabel *>("sendStateLabel")->text().contains(QStringLiteral("已连接")));
    window.findChild<QLineEdit *>("addressEdit")->setText("127.0.0.1");
    window.findChild<QSpinBox *>("portSpin")->setValue(rxPort);
    window.findChild<QPushButton *>("networkButton")->click();
    QTcpSocket source; source.connectToHost(QHostAddress::LocalHost, rxPort);
    QTRY_COMPARE(source.state(), QAbstractSocket::ConnectedState);
    auto *button = window.findChild<QPushButton *>("sendConfiguredButton");
    auto *input = window.findChild<QPlainTextEdit *>("sendPayloadEdit");
    auto *status = window.findChild<QLabel *>("sendPreviewStatus");
    for (const auto &entry : files) {
        const QString path = directory.filePath(entry.first);
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(entry.second), qint64(entry.second.size())); file.close();
        QTimer chooser; chooser.setInterval(20); bool chosen = false;
        connect(&chooser, &QTimer::timeout, &window, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (auto *dialog = qobject_cast<QFileDialog *>(widget); dialog && dialog->isVisible()) {
                    dialog->selectFile(path); chosen = true; chooser.stop();
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); break;
                }
            }
        });
        chooser.start(); window.findChild<QPushButton *>("sendLoadButton")->click(); QVERIFY(chosen);
        QCOMPARE(QByteArray::fromHex(input->toPlainText().toLatin1()), payload);
        button->click(); QTRY_COMPARE(peer->bytesAvailable(), qint64(1036));
        const QByteArray packet = peer->readAll(); QCOMPARE(packet.mid(12), payload);
        peer->write(payloadAck(qFromBigEndian<quint32>(packet.constData() + 4), 0)); peer->flush();
        QTRY_VERIFY(status->text().contains(QStringLiteral("板端已提交")));
    }
    QString description, error; QByteArray decoded;
    QFile bad(directory.filePath("bad.txt")); QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("index\tI_int16\tQ_int16\tI_LE_Q_LE\n0\t32768\t0\t00 80 00 00\n"); bad.close();
    QVERIFY(!TxPayload::loadFile(bad.fileName(), &decoded, &description, &error));
    window.findChild<QPushButton *>("sendSineButton")->click();
    QCOMPARE(static_cast<PlotWidget *>(window.findChild<QWidget *>("sendIqPlot"))->sampleCount(), 256);
    QVERIFY(status->text().contains("1036"));
    window.findChild<QCheckBox *>("sendCyclic")->setChecked(true);
    window.findChild<QSpinBox *>("sendIntervalSpin")->setValue(100);
    button->click(); QTRY_COMPARE(peer->bytesAvailable(), qint64(1036));
    const QByteArray first = peer->readAll();
    QTRY_VERIFY(status->text().contains(QStringLiteral("等待 TXA1")));
    window.findChild<QPushButton *>("captureButton")->click();
    QTRY_COMPARE(source.bytesAvailable(), qint64(1)); QCOMPARE(source.readAll(), QByteArray("I"));
    QTRY_VERIFY(status->text().contains(QStringLiteral("入队 1 B")));
    QVERIFY(!status->text().contains("TXD1 seq="));
    QCOMPARE(qFromLittleEndian<qint16>(first.constData() + 12 + 16 * 4), qint16(12000));
    QCOMPARE(qFromLittleEndian<qint16>(first.constData() + 12 + 48 * 4), qint16(-12000));
    source.write(downsampleFixture(1, true) + downsampleFixture(2, true)); source.flush();
    QTRY_COMPARE(window.receptionSnapshot().frames, quint64(2));
    QTest::qWait(350); QCOMPARE(peer->bytesAvailable(), qint64(0));
    peer->write(payloadAck(qFromBigEndian<quint32>(first.constData() + 4), 0)); peer->flush();
    QTRY_COMPARE(peer->bytesAvailable(), qint64(1036)); const QByteArray second = peer->readAll();
    QVERIFY(qFromBigEndian<quint32>(second.constData() + 4) > qFromBigEndian<quint32>(first.constData() + 4));
    peer->write(payloadAck(qFromBigEndian<quint32>(second.constData() + 4), 4)); peer->flush();
    QTRY_VERIFY(status->text().contains("UP_ACK_TIMEOUT"));
    QVERIFY(!window.findChild<QTimer *>("sendTimer")->isActive());
    QCOMPARE(peer->state(), QAbstractSocket::ConnectedState);
    source.write(downsampleFixture(3, true)); source.flush(); QTRY_COMPARE(window.receptionSnapshot().frames, quint64(3));
    window.findChild<QTabWidget *>()->setCurrentWidget(window.findChild<QWidget *>("sendPage"));
    window.findChild<QTabWidget *>("sendViews")->setCurrentIndex(1);
    QTest::qWait(50); QVERIFY(window.grab().save("D:/QTXIANGMU/tcp/build/txd1-preview.png"));
}

void MonitorTests::tcpBoardConnectionsIgnoreApplicationProxy()
{
    // Qt exempts loopback addresses from proxying, so use an assigned local
    // IPv4 address to exercise the same path as a LAN board connection.
    QHostAddress localAddress;
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp)
            || !interface.flags().testFlag(QNetworkInterface::IsRunning)) continue;
        for (const QNetworkAddressEntry &entry : interface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()
                && (address.toIPv4Address() >> 16) != 0xa9fe) {
                localAddress = address; break;
            }
        }
        if (!localAddress.isNull()) break;
    }
    if (localAddress.isNull()) QSKIP("A non-loopback local IPv4 address is required for proxy regression coverage.");
    struct ProxyRestore {
        QNetworkProxy original = QNetworkProxy::applicationProxy();
        ~ProxyRestore() { QNetworkProxy::setApplicationProxy(original); }
    } restore;
    // HTTP proxying cannot implement a raw TCP listener. Keep it configured
    // globally to reproduce the screenshot, while board connections go direct.
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 9));
    QTcpServer remote; remote.setProxy(QNetworkProxy::NoProxy);
    QVERIFY(remote.listen(localAddress, 0));

    MonitorReceiver receiver;
    const QString error = receiver.startServer(localAddress, 0, MonitorLayout(), false, 1);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QTcpSocket source; source.setProxy(QNetworkProxy::NoProxy);
    source.connectToHost(localAddress, receiver.findChild<QTcpServer *>()->serverPort());
    QTRY_COMPARE(source.state(), QAbstractSocket::ConnectedState);
    source.write(downsampleFixture(77, true)); source.flush();
    QTRY_COMPARE(receiver.snapshot().latest.sequence, quint32(77));
    receiver.send("RX-command");
    QTRY_COMPARE(source.bytesAvailable(), qint64(10)); QCOMPARE(source.readAll(), QByteArray("RX-command"));
    receiver.stop();

    receiver.startClient(localAddress.toString(), remote.serverPort(), MonitorLayout(), false, 2);
    QTRY_VERIFY(remote.hasPendingConnections());
    QTcpSocket *clientPeer = remote.nextPendingConnection(); QVERIFY(clientPeer);
    clientPeer->write(downsampleFixture(88, true)); clientPeer->flush();
    QTRY_COMPARE(receiver.snapshot().latest.sequence, quint32(88));
    receiver.send("client-command");
    QTRY_COMPARE(clientPeer->bytesAvailable(), qint64(14)); QCOMPARE(clientPeer->readAll(), QByteArray("client-command"));

    MonitorSender sender;
    QSignalSpy senderStates(&sender, &MonitorSender::stateChanged);
    sender.startClient(localAddress.toString(), remote.serverPort(), 3);
    QTRY_VERIFY(remote.hasPendingConnections());
    QTcpSocket *sendPeer = remote.nextPendingConnection(); QVERIFY(sendPeer);
    QTRY_VERIFY(!senderStates.isEmpty()); QVERIFY(senderStates.last()[2].toBool());
    sender.send("TX-command", 1);
    QTRY_COMPARE(sendPeer->bytesAvailable(), qint64(10)); QCOMPARE(sendPeer->readAll(), QByteArray("TX-command"));
    sendPeer->write("OK"); sendPeer->flush();
    QTRY_COMPARE(sender.snapshot().rxBytes, quint64(2));
    QCOMPARE(QNetworkProxy::applicationProxy().type(), QNetworkProxy::HttpProxy);
    sender.stop(); receiver.stop();
}

QTEST_MAIN(MonitorTests)
#include "tst_monitor.moc"
