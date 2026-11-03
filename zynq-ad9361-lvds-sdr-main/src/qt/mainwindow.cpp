#include <QImage>
#include <QPainter>
#include <QTemporaryFile>
#include "mainwindow.h"
#include "plotwidget.h"
#include "demodsymbolmodel.h"
#include "spectrum.h"
#include "monitorsender.h"
#include "sendpayloadprotocol.h"
#include <QtEndian>

#include <QCheckBox>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableView>
#include <QThread>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    buildUi();
    receiverThread = new QThread(this); receiverThread->setObjectName("receiverThread");
    receiver = new MonitorReceiver;
    receiver->moveToThread(receiverThread);
    sender = new MonitorSender; sender->moveToThread(receiverThread);
    connect(receiverThread, &QThread::finished, sender, &QObject::deleteLater);
    connect(receiverThread, &QThread::finished, receiver, &QObject::deleteLater);
    connect(receiver, &MonitorReceiver::stateChanged, this,
            [this](quint64 session, const QString &text, bool connected, bool failed) {
                if (session != networkSession || !networkActive) return;
                if (connected) connectTimer->stop();
                else if (sendConnectionBox->currentIndex() == 0) stopCyclicSend();
                stateLabel->setText(text); log(text);
                if (failed) stopNetwork();
            });
    connect(receiver, &MonitorReceiver::notice, this, [this](quint64 session, const QString &text) {
        if (session == networkSession) log(text);
    });
    connect(receiver, &MonitorReceiver::recordingFailed, this, [this](const QString &text) {
        recordingActive = false; recordButton->setText(QStringLiteral("保存接收原始流…")); log(text);
    });
    connect(receiver, &MonitorReceiver::sendResult, this,
            [this](quint64 session, quint64 request, const QByteArray &preview, qint64 total, const QString &error) {
                if (session != networkSession) return;
                handleSendResult(request, preview, total, error, sendConnectionBox->currentIndex() == 0);
            });
    connect(sender, &MonitorSender::sendResult, this,
            [this](quint64 session, quint64 request, const QByteArray &preview, qint64 total, const QString &error) {
                if (session == sendNetworkSession) handleSendResult(request, preview, total, error);
            });
    connect(sender, &MonitorSender::payloadResult, this,
            [this](quint64 session, quint64 request, quint32 sequence, quint32 result, const QString &error) {
                if (session != sendNetworkSession) return;
                txAwaitingAck = false;
                const QString text = error.isEmpty()
                                         ? QStringLiteral("TXA1 seq=%1 result=%2：%3").arg(sequence).arg(result).arg(TxPayload::resultText(result)) : error;
                sendHistoryEdit->appendPlainText(QStringLiteral("%1  #%2  %3")
                                                     .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz")).arg(request).arg(text));
                sendPreviewStatus->setText(text); log(text);
                if (!error.isEmpty() || result != 0) stopCyclicSend();
            });
    connect(sender, &MonitorSender::notice, this, [this](quint64 session, const QString &text) {
        if (session == sendNetworkSession) log(text);
    });
    connect(sender, &MonitorSender::stateChanged, this,
            [this](quint64 session, const QString &text, bool connected, bool failed) {
                if (session != sendNetworkSession || !sendNetworkActive) return;
                if (connected) sendConnectTimer->stop();
                sendStateLabel->setText(text); log(text);
                if (failed) sendHistoryEdit->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + "  " + text);
                if (failed) { stopSendNetwork(); sendStateLabel->setText(text); }
            });
    receiverThread->start();
    connect(freezeWaveform, &QCheckBox::toggled, this, [this](bool enabled) {
        QMetaObject::invokeMethod(receiver, [worker = receiver, enabled] {
            worker->setFreezeWaveform(enabled);
        }, Qt::QueuedConnection);
    });
    connect(resumeWaveformButton, &QPushButton::clicked, this, [this] {
        QMetaObject::invokeMethod(receiver, [worker = receiver] {
            worker->resumeWaveform();
        }, Qt::QueuedConnection);
    });
    refreshTimer = new QTimer(this);
    demoTimer = new QTimer(this);
    connectTimer = new QTimer(this);
    connectTimer->setSingleShot(true);
    sendConnectTimer = new QTimer(this); sendConnectTimer->setSingleShot(true);
    connect(sendConnectTimer, &QTimer::timeout, this, [this] {
        log(QStringLiteral("独立发送连接超时，接收连接继续运行。")); stopSendNetwork();
    });
    connect(connectTimer, &QTimer::timeout, this, [this] {
        log(QStringLiteral("连接超时，请检查板端是否启动 TCP Server。"));
        stopNetwork();
    });
    connect(networkButton, &QPushButton::clicked, this, &MainWindow::toggleNetwork);
    connect(sendNetworkButton, &QPushButton::clicked, this, &MainWindow::toggleSendNetwork);
    connect(demoButton, &QPushButton::clicked, this, &MainWindow::toggleDemo);
    connect(recordButton, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    connect(refreshTimer, &QTimer::timeout, this, &MainWindow::refresh);
    connect(demoTimer, &QTimer::timeout, this, [this] {
        const QByteArray packet = makeDemoFrame(demoSequence++, layout());
        QMetaObject::invokeMethod(receiver, [worker = receiver, packet] {
            worker->ingest(packet.left(7)); worker->ingest(packet.mid(7));
        }, Qt::QueuedConnection);
    });
    connect(hexPreview, &QCheckBox::toggled, this, [this](bool checked) {
        QMetaObject::invokeMethod(receiver, [worker = receiver, checked] { worker->setHexPreview(checked); }, Qt::QueuedConnection);
    });
    elapsed.start(); refreshTimer->start(100);
    log(QStringLiteral("板端 192.168.1.10 是 TCP Client，目标电脑 192.168.1.209:5001。默认选择 Server。"));
    log(QStringLiteral("新顶层默认载荷：4096 B 降采样 I16/Q16 + 1024 B 附加区（解调/参考/捕获FFT分片）；频谱默认采样率 12.24 MS/s，总带宽 12.24 MHz。"));
    log(QStringLiteral("type=5：flags bit0=完整解调帧，bit1=完整 IQ 块；0x0002 为解调填零，0x0003 附带完整解调帧。type=4 和无帧头模式有效性未知。"));
    log(QStringLiteral("后台线程持续接收、解析和保存；界面每 100 ms 取最新帧，暂停显示不影响接收。"));
    log(QStringLiteral("双端口收发：接收监听电脑 5001；独立发送连接板卡 192.168.1.10:5002，两条连接互不关闭。"));
    log(QStringLiteral("5002 默认 TXD1：12 B 帧头 + 1024 B 载荷，等待 TXA1 result=0 才确认 PL 提交；循环发送等待上一帧回执。"));
}

