#include "demodsymbolmodel.h"

int DemodSymbolModel::rowCount(const QModelIndex &parent) const
{ return parent.isValid() ? 0 : words.size() * 16; }
int DemodSymbolModel::columnCount(const QModelIndex &parent) const
{ return parent.isValid() ? 0 : 5; }
QVariant DemodSymbolModel::data(const QModelIndex &index, int role) const
{
    if (role != Qt::DisplayRole || !index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const int pair = highPairFirst ? 15 - index.row() % 16 : index.row() % 16;
    const quint32 word = words[index.row() / 16];
    switch (index.column()) {
    case 0: return index.row();
    case 1: return index.row() / 16;
    case 2: return pair;
    case 3: return int((word >> (pair * 2)) & 1U);
    case 4: return int((word >> (pair * 2 + 1)) & 1U);
    default: return {};
    }
}
QVariant DemodSymbolModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return section + 1;
    const QString headers[] = {QStringLiteral("符号索引"), QStringLiteral("字地址"), QStringLiteral("字内位对"), "I bit", "Q bit"};
    return section >= 0 && section < 5 ? QVariant(headers[section]) : QVariant();
}
void DemodSymbolModel::setWords(const QVector<quint32> &newWords, bool highFirst)
{
    if (words == newWords && highPairFirst == highFirst) return;
    beginResetModel(); words = newWords; highPairFirst = highFirst; endResetModel();
}
