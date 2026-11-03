#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <QVector>

enum class SpectrumInput { ComplexIq, IChannel, QChannel };

struct IqSpectrum {
    QVector<double> frequencyMHz, relativeDb, dbfs;
    int inputSamples = 0, fftSize = 0;
    double peakFrequencyMHz = 0, binWidthMHz = 0;
    double peakDbfs = -120;
    bool hasSignal = false;
};

// Analyze one contiguous block: use the leading power-of-two samples without
// padding or joining frames. Relative amplitude is normalized to this FFT peak.
IqSpectrum calculateIqSpectrum(const QVector<double> &i, const QVector<double> &q,
                              double sampleRateMHz, SpectrumInput input, bool hannWindow,
                              double fullScale = 32768);

// Comparison-only power spectral density. The legacy FFT above is unchanged.
struct WelchSpectrum {
    QVector<double> power, relativeDb;
    double sampleRateMHz = 0, binWidthMHz = 0, resolutionBandwidthMHz = 0;
    double occupiedLowMHz = 0, occupiedHighMHz = 0, occupiedBandwidthMHz = 0;
    int segmentSamples = 0, fftSize = 0, segmentCount = 0, snapshotCount = 0;
    bool hasSignal = false;
};

WelchSpectrum calculateWelchSpectrum(const QVector<double> &i, const QVector<double> &q,
                                    double sampleRateMHz, SpectrumInput input,
                                    bool hannWindow, int requestedSegment, bool removeDc = false);

// Average LINEAR power from distinct snapshots, never dB or repeated repaints.
class SpectrumPowerAverage {
public:
    WelchSpectrum accept(const WelchSpectrum &spectrum, quint64 sourceId, int maximumFrames);
    void reset();
private:
    QVector<QVector<double>> history;
    QVector<double> sum;
    WelchSpectrum cached;
    quint64 lastId = 0;
    int depth = 0;
    bool haveId = false;
};

#endif