MainWindow::~MainWindow()
{
    sendTimer->stop();
    demoTimer->stop(); refreshTimer->stop();
    sendConnectTimer->stop();
    QMetaObject::invokeMethod(sender, [worker = sender] { worker->stop(); }, Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(receiver, [worker = receiver] { worker->stop(true); }, Qt::BlockingQueuedConnection);
    receiverThread->quit(); receiverThread->wait();
}

MonitorSnapshot MainWindow::receptionSnapshot() const { return receiver->snapshot(); }

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("智能数字通信收发系统")); resize(1260, 850);
    auto *root = new QWidget(this); setCentralWidget(root);
    auto *outer = new QVBoxLayout(root);
    auto *heading = new QLabel(QStringLiteral(
        "智能数字通信收发系统\n"
        "AD9361 · ZYNQ  20 MHz IQ 频谱与解调监控"));
    heading->setObjectName("appHeading");
    heading->setStyleSheet("font-size: 21px; font-weight: 700; padding: 10px 14px 9px; color: #000000;"
                           "background: #ffffff; border: 2px solid #7b8794; border-radius: 10px;");
    heading->setMinimumHeight(58);
    outer->addWidget(heading);
    auto *splitter = new QSplitter; outer->addWidget(splitter, 1);
    auto *settings = new QWidget; auto *left = new QVBoxLayout(settings);
    left->setContentsMargins(4, 4, 8, 4);
    auto *networkGroup = new QGroupBox(QStringLiteral("接收连接（支持同连接收发）"));
    auto *network = new QFormLayout(networkGroup);
    modeBox = new QComboBox; modeBox->addItems({"TCP Server", "TCP Client"});
    modeBox->setObjectName("modeBox");
    addressEdit = new QLineEdit("0.0.0.0"); addressEdit->setObjectName("addressEdit");
    addressEdit->setToolTip(QStringLiteral("Server：本机监听 IP，0.0.0.0 监听全部网卡；Client：远端服务器 IP / 主机名。"));
    portSpin = new QSpinBox; portSpin->setRange(1, 65535); portSpin->setValue(5001);
    portSpin->setObjectName("portSpin");
    networkButton = new QPushButton(QStringLiteral("开始监听")); networkButton->setObjectName("networkButton");
    stateLabel = new QLabel(QStringLiteral("未连接")); stateLabel->setWordWrap(true);
    stateLabel->setObjectName("stateLabel");
    auto *hint = new QLabel(QStringLiteral("现有板端主动连接电脑\n电脑网卡请设置 192.168.1.209 / 24")); hint->setWordWrap(true);
    network->addRow(QStringLiteral("模式"), modeBox); network->addRow(QStringLiteral("地址"), addressEdit);
    network->addRow(QStringLiteral("端口"), portSpin); network->addRow(networkButton);
    network->addRow(stateLabel); network->addRow(hint);
    connect(modeBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        addressEdit->setText(index == 0 ? "0.0.0.0" : "192.168.1.10");
        networkButton->setText(index == 0 ? QStringLiteral("开始监听") : QStringLiteral("连接服务器"));
    });
    left->addWidget(networkGroup);
    auto *sendNetworkGroup = new QGroupBox(QStringLiteral("发送连接 · 独立于接收"));
    auto *sendNetworkForm = new QFormLayout(sendNetworkGroup);
    sendConnectionBox = new QComboBox; sendConnectionBox->setObjectName("sendConnectionBox");
    sendConnectionBox->addItems({QStringLiteral("复用接收连接（同端口）"), QStringLiteral("独立 TCP Client（双端口）")});
    sendConnectionBox->setCurrentIndex(1);
    sendAddressEdit = new QLineEdit("192.168.1.10"); sendAddressEdit->setObjectName("sendAddressEdit");
    sendPortSpin = new QSpinBox; sendPortSpin->setObjectName("sendPortSpin");
    sendPortSpin->setRange(1, 65535); sendPortSpin->setValue(5002);
    sendNetworkButton = new QPushButton(QStringLiteral("连接发送端口")); sendNetworkButton->setObjectName("sendNetworkButton");
    sendStateLabel = new QLabel(QStringLiteral("发送未连接 · 接收监听独立运行"));
    sendStateLabel->setObjectName("sendStateLabel"); sendStateLabel->setWordWrap(true);
    sendNetworkForm->addRow(QStringLiteral("发送通道"), sendConnectionBox);
    sendNetworkForm->addRow(QStringLiteral("板卡 IP"), sendAddressEdit);
    sendNetworkForm->addRow(QStringLiteral("板卡端口"), sendPortSpin);
    sendNetworkForm->addRow(sendNetworkButton); sendNetworkForm->addRow(sendStateLabel);
    left->addWidget(sendNetworkGroup);
    connect(sendConnectionBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        stopCyclicSend(); const bool dedicated = index == 1;
        sendAddressEdit->setEnabled(dedicated && !sendNetworkActive);
        sendPortSpin->setEnabled(dedicated && !sendNetworkActive);
        sendNetworkButton->setEnabled(dedicated || sendNetworkActive);
        if (sendProtocolBox) updateSendProtocolControls();
    });
    auto *decodeGroup = new QGroupBox(QStringLiteral("组合包解析设置"));
    auto *decode = new QFormLayout(decodeGroup);
    profileBox = new QComboBox; profileBox->setObjectName("profileBox");
    profileBox->addItems({QStringLiteral("新顶层：IQ16 + 附加区（自动识别）"), QStringLiteral("旧版：IQ12 + 解调 + FFT")});
    wireBox = new QComboBox; wireBox->setObjectName("wireBox");
    wireBox->addItems({QStringLiteral("MONI 帧头（现有 PS 协议）"), QStringLiteral("无帧头，仅固定长度载荷")});
    wireBox->setToolTip(QStringLiteral("无帧头模式要求连接的首字节就是 IQ 起点，载荷不能带 PL 头；无法自动恢复错位。序号由电脑生成。"));
    pairOrderBox = new QComboBox; pairOrderBox->setObjectName("pairOrderBox");
    pairOrderBox->addItems({QStringLiteral("低位符号先：I=bit0，Q=bit1"), QStringLiteral("高位符号先：I=bit30，Q=bit31")});
    pairOrderBox->setToolTip(QStringLiteral("每字 16 组 {Q,I}；先后顺序需核对 BRAM 的窄写宽读配置。"));
    iqBox = new QComboBox;
    iqBox->addItems({QStringLiteral("I12 / Q12 各占 16 位"), QStringLiteral("Q12 / I12 各占 16 位"),
                     QStringLiteral("I[11:0], Q[23:12]"), QStringLiteral("I16 / Q16 有符号")});
    iqBox->setCurrentIndex(3); iqBox->setObjectName("iqBox");
    iqBox->setToolTip(QStringLiteral("载荷为小端；12 位格式会对低 12 位符号扩展。紧凑格式仍每样本占 4 字节。"));
    iqSpin = new QSpinBox; iqSpin->setRange(1, 16384); iqSpin->setValue(1024);
    demodSpin = new QSpinBox; demodSpin->setRange(4, 65536); demodSpin->setSingleStep(4); demodSpin->setValue(1024);
    fftSpin = new QSpinBox; fftSpin->setRange(0, 4096); fftSpin->setValue(0);
    fftSpin->setSpecialValueText(QStringLiteral("无独立 FFT 尾区"));
    decode->addRow(QStringLiteral("板端布局"), profileBox); decode->addRow(QStringLiteral("网络封装"), wireBox);
    decode->addRow(QStringLiteral("IQ 格式"), iqBox); decode->addRow(QStringLiteral("IQ 样本数"), iqSpin);
    decode->addRow(QStringLiteral("解调字节数"), demodSpin); decode->addRow(QStringLiteral("FFT 点数"), fftSpin);
    decode->addRow(QStringLiteral("解调位序"), pairOrderBox);
    auto *decodeHint = new QLabel(QStringLiteral("新顶层：1024 组 I16/Q16 → 256 个解调字\n每字 16 组 {Q,I}，共 4096 组\n采样率在频谱页按板端实际值设置\nIQ 半字位置、帧头及位序需和打包模块核对"));
    decodeHint->setWordWrap(true); decode->addRow(decodeHint); left->addWidget(decodeGroup);
    connect(profileBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        iqSpin->setValue(1024); demodSpin->setValue(1024);
        iqBox->setCurrentIndex(index == 0 ? 3 : 0); fftSpin->setValue(index == 0 ? 0 : 128);
    });
    connect(pairOrderBox, &QComboBox::currentIndexChanged, this, [this] {
        dirty = demodDirty = true;
        if (demodDisplayBox) updateDemodWaveform();
    });
    auto *tools = new QGroupBox(QStringLiteral("接收与显示")); auto *toolLayout = new QVBoxLayout(tools);
    pausePlots = new QCheckBox(QStringLiteral("暂停图表刷新（继续接收）"));
    pausePlots->setObjectName("pausePlots");
    hexPreview = new QCheckBox(QStringLiteral("HEX 预览（每秒限量）"));
    holdDemod = new QCheckBox(QStringLiteral("保留最近有效解调帧（显示来源序号）"));
    holdDemod->setToolTip(QStringLiteral("type=5 按 flags bit0 保留完整帧，包括全零结果；type=4 和无帧头模式仅按非零区保留，有效性未知。"));
    holdDemod->setChecked(true); holdDemod->setObjectName("holdDemod");
    connect(holdDemod, &QCheckBox::toggled, this, [this] { dirty = demodDirty = true; });
    freezeWaveform = new QCheckBox(QStringLiteral("首次有效解调后冻结 I/Q 波形"));
    freezeWaveform->setObjectName("freezeWaveform"); freezeWaveform->setChecked(true);
    freezeWaveform->setToolTip(QStringLiteral("固定首次完整解调帧所在包的 I/Q 波形和星座图，继续接收和保存；type=4 与无帧头模式按非零解调区触发，有效性未知。"));
    resumeWaveformButton = new QPushButton(QStringLiteral("恢复波形（等待下次解调）"));
    resumeWaveformButton->setObjectName("resumeWaveformButton"); resumeWaveformButton->setEnabled(false);
    demoButton = new QPushButton(QStringLiteral("启动演示（无需板卡）")); demoButton->setObjectName("demoButton");
    recordButton = new QPushButton(QStringLiteral("保存接收原始流…"));
    recordButton->setObjectName("recordButton");
    auto *exportButton = new QPushButton(QStringLiteral("导出最新帧 CSV…"));
    exportButton->setObjectName("exportButton");
    auto *clearButton = new QPushButton(QStringLiteral("清空数据和计数"));
    clearButton->setObjectName("clearButton");
    toolLayout->addWidget(freezeWaveform); toolLayout->addWidget(resumeWaveformButton);
    toolLayout->addWidget(pausePlots); toolLayout->addWidget(hexPreview); toolLayout->addWidget(demoButton);
    toolLayout->addWidget(holdDemod);
    toolLayout->addWidget(recordButton); toolLayout->addWidget(exportButton); toolLayout->addWidget(clearButton);
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportCsv);
    connect(clearButton, &QPushButton::clicked, this, &MainWindow::clearData);
    left->addWidget(tools); left->addStretch();
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setWidget(settings); scroll->setMinimumWidth(310);
    splitter->addWidget(scroll);
    auto *rightWidget = new QWidget; auto *right = new QVBoxLayout(rightWidget); right->setContentsMargins(4, 4, 4, 4);
    frameLabel = new QLabel(QStringLiteral("等待组合包：MONI / version 1 / type 5（兼容 type 4）"));
    frameLabel->setObjectName("frameLabel"); frameLabel->setWordWrap(true); right->addWidget(frameLabel);
    demodStatus = new QLabel(QStringLiteral("等待解调区；type=5 按 flags bit0 区分完整帧与填零，完整帧不代表比特正确。"));
    demodStatus->setObjectName("demodStatus"); demodStatus->setWordWrap(true); right->addWidget(demodStatus);
    waveformStatus = new QLabel(QStringLiteral("I/Q 波形实时刷新，等待首次有效解调后冻结。"));
    waveformStatus->setObjectName("waveformStatus"); waveformStatus->setWordWrap(true); right->addWidget(waveformStatus);
    tabs = new QTabWidget; auto *plots = new QWidget; auto *grid = new QGridLayout(plots);
    wavePlot = new PlotWidget(PlotWidget::IWaveform); qWavePlot = new PlotWidget(PlotWidget::QWaveform);
    constellationPlot = new PlotWidget(PlotWidget::Constellation);
    wavePlot->setObjectName("wavePlot"); constellationPlot->setObjectName("constellationPlot");
    fftPlot = new PlotWidget(PlotWidget::Spectrum); demodPlot = new PlotWidget(PlotWidget::Demod);
    fftPlot->setAcquisitionFftData({}, {});
    demodPlot->setObjectName("demodPlot");
    qWavePlot->setObjectName("qWavePlot");
    bitPlot = new PlotWidget(PlotWidget::IBits); qBitPlot = new PlotWidget(PlotWidget::QBits);
    bitPlot->setObjectName("bitPlot"); qBitPlot->setObjectName("qBitPlot");
    auto *waveControls = new QHBoxLayout;
    waveWindowBox = new QComboBox; waveWindowBox->setObjectName("waveWindowBox");
    for (int count : {64, 128, 256, 512}) waveWindowBox->addItem(QStringLiteral("%1 点").arg(count), count);
    waveWindowBox->addItem(QStringLiteral("整包"), 0); waveWindowBox->setCurrentIndex(1);
    waveStartSpin = new QSpinBox; waveStartSpin->setObjectName("waveStartSpin"); waveStartSpin->setRange(0, 0);
    waveStartSpin->setToolTip(QStringLiteral("I/Q 同时移动到此采样索引；冻结或暂停时也可浏览。"));
    wavePreviousButton = new QPushButton(QStringLiteral("上一段")); wavePreviousButton->setObjectName("wavePreviousButton");
    waveNextButton = new QPushButton(QStringLiteral("下一段")); waveNextButton->setObjectName("waveNextButton");
    waveRangeLabel = new QLabel; waveRangeLabel->setObjectName("waveRangeLabel");
    waveControls->addWidget(new QLabel(QStringLiteral("波形显示"))); waveControls->addWidget(waveWindowBox);
    waveControls->addWidget(new QLabel(QStringLiteral("起点"))); waveControls->addWidget(waveStartSpin);
    waveControls->addWidget(wavePreviousButton); waveControls->addWidget(waveNextButton);
    auto *smoothWaveform = new QCheckBox(QStringLiteral("平滑显示"));
    smoothWaveform->setObjectName("smoothWaveform"); smoothWaveform->setChecked(true);
    smoothWaveform->setToolTip(QStringLiteral("用平滑曲线连接原始采样点，保持峰值和平台；鼠标数值、CSV 仍为原始采样。取消后显示原始折线。"));
    waveControls->addWidget(smoothWaveform);
    wavePlot->setSmoothDisplay(true); qWavePlot->setSmoothDisplay(true);
    connect(smoothWaveform, &QCheckBox::toggled, this, [this](bool enabled) {
        wavePlot->setSmoothDisplay(enabled); qWavePlot->setSmoothDisplay(enabled);
    });
    waveControls->addWidget(waveRangeLabel); waveControls->addStretch();
    grid->addLayout(waveControls, 0, 0, 1, 2);
    grid->addWidget(wavePlot, 1, 0); grid->addWidget(qWavePlot, 1, 1);
    grid->addWidget(bitPlot, 2, 0); grid->addWidget(qBitPlot, 2, 1);
    grid->setRowStretch(1, 1); grid->setRowStretch(2, 1); grid->setColumnStretch(0, 1); grid->setColumnStretch(1, 1);
    connect(waveWindowBox, &QComboBox::currentIndexChanged, this, &MainWindow::updateWaveformView);
    connect(waveStartSpin, &QSpinBox::valueChanged, this, &MainWindow::updateWaveformView);
    connect(wavePreviousButton, &QPushButton::clicked, this, [this] {
        waveStartSpin->setValue(waveStartSpin->value() - waveWindowBox->currentData().toInt());
    });
    connect(waveNextButton, &QPushButton::clicked, this, [this] {
        waveStartSpin->setValue(waveStartSpin->value() + waveWindowBox->currentData().toInt());
    });
    updateWaveformView();
    tabs->addTab(plots, QStringLiteral("实时可视化"));
    auto *spectrumPage = new QWidget; spectrumPage->setObjectName("spectrumPage");
    spectrumPage->setStyleSheet("");
    auto *spectrumLayout = new QVBoxLayout(spectrumPage);
    auto *spectrumControls = new QHBoxLayout;
    spectrumInputBox = new QComboBox; spectrumInputBox->setObjectName("spectrumInputBox");
    spectrumInputBox->addItems({QStringLiteral("复数 I+jQ"), QStringLiteral("I 通道"), QStringLiteral("Q 通道")});
    sampleRateSpin = new QDoubleSpinBox; sampleRateSpin->setObjectName("sampleRateSpin");
    sampleRateSpin->setDecimals(5); sampleRateSpin->setRange(0.001, 1000); sampleRateSpin->setValue(12.24);
    sampleRateSpin->setSuffix(" MS/s"); sampleRateSpin->setSingleStep(0.01);
    sampleRateSpin->setToolTip(QStringLiteral("填写板端实际 IQ 采样率；当前工程为 12.24 MS/s，对应 −6.12～+6.12 MHz。"));
    spectrumWindowBox = new QComboBox; spectrumWindowBox->setObjectName("spectrumWindowBox");
    spectrumWindowBox->addItems({QStringLiteral("Hann 窗"), QStringLiteral("矩形窗")});
    spectrumControls->addWidget(new QLabel(QStringLiteral("信号"))); spectrumControls->addWidget(spectrumInputBox);
    spectrumControls->addWidget(new QLabel(QStringLiteral("采样率"))); spectrumControls->addWidget(sampleRateSpin);
    spectrumControls->addWidget(new QLabel(QStringLiteral("窗函数"))); spectrumControls->addWidget(spectrumWindowBox);
    spectrumBandwidthLabel = new QLabel; spectrumBandwidthLabel->setObjectName("spectrumBandwidthLabel");
    spectrumControls->addStretch(); spectrumLayout->addLayout(spectrumControls);
    auto *amplitudeControls = new QHBoxLayout;
    spectrumAmplitudeBox = new QComboBox; spectrumAmplitudeBox->setObjectName("spectrumAmplitudeBox");
    spectrumAmplitudeBox->addItems({QStringLiteral("固定参考（dB）"), QStringLiteral("每帧峰值归一化（dB）")});
    spectrumReferenceLabel = new QLabel; spectrumReferenceLabel->setObjectName("spectrumReferenceLabel");
    amplitudeControls->addWidget(new QLabel(QStringLiteral("幅度显示"))); amplitudeControls->addWidget(spectrumAmplitudeBox);
    spectrumLevelSpin = new QDoubleSpinBox; spectrumLevelSpin->setObjectName("spectrumLevelSpin");
    spectrumLevelSpin->setRange(-120, 120); spectrumLevelSpin->setDecimals(0);
    spectrumLevelSpin->setSingleStep(10); spectrumLevelSpin->setSuffix(" dB"); spectrumLevelSpin->setValue(0);
    spectrumLevelSpin->setToolTip(QStringLiteral("纵轴顶部电平；只移动刻度范围，不改变数据或板端增益。"));
    spectrumDivisionBox = new QComboBox; spectrumDivisionBox->setObjectName("spectrumDivisionBox");
    for (int division : {5, 10, 20}) spectrumDivisionBox->addItem(QStringLiteral("%1 dB/格").arg(division), division);
    spectrumDivisionBox->setCurrentIndex(1);
    amplitudeControls->addWidget(new QLabel(QStringLiteral("参考电平"))); amplitudeControls->addWidget(spectrumLevelSpin);
    amplitudeControls->addWidget(spectrumDivisionBox); amplitudeControls->addStretch();
    spectrumAmplitudePanel = new QWidget;
    spectrumAmplitudePanel->setLayout(amplitudeControls);
    spectrumLayout->addWidget(spectrumAmplitudePanel);
    auto *spreadControls = new QHBoxLayout;
    spreadCompareBox = new QCheckBox(QStringLiteral("扩频前 / 接收频谱对比"));
    spreadCompareBox->setObjectName("spreadCompareBox");
    spreadCompareBox->setChecked(true);
    spreadExportButton = new QPushButton(QStringLiteral("导出扩频前采样 CSV…"));
    spreadExportButton->setObjectName("spreadExportButton");
    spreadControls->addWidget(spreadCompareBox);
    spreadControls->addWidget(spreadExportButton); spreadControls->addStretch();
    spectrumLayout->addLayout(spreadControls);
    connect(spreadCompareBox, &QCheckBox::toggled, this, [this](bool) { updateSpectrum(); });
    connect(spreadExportButton, &QPushButton::clicked, this, [this] {
        if (displayedReference.i.isEmpty()) { log(QStringLiteral("尚未收到完整的扩频前采样。")); return; }
        const SpreadReference ref = displayedReference;
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出真实扩频前采样"),
            QString("pre_spread_%1.csv").arg(ref.captureId), "CSV (*.csv)");
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { log(file.errorString()); return; }
        QTextStream stream(&file);
        stream << "capture_id,sample_rate_hz,index,i,q\n";
        for (int n = 0; n < ref.i.size(); ++n)
            stream << ref.captureId << ',' << ref.sampleRateHz << ',' << n << ',' << ref.i[n] << ',' << ref.q[n] << '\n';
        stream.flush();
        if (stream.status() != QTextStream::Ok) { file.cancelWriting(); log(QStringLiteral("扩频参考CSV写入失败。")); }
        else if (!file.commit()) log(file.errorString());
        else log(QStringLiteral("已导出扩频前采集 %1：%2").arg(ref.captureId).arg(path));
    });
    auto *fineControls = new QHBoxLayout;
    spreadAverageBox = new QComboBox; spreadAverageBox->setObjectName("spreadAverageBox");
    for (int n : {1, 4, 8, 16}) spreadAverageBox->addItem(QStringLiteral("%1 组").arg(n), n);
    spreadAverageBox->setCurrentIndex(2);
    spreadAverageBox->setToolTip(QStringLiteral("仅对不同序号接收快照的线性功率取平均；冻结时不会重复累加同一组。改变配置会清空平均。"));
    spreadDcBox = new QCheckBox(QStringLiteral("去直流")); spreadDcBox->setObjectName("spreadDcBox");
    spreadDcBox->setToolTip(QStringLiteral("默认保留直流。勾选会减去每段均值，中心附近的谱形也会改变。"));
    spreadMarkersBox = new QCheckBox(QStringLiteral("理论零点标尺")); spreadMarkersBox->setObjectName("spreadMarkersBox");
    spreadMarkersBox->setChecked(true);
    spreadMarkersBox->setToolTip(QStringLiteral("当前 QPSK：符号率 80 ksym/s、码片率 4.08 Mchip/s，矩形脉冲理论主瓣宽度 160 kHz / 8.16 MHz。它不是实测零点检测结果；频偏、滤波和噪声会影响实测。"));
    fineControls->addWidget(new QLabel(QStringLiteral("接收功率平均")));
    fineControls->addWidget(spreadAverageBox); fineControls->addSpacing(14);
    auto *resetAverage = new QPushButton(QStringLiteral("重置平均"));
    resetAverage->setObjectName("spreadResetAverageButton");
    fineControls->addWidget(resetAverage); fineControls->addSpacing(14);
    fineControls->addWidget(spreadDcBox); fineControls->addSpacing(14);
    fineControls->addWidget(spreadMarkersBox); fineControls->addStretch();
    spectrumLayout->addLayout(fineControls);
    connect(spreadAverageBox, &QComboBox::currentIndexChanged, this, &MainWindow::updateSpectrum);
    connect(spreadDcBox, &QCheckBox::toggled, this, &MainWindow::updateSpectrum);
    connect(spreadMarkersBox, &QCheckBox::toggled, this, &MainWindow::updateSpectrum);
    connect(resetAverage, &QPushButton::clicked, this, [this] { spreadRxAverage.reset(); updateSpectrum(); });
    auto *rangeControls = new QHBoxLayout;
    rangeControls->addWidget(spectrumBandwidthLabel); rangeControls->addStretch();
    rangeControls->addWidget(spectrumReferenceLabel); spectrumLayout->addLayout(rangeControls);
    spectrumReferenceLabel->setToolTip(QStringLiteral("使用整包原始 IQ，跟随冻结和暂停；dB 为相对参考值的幅度比。固定参考可比较信号强弱。"));
    spectrumStatus = new QLabel(QStringLiteral("等待 IQ 数据")); spectrumStatus->setObjectName("spectrumStatus");
    spectrumStatus->setWordWrap(true);
    spectrumPlot = new PlotWidget(PlotWidget::IqSpectrum); spectrumPlot->setObjectName("spectrumPlot");
    spectrumPlot->setMinimumHeight(350);
    spectrumLayout->addWidget(spectrumPlot, 1);
    spectrumLayout->addWidget(spectrumStatus);
    connect(spectrumInputBox, &QComboBox::currentIndexChanged, this, &MainWindow::updateSpectrum);
    connect(spectrumWindowBox, &QComboBox::currentIndexChanged, this, &MainWindow::updateSpectrum);
    connect(sampleRateSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSpectrum);
    connect(spectrumAmplitudeBox, &QComboBox::currentIndexChanged, this, &MainWindow::updateSpectrum);
    connect(spectrumLevelSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSpectrum);
    connect(spectrumDivisionBox, &QComboBox::currentIndexChanged, this, &MainWindow::updateSpectrum);
    updateSpectrum();
    auto *spectrumScroll = new QScrollArea;
    spectrumScroll->setObjectName("spectrumScroll"); spectrumScroll->setWidgetResizable(true);
    spectrumScroll->setFrameShape(QFrame::NoFrame); spectrumScroll->setWidget(spectrumPage);
    tabs->addTab(spectrumScroll, QStringLiteral("频谱"));
    symbolTable = new QTableView; symbolTable->setObjectName("symbolTable");
    symbolModel = new DemodSymbolModel(symbolTable); symbolTable->setModel(symbolModel);
    symbolTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    symbolTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tabs->addTab(symbolTable, QStringLiteral("解调 I/Q 比特（4096 组）"));
    demodTable = new QTableWidget; demodTable->setObjectName("demodTable"); demodTable->setColumnCount(3);
    demodTable->setHorizontalHeaderLabels({QStringLiteral("字索引"), QStringLiteral("小端 uint32 · HEX"), QStringLiteral("无符号十进制")});
    demodTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    demodTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tabs->addTab(demodTable, QStringLiteral("解调原始字（前 256 字）"));
    tabs->addTab(constellationPlot, QStringLiteral("星座图"));
    auto *demodPage = new QWidget; demodPage->setObjectName("demodPage");
    auto *demodLayout = new QVBoxLayout(demodPage);
    auto *demodControls = new QHBoxLayout;
    demodDisplayBox = new QComboBox; demodDisplayBox->setObjectName("demodDisplayBox");
    demodDisplayBox->addItems({QStringLiteral("I16/Q16 采样（与发送正弦一致）"), QStringLiteral("int32 数值（每字一点）"),
                               QStringLiteral("uint32 数值（每字一点）"), QStringLiteral("int16 连续采样（每字两点）")});
    demodWindowBox = new QComboBox; demodWindowBox->setObjectName("demodWindowBox");
    for (int count : {64, 128, 256, 512}) demodWindowBox->addItem(QStringLiteral("%1 点").arg(count), count);
    demodWindowBox->addItem(QStringLiteral("整帧"), 0); demodWindowBox->setCurrentIndex(2);
    demodStartSpin = new QSpinBox; demodStartSpin->setObjectName("demodStartSpin"); demodStartSpin->setRange(0, 0);
    demodPreviousButton = new QPushButton(QStringLiteral("上一段")); demodPreviousButton->setObjectName("demodPreviousButton");
    demodNextButton = new QPushButton(QStringLiteral("下一段")); demodNextButton->setObjectName("demodNextButton");
    demodControls->addWidget(demodDisplayBox); demodControls->addWidget(demodWindowBox);
    demodControls->addWidget(new QLabel(QStringLiteral("起点"))); demodControls->addWidget(demodStartSpin);
    demodControls->addWidget(demodPreviousButton); demodControls->addWidget(demodNextButton); demodControls->addStretch();
    demodLayout->addLayout(demodControls);
    auto *demodSmooth = new QCheckBox(QStringLiteral("平滑显示")); demodSmooth->setObjectName("demodSmooth");
    demodSmooth->setChecked(true);
    demodSmooth->setToolTip(QStringLiteral("连接实际接收采样点，保留峰值；不拟合或生成正弦数据。"));
    demodControls->addWidget(demodSmooth); demodPlot->setSmoothDisplay(true);
    connect(demodSmooth, &QCheckBox::toggled, this, [this](bool enabled) { demodPlot->setSmoothDisplay(enabled); });
    demodViewStatus = new QLabel; demodViewStatus->setObjectName("demodViewStatus"); demodViewStatus->setWordWrap(true);
    demodLayout->addWidget(demodViewStatus); demodLayout->addWidget(demodPlot, 1);
    tabs->addTab(demodPage, QStringLiteral("解调采样波形"));
    connect(demodDisplayBox, &QComboBox::currentIndexChanged, this, [this] {
        const QSignalBlocker blocker(demodStartSpin); demodStartSpin->setValue(0); updateDemodWaveform();
    });
    connect(demodWindowBox, &QComboBox::currentIndexChanged, this, &MainWindow::updateDemodWaveform);
    connect(demodStartSpin, qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::updateDemodWaveform);
    connect(demodPreviousButton, &QPushButton::clicked, this, [this] {
        demodStartSpin->setValue(demodStartSpin->value() - demodPlot->sampleWindowSize());
    });
    connect(demodNextButton, &QPushButton::clicked, this, [this] {
        demodStartSpin->setValue(demodStartSpin->value() + demodPlot->sampleWindowSize());
    });
    updateDemodWaveform();
    tabs->addTab(fftPlot, QStringLiteral("最近成功捕获（线性）"));
    demodPrintEdit = new QPlainTextEdit; demodPrintEdit->setObjectName("demodPrintEdit");
    demodPrintEdit->setReadOnly(true); demodPrintEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    demodPrintEdit->setPlaceholderText(QStringLiteral("收到解调数据后，点击下方“打印解调数据”。输出固定快照，可复制或保存 TXT 与发送数据核对。"));
    tabs->addTab(demodPrintEdit, QStringLiteral("解调数据打印")); right->addWidget(tabs, 1);
    // TX controls and a live RX plot share one page; sockets stay independent.
    auto *sendPage = new QWidget; sendPage->setObjectName("sendPage");
    auto *sendPageLayout = new QVBoxLayout(sendPage);
    duplexStatusLabel = new QLabel(QStringLiteral("接收：未连接 · 发送：未连接"));
    duplexStatusLabel->setObjectName("duplexStatusLabel"); duplexStatusLabel->setWordWrap(true);
    sendPageLayout->addWidget(duplexStatusLabel);
    auto *sendSplit = new QSplitter(Qt::Horizontal); sendPageLayout->addWidget(sendSplit, 1);
    auto *sendConfigWidget = new QWidget; auto *sendConfigLayout = new QVBoxLayout(sendConfigWidget);
    auto *sendConfigGroup = new QGroupBox(QStringLiteral("发送设置"));
    auto *sendConfigForm = new QFormLayout(sendConfigGroup);
    sendProtocolBox = new QComboBox; sendProtocolBox->setObjectName("sendProtocolBox");
    sendProtocolBox->addItems({QStringLiteral("TXD1 / TXA1（板卡 5002）"), QStringLiteral("原始字节")});
    sendConfigForm->addRow(QStringLiteral("发送协议"), sendProtocolBox);
    sendPageFormat = new QComboBox; sendPageFormat->setObjectName("sendPageFormat");
    sendPageFormat->addItems({QStringLiteral("ASCII / UTF-8"), QStringLiteral("HEX（空格可选）")});
    sendRepeatSpin = new QSpinBox; sendRepeatSpin->setObjectName("sendRepeatSpin");
    sendRepeatSpin->setRange(1, 1000); sendRepeatSpin->setValue(1); sendRepeatSpin->setSuffix(QStringLiteral(" 次"));
    sendAppendLine = new QCheckBox(QStringLiteral("ASCII 末尾追加 CRLF"));
    sendAppendLine->setObjectName("sendAppendLine");
    sendAppendLine->setToolTip(QStringLiteral("仅 ASCII / UTF-8 模式生效，适合向文本协议服务发送一行命令。"));
    sendCyclic = new QCheckBox(QStringLiteral("循环发送")); sendCyclic->setObjectName("sendCyclic");
    sendIntervalSpin = new QSpinBox; sendIntervalSpin->setObjectName("sendIntervalSpin");
    sendIntervalSpin->setRange(100, 3600000); sendIntervalSpin->setValue(1000); sendIntervalSpin->setSuffix(" ms");
    sendConfigForm->addRow(QStringLiteral("数据格式"), sendPageFormat);
    sendConfigForm->addRow(QStringLiteral("重复次数"), sendRepeatSpin);
    sendConfigForm->addRow(sendAppendLine);
    sendConfigForm->addRow(sendCyclic); sendConfigForm->addRow(QStringLiteral("循环间隔"), sendIntervalSpin);
    sendConfigLayout->addWidget(sendConfigGroup);
    auto *payloadGroup = new QGroupBox(QStringLiteral("发送数据"));
    auto *payloadLayout = new QVBoxLayout(payloadGroup);
    sendPayloadEdit = new QPlainTextEdit; sendPayloadEdit->setObjectName("sendPayloadEdit");
    sendPayloadEdit->setPlaceholderText(QStringLiteral("输入要发送的数据，例如 ASCII：I\\n，或 HEX：49 01 A0 FF"));
    sendPayloadEdit->setMinimumHeight(150); sendPayloadEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    payloadLayout->addWidget(sendPayloadEdit, 1);
    auto *sendActionRow = new QHBoxLayout;
    auto *sendConfiguredButton = new QPushButton(QStringLiteral("发送配置数据"));
    sendConfiguredButton->setObjectName("sendConfiguredButton");
    auto *sendClearButton = new QPushButton(QStringLiteral("清空输入"));
    sendClearButton->setObjectName("sendClearButton");
    auto *sendLoadButton = new QPushButton(QStringLiteral("载入文件…")); sendLoadButton->setObjectName("sendLoadButton");
    auto *sendSineButton = new QPushButton(QStringLiteral("生成正弦测试")); sendSineButton->setObjectName("sendSineButton");
    sendActionRow->addWidget(sendConfiguredButton); sendActionRow->addWidget(sendClearButton); sendActionRow->addStretch();
    payloadLayout->addLayout(sendActionRow); sendConfigLayout->addWidget(payloadGroup, 1);
    auto *sendFileRow = new QHBoxLayout;
    auto *sendStopButton = new QPushButton(QStringLiteral("停止循环")); sendStopButton->setObjectName("sendStopButton");
    sendFileRow->addWidget(sendLoadButton); sendFileRow->addWidget(sendSineButton); sendFileRow->addWidget(sendStopButton); sendFileRow->addStretch();
    sendConfigLayout->addLayout(sendFileRow);
    auto *sendHint = new QLabel(QStringLiteral("TXD1：1024 B 载荷，自动加帧头和递增序号。\n测试数据按低16位 I、高16位 Q 生成；PL 如何解释字由板端逻辑决定。"));
    sendHint->setWordWrap(true); sendConfigLayout->addWidget(sendHint);
    sendSplit->addWidget(sendConfigWidget);

    auto *sendPreviewWidget = new QWidget; auto *sendPreviewLayout = new QVBoxLayout(sendPreviewWidget);
    auto *previewGroup = new QGroupBox(QStringLiteral("接收波形 / 发送数据可视化"));
    auto *previewLayout = new QVBoxLayout(previewGroup);
    sendPreviewStatus = new QLabel(QStringLiteral("等待输入数据")); sendPreviewStatus->setObjectName("sendPreviewStatus");
    sendPreviewStatus->setWordWrap(true); previewLayout->addWidget(sendPreviewStatus);
    sendHexView = new QPlainTextEdit; sendHexView->setObjectName("sendHexView"); sendHexView->setReadOnly(true);
    sendHexView->setLineWrapMode(QPlainTextEdit::WidgetWidth); sendHexView->setMaximumHeight(95);
    sendHexView->setPlaceholderText(QStringLiteral("HEX 预览")); previewLayout->addWidget(sendHexView);
    sendByteTable = new QTableWidget; sendByteTable->setObjectName("sendByteTable"); sendByteTable->setColumnCount(5);
    sendByteTable->setHorizontalHeaderLabels({QStringLiteral("偏移"), QStringLiteral("HEX"), QStringLiteral("十进制"),
                                              QStringLiteral("二进制"), QStringLiteral("字符")});
    sendByteTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    sendByteTable->setEditTriggers(QAbstractItemView::NoEditTriggers); sendByteTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto *sendViews = new QTabWidget; sendViews->setObjectName("sendViews");
    duplexReceivePlot = new PlotWidget(PlotWidget::Waveform); duplexReceivePlot->setObjectName("duplexReceivePlot");
    duplexReceivePlot->setSmoothDisplay(true);
    sendViews->addTab(duplexReceivePlot, QStringLiteral("接收 I/Q 波形"));
    sendIqPlot = new PlotWidget(PlotWidget::Waveform); sendIqPlot->setObjectName("sendIqPlot");
    sendViews->addTab(sendIqPlot, QStringLiteral("发送 I16/Q16 预览"));
    sendBytePlot = new PlotWidget(PlotWidget::SendBytes); sendBytePlot->setObjectName("sendBytePlot");
    sendViews->addTab(sendBytePlot, QStringLiteral("字节曲线")); sendViews->addTab(sendByteTable, QStringLiteral("字节明细"));
    previewLayout->addWidget(sendViews, 1); sendPreviewLayout->addWidget(previewGroup, 3);
    auto *historyGroup = new QGroupBox(QStringLiteral("发送历史")); auto *historyLayout = new QVBoxLayout(historyGroup);
    sendHistoryEdit = new QPlainTextEdit; sendHistoryEdit->setObjectName("sendHistoryEdit"); sendHistoryEdit->setReadOnly(true);
    sendHistoryEdit->setMaximumBlockCount(100); sendHistoryEdit->setPlaceholderText(QStringLiteral("发送后显示最近的数据摘要"));
    historyLayout->addWidget(sendHistoryEdit); sendPreviewLayout->addWidget(historyGroup, 1);
    sendSplit->addWidget(sendPreviewWidget); sendSplit->setStretchFactor(0, 1); sendSplit->setStretchFactor(1, 2);
    tabs->insertTab(1, sendPage, QStringLiteral("收发工作台"));
    connect(sendPageFormat, &QComboBox::currentIndexChanged, this, &MainWindow::updateSendPreview);
    connect(sendProtocolBox, &QComboBox::currentIndexChanged, this, &MainWindow::updateSendProtocolControls);
    connect(sendRepeatSpin, qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::updateSendPreview);
    connect(sendAppendLine, &QCheckBox::toggled, this, &MainWindow::updateSendPreview);
    connect(sendPayloadEdit, &QPlainTextEdit::textChanged, this, &MainWindow::updateSendPreview);
    connect(sendConfiguredButton, &QPushButton::clicked, this, &MainWindow::sendConfigured);
    connect(sendClearButton, &QPushButton::clicked, this, [this] { sendPayloadEdit->clear(); });
    sendTimer = new QTimer(this); sendTimer->setObjectName("sendTimer");
    connect(sendTimer, &QTimer::timeout, this, [this] { if (!txAwaitingAck) sendBytes(cyclicPacket); });
    connect(sendStopButton, &QPushButton::clicked, this, &MainWindow::stopCyclicSend);
    connect(sendCyclic, &QCheckBox::toggled, this, [this](bool checked) { if (!checked) stopCyclicSend(); });
    connect(sendIntervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::stopCyclicSend);
    connect(sendLoadButton, &QPushButton::clicked, this, [this] {
        const QString filename = QFileDialog::getOpenFileName(this, QStringLiteral("载入发送文件（最多 64 KiB）"), {}, "All files (*)");
        if (filename.isEmpty()) return;
        QByteArray bytes; QString description, error;
        if (!TxPayload::loadFile(filename, &bytes, &description, &error)) {
            sendPreviewStatus->setText(error); log(error); return;
        }
        if (payloadProtocolEnabled()) {
            QByteArray payload;
            if (!TxPayload::normalize(bytes, &payload, &error)) { sendPreviewStatus->setText(error); log(error); return; }
            bytes = payload;
        }
        stopCyclicSend(); sendPageFormat->setCurrentIndex(1);
        sendPayloadEdit->setPlainText(QString::fromLatin1(bytes.toHex(' ')));
        log(QStringLiteral("载入 %1：%2；输入载荷 %3 B").arg(filename, description).arg(bytes.size()));
    });
    connect(sendSineButton, &QPushButton::clicked, this, [this] {
        sendProtocolBox->setCurrentIndex(0); sendPageFormat->setCurrentIndex(1);
        sendPayloadEdit->setPlainText(QString::fromLatin1(TxPayload::sine().toHex(' ')));
        log(QStringLiteral("已生成 256 组 I16/Q16 正弦：I=12000*sin(2πn/64)，Q=0；4 周期，1024 B 载荷。"));
    });
    sendRepeatSpin->setEnabled(false); sendAppendLine->setEnabled(false);
    updateSendPreview();
    auto *printRow = new QHBoxLayout;
    auto *printDemodButton = new QPushButton(QStringLiteral("打印解调数据"));
    printDemodButton->setObjectName("printDemodButton");
    printDemodButton->setToolTip(QStringLiteral("打印当前显示的解调帧，包含来源序号、全部原始字、小端字节和完整 I/Q 比特序列；后续接收不会覆盖打印内容。"));
    copyDemodPrintButton = new QPushButton(QStringLiteral("复制打印内容"));
    copyDemodPrintButton->setObjectName("copyDemodPrintButton"); copyDemodPrintButton->setEnabled(false);
    saveDemodPrintButton = new QPushButton(QStringLiteral("保存打印 TXT…"));
    saveDemodPrintButton->setObjectName("saveDemodPrintButton"); saveDemodPrintButton->setEnabled(false);
    printRow->addWidget(printDemodButton); printRow->addWidget(copyDemodPrintButton);
    printRow->addWidget(saveDemodPrintButton); printRow->addStretch(); right->addLayout(printRow);
    connect(printDemodButton, &QPushButton::clicked, this, &MainWindow::printDemodData);
    connect(copyDemodPrintButton, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(demodPrintEdit->toPlainText());
    });
    connect(saveDemodPrintButton, &QPushButton::clicked, this, &MainWindow::saveDemodPrint);
    logEdit = new QPlainTextEdit; logEdit->setObjectName("logEdit");
    logEdit->setReadOnly(true); logEdit->setMaximumBlockCount(500); logEdit->setMaximumHeight(145); right->addWidget(logEdit);
    auto *sendRow = new QHBoxLayout;
    sendFormat = new QComboBox; sendFormat->addItems({"ASCII / UTF-8", "HEX"}); sendFormat->setObjectName("sendFormat");
    sendEdit = new QLineEdit; sendEdit->setObjectName("sendEdit");
    sendEdit->setPlaceholderText(QStringLiteral("发送内容，例如 I 或 HEX 01"));
    auto *sendButton = new QPushButton(QStringLiteral("发送")); sendButton->setObjectName("sendButton");
    auto *captureButton = new QPushButton(QStringLiteral("请求采集 I")); captureButton->setObjectName("captureButton");
    sendRow->addWidget(sendFormat); sendRow->addWidget(sendEdit, 1); sendRow->addWidget(sendButton); sendRow->addWidget(captureButton);
    connect(sendButton, &QPushButton::clicked, this, &MainWindow::sendInput);
    connect(sendEdit, &QLineEdit::returnPressed, this, &MainWindow::sendInput);
    captureButton->setToolTip(QStringLiteral("通过接收连接向采集端发送 I，不走独立发送端口。"));
    connect(captureButton, &QPushButton::clicked, this, [this] { sendBytes("I", true); });
    right->addLayout(sendRow); splitter->addWidget(rightWidget);
    connect(tabs, &QTabWidget::currentChanged, this,
            [this, spectrumPage, sendPage, printDemodButton](int) {
                const bool spectrumVisible = tabs->currentWidget() == spectrumPage;
                const bool sendVisible = tabs->currentWidget() == sendPage;
                logEdit->setMaximumHeight(spectrumVisible || sendVisible ? 65 : 145);
                printDemodButton->setVisible(!spectrumVisible && !sendVisible);
                copyDemodPrintButton->setVisible(!spectrumVisible && !sendVisible);
                saveDemodPrintButton->setVisible(!spectrumVisible && !sendVisible);
                frameLabel->setVisible(!sendVisible); demodStatus->setVisible(!sendVisible);
                waveformStatus->setVisible(!sendVisible);
            });
    splitter->setStretchFactor(0, 0); splitter->setStretchFactor(1, 1); splitter->setSizes({310, 930});
    statsLabel = new QLabel; statsLabel->setObjectName("statsLabel"); statusBar()->addWidget(statsLabel, 1);
    statsLabel->setText(QStringLiteral("RX 0 B   TX 0 B   有效帧 0"));
    setStyleSheet(R"(
    QMainWindow, QWidget {
        font-family: "Microsoft YaHei", "Segoe UI";
        font-size: 12px;
        background-color: white;
        color: black;
    }

    QLabel {
        background: transparent;
        color: black;
    }

    QGroupBox {
        background: white;
        border: 2px solid #7b8794;
        border-radius: 9px;
        margin-top: 11px;
        padding: 13px 9px 8px;
    }

    QGroupBox::title {
        subcontrol-origin: margin;
        left: 12px;
        padding: 0 6px;
        color: black;
    }

    QPushButton {
        min-height: 29px;
        padding: 3px 10px;
        background: white;
        color: black;
        border: 2px solid #65717c;
        border-radius: 5px;
    }

    QPushButton:hover { background: #e8edf2; border-color: #35404a; }
    QPushButton:pressed { background: #d6dfe8; border-color: #202830; }
    QPushButton:disabled {
        background: #f5f5f5;
        color: black;
        border-color: #dddddd;
    }

    QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
        min-height: 30px;
        padding: 2px 7px;
        background: white;
        color: black;
        border: 2px solid #65717c;
        border-radius: 5px;
        selection-background-color: #dcecff;
        selection-color: black;
    }

    QComboBox QAbstractItemView {
        background: white;
        color: black;
        selection-background-color: #dcecff;
        selection-color: black;
    }

    QCheckBox {
        color: black;
        background: transparent;
        spacing: 7px;
    }

    QTabWidget::pane {
        background: white;
        border: 2px solid #7b8794;
    }

    QTabBar::tab {
        background: #f0f0f0;
        color: black;
        padding: 7px 13px;
        border: 2px solid #7b8794;
    }

    QTabBar::tab:selected {
        background: white;
        color: black;
        font-weight: bold;
    }

    QPlainTextEdit, QTableView, QTableWidget {
        background: white;
        alternate-background-color: #f5f5f5;
        color: black;
        border: 2px solid #7b8794;
        selection-background-color: #dcecff;
        selection-color: black;
        gridline-color: #dddddd;
    }

    QHeaderView::section {
        background: #f0f0f0;
        color: black;
        padding: 5px;
        border: 1px solid #aeb7c0;
    }

    QStatusBar, QSplitter::handle {
        background: white;
        color: black;
    }

    QToolTip {
        background: white;
        color: black;
        border: 2px solid #65717c;
    }

    QPlainTextEdit {
        font-family: Consolas, "Microsoft YaHei";
    }

    QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
        border: 2px solid #303943;
    }

    QSpinBox, QDoubleSpinBox {
        padding-right: 34px;
    }

    QSpinBox::up-button, QDoubleSpinBox::up-button {
        subcontrol-origin: border;
        subcontrol-position: top right;
        width: 28px;
        height: 17px;
        margin-top: 2px;
        margin-right: 2px;
        background: #e2e7ed;
        border-left: 2px solid #65717c;
        border-bottom: 1px solid #65717c;
        border-top-right-radius: 3px;
    }

    QSpinBox::down-button, QDoubleSpinBox::down-button {
        subcontrol-origin: border;
        subcontrol-position: bottom right;
        width: 28px;
        height: 17px;
        margin-bottom: 2px;
        margin-right: 2px;
        background: #e2e7ed;
        border-left: 2px solid #65717c;
        border-top: 1px solid #65717c;
        border-bottom-right-radius: 3px;
    }

    QSpinBox::up-arrow, QDoubleSpinBox::up-arrow,
    QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
        width: 12px;
        height: 8px;
    }

    QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
    QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
        background: #cbd5df;
    }

    QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed,
    QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {
        background: #acbac8;
    }

    QComboBox {
        padding-right: 34px;
    }

    QComboBox::drop-down {
        subcontrol-origin: border;
        subcontrol-position: top right;
        width: 28px;
        margin: 2px;
        background: #e2e7ed;
        border-left: 2px solid #65717c;
    }

    QComboBox::down-arrow {
        width: 12px;
        height: 8px;
    }

    QComboBox::drop-down:hover {
        background: #cbd5df;
    }

    QTabBar::tab:selected {
        background: #e2e7ed;
        border: 2px solid #65717c;
    }

    QCheckBox::indicator {
        width: 16px;
        height: 16px;
    }
)");
    // Keep the arrow image files alive for the lifetime of this window.
    auto controlArrowImage = [this](bool up) {
        QImage image(12, 8, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter arrowPainter(&image);
            arrowPainter.setPen(Qt::NoPen);
            arrowPainter.setBrush(Qt::black);
            const QPolygon triangle = up
                                          ? QPolygon{QPoint(0, 7), QPoint(6, 0), QPoint(11, 7)}
                                          : QPolygon{QPoint(0, 0), QPoint(11, 0), QPoint(6, 7)};
            arrowPainter.drawPolygon(triangle);
        }
        auto *imageFile = new QTemporaryFile(this);
        if (!imageFile->open()) return QString();
        if (!image.save(imageFile, "PNG")) return QString();
        imageFile->flush();
        QString path = imageFile->fileName();
        imageFile->close();
        return path.replace(QChar('\\'), QChar('/'));
    };
    const QString upArrowImage = controlArrowImage(true);
    const QString downArrowImage = controlArrowImage(false);
    if (!upArrowImage.isEmpty() && !downArrowImage.isEmpty()) {
        setStyleSheet(styleSheet() + QStringLiteral(
                                         "QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(\"%1\"); }"
                                         "QSpinBox::down-arrow, QDoubleSpinBox::down-arrow, QComboBox::down-arrow { image: url(\"%2\"); }")
                                         .arg(upArrowImage, downArrowImage));
    }

}

