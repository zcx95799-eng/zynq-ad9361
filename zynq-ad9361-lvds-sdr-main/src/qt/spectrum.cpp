#include "spectrum.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

IqSpectrum calculateIqSpectrum(const QVector<double> &i, const QVector<double> &q,
                              double sampleRateMHz, SpectrumInput input, bool hannWindow, double fullScale)
{
    IqSpectrum result;
    const int samples = input == SpectrumInput::IChannel ? int(i.size())
        : input == SpectrumInput::QChannel ? int(q.size()) : int(std::min(i.size(), q.size()));
    if (samples < 2 || !std::isfinite(sampleRateMHz) || sampleRateMHz <= 0
        || !std::isfinite(fullScale) || fullScale <= 0) return result;
    int count = 1;
    while (count <= samples / 2) count *= 2;
    const double pi = std::acos(-1.0);
    std::vector<std::complex<double>> values(count);
    double windowSum = 0;
    for (int n = 0; n < count; ++n) {
        const double real = input == SpectrumInput::QChannel ? q[n] : i[n];
        const double imag = input == SpectrumInput::ComplexIq ? q[n] : 0;
        if (!std::isfinite(real) || !std::isfinite(imag)) return result;
        // N=2 uses a rectangular window: a symmetric two-point Hann is all zero.
        const double weight = hannWindow && count > 2 ? 0.5 - 0.5 * std::cos(2 * pi * n / (count - 1)) : 1;
        values[n] = std::complex<double>(real, imag) * weight;
        windowSum += weight;
    }
    // Iterative radix-2 FFT, forward transform exp(-j*2*pi*k*n/N).
    for (int n = 1, reversed = 0; n < count; ++n) {
        int bit = count >> 1;
        for (; reversed & bit; bit >>= 1) reversed ^= bit;
        reversed ^= bit;
        if (n < reversed) std::swap(values[n], values[reversed]);
    }
    for (int length = 2; length <= count; length *= 2) {
        const std::complex<double> rotation = std::polar(1.0, -2 * pi / length);
        for (int start = 0; start < count; start += length) {
            std::complex<double> phase(1, 0);
            for (int n = 0; n < length / 2; ++n) {
                const auto even = values[start + n];
                const auto odd = phase * values[start + n + length / 2];
                values[start + n] = even + odd; values[start + n + length / 2] = even - odd;
                phase *= rotation;
            }
        }
    }
    QVector<double> magnitudes(count);
    double peak = 0;
    int peakIndex = 0;
    result.inputSamples = samples; result.fftSize = count; result.binWidthMHz = sampleRateMHz / count;
    result.frequencyMHz.resize(count); result.relativeDb.resize(count); result.dbfs.resize(count);
    for (int n = 0; n < count; ++n) {
        result.frequencyMHz[n] = (n - count / 2) * result.binWidthMHz;
        magnitudes[n] = std::abs(values[(n + count / 2) % count]);
        if (magnitudes[n] > peak) { peak = magnitudes[n]; peakIndex = n; }
    }
    result.hasSignal = peak > 0;
    if (result.hasSignal) result.peakFrequencyMHz = result.frequencyMHz[peakIndex];
    // Two-sided coherent amplitude, referenced to the selected digital full scale.
    // Window gain and FFT length are removed; no per-frame peak normalization.
    const double reference = windowSum * fullScale;
    result.peakDbfs = 20 * std::log10(std::max(peak / reference, 1e-6));
    for (int n = 0; n < count; ++n) {
        result.relativeDb[n] = peak > 0 ? 20 * std::log10(std::max(magnitudes[n] / peak, 1e-6)) : -120;
        result.dbfs[n] = 20 * std::log10(std::max(magnitudes[n] / reference, 1e-6));
    }
    return result;
}


namespace {
void comparisonFft(std::vector<std::complex<double>> &values)
{
    const int count = int(values.size());
    for (int n = 1, reversed = 0; n < count; ++n) {
        int bit = count >> 1;
        for (; reversed & bit; bit >>= 1) reversed ^= bit;
        reversed ^= bit;
        if (n < reversed) std::swap(values[n], values[reversed]);
    }
    const double pi = std::acos(-1.0);
    for (int length = 2; length <= count; length *= 2) {
        const auto rotation = std::polar(1.0, -2 * pi / length);
        for (int start = 0; start < count; start += length) {
            std::complex<double> phase(1, 0);
            for (int n = 0; n < length / 2; ++n) {
                const auto even = values[start+n], odd = phase * values[start+n+length/2];
                values[start+n] = even+odd; values[start+n+length/2] = even-odd;
                phase *= rotation;
            }
        }
    }
}

void describePower(WelchSpectrum &spectrum)
{
    const int count = int(spectrum.power.size());
    spectrum.relativeDb.resize(count);
    double total = 0, peak = 0;
    for (double p : spectrum.power) { total += p; peak = std::max(peak, p); }
    spectrum.hasSignal = peak > 0 && std::isfinite(total);
    spectrum.occupiedLowMHz = spectrum.occupiedHighMHz = spectrum.occupiedBandwidthMHz = 0;
    for (int n=0; n<count; ++n)
        spectrum.relativeDb[n] = spectrum.hasSignal
            ? 10*std::log10(std::max(spectrum.power[n]/peak, 1e-12)) : -120;
    if (!spectrum.hasSignal) return;
    // Integrate power on the original FFT grid, before any display reduction.
    double cumulative = 0; int low = -1, high = count-1;
    for (int n=0; n<count; ++n) {
        cumulative += spectrum.power[n];
        if (low < 0 && cumulative >= total*0.005) low=n;
        if (cumulative >= total*0.995) { high=n; break; }
    }
    spectrum.occupiedLowMHz = (low-count/2)*spectrum.binWidthMHz;
    spectrum.occupiedHighMHz = (high-count/2)*spectrum.binWidthMHz;
    spectrum.occupiedBandwidthMHz = (high-low+1)*spectrum.binWidthMHz;
}
}

