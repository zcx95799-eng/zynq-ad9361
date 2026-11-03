#ifndef MONITORPROTOCOL_H
#define MONITORPROTOCOL_H

#include <QByteArray>
#include <QString>
#include <QVector>

struct MonitorFrame {
    quint8 type = 0;
    quint16 flags = 0;
    quint32 sequence = 0;
    QByteArray payload;
    bool hasWireHeader = true;
};

// TCP is a byte stream. Retain partial headers/payloads across readyRead calls.
class MonitorParser {
public:
    static constexpr int HeaderBytes = 16;
    static constexpr int MaxPayloadBytes = 256 * 1024;
    QVector<MonitorFrame> feed(const QByteArray &bytes);
    void reset();
    quint64 discardedBytes() const { return discarded; }
    quint64 invalidHeaders() const { return invalid; }
    int bufferedBytes() const { return buffer.size(); }
private:
    QByteArray buffer;
    quint64 discarded = 0;
    quint64 invalid = 0;
};

enum class IqEncoding { I12Q12In16, Q12I12In16, PackedI12Q12, SignedI16Q16 };
struct MonitorLayout {
    int iqSamples = 1024;
    int demodBytes = 1024;
    int fftPoints = 0;
    IqEncoding iqEncoding = IqEncoding::SignedI16Q16;
    int payloadBytes() const { return iqSamples * 4 + demodBytes + fftPoints * 8; }
};
// SPR1 is a real pre-PN payload observation, never a demodulated frame.
struct SpreadReference {
    QVector<double> i, q;
    quint32 captureId = 0, sampleRateHz = 0;
};
struct SpreadReferenceChunk {
    QVector<double> i, q;
    quint32 captureId = 0, sampleRateHz = 0;
    int offset = 0, total = 0;
    bool present = false;
};
class SpreadReferenceAssembler {
public:
    bool accept(const SpreadReferenceChunk &chunk, SpreadReference *complete);
    void reset();
private:
    SpreadReference pending;
    int expectedTotal = 0;
    bool active = false;
};
// FFP1 carries raw uint64 acquisition powers, split over the existing auxiliary
// area. It is unrelated to business bits and to the ordinary receive-IQ PSD.
struct AcquisitionFftFrame {
    QVector<double> power;
    quint32 captureId = 0, searchId = 0, flags = 0, overwritten = 0;
    int peakBin = 0; // index in the complete uploaded window
    int windowFrames = 1;
    bool continuousWindow = false;
    bool successful() const { return flags & 1U; }
};
struct AcquisitionFftChunk {
    AcquisitionFftFrame frame;
    int offset = 0, total = 0;
    bool present = false;
};
class AcquisitionFftAssembler {
public:
    bool accept(const AcquisitionFftChunk &chunk, AcquisitionFftFrame *complete);
    void reset();
private:
    AcquisitionFftFrame pending;
    int total = 0;
    bool active = false;
};

struct MonitorData {
    quint8 type = 0;
    quint32 sequence = 0;
    quint16 flags = 0;
    QVector<double> i, q, fftReal, fftImag, fftMagnitude, fftDb;
    QVector<quint32> demodWords;
    SpreadReferenceChunk spreadReference;
    AcquisitionFftChunk acquisitionFft;
    bool demodAllZero = true;
    // The supplied type-5 PL uses flags bit 0 for a complete demod frame.
    // Raw payloads and legacy type 4 do not have confirmed validity metadata.
    bool demodValidityKnown = false;
    bool demodFrameValid = false;
    bool retainDemod() const {
        return demodValidityKnown ? demodFrameValid : !demodAllZero;
    }
};
// Each word contains 16 {Q,I} pairs. BRAM read ordering must be confirmed.
void unpackDemodWords(const QVector<quint32> &words, bool highPairFirst,
                      QVector<double> *iBits, QVector<double> *qBits);
bool decodeMonitorFrame(const MonitorFrame &frame, const MonitorLayout &layout,
                        MonitorData *data, QString *error);
// The demo uses the same wire framing and decoder as live reception.
QByteArray makeDemoFrame(quint32 sequence, const MonitorLayout &layout);
#endif