MonitorLayout MainWindow::layout() const
{
    MonitorLayout result;
    result.iqSamples = iqSpin->value(); result.demodBytes = demodSpin->value();
    result.fftPoints = fftSpin->value(); result.iqEncoding = IqEncoding(iqBox->currentIndex());
    return result;
}

void MainWindow::updateWaveformView()
{
    const int total = wavePlot->sampleCount();
    const int requested = waveWindowBox->currentData().toInt();
    const int count = requested > 0 ? std::min(requested, total) : total;
    {
        const QSignalBlocker blocker(waveStartSpin);
        waveStartSpin->setMaximum(std::max(0, total - count));
        waveStartSpin->setSingleStep(std::max(1, count));
    }
    const int start = waveStartSpin->value();
    waveStartSpin->setEnabled(total > count);
    wavePreviousButton->setEnabled(start > 0);
    waveNextButton->setEnabled(start < total - count);
    waveRangeLabel->setText(total > 0
                                ? QStringLiteral("%1–%2 / %3 点").arg(start).arg(start + count - 1).arg(total)
                                : QStringLiteral("等待采样"));
    wavePlot->setSampleWindow(start, requested); qWavePlot->setSampleWindow(start, requested);
    if (duplexReceivePlot) {
        duplexReceivePlot->setData(displayedWaveform.i, displayedWaveform.q);
        duplexReceivePlot->setSampleWindow(start, requested);
    }
}

