#include "plotwidget.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QToolTip>
#include <algorithm>
#include <cmath>

PlotWidget::PlotWidget(Mode mode, QWidget *parent) : QWidget(parent), mode(mode)
{
    setMinimumSize(280, 180); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
}
void PlotWidget::setData(const QVector<double> &a, const QVector<double> &b)
{ acquisitionView = false; first = a; second = b; update(); }
void PlotWidget::clear() { acquisitionHistory.clear(); acquisitionSuccess = AcquisitionFftFrame(); first.clear(); second.clear(); spreadReceived=WelchSpectrum(); spreadReference=WelchSpectrum(); spreadHoverPanel=0; update(); }
void PlotWidget::setMode(Mode value)
{
    mode = value; hoveredSample = -1; QToolTip::hideText(); update();
}

bool PlotWidget::supportsWindow() const
{
    return mode == Waveform || mode == IWaveform || mode == QWaveform || mode == DemodSamples
           || mode == Bits || mode == IBits || mode == QBits || mode == Demod || mode == DemodUnsigned;
}

void PlotWidget::setSpectrumSampleRate(double sampleRateMHz)
{
    if (std::isfinite(sampleRateMHz) && sampleRateMHz > 0) spectrumSampleRateMHz = sampleRateMHz;
    hoveredSample = -1; QToolTip::hideText(); update();
}

void PlotWidget::setSpectrumAmplitudeScale(bool dbfs)
{
    spectrumDbfs = dbfs; hoveredSample = -1; QToolTip::hideText(); update();
}

void PlotWidget::setSpectrumAxis(double referenceDb, double dbPerDivision)
{
    if (!std::isfinite(referenceDb) || !std::isfinite(dbPerDivision) || dbPerDivision <= 0) return;
    spectrumReferenceDb = referenceDb; spectrumDbPerDivision = dbPerDivision; update();
}

QRectF PlotWidget::spectrumArea() const
{
    return QRectF(106, 62, width() - 164, height() - 116);
}

