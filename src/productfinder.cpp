#include "productfinder.h"
#include "barcode.h"

#include <QSqlQuery>
#include <QVariant>

namespace ProductFinder {

namespace {

QString likePattern(const QString &text)
{
    QString t = text;
    t.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('%'), QLatin1String("\\%"))
        .replace(QLatin1Char('_'), QLatin1String("\\_"));
    return QLatin1Char('%') + t + QLatin1Char('%');
}

QList<int> ids(const QString &sql, const QVariantList &binds)
{
    QList<int> out;
    QSqlQuery q;
    q.prepare(sql);
    for (const QVariant &b : binds)
        q.addBindValue(b);
    if (q.exec())
        while (q.next())
            out << q.value(0).toInt();
    return out;
}

} // namespace

Result find(const QString &input, int limit)
{
    Result r;
    const QString text = input.trimmed();
    if (text.isEmpty())
        return r;

    // 1) штрихкод (сканер печатает его без пробелов; с пробелами/дефисами тоже поймём)
    const QString code = Barcode::normalize(text);
    if (!code.isEmpty()) {
        const int owner = Barcode::productIdFor(code);
        if (owner >= 0) {
            r.ids << owner;
            r.exact = true;
            return r;
        }
    }
    // 2) артикул целиком, 3) артикул на Маркете целиком (без учёта регистра)
    for (const char *column : {"sku", "market_sku"}) {
        const QList<int> hit = ids(QStringLiteral("SELECT id FROM products WHERE lower(%1) = lower(?) ORDER BY sku LIMIT 50")
                                       .arg(QLatin1String(column)),
                                   {text});
        if (!hit.isEmpty()) {
            r.ids = hit;
            r.exact = hit.size() == 1;
            return r;
        }
    }
    // 4) часть артикула, названия, артикула на Маркете или штрихкода
    const QString like = likePattern(text);
    r.ids = ids(QStringLiteral("SELECT p.id FROM products p WHERE p.sku ILIKE ? OR p.name ILIKE ? OR p.market_sku ILIKE ? "
                               "OR EXISTS (SELECT 1 FROM product_barcodes b WHERE b.product_id = p.id AND b.barcode ILIKE ?) "
                               "ORDER BY p.sku LIMIT %1").arg(limit),
                {like, like, like, like});
    return r;
}

QSet<QString> offerIds(const QString &text)
{
    QSet<QString> out;
    const Result r = find(text, 500);
    for (int id : r.ids)
        out.insert(offerIdFor(id));
    return out;
}

QString offerIdFor(int productId)
{
    QSqlQuery q;
    q.prepare("SELECT COALESCE(NULLIF(TRIM(market_sku), ''), sku) FROM products WHERE id = ?");
    q.addBindValue(productId);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

int productForOffer(const QString &offerId)
{
    const QList<int> hit = ids("SELECT id FROM products WHERE COALESCE(NULLIF(TRIM(market_sku), ''), sku) = ? "
                               "ORDER BY id LIMIT 1", {offerId});
    return hit.isEmpty() ? -1 : hit.first();
}

} // namespace ProductFinder