void MainWindow::updateSpectrum()
{
    if (spreadCompareBox && spreadCompareBox->isChecked()) {
        updateSpreadComparison(); return;
    }
    spectrumPlot->setSpectrumComparison(false);
    spectrumAmplitudePanel->show();
    spreadAverageBox->setEnabled(false); spreadDcBox->setEnabled(false); spreadMarkersBox->setEnabled(false);
    spectrumAmplitudeBox->setEnabled(true); spectrumLevelSpin->setEnabled(true);
    spectrumDivisionBox->setEnabled(true);
    const double rate = sampleRateSpin->value();
    spectrumBandwidthLabel->setText(QStringLiteral("带宽 %1 MHz（%2～+%3 MHz）")
                                        .arg(rate, 0, 'g', 6).arg(-rate / 2, 0, 'g', 6).arg(rate / 2, 0, 'g', 6));
    spectrumPlot->setSpectrumSampleRate(rate);
    const bool dbfs = spectrumAmplitudeBox->currentIndex() == 0;
    const double fullScale = layout().iqEncoding == IqEncoding::SignedI16Q16 ? 32768 : 2048;
    spectrumPlot->setSpectrumAmplitudeScale(dbfs);
    spectrumPlot->setSpectrumAxis(spectrumLevelSpin->value(), spectrumDivisionBox->currentData().toDouble());
    spectrumReferenceLabel->setText(dbfs
                                        ? QStringLiteral("幅度参考 %1 · 固定参考 dB").arg(fullScale, 0, 'f', 0)
                                        : QStringLiteral("本帧峰值固定为 0 dB，显示频谱形状"));
    const auto spectrum = calculateIqSpectrum(displayedWaveform.i, displayedWaveform.q, rate,
                                              SpectrumInput(spectrumInputBox->currentIndex()), spectrumWindowBox->currentIndex() == 0, fullScale);
    if (spectrum.fftSize == 0) {
        spectrumPlot->clear(); spectrumStatus->setText(QStringLiteral("等待至少 2 组有效 IQ 采样")); return;
    }
    spectrumPlot->setData(dbfs ? spectrum.dbfs : spectrum.relativeDb);
    spectrumStatus->setText(QStringLiteral("来源序号 %1 · %2 · FFT %3 / %4 点 · 频率间隔 %5 kHz · %6")
                                .arg(displayedWaveform.sequence).arg(spectrumInputBox->currentText())
                                .arg(spectrum.fftSize).arg(spectrum.inputSamples).arg(spectrum.binWidthMHz * 1000, 0, 'f', 3)
                                .arg(spectrum.hasSignal
                                         ? QStringLiteral("峰值 %1 MHz（%2 %3）").arg(spectrum.peakFrequencyMHz, 0, 'f', 6)
                                               .arg(dbfs ? spectrum.peakDbfs : 0, 0, 'f', 2).arg("dB")
                                         : QStringLiteral("全零数据，无可测峰值")));
}

