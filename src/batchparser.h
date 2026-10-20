#pragma once

#include <QList>
#include <QString>

// Разбор списка "приход / списание списком": по строке на позицию, как на сайте (web/src/batch.js).
//   КОД            -> 1 шт (так выглядит каждый скан сканером)
//   КОД 5          -> 5 шт
//   КОД;5  КОД<Tab>5  (вставка из Excel: колонки правее количества игнорируются)
// КОД - штрихкод, артикул или артикул на Маркете.
namespace BatchParser {

constexpr qint64 kMaxQty = 100'000'000;
constexpr int kMaxLines = 2000;
constexpr int kMaxTextLength = 200'000;

struct Line {
    int n = 0;          // номер строки в исходном тексте (с единицы)
    QString key;
    qint64 qty = 1;
    QString error;      // пусто - строка разобрана
};

QList<Line> parse(const QString &text);

} // namespace BatchParser
