#pragma once

#include "marketcards.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QSqlQuery;

namespace ProductMarket {

constexpr int kColumnCount = 12;

// Столбцы products в порядке, который читает fromRow(); prefix - псевдоним таблицы («p.»).
QString selectColumns(const QString &prefix = QString());
ProductMarketInfo fromRow(const QSqlQuery &q, int first);
ProductMarketInfo fromValues(const QVariantList &row, int first);
// Для UPDATE ("market_vendor = ?, ...") и INSERT (столбцы и «?, ?, ...»); значения - bind() в том же порядке.
QString updateAssignments();
QString insertColumns();
QString insertMarks();
void bind(QSqlQuery &q, const ProductMarketInfo &info);

// Кратко для списков и подсказок: «Nike · Китай · 33×22×12 см, 1.2 кг · категория, характеристик: 4».
QString summary(const ProductMarketInfo &info);

bool load(int productId, ProductMarketInfo *out);
QHash<int, ProductMarketInfo> loadMany(const QList<int> &productIds, QString *error = nullptr);
// Записывает параметры нескольким товарам одним запросом. Остальные данные товара не затрагиваются.
bool saveMany(const QList<QPair<int, ProductMarketInfo>> &items, QString *error);

} // namespace ProductMarket