void PlotWidget::paintIqSpectrum(QPainter &painter)
{
    if (spectrumComparison) { paintSpreadComparison(painter); return; }
    const QColor spectrumColor("#0057b8"), text("#000000");
    painter.fillRect(rect(), QColor("#ffffff"));
    const QRectF area = spectrumArea();
    if (area.width() <= 0 || area.height() <= 0) return;
    const double bottomDb = spectrumReferenceDb - 10 * spectrumDbPerDivision;
    auto frequency = [&](int index) { return (double(index) / first.size() - 0.5) * spectrumSampleRateMHz; };
    auto point = [&](int index, double value) {
        return QPointF(area.left() + double(index) / first.size() * area.width(),
                       area.top() + (spectrumReferenceDb - value) / (10 * spectrumDbPerDivision) * area.height());
    };
    QFont labelFont = font(); labelFont.setPixelSize(12); painter.setFont(labelFont);
    painter.setPen(text);
    painter.drawText(QRectF(14, 9, width() - 28, 18), Qt::AlignLeft,
                     QStringLiteral("SPECTRUM   /   I/Q 频谱"));
    int peak = -1;
    if (!first.isEmpty()) {
        peak = int(std::max_element(first.cbegin(), first.cend()) - first.cbegin());
        if (first[peak] <= -120) peak = -1;
    }
    painter.setPen(QColor("#000000"));
    QFont markerFont = labelFont; markerFont.setPixelSize(15); markerFont.setBold(true); painter.setFont(markerFont);
    painter.drawText(QRectF(area.left(), 32, area.width(), 23), Qt::AlignLeft | Qt::AlignVCenter,
                     peak >= 0 ? QStringLiteral("M1   %1 MHz    %2 dB%3")
                                     .arg(frequency(peak), 0, 'f', 6).arg(first[peak], 0, 'f', 2)
                                     .arg(first[peak] > spectrumReferenceDb || first[peak] < bottomDb ? QStringLiteral("   [超出纵轴]") : QString())
                               : QStringLiteral("M1   —    等待有效信号"));
    painter.setFont(labelFont);
    auto setting = [&](int row, const QString &name, const QString &value) {
        const double y = area.top() + row * 49;
        if (y + 36 > area.bottom()) return;
        painter.setPen(QColor("#000000")); painter.drawText(QRectF(12, y, 90, 18), name);
        painter.setPen(text); painter.drawText(QRectF(12, y + 18, 90, 18), value);
    };
    setting(0, "REF", QStringLiteral("%1 dB").arg(spectrumReferenceDb, 0, 'g', 4));
    setting(1, "SCALE", QStringLiteral("%1 dB/div").arg(spectrumDbPerDivision, 0, 'g', 4));
    setting(2, "CENTER", "0 MHz");
    setting(3, "SPAN", QStringLiteral("%1 MHz").arg(spectrumSampleRateMHz, 0, 'g', 6));
    if (area.height() > 250) setting(4, QStringLiteral("Δf"), first.isEmpty() ? QStringLiteral("—")
                                                         : QStringLiteral("%1 kHz").arg(spectrumSampleRateMHz * 1000 / first.size(), 0, 'f', 3));
    painter.fillRect(area, QColor("#ffffff"));
    for (int division = 0; division <= 10; ++division) {
        const double fraction = division / 10.0;
        const double x = area.left() + fraction * area.width(), y = area.top() + fraction * area.height();
        painter.setPen(QPen(QColor("#c8cdd3"), 0.8, Qt::DotLine));
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        painter.setPen(text);
        painter.drawText(QRectF(area.right() + 6, y - 9, 50, 18), Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(spectrumReferenceDb - division * spectrumDbPerDivision, 'g', 4));
        if (division % 2 == 0)
            painter.drawText(QRectF(x - 32, area.bottom() + 6, 64, 18), Qt::AlignCenter,
                             QString::number((fraction - 0.5) * spectrumSampleRateMHz, 'g', 5));
    }
    painter.setPen(QColor("#000000")); painter.drawRect(area);
    painter.drawText(QRectF(area.right() + 6, area.top() - 24, 48, 18), "dB");
    painter.drawText(QRectF(area.left(), height() - 23, area.width(), 18), Qt::AlignCenter,
                     QStringLiteral("基带频率 / MHz    ·    %1").arg(spectrumDbfs ? QStringLiteral("固定参考") : QStringLiteral("峰值归一化")));
    if (first.isEmpty()) {
        painter.drawText(area, Qt::AlignCenter, QStringLiteral("等待 IQ 数据 · 可启动演示")); return;
    }
    painter.save(); painter.setClipRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
    painter.setPen(QPen(spectrumColor, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath trace; trace.moveTo(point(0, first[0]));
    // Preserve the actual FFT bins and narrow peaks; styling never changes amplitudes.
    for (int index = 1; index < first.size(); ++index) trace.lineTo(point(index, first[index]));
    painter.drawPath(trace);
    if (hoveredSample >= 0 && hoveredSample < first.size()) {
        const QPointF hover = point(hoveredSample, first[hoveredSample]);
        painter.setPen(QPen(text, 1, Qt::DashLine));
        painter.drawLine(QPointF(hover.x(), area.top()), QPointF(hover.x(), area.bottom()));
        painter.drawEllipse(hover, 3, 3);
    }
    painter.restore();
    if (peak >= 0) {
        const QPointF actual = point(peak, first[peak]);
        const double y = std::clamp(actual.y(), area.top() + 2, area.bottom() - 2);
        const double x = std::clamp(actual.x(), area.left() + 10, area.right() - 10);
        const bool labelBelow = y < area.top() + 28;
        const double direction = labelBelow ? 1 : -1;
        const double labelY = labelBelow ? y + 8 : y - 25;
        painter.setPen(spectrumColor); painter.setBrush(spectrumColor);
        painter.drawPolygon(QPolygonF{QPointF(x, y), QPointF(x - 5, y + direction * 7), QPointF(x + 5, y + direction * 7)});
        painter.drawRoundedRect(QRectF(x - 9, labelY, 18, 17), 2, 2);
        painter.setPen(QColor("#ffffff"));
        painter.drawText(QRectF(x - 9, labelY, 18, 17), Qt::AlignCenter, "1");
    }
}

void PlotWidget::setSmoothDisplay(bool enabled)
{
    smoothDisplay = enabled; update();
}

void PlotWidget::setSampleWindow(int start, int count)
{
    start = std::max(0, start); count = std::max(0, count);
    if (windowStart == start && windowCount == count) return;
    windowStart = start; windowCount = count;
    hoveredSample = -1; QToolTip::hideText(); update();
}

int PlotWidget::sampleWindowSize() const
{
    return supportsWindow() && windowCount > 0 ? std::min(windowCount, sampleCount()) : sampleCount();
}

int PlotWidget::sampleWindowStart() const
{
    return windowCount > 0 && supportsWindow()
    ? std::clamp(windowStart, 0, sampleCount() - sampleWindowSize()) : 0;
}

void PlotWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (mode == IqSpectrum && spectrumComparison) {
        const QRectF overview=spreadOverviewArea(), zoom=spreadZoomArea();
        spreadCursor=event->position();
        spreadHoverPanel=overview.contains(spreadCursor)?1:zoom.contains(spreadCursor)?2:0;
        if (!spreadHoverPanel) { QToolTip::hideText(); update(); return; }
        const QRectF area=spreadHoverPanel==1?overview:zoom;
        const double half=spreadHoverPanel==1?std::max(spectrumSampleRateMHz,referenceSampleRateMHz)/2:0.32;
        const double f=(spreadCursor.x()-area.center().x())/area.width()*2*half;
        QString text=QStringLiteral("频率 %1 kHz").arg(f*1000,0,'f',2);
        auto append=[&](const WelchSpectrum &s,const QString &label) {
            if (!s.hasSignal || std::abs(f)>s.sampleRateMHz/2) return;
            const int n=std::clamp(int(std::lround((f/s.sampleRateMHz+0.5)*s.relativeDb.size())),0,int(s.relativeDb.size())-1);
            text+=QStringLiteral("\n%1：%2 dB").arg(label).arg(s.relativeDb[n],0,'f',2);
        };
        append(spreadReference,QStringLiteral("扩频前"));
        if (spreadHoverPanel==1) append(spreadReceived,QStringLiteral("接收"));
        QToolTip::showText(event->globalPosition().toPoint(),text,this); update(); return;
    }
    const QRectF area = mode == IqSpectrum ? spectrumArea() : QRectF(72, 42, width() - 94, height() - 86);
    const bool waveform = mode == Waveform || mode == IWaveform || mode == QWaveform || mode == DemodSamples;
    const bool spectrum = mode == IqSpectrum;
    if ((!supportsWindow() && !spectrum) || first.isEmpty() || area.width() <= 0 || !area.contains(event->position())) {
        hoveredSample = -1; QToolTip::hideText(); update(); return;
    }
    const int start = sampleWindowStart(), count = sampleWindowSize();
    hoveredSample = start + std::clamp(int(std::lround(
                                           (event->position().x() - area.left()) / area.width() * (spectrum ? count : count - 1))), 0, count - 1);
    const bool bits = mode == Bits || mode == IBits || mode == QBits;
    QString text = spectrum
                       ? QStringLiteral("频率 %1 MHz\n%2幅度 %3 %4")
                             .arg((double(hoveredSample) / count - 0.5) * spectrumSampleRateMHz, 0, 'f', 6)
                             .arg(spectrumDbfs ? QStringLiteral("数字") : QStringLiteral("相对"))
                             .arg(first[hoveredSample], 0, 'f', 2).arg("dB")
                       : QStringLiteral("采样索引 %1\n%2 = %3").arg(hoveredSample)
                             .arg(mode == QWaveform ? "Q" : "I").arg(first[hoveredSample], 0, 'g', 10);
    if (!waveform && !spectrum) text = bits
                   ? QStringLiteral("符号索引 %1\n%2 = %3").arg(hoveredSample).arg(mode == QBits ? "Q" : "I").arg(first[hoveredSample])
                   : QStringLiteral("原始字索引 %1\n数值 %2").arg(hoveredSample).arg(first[hoveredSample], 0, 'g', 12);
    if ((mode == Waveform || mode == Bits || mode == DemodSamples) && hoveredSample < second.size())
        text += QStringLiteral("\nQ = %1").arg(second[hoveredSample], 0, 'g', 10);
    QToolTip::showText(event->globalPosition().toPoint(), text, this); update();
}

void PlotWidget::leaveEvent(QEvent *event)
{
    spreadHoverPanel=0; hoveredSample = -1; QToolTip::hideText(); update(); QWidget::leaveEvent(event);
}

void PlotWidget::paintEvent(QPaintEvent *)
{
    if (acquisitionView) { QPainter painter(this); paintAcquisitionFft(painter); return; }
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    if (mode == IqSpectrum) { paintIqSpectrum(painter); return; }
    painter.fillRect(rect(), QColor("#ffffff"));
    const bool isWaveform = mode == Waveform || mode == IWaveform || mode == QWaveform || mode == DemodSamples;
    const bool isBits = mode == Bits || mode == IBits || mode == QBits;
    const bool qChannel = mode == QWaveform || mode == QBits;
    const bool isIqSpectrum = mode == IqSpectrum;
    const QString titles[] = {QStringLiteral("I / Q 时域波形"), QStringLiteral("原始 I/Q 星座图"),
                              QStringLiteral("板端 FFT · 相对幅度"), QStringLiteral("原始解调字 · int32 数值（非 IQ 采样）"),
                              QStringLiteral("解调 I/Q 比特 · 0/1 阶梯"),
                              QStringLiteral("I 通道 · 时域波形"), QStringLiteral("Q 通道 · 时域波形"),
                              QStringLiteral("解调 I 比特 · 前 256 组"), QStringLiteral("解调 Q 比特 · 前 256 组"),
                              QStringLiteral("I/Q 频谱 · 相对幅度"), QStringLiteral("发送字节值 · 前 256 B（0–255）"),
                              QStringLiteral("原始解调字 · uint32 数值（非 IQ 采样）"),
                              QStringLiteral("解调后载荷 · 采样波形")};
    painter.setPen(QColor("#000000"));
    painter.drawText(QRectF(16, 8, width() - 32, 25), isIqSpectrum && spectrumDbfs
                                                          ? QStringLiteral("I/Q 双边频谱 · 固定参考幅度") : titles[mode]);
    QRectF area(72, 42, width() - 94, height() - 86);
    if (mode == Constellation) {
        const double side = std::min(area.width(), area.height());
        area = QRectF(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
    }
    if (area.width() <= 0 || area.height() <= 0) return;
    const int startSample = sampleWindowStart(), visibleCount = sampleWindowSize();
    const int endSample = startSample + visibleCount;
    double minimum = 0, maximum = 1, xMin = startSample;
    double xMax = startSample + std::max(1, visibleCount - 1);
    if (mode == Spectrum || isIqSpectrum) { minimum = -120; maximum = isIqSpectrum && spectrumDbfs ? 6 : 0; }
    else if (!first.isEmpty()) {
        minimum = maximum = first[startSample];
        for (int n = startSample; n < endSample; ++n) {
            minimum = std::min(minimum, first[n]); maximum = std::max(maximum, first[n]);
        }
        for (int n = supportsWindow() ? startSample : 0;
             n < (supportsWindow() ? std::min(endSample, int(second.size())) : second.size()); ++n) {
            minimum = std::min(minimum, second[n]); maximum = std::max(maximum, second[n]);
        }
        if (mode == Constellation || isWaveform) {
            const double span = std::max(1.0, std::max(std::abs(minimum), std::abs(maximum)) * 1.1);
            minimum = -span; maximum = span;
        } else { minimum = std::min(0.0, minimum); maximum = std::max(minimum + 1.0, maximum * 1.05); }
    }
    if (mode == Constellation) { xMin = minimum; xMax = maximum; }
    if (isBits) { minimum = -0.2; maximum = mode == Bits ? 3.2 : 1.2; }
    if (mode == SendBytes) { minimum = 0; maximum = 255; }
    if (isIqSpectrum) { xMin = -spectrumSampleRateMHz / 2; xMax = spectrumSampleRateMHz / 2; }
    auto point = [&](double x, double y) {
        if (isIqSpectrum) x = (x / std::max(1, int(first.size())) - 0.5) * spectrumSampleRateMHz;
        return QPointF(area.left() + (x - xMin) / (xMax - xMin) * area.width(),
                       area.bottom() - (y - minimum) / (maximum - minimum) * area.height());
    };
    for (int tick = 0; tick <= 4; ++tick) {
        const double fraction = tick / 4.0;
        const double yValue = isIqSpectrum && spectrumDbfs ? -120 + tick * 30
                                                           : minimum + fraction * (maximum - minimum);
        const double y = area.bottom() - (yValue - minimum) / (maximum - minimum) * area.height();
        const double x = area.left() + fraction * area.width();
        painter.setPen(QColor("#c8cdd3"));
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        painter.setPen(QColor("#000000"));
        if (!isBits) painter.drawText(QRectF(2, y - 9, area.left() - 8, 18), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(yValue, 'g', 4));
        painter.drawText(QRectF(x - 34, area.bottom() + 5, 68, 18), Qt::AlignCenter,
                         isWaveform ? QString::number(qRound(xMin + fraction * (xMax - xMin)))
                                    : QString::number(xMin + fraction * (xMax - xMin), 'g', 4));
    }
    if (isBits) {
        const QString labels[] = {"I 0", "I 1", "Q 0", "Q 1"};
        for (int n = 0; n < (mode == Bits ? 4 : 2); ++n)
            painter.drawText(QRectF(2, point(0, n).y() - 9, area.left() - 8, 18),
                             Qt::AlignRight | Qt::AlignVCenter,
                             mode == Bits ? labels[n] : QString("%1 %2").arg(qChannel ? "Q" : "I").arg(n));
    }
    const QString xLabel = mode == Constellation ? "I" : isIqSpectrum ? QStringLiteral("基带频率 / MHz") : mode == Spectrum ? "FFT bin" : isBits ? QStringLiteral("解调符号索引") : isWaveform
                                                         ? (visibleCount > area.width() * 2 ? QStringLiteral("采样索引 · 峰值概览")
                                                            : smoothDisplay ? QStringLiteral("采样索引 · 平滑显示") : QStringLiteral("采样索引 · 原始折线"))
                                                         : mode == SendBytes ? QStringLiteral("字节偏移") : QStringLiteral("字索引");
    painter.drawText(QRectF(area.left(), height() - 21, area.width(), 18), Qt::AlignCenter, xLabel);
    if (mode == Spectrum || isIqSpectrum) painter.drawText(8, 32, "dB");
    if (mode == Constellation) painter.drawText(8, 32, "Q");
    if (first.isEmpty()) { painter.drawText(area, Qt::AlignCenter, mode == SendBytes
                                                    ? QStringLiteral("输入发送数据以查看字节图") : QStringLiteral("等待有效 MONI 数据 · 可点击演示")); return; }
    painter.save(); painter.setClipRect(area.adjusted(-1, -1, 1, 1));
    if (mode == Constellation) {
        painter.setPen(Qt::NoPen); painter.setBrush(QColor(106, 27, 154, 220));
        for (int n = 0; n < std::min(first.size(), second.size()); ++n) painter.drawEllipse(point(first[n], second[n]), 2.2, 2.2);
    } else {
        auto drawCurve = [&](const QVector<double> &values, QColor color) {
            const int begin = supportsWindow() ? startSample : 0;
            const int end = supportsWindow() ? std::min(endSample, int(values.size())) : int(values.size());
            const int count = end - begin;
            if (count <= 0) return;
            painter.setPen(QPen(color, isWaveform ? 1.6 : 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (count == 1) { painter.drawEllipse(point(begin, values[begin]), 2, 2); return; }
            // Per-pixel min/max preserves peaks when many samples share a pixel.
            if (count > area.width() * 2) {
                const int columns = std::max(1, int(area.width()));
                for (int column = 0; column < columns; ++column) {
                    const int start = begin + column * count / columns;
                    const int end = begin + (column + 1) * count / columns;
                    double low = values[start], high = low;
                    for (int n = start + 1; n < end; ++n) { low = std::min(low, values[n]); high = std::max(high, values[n]); }
                    painter.drawLine(point(start, low), point(start, high));
                }
            } else {
                QPainterPath path; path.moveTo(point(begin, values[begin]));
                // Shape-preserving cubic Hermite interpolation passes through every
                // sample, keeps flat plateaus, and never adds an overshoot at peaks.
                // Slopes use the full data so moving a window does not change the curve.
                auto slope = [&](int index) {
                    if (index == 0) return values[1] - values[0];
                    if (index == values.size() - 1) return values[index] - values[index - 1];
                    const double left = values[index] - values[index - 1];
                    const double right = values[index + 1] - values[index];
                    if (left == 0 || right == 0 || (left > 0) != (right > 0)) return 0.0;
                    return 2 * left * right / (left + right);
                };
                for (int n = begin + 1; n < end; ++n) {
                    if (isWaveform && smoothDisplay) {
                        path.cubicTo(point(n - 1 + 1.0 / 3, values[n - 1] + slope(n - 1) / 3),
                                     point(n - 1.0 / 3, values[n] - slope(n) / 3),
                                     point(n, values[n]));
                    } else {
                        if (isBits) path.lineTo(point(n, values[n - 1]));
                        path.lineTo(point(n, values[n]));
                    }
                }
                painter.drawPath(path);
                if (isWaveform && !smoothDisplay && area.width() / (count - 1) >= 4) {
                    painter.setBrush(color);
                    for (int n = begin; n < end; ++n) painter.drawEllipse(point(n, values[n]), 1.7, 1.7);
                    painter.setBrush(Qt::NoBrush);
                }
            }
        };
        drawCurve(first, QColor(qChannel ? "#b3261e" : "#0057b8"));
        if (mode == Bits) {
            QVector<double> qLane = second;
            for (double &value : qLane) value += 2;
            drawCurve(qLane, QColor("#b3261e"));
        } else drawCurve(second, QColor("#b3261e"));
    }
    if ((supportsWindow() || isIqSpectrum) && hoveredSample >= startSample && hoveredSample < endSample) {
        const QPointF sample = point(hoveredSample, first[hoveredSample]);
        painter.setPen(QPen(QColor("#666666"), 1, Qt::DashLine));
        painter.drawLine(QPointF(sample.x(), area.top()), QPointF(sample.x(), area.bottom()));
        painter.setPen(QPen(QColor("#000000"), 1.5));
        painter.drawEllipse(sample, 3.5, 3.5);
    }
    painter.restore();
    if (isWaveform) {
        if (mode != QWaveform) {
            painter.setPen(QColor("#000000")); painter.drawText(width() - 100, 26, "I");
        }
        if (mode != IWaveform && (mode != DemodSamples || !second.isEmpty())) {
            painter.setPen(QColor("#000000")); painter.drawText(width() - 60, 26, "Q");
        }
    }
}

void PlotWidget::setSpectrumComparison(bool enabled, double rate)
{
    if (spectrumComparison != enabled) { spreadHoverPanel=0; QToolTip::hideText(); }
    spectrumComparison = enabled;
    if (std::isfinite(rate) && rate > 0) referenceSampleRateMHz = rate;
    hoveredSample = -1; update();
}

void PlotWidget::setSpreadSpectrumData(const WelchSpectrum &received, const WelchSpectrum &reference, bool markers)
{
    spreadReceived = received; spreadReference = reference; spreadTheoryMarkers = markers;
    spectrumSampleRateMHz = received.sampleRateMHz > 0 ? received.sampleRateMHz : 12.24;
    referenceSampleRateMHz = reference.sampleRateMHz > 0 ? reference.sampleRateMHz : 12.24;
    first = received.relativeDb; second = reference.relativeDb;
    update();
}

QRectF PlotWidget::spreadOverviewArea() const
{
    return QRectF(68, 76, width()-94, std::max(60.0, (height()-166)*0.60));
}

QRectF PlotWidget::spreadZoomArea() const
{
    const QRectF overview=spreadOverviewArea();
    return QRectF(overview.left(), overview.bottom()+58, overview.width(),
                  std::max(45.0, height()-overview.bottom()-88));
}

void PlotWidget::paintSpreadComparison(QPainter &painter)
{
    painter.fillRect(rect(), Qt::white);
    const QColor blue("#0057b8"), red("#b3261e"), ink("#17202a"), grid("#d6dce4");
    QFont labels=font(); labels.setPixelSize(12); painter.setFont(labels);
    const QRectF overview=spreadOverviewArea(), zoom=spreadZoomArea();
    if (overview.width()<80 || height()<240) return;
    const double span=std::max(spectrumSampleRateMHz,referenceSampleRateMHz);
    const double symbolRate=0.08, chipRate=4.08, zoomHalf=4*symbolRate;
    const double bottomDb=-70, topDb=3;
    auto write=[&](QRectF box, const QString &text, const QColor &color, int alignment=Qt::AlignLeft|Qt::AlignVCenter) {
        painter.setPen(color); painter.drawText(box,alignment,
            painter.fontMetrics().elidedText(text,Qt::ElideRight,int(box.width())));
    };
    write(QRectF(12,4,width()/2.0-14,22),QStringLiteral("红  扩频前实采 · PN 异或前"),red);
    write(QRectF(width()/2.0,4,width()/2.0-12,22),QStringLiteral("蓝  扩频后接收 IQ"),blue);
    write(QRectF(12,27,width()-24,20),
          QStringLiteral("Welch 功率谱 · 各自峰值 0 dB · 蓝线平均 %1 组 · 虚线为矩形脉冲理论零点")
          .arg(spreadReceived.snapshotCount),ink);
    write(QRectF(overview.left(),52,overview.width(),20),QStringLiteral("全带宽对比 / MHz   ·   扩频前后共用横轴、纵轴"),ink);

    auto yOf=[&](const QRectF &area,double db) {
        return area.top()+(topDb-std::clamp(db,bottomDb,topDb))/(topDb-bottomDb)*area.height();
    };
    auto xOf=[](const QRectF &area,double f,double half) { return area.center().x()+f/(2*half)*area.width(); };
    auto axes=[&](const QRectF &area,double half,bool khz) {
        painter.fillRect(area,QColor("#fcfdff"));
        for (int db=0; db>=-70; db-=10) {
            const double y=yOf(area,db);
            painter.setPen(QPen(grid,0.8,Qt::DotLine)); painter.drawLine(QPointF(area.left(),y),QPointF(area.right(),y));
            write(QRectF(6,y-8,52,16),QString::number(db),ink,Qt::AlignRight|Qt::AlignVCenter);
        }
        const int ticks=khz?8:6;
        for (int n=0; n<=ticks; ++n) {
            const double f=(2.0*n/ticks-1)*half, x=xOf(area,f,half);
            painter.setPen(QPen(grid,0.8,Qt::DotLine)); painter.drawLine(QPointF(x,area.top()),QPointF(x,area.bottom()));
            write(QRectF(x-34,area.bottom()+3,68,18),QString::number(khz?f*1000:f,'g',4),ink,Qt::AlignCenter);
        }
        painter.setPen(QPen(QColor("#697586"),1.1)); painter.drawRect(area);
        write(QRectF(7,area.top()-19,48,18),QStringLiteral("dB"),ink,Qt::AlignRight|Qt::AlignVCenter);
    };
    axes(overview,span/2,false); axes(zoom,zoomHalf,true);

    // Render the peak in each pixel column. Dense FFT bins no longer create
    // vertical combs; the underlying PSD and B99 calculation are untouched.
    auto trace=[&](const QRectF &area,const QVector<double> &values,double rate,double half,const QColor &color) {
        if (values.isEmpty()) return;
        const int columns=std::max(2,int(std::ceil(area.width()))+1);
        QVector<double> maxima(columns,-1e10);
        for (int n=0; n<values.size(); ++n) {
            const double f=(double(n)/values.size()-0.5)*rate;
            if (f < -half || f > half) continue;
            const int col=std::clamp(int((f/(2*half)+0.5)*(columns-1)),0,columns-1);
            maxima[col]=std::max(maxima[col],values[n]);
        }
        QPainterPath path; bool started=false;
        for (int col=0; col<columns; ++col) {
            if (maxima[col]<-1e9) continue;
            const QPointF point(area.left()+double(col)/(columns-1)*area.width(),yOf(area,maxima[col]));
            if (!started) { path.moveTo(point); started=true; } else path.lineTo(point);
        }
        painter.save(); painter.setClipRect(area.adjusted(1,1,-1,-1));
        painter.setPen(QPen(color,1.8)); painter.drawPath(path); painter.restore();
    };
    auto marker=[&](const QRectF &area,double rate,double half,const QColor &color) {
        if (rate>=half) return;
        painter.save(); painter.setClipRect(area);
        painter.setPen(QPen(color,1.0,Qt::DashLine));
        for (double sign : {-1.0,1.0}) {
            const double x=xOf(area,sign*rate,half);
            painter.drawLine(QPointF(x,area.top()),QPointF(x,area.bottom()));
        }
        painter.restore();
    };
    if (spreadTheoryMarkers) {
        marker(overview,chipRate,span/2,QColor("#5688bf"));
        marker(overview,symbolRate,span/2,QColor("#c47069"));
        marker(zoom,symbolRate,zoomHalf,QColor("#c47069"));
        write(QRectF(overview.left()+8,overview.bottom()-24,overview.width()-16,20),
              QStringLiteral("矩形脉冲理论主瓣宽：160 kHz → 8.16 MHz（51 倍）；虚线为首零点"),QColor("#536174"));
        for (double sign : {-1.0,1.0}) {
            const double x=xOf(zoom,sign*symbolRate,zoomHalf);
            write(QRectF(x-48,zoom.top()+3,96,18),sign<0?QStringLiteral("−80 kHz"):QStringLiteral("+80 kHz"),red,Qt::AlignCenter);
        }
    }
    if (spreadReceived.hasSignal) trace(overview,first,spectrumSampleRateMHz,span/2,blue);
    if (spreadReference.hasSignal) {
        trace(overview,second,referenceSampleRateMHz,span/2,red);
        trace(zoom,second,referenceSampleRateMHz,zoomHalf,red);
    }
    write(QRectF(zoom.left(),zoom.top()-25,zoom.width(),20),
          QStringLiteral("扩频前局部放大 / kHz   ·   同一红线，仅横轴放大"),red);
    if (!spreadReference.hasSignal)
        write(zoom.adjusted(10,2,-10,-2),second.isEmpty()?QStringLiteral("等待完整扩频前采样；请发送正常业务数据"):
              QStringLiteral("扩频前采样为零，无可测频谱"),ink,Qt::AlignCenter);
    if (!spreadReceived.hasSignal)
        write(overview.adjusted(10,20,-10,-20),first.isEmpty()?QStringLiteral("等待接收 IQ"):
              QStringLiteral("接收采样为零，无可测频谱"),ink,Qt::AlignCenter);
    if (spreadHoverPanel) {
        const auto area=spreadHoverPanel==1?overview:zoom;
        if (area.contains(spreadCursor)) {
            painter.setPen(QPen(QColor("#586577"),0.8,Qt::DashLine));
            painter.drawLine(QPointF(spreadCursor.x(),area.top()),QPointF(spreadCursor.x(),area.bottom()));
        }
    }
}

void PlotWidget::setAcquisitionFftData(const QVector<AcquisitionFftFrame> &history,
                                      const AcquisitionFftFrame &successful)
{
    Q_UNUSED(history);
    acquisitionView = true;
    acquisitionHistory.clear(); // No ordinary/history curves are displayed.
    acquisitionSuccess = successful;
    update();
}

void PlotWidget::paintAcquisitionFft(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Qt::white);
    QFont f=font(); f.setPixelSize(12); p.setFont(f); p.setPen(Qt::black);
    p.drawText(QRectF(16,8,width()-32,22),Qt::AlignLeft|Qt::AlignVCenter,
               QStringLiteral("最近一次成功捕获 · 原始功率（线性显示，保持至下次成功）"));
    const auto &frame=acquisitionSuccess;
    const bool valid=frame.successful() && frame.power.size()==frame.windowFrames*128;
    const QRectF area(78,87,width()-107,height()-134);
    if(area.width()<50 || area.height()<35) return;
    int peak=-1;
    double maximum=1;
    if(valid) {
        peak=int(std::max_element(frame.power.cbegin(),frame.power.cend())-frame.power.cbegin());
        maximum=std::max(1.0,frame.power[peak]);
    }
    // Only one shared linear scale: never reshape, logarithmically compress,
    // shift, normalize individual FFT frames, or insert zero-valued samples.
    const int exponent=valid && maximum>=10000 ? int(std::floor(std::log10(maximum))) : 0;
    const double multiplier=std::pow(10.0,exponent), top=maximum*1.12;
    const int count=valid?int(frame.power.size()):1024;
    auto point=[&](int sample,double value) {
        return QPointF(area.left()+double(sample)/std::max(1,count-1)*area.width(),
                       area.bottom()-value/top*area.height());
    };
    if(valid) {
        p.drawText(QRectF(16,33,width()-32,20),Qt::AlignLeft|Qt::AlignVCenter,
                   QStringLiteral("%1 帧连续数据 / %2 点 · 峰值点 %3 · P=%4 · 捕获帧 %5")
                       .arg(frame.windowFrames).arg(count).arg(peak)
                       .arg(frame.power[peak],0,'g',7).arg(frame.captureId));
    } else {
        p.drawText(QRectF(16,33,width()-32,20),Qt::AlignLeft|Qt::AlignVCenter,
                   QStringLiteral("等待最近一次成功捕获的完整数据；普通数据不会替换本图。"));
    }
    p.drawText(QRectF(8,61,width()-16,20),Qt::AlignLeft|Qt::AlignVCenter,
               exponent ? QStringLiteral("原始功率 P / ×10^%1").arg(exponent) : QStringLiteral("原始功率 P"));
    for(int tick=0;tick<=5;++tick) {
        const double value=top*tick/5.0,y=area.bottom()-area.height()*tick/5.0;
        p.setPen(QColor("#d5d9de"));
        p.drawLine(QPointF(area.left(),y),QPointF(area.right(),y));
        p.setPen(Qt::black);
        p.drawText(QRectF(0,y-9,area.left()-9,18),Qt::AlignRight|Qt::AlignVCenter,
                   QString::number(value/multiplier,'g',3));
    }
    for(int tick=0;tick<=8;++tick) {
        const int sample=tick*(count-1)/8;
        const double x=point(sample,0).x();
        p.setPen(QPen(QColor("#d5d9de"),0.8,Qt::DotLine));
        p.drawLine(QPointF(x,area.top()),QPointF(x,area.bottom()));
        p.setPen(Qt::black);
        p.drawText(QRectF(x-28,area.bottom()+5,56,18),Qt::AlignCenter,QString::number(sample));
    }
    p.setPen(Qt::black);p.drawRect(area);
    p.drawText(QRectF(area.left(),height()-24,area.width(),18),Qt::AlignCenter,
               QStringLiteral("连续 FFT 有效输出点序号（按采集顺序）"));
    if(!valid) {p.drawText(area,Qt::AlignCenter,QStringLiteral("等待成功捕获"));return;}
    p.save();p.setClipRect(area.adjusted(0.5,-1,-0.5,1));
    const QColor green("#008238");
    p.setPen(QPen(green,1.4,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    QPainterPath path;path.moveTo(point(0,frame.power[0]));
    for(int n=1;n<count;++n)path.lineTo(point(n,frame.power[n]));
    p.drawPath(path);
    const QPointF pos=point(peak,frame.power[peak]);
    p.setPen(green);p.setBrush(green);
    p.drawPolygon(QPolygonF{QPointF(pos.x(),pos.y()-2),QPointF(pos.x()-4,pos.y()-9),QPointF(pos.x()+4,pos.y()-9)});
    p.restore();
    if(!frame.continuousWindow) {
        p.setPen(QColor("#805000"));
        p.drawText(QRectF(area.left()+8,area.top()+6,area.width()-16,20),Qt::AlignLeft,
                   QStringLiteral("当前为旧版单帧数据；更新板端监视模块后可显示连续捕获窗口。"));
    }
}
