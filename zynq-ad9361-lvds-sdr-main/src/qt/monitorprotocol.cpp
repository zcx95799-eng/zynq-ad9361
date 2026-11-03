#include "monitorprotocol.h"
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace {
quint32 readLe32(const char *p) { return qFromLittleEndian<quint32>(p); }
int signed12(quint32 value) {
    const int low = int(value & 0xfffU);
    return low >= 2048 ? low - 4096 : low;
}
qint64 signed32(quint32 value) {
    return value & 0x80000000U ? qint64(value) - 0x100000000LL : qint64(value);
}
int signed16(quint32 value) {
    const int low = int(value & 0xffffU);
    return low >= 32768 ? low - 65536 : low;
}
void appendLe32(QByteArray &bytes, quint32 value) {
    char word[4]; qToLittleEndian(value, word); bytes.append(word, 4);
}
}

QVector<MonitorFrame> MonitorParser::feed(const QByteArray &bytes)
{
    QVector<MonitorFrame> frames;
    for (qsizetype offset = 0; offset < bytes.size(); offset += 65536) {
        buffer.append(bytes.constData() + offset,
                      std::min<qsizetype>(65536, bytes.size() - offset));
        while (!buffer.isEmpty()) {
            const int magic = buffer.indexOf("MONI");
            if (magic < 0) {
                const int remove = std::max(0, int(buffer.size()) - 3);
                discarded += quint64(remove); buffer.remove(0, remove); break;
            }
            if (magic > 0) { discarded += quint64(magic); buffer.remove(0, magic); }
            if (buffer.size() < HeaderBytes) break;
            const quint32 length = qFromBigEndian<quint32>(buffer.constData() + 12);
            if (quint8(buffer[4]) != 1 || length > MaxPayloadBytes || length == 0) {
                ++invalid; ++discarded; buffer.remove(0, 1); continue;
            }
            const int total = HeaderBytes + int(length);
            if (buffer.size() < total) break;
            MonitorFrame frame;
            frame.type = quint8(buffer[5]);
            frame.flags = qFromBigEndian<quint16>(buffer.constData() + 6);
            frame.sequence = qFromBigEndian<quint32>(buffer.constData() + 8);
            frame.payload = buffer.mid(HeaderBytes, int(length));
            frames.append(frame); buffer.remove(0, total);
        }
    }
    return frames;
}

void MonitorParser::reset() { buffer.clear(); discarded = invalid = 0; }

void unpackDemodWords(const QVector<quint32> &words, bool highPairFirst,
                      QVector<double> *iBits, QVector<double> *qBits)
{
    iBits->clear(); qBits->clear();
    iBits->reserve(words.size() * 16); qBits->reserve(words.size() * 16);
    for (quint32 word : words) {
        for (int pair = 0; pair < 16; ++pair) {
            const int shift = 2 * (highPairFirst ? 15 - pair : pair);
            iBits->append((word >> shift) & 1U);
            qBits->append((word >> (shift + 1)) & 1U);
        }
    }
}