void MainWindow::updateSpreadComparison()
{
    const double rxRate=sampleRateSpin->value();
    const double refRate=displayedReference.sampleRateHz/1e6;
    const auto input=SpectrumInput(spectrumInputBox->currentIndex());
    const bool hann=spectrumWindowBox->currentIndex()==0, dc=spreadDcBox->isChecked();
    const int depth=spreadAverageBox->currentData().toInt();
    const QString config=QStringLiteral("%1/%2/%3/%4/%5")
        .arg(rxRate,0,'g',12).arg(int(input)).arg(hann).arg(dc).arg(depth);
    if (config!=spreadSpectrumConfig) {
        spreadSpectrumConfig=config; spreadRxAverage.reset(); spreadReferenceCached=false;
    }
    if (displayedReference.i.isEmpty()) {
        spreadReferenceEstimate=WelchSpectrum(); spreadReferenceCached=false;
    } else if (!spreadReferenceCached || spreadReferenceId!=displayedReference.captureId ||
               spreadReferenceEstimate.sampleRateMHz!=refRate) {
        spreadReferenceEstimate=calculateWelchSpectrum(displayedReference.i,displayedReference.q,
            refRate,input,hann,4096,dc);
        spreadReferenceId=displayedReference.captureId; spreadReferenceCached=true;
    }
    // 128-sample segments provide 15 overlapping estimates from a 1024-IQ
    // block. This is appropriate to the broad ~4.08 MHz chip spectrum; the
    // narrow reference uses 4096 samples to retain its ~80 kHz first null.
    const auto block=calculateWelchSpectrum(displayedWaveform.i,displayedWaveform.q,
        rxRate,input,hann,128,dc);
    const auto rx=spreadRxAverage.accept(block,displayedWaveform.sequence,depth);
    const auto &ref=spreadReferenceEstimate;
    spectrumAmplitudeBox->setEnabled(false); spectrumLevelSpin->setEnabled(false);
    spectrumDivisionBox->setEnabled(false);
    spectrumAmplitudePanel->hide();
    spreadAverageBox->setEnabled(true); spreadDcBox->setEnabled(true); spreadMarkersBox->setEnabled(true);
    spectrumReferenceLabel->setText(QStringLiteral("功率谱各自归一化 · 不比较绝对功率"));
    spectrumBandwidthLabel->setText(QStringLiteral("Fs：接收 %1 / 扩频前 %2 MS/s")
        .arg(rxRate,0,'g',6).arg(refRate>0?QString::number(refRate,'g',6):QStringLiteral("等待")));
    spectrumPlot->setSpectrumComparison(true,refRate);
    spectrumPlot->setSpreadSpectrumData(rx,ref,spreadMarkersBox->isChecked());
    spreadExportButton->setEnabled(!displayedReference.i.isEmpty());
    QString status;
    if (!ref.power.isEmpty()) {
        status=QStringLiteral("红：采集 %1，%2 点实采；%3 点/段 × %4 段；等效分辨带宽 %5 kHz；B99 %6 kHz。\n")
            .arg(displayedReference.captureId).arg(displayedReference.i.size()).arg(ref.segmentSamples)
            .arg(ref.segmentCount).arg(ref.resolutionBandwidthMHz*1000,0,'f',2)
            .arg(ref.occupiedBandwidthMHz*1000,0,'f',2);
    } else status=QStringLiteral("红：等待完整扩频前实采，请先发送业务数据。\n");
    if (!rx.power.isEmpty()) {
        status+=QStringLiteral("蓝：序号 %1；%2 点/段 × %3 段/组，已平均 %4/%5 组；等效分辨带宽 %6 kHz；B99 %7 MHz。\n")
            .arg(displayedWaveform.sequence).arg(rx.segmentSamples).arg(rx.segmentCount).arg(rx.snapshotCount)
            .arg(depth).arg(rx.resolutionBandwidthMHz*1000,0,'f',2).arg(rx.occupiedBandwidthMHz,0,'f',3);
    } else status+=QStringLiteral("蓝：等待接收 IQ。\n");
    status+=QStringLiteral("虚线仅为当前配置理论首零点；实测含滤波/噪声，B99 不等于零点带宽。两端为独立快照，接收仍跟随原暂停/冻结。");
    spectrumStatus->setText(status);
}