WelchSpectrum calculateWelchSpectrum(const QVector<double> &i, const QVector<double> &q,
                                    double sampleRateMHz, SpectrumInput input,
                                    bool hannWindow, int requestedSegment, bool removeDc)
{
    WelchSpectrum result;
    const int samples = input==SpectrumInput::IChannel ? int(i.size())
        : input==SpectrumInput::QChannel ? int(q.size()) : int(std::min(i.size(),q.size()));
    if (samples<8 || !std::isfinite(sampleRateMHz) || sampleRateMHz<=0) return result;
    for (int n=0; n<samples; ++n) {
        if ((input!=SpectrumInput::QChannel && !std::isfinite(i[n])) ||
            (input!=SpectrumInput::IChannel && !std::isfinite(q[n]))) return result;
    }
    int segment=1;
    const int limit=std::min(samples,std::clamp(requestedSegment,8,16384));
    while (segment<=limit/2) segment*=2;
    const int fftSize=segment*4, hop=segment/2;
    const double pi=std::acos(-1.0);
    QVector<double> window(segment);
    double energy=0, coherent=0;
    for (int n=0; n<segment; ++n) {
        window[n]=hannWindow ? 0.5-0.5*std::cos(2*pi*n/(segment-1)) : 1.0;
        energy+=window[n]*window[n]; coherent+=window[n];
    }
    result.power.fill(0,fftSize);
    for (int start=0; start+segment<=samples; start+=hop) {
        std::complex<double> mean(0,0);
        if (removeDc) {
            for (int n=0; n<segment; ++n)
                mean+=std::complex<double>(input==SpectrumInput::QChannel?q[start+n]:i[start+n],
                    input==SpectrumInput::ComplexIq?q[start+n]:0);
            mean/=segment;
        }
        std::vector<std::complex<double>> values(fftSize,0);
        for (int n=0; n<segment; ++n) {
            const std::complex<double> value(input==SpectrumInput::QChannel?q[start+n]:i[start+n],
                input==SpectrumInput::ComplexIq?q[start+n]:0);
            values[n]=(value-mean)*window[n];
        }
        comparisonFft(values);
        for (int n=0; n<fftSize; ++n)
            result.power[n]+=std::norm(values[(n+fftSize/2)%fftSize]);
        ++result.segmentCount;
    }
    // Two-sided PSD in input-units squared / Hz. Zero padding changes the
    // plotting grid only; resolution is reported using the window ENBW.
    const double scale=sampleRateMHz*1e6*energy*result.segmentCount;
    for (double &p : result.power) p/=scale;
    result.sampleRateMHz=sampleRateMHz; result.fftSize=fftSize;
    result.segmentSamples=segment; result.snapshotCount=1;
    result.binWidthMHz=sampleRateMHz/fftSize;
    result.resolutionBandwidthMHz=sampleRateMHz*energy/(coherent*coherent);
    describePower(result);
    return result;
}

void SpectrumPowerAverage::reset()
{
    history.clear(); sum.clear(); cached=WelchSpectrum(); haveId=false; depth=0;
}

WelchSpectrum SpectrumPowerAverage::accept(const WelchSpectrum &spectrum, quint64 sourceId, int maximumFrames)
{
    const int requested=std::clamp(maximumFrames,1,32);
    if (spectrum.power.isEmpty()) { reset(); return spectrum; }
    if (depth!=requested || sum.size()!=spectrum.power.size() ||
        cached.sampleRateMHz!=spectrum.sampleRateMHz ||
        cached.segmentSamples!=spectrum.segmentSamples || (haveId && sourceId<lastId)) reset();
    if (haveId && sourceId==lastId) return cached;
    depth=requested;
    if (sum.isEmpty()) sum.fill(0,spectrum.power.size());
    history.append(spectrum.power);
    for (int n=0; n<sum.size(); ++n) sum[n]+=spectrum.power[n];
    if (history.size()>depth) {
        const auto oldest=history.takeFirst();
        for (int n=0; n<sum.size(); ++n) sum[n]-=oldest[n];
    }
    cached=spectrum; cached.snapshotCount=int(history.size());
    for (int n=0; n<sum.size(); ++n) cached.power[n]=std::max(0.0,sum[n]/history.size());
    describePower(cached); lastId=sourceId; haveId=true;
    return cached;
}