bool decodeMonitorFrame(const MonitorFrame &frame, const MonitorLayout &layout,
                        MonitorData *data, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!data || layout.iqSamples < 1 || layout.iqSamples > 16384
        || layout.demodBytes < 4 || layout.demodBytes > 65536
        || layout.demodBytes % 4 != 0 || layout.fftPoints < 0 || layout.fftPoints > 4096)
        return fail(QStringLiteral("解析配置无效：解调区字节数必须为 4 的倍数。"));
    if (frame.type != 4 && frame.type != 5)
        return fail(QStringLiteral("暂不解析帧类型 %1，仅支持组合包 type=4 / type=5。").arg(frame.type));
    if (frame.type == 5 && layout.fftPoints != 0)
        return fail(QStringLiteral("type=5 按降采样 IQ + 解调解析，不含 FFT；请选择新顶层布局，FFT 点数设为 0。"));
    if (frame.payload.size() != layout.payloadBytes())
        return fail(QStringLiteral("载荷长度 %1，配置要求 %2；请核对 PL 打包模块及解析设置。")
                    .arg(frame.payload.size()).arg(layout.payloadBytes()));
    MonitorData result;
    result.type = frame.type; result.sequence = frame.sequence; result.flags = frame.flags;
    result.demodValidityKnown = frame.type == 5 && frame.hasWireHeader;
    result.demodFrameValid = result.demodValidityKnown && (frame.flags & 1U);
    result.i.reserve(layout.iqSamples); result.q.reserve(layout.iqSamples);
    const char *p = frame.payload.constData();
    for (int n = 0; n < layout.iqSamples; ++n, p += 4) {
        const quint32 word = readLe32(p);
        switch (layout.iqEncoding) {
        case IqEncoding::I12Q12In16:
            result.i.append(signed12(word)); result.q.append(signed12(word >> 16)); break;
        case IqEncoding::Q12I12In16:
            result.i.append(signed12(word >> 16)); result.q.append(signed12(word)); break;
        case IqEncoding::PackedI12Q12:
            result.i.append(signed12(word)); result.q.append(signed12(word >> 12)); break;
        case IqEncoding::SignedI16Q16:
            result.i.append(signed16(word)); result.q.append(signed16(word >> 16)); break;
        }
    }
    if (frame.type == 5 && frame.hasWireHeader && (frame.flags & 0x0200U)) {
        if ((frame.flags & 0x0101U) || layout.demodBytes != 1024 ||
            layout.iqEncoding != IqEncoding::SignedI16Q16)
            return fail(QStringLiteral("捕获 FFT 包要求 I16/Q16、1024 B 附加区，不能同时标记解调或参考。"));
        const quint32 magic = readLe32(p);
        const bool window = magic == 0x31574646U; // FFW1, continuous acquisition output
        if (!window && magic != 0x31504646U)
            return fail(QStringLiteral("捕获功率 FFW1/FFP1 标识错误。"));
        auto &chunk = result.acquisitionFft;
        auto &fft = chunk.frame;
        fft.captureId = readLe32(p + 4); fft.searchId = readLe32(p + 8);
        const quint32 descriptor = readLe32(p + 12);
        chunk.offset = int(descriptor & 0xffffU); chunk.total = int(descriptor >> 16);
        fft.flags = readLe32(p + 16); fft.peakBin = int(readLe32(p + 20));
        fft.overwritten = readLe32(p + 24);
        fft.continuousWindow = window;
        const quint32 geometry = readLe32(p + 28);
        fft.windowFrames = window ? int(geometry >> 16) : 1;
        if ((window && (fft.windowFrames < 1 || fft.windowFrames > 8 ||
                        (geometry & 0xffffU) != 128 || fft.flags != 1U)) ||
            (!window && (geometry != 0 || (fft.flags & ~1U))) ||
            chunk.total != 128 * fft.windowFrames || chunk.offset >= chunk.total ||
            chunk.offset % 124 != 0 || fft.peakBin < 0 || fft.peakBin >= chunk.total ||
            (window && fft.peakBin < chunk.total - 128))
            return fail(QStringLiteral("连续捕获功率的帧数、分片地址或峰值位置错误。"));
        const int count = std::min(124, chunk.total - chunk.offset);
        fft.power.reserve(count);
        for (int n = 0; n < count; ++n)
            fft.power.append(double(qFromLittleEndian<quint64>(p + 32 + n * 8)));
        chunk.present = true;
        result.demodWords.fill(0, layout.demodBytes / 4);
        p += layout.demodBytes;
    }
 else if (frame.type == 5 && frame.hasWireHeader && (frame.flags & 0x0100U)) {
        if (result.demodFrameValid || layout.demodBytes != 1024 ||
            layout.iqEncoding != IqEncoding::SignedI16Q16)
            return fail(QStringLiteral("扩频参考包要求 I16/Q16、1024 B 附加区，且不能同时标记解调帧。"));
        if (readLe32(p) != 0x31525053U)
            return fail(QStringLiteral("扩频参考 SPR1 标识错误。"));
        auto &ref = result.spreadReference;
        ref.captureId = readLe32(p + 4); ref.sampleRateHz = readLe32(p + 8);
        const quint32 descriptor = readLe32(p + 12);
        ref.offset = int(descriptor & 0xffffU); ref.total = int(descriptor >> 16);
        if (ref.total != 8192 || ref.offset >= ref.total || ref.offset % 252 != 0 ||
            ref.sampleRateHz < 1000 || ref.sampleRateHz > 1000000000U)
            return fail(QStringLiteral("扩频参考采样参数或分片地址错误。"));
        const int count = std::min(252, ref.total - ref.offset);
        for (int n = 0; n < count; ++n) {
            const quint32 word = readLe32(p + 16 + n * 4);
            ref.i.append(signed16(word)); ref.q.append(signed16(word >> 16));
        }
        ref.present = true;
        // Keep the original demod views empty/zero; auxiliary words are not bits.
        result.demodWords.fill(0, layout.demodBytes / 4);
        p += layout.demodBytes;
    } else for (int n = 0; n < layout.demodBytes / 4; ++n, p += 4) {
        const quint32 word = readLe32(p);
        result.demodWords.append(word);
        if (word) result.demodAllZero = false;
    }
    double peak = 0;
    for (int n = 0; n < layout.fftPoints; ++n, p += 8) {
        const double real = double(signed32(readLe32(p)));
        const double imag = double(signed32(readLe32(p + 4)));
        const double magnitude = std::hypot(real, imag);
        result.fftReal.append(real); result.fftImag.append(imag); result.fftMagnitude.append(magnitude);
        peak = std::max(peak, magnitude);
    }
    for (double magnitude : result.fftMagnitude)
        result.fftDb.append(peak > 0 ? 20 * std::log10(std::max(magnitude / peak, 1e-6)) : -120);
    *data = std::move(result);
    if (error) error->clear();
    return true;
}