void MainWindow::setSettingsEnabled(bool enabled)
{
    modeBox->setEnabled(enabled); addressEdit->setEnabled(enabled); portSpin->setEnabled(enabled);
    iqBox->setEnabled(enabled); iqSpin->setEnabled(enabled); demodSpin->setEnabled(enabled); fftSpin->setEnabled(enabled);
    profileBox->setEnabled(enabled); wireBox->setEnabled(enabled);
}

void MainWindow::toggleNetwork()
{
    if (networkActive) { stopNetwork(); return; }
    if (demoTimer->isActive()) { log(QStringLiteral("请先停止演示，再打开网络。")); return; }
    if (demodSpin->value() % 4) { log(QStringLiteral("解调字节数必须为 4 的倍数。")); return; }
    lastDemodResult = MonitorData(); demodDirty = true;
    const MonitorLayout settings = layout(); const bool raw = wireBox->currentIndex() == 1;
    const quint64 session = ++networkSession;
    if (modeBox->currentIndex() == 0) {
        QHostAddress address;
        if (!address.setAddress(addressEdit->text().trimmed())) { log(QStringLiteral("监听地址应是本机 IP，或 0.0.0.0。")); return; }
        QString error; const quint16 port = quint16(portSpin->value());
        QMetaObject::invokeMethod(receiver, [&, worker = receiver] {
            error = worker->startServer(address, port, settings, raw, session);
        }, Qt::BlockingQueuedConnection);
        if (!error.isEmpty()) { log(QStringLiteral("监听失败：%1").arg(error)); return; }
        stateLabel->setText(QStringLiteral("监听 %1:%2，等待板端连接").arg(address.toString()).arg(port)); log(stateLabel->text());
    } else {
        if (addressEdit->text().trimmed().isEmpty()) { log(QStringLiteral("请填写远端服务器地址。")); return; }
        stateLabel->setText(QStringLiteral("正在连接 %1:%2…").arg(addressEdit->text()).arg(portSpin->value()));
    }
    networkActive = true; networkButton->setText(QStringLiteral("停止 / 断开"));
    demoButton->setEnabled(false); setSettingsEnabled(false);
    if (modeBox->currentIndex() == 1) {
        connectTimer->start(5000);
        const QString address = addressEdit->text().trimmed(); const quint16 port = quint16(portSpin->value());
        QMetaObject::invokeMethod(receiver, [worker = receiver, address, port, settings, raw, session] {
            worker->startClient(address, port, settings, raw, session);
        }, Qt::QueuedConnection);
    }
}

void MainWindow::stopNetwork()
{
    if (sendConnectionBox->currentIndex() == 0) stopCyclicSend();
    connectTimer->stop(); networkActive = false; ++networkSession;
    QMetaObject::invokeMethod(receiver, [worker = receiver] { worker->stop(); }, Qt::BlockingQueuedConnection);
    networkButton->setText(modeBox->currentIndex() == 0 ? QStringLiteral("开始监听") : QStringLiteral("连接服务器"));
    stateLabel->setText(QStringLiteral("未连接")); demoButton->setEnabled(true); setSettingsEnabled(true);
    log(QStringLiteral("网络已停止。"));
}

void MainWindow::toggleSendNetwork()
{
    if (sendNetworkActive) { stopSendNetwork(); return; }
    if (sendConnectionBox->currentIndex() == 0) return;
    const QString address = sendAddressEdit->text().trimmed();
    if (address.isEmpty()) { log(QStringLiteral("请填写发送目标地址。")); return; }
    const quint16 port = quint16(sendPortSpin->value());
    const quint64 session = ++sendNetworkSession;
    sendNetworkActive = true; sendAddressEdit->setEnabled(false); sendPortSpin->setEnabled(false);
    sendNetworkButton->setText(QStringLiteral("断开发送连接"));
    sendStateLabel->setText(QStringLiteral("正在连接发送端 %1:%2…").arg(address).arg(port));
    sendConnectTimer->start(5000);
    QMetaObject::invokeMethod(sender, [worker = sender, address, port, session] {
        worker->startClient(address, port, session);
    }, Qt::QueuedConnection);
}

void MainWindow::stopSendNetwork()
{
    if (sendConnectionBox->currentIndex() == 1) stopCyclicSend();
    sendConnectTimer->stop(); sendNetworkActive = false;
    QMetaObject::invokeMethod(sender, [worker = sender] { worker->stop(); }, Qt::BlockingQueuedConnection);
    if (txAwaitingAck) sendHistoryEdit->appendPlainText(QStringLiteral("发送已停止，尚未收到 TXA1，无法确认 PL 提交。"));
    txAwaitingAck = false; ++sendNetworkSession;
    const bool dedicated = sendConnectionBox->currentIndex() == 1;
    sendAddressEdit->setEnabled(dedicated); sendPortSpin->setEnabled(dedicated);
    sendNetworkButton->setText(QStringLiteral("连接发送端口")); sendNetworkButton->setEnabled(dedicated);
    sendStateLabel->setText(QStringLiteral("发送未连接 · 接收连接继续运行"));
    log(QStringLiteral("独立发送连接已停止，接收连接保持原状态。"));
}

void MainWindow::refresh()
{
    const qint64 now = elapsed.elapsed();
    const MonitorSnapshot snapshot = receiver->snapshot(now - previousMs >= 1000);
    const SendSnapshot sent = sender->snapshot(now - previousMs >= 1000);
    rxBytes = snapshot.rxBytes; txBytes = snapshot.txBytes + sent.txBytes; frames = snapshot.frames;
    duplexStatusLabel->setText(QStringLiteral("接收：%1 · RX %2 B / %3 帧%4   |   发送：%5 · TX %6 B · 应答 %7 B")
                                   .arg(stateLabel->text()).arg(rxBytes).arg(frames)
                                   .arg(pausePlots->isChecked() ? QStringLiteral("（图表暂停）")
                                        : !snapshot.frozenWaveform.i.isEmpty() ? QStringLiteral("（波形冻结）") : QString())
                                   .arg(sendConnectionBox->currentIndex() == 0 ? QStringLiteral("复用接收连接") : sendStateLabel->text())
                                   .arg(txBytes).arg(sent.rxBytes));
    rejected = snapshot.rejected; sequenceJumps = snapshot.sequenceJumps;
    nonzeroDemodSections = snapshot.nonzeroDemodSections;
    validDemodFrames = snapshot.validDemodFrames;
    if (snapshot.revision != seenRevision) {
        seenRevision = snapshot.revision; latest = snapshot.latest;
        lastDemodResult = snapshot.lastDemodResult; dirty = true;
    }
    if (now - previousMs >= 1000) {
        const double seconds = (now - previousMs) / 1000.0;
        statsLabel->setText(QStringLiteral("RX %1 B   TX %2 B   有效帧 %3   %4 KiB/s   %5 帧/s   拒绝 %6   序号跳变 %7   缓存 %8 B   完整解调帧 %9")
                                .arg(rxBytes).arg(txBytes).arg(frames).arg((rxBytes - previousRx) / seconds / 1024, 0, 'f', 1)
                                .arg((frames - previousFrames) / seconds, 0, 'f', 1).arg(rejected).arg(sequenceJumps)
                                .arg(snapshot.bufferedBytes).arg(validDemodFrames));
        previousRx = rxBytes; previousFrames = frames; previousMs = now;
        if (!snapshot.error.isEmpty()) log(snapshot.error);
        if (!sent.reply.isEmpty()) log(QStringLiteral("独立发送端应答（最近片段，最多 64 B）：%1")
                    .arg(QString::fromLatin1(sent.reply.toHex(' '))));
        if (!snapshot.hex.isEmpty()) {
            log(QStringLiteral("RX 最近片段前 64 B：%1").arg(QString::fromLatin1(snapshot.hex.toHex(' '))));
        }
    }
    if (!pausePlots->isChecked()) displayedReference = snapshot.spreadReference;
    if (!dirty || pausePlots->isChecked() || latest.i.isEmpty()) return;
    dirty = false;
    const bool frozen = freezeWaveform->isChecked() && !snapshot.frozenWaveform.i.isEmpty();
    const MonitorData &waveform = frozen ? snapshot.frozenWaveform : latest;
    wavePlot->setData(waveform.i); qWavePlot->setData(waveform.q);
    displayedWaveform = waveform; updateSpectrum();
    updateWaveformView();
    constellationPlot->setData(waveform.i, waveform.q);
    if (latest.type == 5)
        fftPlot->setAcquisitionFftData(snapshot.fftHistory, snapshot.successfulFft);
    else fftPlot->setData(latest.fftDb);
    resumeWaveformButton->setEnabled(frozen);
    waveformStatus->setText(frozen
                                ? QStringLiteral("I/Q 波形与星座图已冻结 · 来源序号 %1%2；继续接收，点击“恢复波形”后等待下次解调。")
                                      .arg(waveform.sequence)
                                      .arg(waveform.demodValidityKnown ? QString() : QStringLiteral("（解调有效性未知）"))
                                : (freezeWaveform->isChecked()
                                       ? QStringLiteral("I/Q 波形实时刷新，等待首次有效解调后冻结。")
                                       : QStringLiteral("I/Q 波形实时刷新（自动冻结已关闭）。")));
    const MonitorData &shown = holdDemod->isChecked() && !lastDemodResult.demodWords.isEmpty() ? lastDemodResult : latest;
    displayedDemod = shown;
    displayedDemodIsDemo = demoTimer->isActive();
    updateDemodWaveform();
    QString sectionStatus;
    if (latest.spreadReference.present) {
        sectionStatus = QStringLiteral("扩频前采样分片（不是解调帧）");
    } else if (latest.demodValidityKnown) {
        sectionStatus = latest.demodFrameValid
                            ? (latest.demodAllZero ? QStringLiteral("完整解调帧（全零数据）") : QStringLiteral("完整解调帧"))
                            : (latest.demodAllZero ? QStringLiteral("填零，本包无完整解调帧") : QStringLiteral("有效位未置位，非零区不能当作解调帧"));
    } else {
        sectionStatus = latest.demodAllZero ? QStringLiteral("全零，有效性未知") : QStringLiteral("非零，有效性未知");
    }
    const QString countStatus = latest.demodValidityKnown
                                    ? QStringLiteral("完整解调帧累计 %1（不代表比特正确）").arg(validDemodFrames)
                                    : QStringLiteral("非零区累计 %1（不代表解调成功）").arg(nonzeroDemodSections);
    demodStatus->setText(QStringLiteral("当前解调区：%1；显示来源序号 %2；%3%4%5")
                             .arg(sectionStatus).arg(shown.sequence).arg(countStatus)
                             .arg(shown.sequence != latest.sequence ? QStringLiteral("；保留历史结果") : QString())
                             .arg(!shown.demodValidityKnown ? QStringLiteral("；来源有效性未知") : QString()));
    if (demodDirty || renderedDemodWords != shown.demodWords || renderedPairOrder != pairOrderBox->currentIndex()) {
        demodDirty = false;
        renderedDemodWords = shown.demodWords; renderedPairOrder = pairOrderBox->currentIndex();
        QVector<double> iBits, qBits;
        unpackDemodWords(shown.demodWords.mid(0, 16), pairOrderBox->currentIndex() == 1, &iBits, &qBits);
        bitPlot->setData(iBits); qBitPlot->setData(qBits);
        symbolModel->setWords(shown.demodWords, pairOrderBox->currentIndex() == 1);
        const int rows = std::min(256, int(shown.demodWords.size()));
        demodTable->setUpdatesEnabled(false); demodTable->setRowCount(rows);
        for (int n = 0; n < rows; ++n) {
            const QString values[] = {QString::number(n), QString("0x%1").arg(shown.demodWords[n], 8, 16, QLatin1Char('0')),
                                      QString::number(shown.demodWords[n])};
            for (int column = 0; column < 3; ++column) {
                if (!demodTable->item(n, column)) demodTable->setItem(n, column, new QTableWidgetItem);
                demodTable->item(n, column)->setText(values[column]);
            }
        }
        demodTable->setUpdatesEnabled(true);
    }
    frameLabel->setText(QStringLiteral("%1序号 %2  ·  IQ %3 点  ·  FFT %4 点  ·  flags 0x%5  ·  丢弃 %6 B / 无效帧头 %7")
                            .arg(demoTimer->isActive() ? QStringLiteral("[演示数据] ") : QString()).arg(latest.sequence)
                            .arg(latest.i.size()).arg(latest.type == 5 && !snapshot.fftHistory.isEmpty()
                                ? snapshot.fftHistory.last().power.size() : latest.fftDb.size()).arg(latest.flags, 4, 16, QLatin1Char('0'))
                            .arg(snapshot.discardedBytes).arg(snapshot.invalidHeaders));
    frameLabel->setText(frameLabel->text() + QStringLiteral(" · type %1").arg(latest.type));
    if (wireBox->currentIndex() == 1 && !demoTimer->isActive())
        frameLabel->setText(frameLabel->text() + QStringLiteral(" · 无帧头：本地序号，起点需对齐"));
    else if (latest.fftDb.isEmpty())
        frameLabel->setText(frameLabel->text() + QStringLiteral(" · 降采样 IQ"));
}

