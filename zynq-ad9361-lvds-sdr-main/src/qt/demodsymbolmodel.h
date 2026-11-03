#ifndef DEMODSYMBOLMODEL_H
#define DEMODSYMBOLMODEL_H
#include <QAbstractTableModel>
#include <QVector>

// Computes only visible cells; no allocation or rewrite of 20,480 table items.
class DemodSymbolModel : public QAbstractTableModel {
public:
    explicit DemodSymbolModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void setWords(const QVector<quint32> &words, bool highPairFirst);
private:
    QVector<quint32> words;
    bool highPairFirst = false;
};
#endif
