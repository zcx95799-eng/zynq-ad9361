#ifndef PLOTWIDGET_H
#define PLOTWIDGET_H
#include <QWidget>
#include <QVector>
#include "spectrum.h"
#include "monitorprotocol.h"
class QPainter;
class PlotWidget : public QWidget {
public:
    enum Mode { Waveform, Constellation, Spectrum, Demod, Bits,
                IWaveform, QWaveform, IBits, QBits, IqSpectrum, SendBytes, DemodUnsigned, DemodSamples };
    explicit PlotWidget(Mode mode, QWidget *parent = nullptr);
    void setData(const QVector<double> &first, const QVector<double> &second = {});
    void setMode(Mode value);
    void setSampleWindow(int start, int count);
    void setSmoothDisplay(bool enabled);
    void setSpectrumSampleRate(double sampleRateMHz);
    void setSpectrumAmplitudeScale(bool dbfs);
    void setSpectrumAxis(double referenceDb, double dbPerDivision);
    void setSpectrumComparison(bool enabled, double referenceRateMHz = 12.24);
    void setSpreadSpectrumData(const WelchSpectrum &received, const WelchSpectrum &reference, bool markers);
    void setAcquisitionFftData(const QVector<AcquisitionFftFrame> &history,
                               const AcquisitionFftFrame &successful);
    int sampleCount() const { return int(first.size()); }
    int sampleWindowStart() const;
    int sampleWindowSize() const;
    void clear();
protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    bool acquisitionView = false;
    QVector<AcquisitionFftFrame> acquisitionHistory;
    AcquisitionFftFrame acquisitionSuccess;
    void paintAcquisitionFft(QPainter &painter);
    bool supportsWindow() const;
    QRectF spectrumArea() const;
    void paintIqSpectrum(QPainter &painter);
    void paintSpreadComparison(QPainter &painter);
    QRectF spreadOverviewArea() const;
    QRectF spreadZoomArea() const;
    WelchSpectrum spreadReceived, spreadReference;
    bool spreadTheoryMarkers = true;
    QPointF spreadCursor;
    int spreadHoverPanel = 0;
    bool spectrumComparison = false;
    double referenceSampleRateMHz = 12.24;
    Mode mode;
    QVector<double> first, second;
    int windowStart = 0, windowCount = 0, hoveredSample = -1;
    bool smoothDisplay = false;
    double spectrumSampleRateMHz = 20.0;
    bool spectrumDbfs = false;
    double spectrumReferenceDb = 0, spectrumDbPerDivision = 10;
};
#endif