void MainWindow::toggleDemo()
{
    if (demoTimer->isActive()) {
        demoTimer->stop(); demoButton->setText(QStringLiteral("启动演示（无需板卡）"));
        networkButton->setEnabled(true); setSettingsEnabled(true); stateLabel->setText(QStringLiteral("演示已停止"));
        log(QStringLiteral("演示已停止；图表保留最后一帧演示数据。")); return;
    }
    if (networkActive) return;
    if (demodSpin->value() % 4) { log(QStringLiteral("解调字节数必须为 4 的倍数。")); return; }
    clearData(); demoSequence = 0; networkButton->setEnabled(false); setSettingsEnabled(false);
    const MonitorLayout settings = layout();
    QMetaObject::invokeMethod(receiver, [worker = receiver, settings] { worker->startDemo(settings); }, Qt::BlockingQueuedConnection);
    demoButton->setText(QStringLiteral("停止演示")); stateLabel->setText(QStringLiteral("演示模式 · 模拟信号"));
    log(QStringLiteral("演示使用合成 IQ 与解调字（每十包一次，其余填零）；旧版另含 FFT。")); demoTimer->start(80);
}

void MainWindow::startPreview() { if (!demoTimer->isActive()) toggleDemo(); }

void MainWindow::updateDemodWaveform()
{
    const auto &words = displayedDemod.demodWords;
    const int format = demodDisplayBox->currentIndex();
    const bool paired = format == 0, int16Samples = format == 3;
    QVector<double> first, second;
    if (paired || int16Samples) {
        demodPlot->setMode(PlotWidget::DemodSamples);
        auto signed16 = [](quint32 value) { const int half = int(value & 0xffffU); return half >= 32768 ? half - 65536 : half; };
        for (quint32 word : words) {
            first.append(signed16(word));
            if (paired) second.append(signed16(word >> 16));
            else first.append(signed16(word >> 16));
        }
    } else {
        const bool signedWords = format == 1;
        demodPlot->setMode(signedWords ? PlotWidget::Demod : PlotWidget::DemodUnsigned);
        for (quint32 word : words) first.append(signedWords && (word & 0x80000000U)
                             ? double(qint64(word) - 0x100000000LL) : double(word));
    }
    demodPlot->setData(first, second);
    const int selected = demodWindowBox->currentData().toInt();
    const int count = selected == 0 ? first.size() : std::min(selected, int(first.size()));
    { const QSignalBlocker blocker(demodStartSpin); demodStartSpin->setRange(0, std::max(0, int(first.size()) - count)); }
    const int start = demodStartSpin->value();
    demodPlot->setSampleWindow(start, count);
    demodPreviousButton->setEnabled(start > 0);
    demodNextButton->setEnabled(start + count < first.size());
    QString text = words.isEmpty() ? QStringLiteral("等待解调数据")
                                   : QStringLiteral("来源序号 %1 · %2 %3–%4 / 共 %5 · %6")
                                         .arg(displayedDemod.sequence).arg(QStringLiteral("采样"))
                                         .arg(start).arg(start + count - 1).arg(first.size())
                                         .arg(paired ? QStringLiteral("小端载荷还原：低16位 I、高16位 Q；青色 I、橙色 Q")
                                              : int16Samples ? QStringLiteral("小端载荷还原：连续 int16 数值，低半字先")
                                                             : QStringLiteral("每个解调字还原为一个32位数值"));
    if (!words.isEmpty()) text += QStringLiteral(" · 数据取自接收解调区");
    bool counter = words.size() >= 16;
    for (int n = 0; counter && n < words.size() - 1; ++n) counter = words[n] == quint32(n);
    if (counter) text += QStringLiteral("\n疑似递增计数测试数据：前 %1 字等于索引，末字 0x%2；请核对 PL 数据源和末字高位。")
                    .arg(words.size() - 1).arg(words.last(), 8, 16, QLatin1Char('0'));
    demodViewStatus->setText(text);
}

QByteArray MainWindow::decodeSendPayload(const QString &input, int format, QString *error) const
{
    if (format == 0) return input.toUtf8();
    QString text = input;
    text.remove(' '); text.remove('\t'); text.remove('\r'); text.remove('\n');
    if (text.isEmpty()) return {};
    if (text.size() % 2) { if (error) *error = QStringLiteral("HEX 必须为偶数个十六进制字符。"); return {}; }
    for (QChar c : text) {
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F')) {
            if (error) *error = QStringLiteral("HEX 含有无效字符，请使用 0–9、A–F。");
            return {};
        }
    }
    return QByteArray::fromHex(text.toLatin1());
}

void MainWindow::updateSendPreview()
{
    if (!sendPayloadEdit || !sendPageFormat) return;
    stopCyclicSend();
    QString error;
    QByteArray bytes = decodeSendPayload(sendPayloadEdit->toPlainText(), sendPageFormat->currentIndex(), &error);
    if (sendPageFormat->currentIndex() == 0 && sendAppendLine->isChecked() && !bytes.isEmpty()) bytes.append("\r\n");
    QByteArray iqBytes;
    if (error.isEmpty() && payloadProtocolEnabled() && !bytes.isEmpty()) {
        if (TxPayload::normalize(bytes, &iqBytes, &error)) bytes = TxPayload::frame(iqBytes, quint32(sendSequence + 1));
    }
    sendIqPlot->clear();
    if (!iqBytes.isEmpty()) {
        QVector<double> i, q;
        for (int n = 0; n < iqBytes.size(); n += 4) {
            i.append(qFromLittleEndian<qint16>(iqBytes.constData() + n));
            q.append(qFromLittleEndian<qint16>(iqBytes.constData() + n + 2));
        }
        sendIqPlot->setData(i, q);
    }
    if (!error.isEmpty()) {
        sendPreviewStatus->setText(QStringLiteral("输入错误：%1").arg(error));
        sendHexView->clear(); sendByteTable->setRowCount(0); sendBytePlot->clear(); return;
    }
    const int repeat = sendRepeatSpin->value();
    const qsizetype total = bytes.size() * qsizetype(repeat);
    sendPreviewStatus->setText(bytes.isEmpty()
                                   ? QStringLiteral("等待输入数据")
                                   : QStringLiteral("单次 %1 B · 重复 %2 次 · 实际发送 %3 B · 右侧表格最多显示前 256 B")
                                         .arg(bytes.size()).arg(repeat).arg(total));
    if (!iqBytes.isEmpty()) sendPreviewStatus->setText(QStringLiteral("TXD1 · 1024 B 载荷 + 12 B 帧头 = 1036 B · 预览序号 %1 · 等待 TXA1 确认 PL 提交").arg(quint32(sendSequence + 1)));
    sendHexView->setPlainText(QString::fromLatin1(bytes.left(512).toHex(' '))
                              + (bytes.size() > 512 ? QStringLiteral(" …（预览前 512 B）") : QString()));
    const int shown = std::min(256, int(bytes.size()));
    QVector<double> values; values.reserve(shown);
    for (int n = 0; n < shown; ++n) values.append(uchar(bytes.at(n)));
    sendBytePlot->setData(values);
    sendByteTable->setUpdatesEnabled(false); sendByteTable->setRowCount(shown);
    for (int n = 0; n < shown; ++n) {
        const uchar value = uchar(bytes.at(n));
        const QString printable = value >= 0x20 && value < 0x7f
                                      ? QString(QChar(value)) : QStringLiteral("·");
        const QString values[] = {QString::number(n), QString("0x%1").arg(value, 2, 16, QLatin1Char('0')),
                                  QString::number(value), QString("%1").arg(value, 8, 2, QLatin1Char('0')), printable};
        for (int column = 0; column < 5; ++column) {
            if (!sendByteTable->item(n, column)) sendByteTable->setItem(n, column, new QTableWidgetItem);
            sendByteTable->item(n, column)->setText(values[column]);
        }
    }
    sendByteTable->setUpdatesEnabled(true);
}

bool MainWindow::configuredPayload(QByteArray *packet, QString *error) const
{
    QByteArray bytes = decodeSendPayload(sendPayloadEdit->toPlainText(), sendPageFormat->currentIndex(), error);
    if (!error->isEmpty()) return false;
    if (sendPageFormat->currentIndex() == 0 && sendAppendLine->isChecked() && !bytes.isEmpty()) bytes.append("\r\n");
    if (bytes.isEmpty()) { *error = QStringLiteral("发送配置数据为空。"); return false; }
    if (payloadProtocolEnabled()) {
        if (sendRepeatSpin->value() != 1) { *error = QStringLiteral("TXD1 每次发送一帧，请使用循环发送并等待回执。"); return false; }
        return TxPayload::normalize(bytes, packet, error);
    }
    if (bytes.size() * qint64(sendRepeatSpin->value()) > 1024 * 1024) {
        *error = QStringLiteral("单次发送最多 1 MiB，请减少数据长度或重复次数。"); return false;
    }
    packet->clear(); packet->reserve(bytes.size() * sendRepeatSpin->value());
    for (int n = 0; n < sendRepeatSpin->value(); ++n) packet->append(bytes);
    return true;
}

bool MainWindow::payloadProtocolEnabled() const
{
    return sendConnectionBox->currentIndex() == 1 && sendProtocolBox->currentIndex() == 0;
}

void MainWindow::updateSendProtocolControls()
{
    const bool framed = payloadProtocolEnabled();
    sendRepeatSpin->setEnabled(!framed); sendAppendLine->setEnabled(!framed);
    if (framed) { sendRepeatSpin->setValue(1); sendAppendLine->setChecked(false); }
    updateSendPreview();
}

void MainWindow::stopCyclicSend()
{
    if (sendTimer) sendTimer->stop();
    cyclicPacket.clear();
}

void MainWindow::sendConfigured()
{
    if (txAwaitingAck) { sendPreviewStatus->setText(QStringLiteral("正在等待上一帧 TXA1 回执。")); return; }
    stopCyclicSend();
    QByteArray packet; QString error;
    if (!configuredPayload(&packet, &error)) { sendPreviewStatus->setText(error); log(error); return; }
    sendBytes(packet);
    if (sendCyclic->isChecked() && (sendConnectionBox->currentIndex() == 1 ? sendNetworkActive : networkActive)) {
        cyclicPacket = packet; sendTimer->start(sendIntervalSpin->value());
    }
}

void MainWindow::handleSendResult(quint64 request, const QByteArray &preview, qint64 total, const QString &error, bool stopLoopOnFailure)
{
    const QString time = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    if (!error.isEmpty()) {
        if (stopLoopOnFailure) txAwaitingAck = false;
        if (stopLoopOnFailure) stopCyclicSend();
        sendPreviewStatus->setText(error);
        sendHistoryEdit->appendPlainText(QStringLiteral("%1  #%2  %3").arg(time).arg(request).arg(error));
        return;
    }
    sendHistoryEdit->appendPlainText(QStringLiteral("%1  #%2  已入队 %3 B\nHEX: %4%5")
                                         .arg(time).arg(request).arg(total).arg(QString::fromLatin1(preview.left(64).toHex(' ')))
                                         .arg(total > 64 ? QStringLiteral(" …") : QString()));
    sendPreviewStatus->setText(QStringLiteral("第 %1 次发送已入队 %2 B · 接收继续运行")
                                   .arg(request).arg(total));
    if (txAwaitingAck && total == 1036 && preview.startsWith("TXD1")) {
        sendPreviewStatus->setText(QStringLiteral("TXD1 seq=%1 已入队 %2 B；等待 TXA1，尚未确认 PL 提交").arg(quint32(request)).arg(total));
        sendHexView->setPlainText(QString::fromLatin1(preview.toHex(' ')) + QStringLiteral(" …（已入队帧前 512 B）"));
        // The payload stays unchanged between cycles; only the sequence header changes.
        for (int n = 0; n < 12 && n < sendByteTable->rowCount(); ++n) {
            const uchar value = uchar(preview.at(n));
            const QString values[] = {QString::number(n), QString("0x%1").arg(value, 2, 16, QLatin1Char('0')),
                                      QString::number(value), QString("%1").arg(value, 8, 2, QLatin1Char('0')),
                                      value >= 0x20 && value < 0x7f ? QString(QChar(value)) : QStringLiteral("·")};
            for (int col = 0; col < 5; ++col) sendByteTable->item(n, col)->setText(values[col]);
        }
        QVector<double> values;
        for (char value : preview.left(256)) values.append(uchar(value));
        sendBytePlot->setData(values);
    }
}

