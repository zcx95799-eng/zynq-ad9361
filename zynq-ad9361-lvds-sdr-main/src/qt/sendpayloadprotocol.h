#ifndef SENDPAYLOADPROTOCOL_H
#define SENDPAYLOADPROTOCOL_H
#include <QByteArray>
#include <QString>

namespace TxPayload {
constexpr int PayloadBytes = 1024;
QByteArray frame(const QByteArray &payload, quint32 sequence);
bool normalize(const QByteArray &input, QByteArray *payload, QString *error);
bool loadFile(const QString &filename, QByteArray *payload, QString *description, QString *error);
QByteArray sine();
QString resultText(quint32 result);
}
#endif
