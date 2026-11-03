#include "sendpayloadprotocol.h"
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtEndian>
#include <cmath>

namespace TxPayload {
QByteArray frame(const QByteArray &payload, quint32 sequence)
{
    if (payload.size() != PayloadBytes) return {};
    QByteArray header(12, '\0'); header.replace(0, 4, "TXD1");
    qToBigEndian(sequence, header.data() + 4);
    qToBigEndian(quint32(PayloadBytes), header.data() + 8);
    return header + payload;
}

bool normalize(const QByteArray &input, QByteArray *payload, QString *error)
{
    if (input.size() == PayloadBytes) { *payload = input; return true; }
    if (input.size() == 12 + PayloadBytes && input.startsWith("TXD1")
        && qFromBigEndian<quint32>(input.constData() + 8) == PayloadBytes) {
        *payload = input.mid(12); return true;
    }
    *error = QStringLiteral("TXD1 需要 1024 B 载荷（256 个 LE32 字），当前 %1 B；也可载入完整的 1036 B TXD1 帧。").arg(input.size());
    return false;
}

bool loadFile(const QString &filename, QByteArray *payload, QString *description, QString *error)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return false; }
    if (file.size() > 65536) { *error = QStringLiteral("请载入不超过 64 KiB 的文件。"); return false; }
    const QByteArray input = file.readAll();
    if (file.error() != QFileDevice::NoError) { *error = file.errorString(); return false; }
    QString text = QString::fromUtf8(input);
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    const QString suffix = QFileInfo(filename).suffix().toLower();
    QByteArray decoded;
    if (text.startsWith("index\tI_int16\tQ_int16\tI_LE_Q_LE")) {
        const QStringList lines = text.trimmed().split('\n');
        for (int n = 1; n < lines.size(); ++n) {
            const QStringList fields = lines[n].trimmed().split('\t');
            bool indexOk = false, iOk = false, qOk = false;
            const int index = fields.value(0).toInt(&indexOk);
            const int i = fields.value(1).toInt(&iOk), q = fields.value(2).toInt(&qOk);
            QByteArray word(4, '\0');
            if (fields.size() != 4 || !indexOk || index != n - 1 || !iOk || !qOk
                || i < -32768 || i > 32767 || q < -32768 || q > 32767) {
                *error = QStringLiteral("IQ 数值表第 %1 行格式、索引或 int16 范围无效。").arg(n + 1); return false;
            }
            qToLittleEndian(qint16(i), word.data()); qToLittleEndian(qint16(q), word.data() + 2);
            QString hex = fields[3]; hex.remove(QRegularExpression("\\s"));
            if (!QRegularExpression("^[0-9A-Fa-f]{8}$").match(hex).hasMatch()
                || QByteArray::fromHex(hex.toLatin1()) != word) {
                *error = QStringLiteral("IQ 数值表第 %1 行字节与 I/Q 数值不一致。").arg(n + 1); return false;
            }
            decoded += word;
        }
        *description = QStringLiteral("IQ 数值表 → %1 组小端 I16/Q16，%2 B").arg(lines.size() - 1).arg(decoded.size());
    } else if (suffix == "hex") {
        text.remove(QRegularExpression("\\s"));
        if (text.isEmpty() || text.size() % 2 || !QRegularExpression("^[0-9A-Fa-f]+$").match(text).hasMatch()) {
            *error = QStringLiteral("HEX 文件必须包含偶数个有效十六进制字符。"); return false;
        }
        decoded = QByteArray::fromHex(text.toLatin1());
        *description = QStringLiteral("HEX 文本 → %1 B").arg(decoded.size());
    } else if (suffix == "txt" || suffix == "csv" || suffix == "tsv") {
        *error = QStringLiteral("未识别的文本格式；请使用 IQ 数值表、.hex 或原始 .bin 文件。"); return false;
    } else {
        decoded = input; *description = QStringLiteral("原始二进制 %1 B").arg(decoded.size());
    }
    *payload = decoded; return true;
}

QByteArray sine()
{
    QByteArray payload(PayloadBytes, '\0');
    for (int n = 0; n < 256; ++n)
        qToLittleEndian(qint16(std::round(12000 * std::sin(2 * 3.14159265358979323846 * n / 64))), payload.data() + n * 4);
    return payload;
}

QString resultText(quint32 result)
{
    switch (result) {
    case 0: return QStringLiteral("板端已提交到 PL");
    case 1: return QStringLiteral("硬件签名不匹配（UP_BAD_HARDWARE）");
    case 2: return QStringLiteral("PL 锁定等待超时（UP_BUSY_TIMEOUT）");
    case 3: return QStringLiteral("GPIO 命令错误（UP_COMMAND_ERROR）");
    case 4: return QStringLiteral("GPIO 握手超时（UP_ACK_TIMEOUT）");
    case 5: return QStringLiteral("整帧上传超时（UP_FRAME_TIMEOUT）");
    default: return QStringLiteral("未知板端结果 %1").arg(result);
    }
}
}