void MainWindow::sendBytes(const QByteArray &bytes, bool useReceiveConnection)
{
    if (bytes.isEmpty()) { log(QStringLiteral("发送内容为空。")); return; }
    const quint64 request = ++sendSequence;
    sendPreviewStatus->setText(QStringLiteral("正在提交第 %1 次发送 · %2 B").arg(request).arg(bytes.size()));
    if (!useReceiveConnection && sendConnectionBox->currentIndex() == 1) {
        if (txAwaitingAck) { sendPreviewStatus->setText(QStringLiteral("正在等待上一帧 TXA1 回执。")); return; }
        if (!sendNetworkActive) {
            handleSendResult(request, {}, 0, QStringLiteral("发送失败：请先连接独立发送端口。")); return;
        }
        if (payloadProtocolEnabled()) {
            QByteArray payload; QString error;
            if (!TxPayload::normalize(bytes, &payload, &error)) { handleSendResult(request, {}, 0, error); return; }
            txAwaitingAck = true;
            QMetaObject::invokeMethod(sender, [worker = sender, payload, request] { worker->sendPayload(payload, request); }, Qt::QueuedConnection);
        } else QMetaObject::invokeMethod(sender, [worker = sender, bytes, request] { worker->send(bytes, request); }, Qt::QueuedConnection);
    } else {
        if (!networkActive) {
            handleSendResult(request, {}, 0, QStringLiteral("发送失败：当前没有 TCP 连接。"), sendConnectionBox->currentIndex() == 0); return;
        }
        QMetaObject::invokeMethod(receiver, [worker = receiver, bytes, request] { worker->send(bytes, request); }, Qt::QueuedConnection);
    }
}

void MainWindow::sendInput()
{
    if (sendFormat->currentIndex() == 0) { sendBytes(sendEdit->text().toUtf8()); return; }
    QString text = sendEdit->text(); text.remove(' '); text.remove('\t'); text.remove('\r'); text.remove('\n');
    if (text.isEmpty() || text.size() % 2) { log(QStringLiteral("HEX 必须为偶数个十六进制字符，例如 01 或 49。")); return; }
    for (QChar c : text) {
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F')) {
            log(QStringLiteral("HEX 含有无效字符，请使用 0–9、A–F（可用空格分隔）。")); return;
        }
    }
    sendBytes(QByteArray::fromHex(text.toLatin1()));
}

void MainWindow::toggleRecording()
{
    if (recordingActive) {
        QMetaObject::invokeMethod(receiver, [worker = receiver] { worker->stopRecording(); }, Qt::BlockingQueuedConnection);
        recordingActive = false; recordButton->setText(QStringLiteral("保存接收原始流…")); log(QStringLiteral("原始流保存结束。")); return;
    }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存原始 TCP 字节流"), "monitor.bin", "Binary (*.bin)");
    if (path.isEmpty()) return;
    QString error;
    QMetaObject::invokeMethod(receiver, [&, worker = receiver] { error = worker->startRecording(path); }, Qt::BlockingQueuedConnection);
    if (!error.isEmpty()) { log(QStringLiteral("打开文件失败：%1").arg(error)); return; }
    recordingActive = true;
    recordButton->setText(QStringLiteral("停止保存原始流")); log(QStringLiteral("开始保存后续接收的原始字节流：%1").arg(path));
}

void MainWindow::exportCsv()
{
    MonitorSnapshot snapshot = receiver->snapshot();
    latest = snapshot.latest; lastDemodResult = snapshot.lastDemodResult;
    if (latest.i.isEmpty()) { log(QStringLiteral("尚无有效帧可以导出。")); return; }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出最新已接收帧"),
                                                      QString("monitor_%1.csv").arg(latest.sequence), "CSV (*.csv)");
    if (path.isEmpty()) return;
    // Freeze one coherent snapshot after the dialog; the receiver keeps running.
    snapshot = receiver->snapshot(); latest = snapshot.latest; lastDemodResult = snapshot.lastDemodResult;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { log(file.errorString()); return; }
    bool written = false;
    {
        QTextStream out(&file); out.setRealNumberPrecision(12);
        out << "section,sequence,index,I_or_Re,Q_or_Im,magnitude,relative_dB,demod_uint32\n";
        for (int n = 0; n < latest.i.size(); ++n)
            out << "IQ," << latest.sequence << ',' << n << ',' << latest.i[n] << ',' << latest.q[n] << ",,,\n";
        for (int n = 0; n < latest.demodWords.size(); ++n)
            out << "DEMOD," << latest.sequence << ',' << n << ",,,,," << latest.demodWords[n] << '\n';
        for (int n = 0; n < latest.fftReal.size(); ++n)
            out << "FFT," << latest.sequence << ',' << n << ',' << latest.fftReal[n] << ',' << latest.fftImag[n]
                << ',' << latest.fftMagnitude[n] << ',' << latest.fftDb[n] << ",\n";
        // Raw IQ/DEMOD/FFT rows always describe latest; held display has its own sequence.
        const MonitorData &shown = holdDemod->isChecked() && !lastDemodResult.demodWords.isEmpty() ? lastDemodResult : latest;
        QVector<double> iBits, qBits;
        unpackDemodWords(shown.demodWords, pairOrderBox->currentIndex() == 1, &iBits, &qBits);
        for (int n = 0; n < iBits.size(); ++n)
            out << "DEMOD_SYMBOL," << shown.sequence << ',' << n << ',' << int(iBits[n]) << ',' << int(qBits[n]) << ",,,\n";
        // Keep the eight-column CSV layout; validity accompanies both sources.
        out << "DEMOD_STATUS," << latest.sequence << ",0," << int(latest.demodValidityKnown) << ',';
        if (latest.demodValidityKnown) out << int(latest.demodFrameValid);
        out << ",,,\n";
        out << "DEMOD_SYMBOL_STATUS," << shown.sequence << ",0," << int(shown.demodValidityKnown) << ',';
        if (shown.demodValidityKnown) out << int(shown.demodFrameValid);
        out << ",,,\n";
        out.flush(); written = out.status() == QTextStream::Ok;
    }
    if (!written) { file.cancelWriting(); log(QStringLiteral("CSV 写入失败。")); return; }
    if (!file.commit()) { log(QStringLiteral("CSV 保存失败：%1").arg(file.errorString())); return; }
    log(QStringLiteral("已导出序号 %1：%2").arg(latest.sequence).arg(path));
}

void MainWindow::printDemodData()
{
    if (displayedDemod.demodWords.isEmpty()) {
        log(QStringLiteral("暂无可打印的解调数据，请先接收数据或启动演示。")); return;
    }
    // Use the displayed frame and rendered ordering, including while plots are paused.
    const MonitorData data = displayedDemod;
    const bool highFirst = renderedPairOrder == 1;
    QString report; QTextStream out(&report);
    out << QStringLiteral("解调数据打印 · 固定快照\n打印时间：")
        << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz") << '\n';
    out << QStringLiteral("数据来源：") << (displayedDemodIsDemo ? QStringLiteral("演示合成数据\n") : QStringLiteral("网络接收数据\n"));
    out << QStringLiteral("来源序号：") << data.sequence << "  type=" << int(data.type)
        << "  flags=0x" << QString("%1").arg(data.flags, 4, 16, QLatin1Char('0')) << '\n';
    const QString validity = data.demodValidityKnown
                                 ? (data.demodFrameValid ? QStringLiteral("完整解调帧") : QStringLiteral("本包无完整解调帧，当前为填零或无效数据"))
                                 : QStringLiteral("有效性未知（type=4 / 无帧头模式）");
    out << QStringLiteral("解调状态：") << validity
        << (data.demodAllZero ? QStringLiteral("；数据全零\n") : QStringLiteral("；数据含非零字\n"));
    out << QStringLiteral("字数：") << data.demodWords.size() << QStringLiteral("  字节数：")
        << data.demodWords.size() * 4 << QStringLiteral("  I/Q 符号数：") << data.demodWords.size() * 16 << '\n';
    out << QStringLiteral("位序：") << (highFirst
                                            ? QStringLiteral("高位符号先，首对 I=bit30 / Q=bit31")
                                            : QStringLiteral("低位符号先，首对 I=bit0 / Q=bit1")) << '\n';
    out << QStringLiteral("LE 为网络载荷中的小端字节；BIN 按 bit31→bit0 显示；I/Q 序列按所选符号位序展开。\n"
                          "核对方法：原始字/字节与板端发送区对照，I/Q 序列与已知发送比特对照；完整帧标记不代表比特正确。\n\n"
                          "=== 全部解调原始字 ===\n");
    for (int n = 0; n < data.demodWords.size(); ++n) {
        const quint32 word = data.demodWords[n];
        QStringList bytes;
        for (int b = 0; b < 4; ++b) bytes.append(QString("%1").arg((word >> (8 * b)) & 0xffU, 2, 16, QLatin1Char('0')));
        out << QString("WORD[%1] BYTE+%2 HEX=0x%3 DEC=%4 LE=%5 BIN=%6\n")
                   .arg(n, 4, 10, QLatin1Char('0')).arg(n * 4, 4, 10, QLatin1Char('0'))
                   .arg(word, 8, 16, QLatin1Char('0')).arg(word).arg(bytes.join(' '))
                   .arg(word, 32, 2, QLatin1Char('0'));
    }
    QVector<double> iBits, qBits;
    unpackDemodWords(data.demodWords, highFirst, &iBits, &qBits);
    auto writeBits = [&out](const char *name, const QVector<double> &bits) {
        out << "\n=== " << name << QStringLiteral(" 完整比特序列（每行 64 位，索引从 0 开始） ===\n");
        for (int offset = 0; offset < bits.size(); offset += 64) {
            const int end = std::min(offset + 64, int(bits.size()));
            QString line; line.reserve(end - offset);
            for (int n = offset; n < end; ++n) line += bits[n] == 0 ? '0' : '1';
            out << QString("%1[%2..%3] = %4\n").arg(name)
                       .arg(offset, 4, 10, QLatin1Char('0')).arg(end - 1, 4, 10, QLatin1Char('0')).arg(line);
        }
    };
    writeBits("I", iBits); writeBits("Q", qBits); out.flush();
    demodPrintEdit->setPlainText(report); tabs->setCurrentWidget(demodPrintEdit);
    copyDemodPrintButton->setEnabled(true); saveDemodPrintButton->setEnabled(true);
    log(QStringLiteral("已打印解调来源序号 %1：%2 字 / %3 组 I/Q，完整内容见“解调数据打印”页。")
            .arg(data.sequence).arg(data.demodWords.size()).arg(iBits.size()));
}

void MainWindow::saveDemodPrint()
{
    if (demodPrintEdit->toPlainText().isEmpty()) return;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存解调打印快照"),
                                                      "demod_print.txt", QStringLiteral("文本文件 (*.txt)"));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { log(QStringLiteral("打印文件打开失败：%1").arg(file.errorString())); return; }
    const QByteArray text = demodPrintEdit->toPlainText().toUtf8();
    if (file.write(text) != text.size()) { file.cancelWriting(); log(QStringLiteral("打印文件写入失败。")); return; }
    if (!file.commit()) { log(QStringLiteral("打印文件保存失败：%1").arg(file.errorString())); return; }
    log(QStringLiteral("已保存解调打印快照：%1").arg(path));
}

void MainWindow::clearData()
{
    QMetaObject::invokeMethod(sender, [worker = sender] { worker->clearCounters(); }, Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(receiver, [worker = receiver] { worker->clearCounters(); }, Qt::BlockingQueuedConnection);
    seenRevision = receiver->snapshot().revision;
    latest = MonitorData(); lastDemodResult = MonitorData(); demodDirty = true; dirty = false;
    displayedDemod = MonitorData(); demodPrintEdit->clear();
    { const QSignalBlocker blocker(demodStartSpin); demodStartSpin->setValue(0); }
    updateDemodWaveform();
    displayedReference = SpreadReference();
    spreadRxAverage.reset(); spreadReferenceEstimate=WelchSpectrum();
    spreadReferenceCached=false; spreadSpectrumConfig.clear();
    displayedWaveform = MonitorData(); updateSpectrum();
    displayedDemodIsDemo = false;
    copyDemodPrintButton->setEnabled(false); saveDemodPrintButton->setEnabled(false);
    renderedDemodWords.clear(); renderedPairOrder = -1;
    nonzeroDemodSections = validDemodFrames = 0;
    rxBytes = txBytes = frames = rejected = previousRx = previousFrames = sequenceJumps = 0;
    previousMs = elapsed.elapsed();
    wavePlot->clear(); constellationPlot->clear(); fftPlot->clear(); demodPlot->clear();
    qWavePlot->clear(); qBitPlot->clear();
    { const QSignalBlocker blocker(waveStartSpin); waveStartSpin->setValue(0); }
    updateWaveformView();
    resumeWaveformButton->setEnabled(false);
    waveformStatus->setText(freezeWaveform->isChecked()
                                ? QStringLiteral("I/Q 波形实时刷新，等待首次有效解调后冻结。")
                                : QStringLiteral("I/Q 波形实时刷新（自动冻结已关闭）。"));
    bitPlot->clear(); symbolModel->setWords({}, false);
    demodStatus->setText(QStringLiteral("等待解调区；type=5 按 flags bit0 区分完整帧与填零，完整帧不代表比特正确。"));
    demodTable->setRowCount(0); logEdit->clear(); frameLabel->setText(QStringLiteral("等待新的有效 MONI 组合包"));
    if (sendHistoryEdit) sendHistoryEdit->clear();
    updateSendPreview();
    statsLabel->setText(QStringLiteral("RX 0 B   TX 0 B   有效帧 0"));
}

void MainWindow::log(const QString &message)
{ logEdit->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz ") + message); }