QByteArray makeDemoFrame(quint32 sequence, const MonitorLayout &layout)
{
    QByteArray payload;
    const double pi = std::acos(-1.0);
    for (int n = 0; n < layout.iqSamples; ++n) {
        const double angle = 2 * pi * n / 32.0 + sequence * 0.12;
        const int i = int(1400 * std::cos(angle)), q = int(1400 * std::sin(angle));
        const quint32 lo = quint32(i) & 0xffffU, hi = quint32(q) & 0xffffU;
        quint32 word = lo | (hi << 16);
        if (layout.iqEncoding == IqEncoding::Q12I12In16) word = hi | (lo << 16);
        if (layout.iqEncoding == IqEncoding::PackedI12Q12)
            word = (quint32(i) & 0xfffU) | ((quint32(q) & 0xfffU) << 12);
        appendLe32(payload, word);
    }
    // In the new layout, demonstrate sparse results between zero-filled sections.
    for (int n = 0; n < layout.demodBytes / 4; ++n) {
        quint32 word = 0;
        if (layout.fftPoints > 0 || sequence % 10 == 0) {
            for (int pair = 0; pair < 16; ++pair)
                word |= quint32((n + pair + sequence) & 3) << (pair * 2);
        }
        appendLe32(payload, word);
    }
    for (int n = 0; n < layout.fftPoints; ++n) {
        const double distance = (n - layout.fftPoints / 4.0) / 2.5;
        const int amplitude = int(800000 * std::exp(-distance * distance / 2) + 1000);
        appendLe32(payload, quint32(amplitude)); appendLe32(payload, quint32(-amplitude / 3));
    }
    QByteArray packet(16, '\0'); packet.replace(0, 4, "MONI"); packet[4] = 1;
    packet[5] = layout.fftPoints == 0 ? 5 : 4;
    if (layout.fftPoints == 0)
        qToBigEndian(quint16(sequence % 10 == 0 ? 3 : 2), packet.data() + 6);
    qToBigEndian(sequence, packet.data() + 8);
    qToBigEndian(quint32(payload.size()), packet.data() + 12);
    packet.append(payload);
    return packet;
}

void SpreadReferenceAssembler::reset()
{ pending = SpreadReference(); expectedTotal = 0; active = false; }

bool SpreadReferenceAssembler::accept(const SpreadReferenceChunk &chunk, SpreadReference *complete)
{
    if (!chunk.present) return false;
    const int count = std::min(252, chunk.total - chunk.offset);
    if (chunk.total != 8192 || chunk.offset < 0 || chunk.offset >= chunk.total ||
        chunk.offset % 252 != 0 || chunk.i.size() != count || chunk.q.size() != count) {
        reset(); return false;
    }
    if (chunk.offset == 0) {
        reset(); active = true; expectedTotal = chunk.total;
        pending.captureId = chunk.captureId; pending.sampleRateHz = chunk.sampleRateHz;
        pending.i.reserve(chunk.total); pending.q.reserve(chunk.total);
    }
    if (!active || pending.captureId != chunk.captureId ||
        pending.sampleRateHz != chunk.sampleRateHz || expectedTotal != chunk.total ||
        pending.i.size() != chunk.offset) {
        reset(); return false;
    }
    pending.i += chunk.i; pending.q += chunk.q;
    if (pending.i.size() != expectedTotal) return false;
    if (complete) *complete = pending;
    reset(); return true;
}

void AcquisitionFftAssembler::reset()
{ pending = AcquisitionFftFrame(); total = 0; active = false; }

bool AcquisitionFftAssembler::accept(const AcquisitionFftChunk &chunk,
                                    AcquisitionFftFrame *complete)
{
    if (!chunk.present) return false; // IQ, demod and reference packets may interleave.
    if (chunk.offset == 0) {
        reset(); active = true; total = chunk.total;
        pending = chunk.frame; pending.power.clear(); pending.power.reserve(total);
    }
    if (!active || total != chunk.total || pending.captureId != chunk.frame.captureId ||
        pending.searchId != chunk.frame.searchId || pending.flags != chunk.frame.flags ||
        pending.peakBin != chunk.frame.peakBin || pending.overwritten != chunk.frame.overwritten ||
        pending.windowFrames != chunk.frame.windowFrames ||
        pending.continuousWindow != chunk.frame.continuousWindow ||
        pending.power.size() != chunk.offset ||
        chunk.frame.power.size() != std::min(124, total - chunk.offset)) {
        reset(); return false;
    }
    pending.power += chunk.frame.power;
    if (pending.power.size() != total) return false;
    if (complete) *complete = pending;
    reset(); return true;
}
