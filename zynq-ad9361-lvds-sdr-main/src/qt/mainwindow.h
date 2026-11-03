#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QElapsedTimer>
#include "monitorreceiver.h"
#include "spectrum.h"

class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QCheckBox;
class QTableWidget;
class QTableView;
class QThread;
class DemodSymbolModel;
class QTimer;
class QTabWidget;
class PlotWidget;
class MonitorSender;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void startPreview();
    MonitorSnapshot receptionSnapshot() const;

private:
    void buildUi();
    void toggleNetwork();
    void stopNetwork();
    void refresh();
    void updateWaveformView();
    void updateDemodWaveform();
    void updateSpectrum();
    void updateSpreadComparison();
    void toggleDemo();
    void sendBytes(const QByteArray &bytes, bool useReceiveConnection = false);
    void toggleSendNetwork();
    void stopSendNetwork();
    void handleSendResult(quint64 request, const QByteArray &preview, qint64 total, const QString &error, bool stopLoopOnFailure = true);
    void sendInput();
    void updateSendPreview();
    void sendConfigured();
    bool configuredPayload(QByteArray *packet, QString *error) const;
    bool payloadProtocolEnabled() const;
    void updateSendProtocolControls();
    void stopCyclicSend();
    QByteArray decodeSendPayload(const QString &text, int format, QString *error) const;
    void toggleRecording();
    void exportCsv();
    void printDemodData();
    void saveDemodPrint();
    void clearData();
    void log(const QString &message);
    MonitorLayout layout() const;
    void setSettingsEnabled(bool enabled);

    QThread *receiverThread = nullptr;
    MonitorReceiver *receiver = nullptr;
    MonitorSender *sender = nullptr;
    quint64 sendNetworkSession = 0;
    bool sendNetworkActive = false;
    QTimer *sendConnectTimer = nullptr;
    QComboBox *sendConnectionBox = nullptr;
    QLineEdit *sendAddressEdit = nullptr;
    QSpinBox *sendPortSpin = nullptr;
    QPushButton *sendNetworkButton = nullptr;
    QLabel *sendStateLabel = nullptr, *duplexStatusLabel = nullptr;
    PlotWidget *duplexReceivePlot = nullptr;
    quint64 networkSession = 0, seenRevision = 0;
    bool recordingActive = false;
    QTimer *refreshTimer = nullptr, *demoTimer = nullptr, *connectTimer = nullptr;
    QComboBox *modeBox = nullptr, *iqBox = nullptr, *sendFormat = nullptr;
    QComboBox *profileBox = nullptr, *wireBox = nullptr, *pairOrderBox = nullptr;
    QComboBox *waveWindowBox = nullptr;
    QComboBox *demodDisplayBox = nullptr, *demodWindowBox = nullptr;
    QSpinBox *demodStartSpin = nullptr;
    QLabel *demodViewStatus = nullptr;
    QPushButton *demodPreviousButton = nullptr, *demodNextButton = nullptr;
    QSpinBox *waveStartSpin = nullptr;
    QLabel *waveRangeLabel = nullptr;
    QPushButton *wavePreviousButton = nullptr, *waveNextButton = nullptr;
    QComboBox *spectrumInputBox = nullptr, *spectrumWindowBox = nullptr;
    QComboBox *spectrumAmplitudeBox = nullptr;
    QLabel *spectrumReferenceLabel = nullptr;
    QDoubleSpinBox *sampleRateSpin = nullptr;
    QDoubleSpinBox *spectrumLevelSpin = nullptr;
    QComboBox *spectrumDivisionBox = nullptr;
    QLabel *spectrumStatus = nullptr;
    QLabel *spectrumBandwidthLabel = nullptr;
    PlotWidget *spectrumPlot = nullptr;
    MonitorData displayedWaveform;
    QCheckBox *spreadCompareBox = nullptr;
    QPushButton *spreadExportButton = nullptr;
    SpreadReference displayedReference;
    QComboBox *spreadAverageBox = nullptr;
    QCheckBox *spreadDcBox = nullptr, *spreadMarkersBox = nullptr;
    QWidget *spectrumAmplitudePanel = nullptr;
    SpectrumPowerAverage spreadRxAverage;
    WelchSpectrum spreadReferenceEstimate;
    QString spreadSpectrumConfig;
    quint32 spreadReferenceId = 0;
    bool spreadReferenceCached = false;
    QLineEdit *addressEdit = nullptr, *sendEdit = nullptr;
    QSpinBox *portSpin = nullptr, *iqSpin = nullptr, *demodSpin = nullptr, *fftSpin = nullptr;
    QPushButton *networkButton = nullptr, *demoButton = nullptr, *recordButton = nullptr;
    QLabel *stateLabel = nullptr, *statsLabel = nullptr, *frameLabel = nullptr;
    QLabel *demodStatus = nullptr, *waveformStatus = nullptr;
    QPlainTextEdit *logEdit = nullptr;
    QPlainTextEdit *demodPrintEdit = nullptr;
    QPlainTextEdit *sendPayloadEdit = nullptr;
    QPlainTextEdit *sendHexView = nullptr;
    QPlainTextEdit *sendHistoryEdit = nullptr;
    QTableWidget *sendByteTable = nullptr;
    QComboBox *sendPageFormat = nullptr;
    QComboBox *sendProtocolBox = nullptr;
    bool txAwaitingAck = false;
    PlotWidget *sendIqPlot = nullptr;
    QSpinBox *sendRepeatSpin = nullptr;
    QCheckBox *sendAppendLine = nullptr;
    QCheckBox *sendCyclic = nullptr;
    QSpinBox *sendIntervalSpin = nullptr;
    QTimer *sendTimer = nullptr;
    QByteArray cyclicPacket;
    PlotWidget *sendBytePlot = nullptr;
    QLabel *sendPreviewStatus = nullptr;
    QTabWidget *tabs = nullptr;
    QPushButton *copyDemodPrintButton = nullptr, *saveDemodPrintButton = nullptr;
    QCheckBox *hexPreview = nullptr, *pausePlots = nullptr;
    QCheckBox *holdDemod = nullptr;
    QCheckBox *freezeWaveform = nullptr;
    QPushButton *resumeWaveformButton = nullptr;
    QTableWidget *demodTable = nullptr;
    QTableView *symbolTable = nullptr;
    DemodSymbolModel *symbolModel = nullptr;
    PlotWidget *wavePlot = nullptr, *constellationPlot = nullptr, *fftPlot = nullptr, *demodPlot = nullptr;
    PlotWidget *bitPlot = nullptr;
    PlotWidget *qWavePlot = nullptr, *qBitPlot = nullptr;
    MonitorData latest;
    MonitorData lastDemodResult;
    MonitorData displayedDemod;
    bool displayedDemodIsDemo = false;
    quint64 nonzeroDemodSections = 0;
    quint64 validDemodFrames = 0;
    bool demodDirty = true;
    QVector<quint32> renderedDemodWords;
    int renderedPairOrder = -1;
    QElapsedTimer elapsed;
    quint64 rxBytes = 0, txBytes = 0, frames = 0, rejected = 0;
    quint64 previousRx = 0, previousFrames = 0;
    qint64 previousMs = 0;
    quint32 demoSequence = 0;
    quint64 sequenceJumps = 0;
    quint64 sendSequence = 0;
    bool dirty = false, networkActive = false;
};
#endif // MAINWINDOW_H
