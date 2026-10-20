#include "productfinder.h"
#include "barcode.h"

#include <QSqlQuery>
#include <functional>
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

int findExact(const QString &input, QString *error)
{
    const QString text = input.trimmed();
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return -1;
    };
    const Result r = find(text);
    if (r.ids.isEmpty())
        return fail(QStringLiteral("Товар «%1» не найден: нет такого артикула или штрихкода").arg(text));
    if (r.exact)
        return r.ids.first();
    // Несколько товаров с одним артикулом или частичное совпадение: смотрим, нет ли ровно одного полного.
    const QList<int> full = ids("SELECT id FROM products WHERE lower(sku) = lower(?) OR lower(market_sku) = lower(?)", {text, text});
    if (full.size() == 1)
        return full.first();
    return fail(full.size() > 1
                    ? QStringLiteral("Под «%1» подходит несколько товаров - введите артикул или штрихкод точнее").arg(text)
                    : QStringLiteral("Товар «%1» не найден: нужен артикул или штрихкод целиком").arg(text));
}

void findExactMany(const QStringList &keys, QHash<QString, int> *ids, QHash<QString, QString> *errors)
{
    ids->clear();
    errors->clear();
    QStringList todo;
    {
        QSet<QString> seen;
        for (const QString &k : keys) {
            const QString t = k.trimmed();
            if (!t.isEmpty() && !seen.contains(t)) {
                seen.insert(t);
                todo << t;
            }
        }
    }

    constexpr int kChunk = 400;
    auto chunks = [](const QStringList &list, const std::function<void(const QStringList &)> &fn) {
        for (int i = 0; i < list.size(); i += kChunk)
            fn(list.mid(i, kChunk));
    };
    auto valuesList = [](int n) {
        QStringList marks;
        for (int i = 0; i < n; ++i)
            marks << QStringLiteral("(?::text)");
        return marks.join(QLatin1Char(','));
    };

    // 1) штрихкоды
    QHash<QString, QStringList> keysByCode;
    for (const QString &k : std::as_const(todo)) {
        const QString code = Barcode::normalize(k);
        if (!code.isEmpty())
            keysByCode[code] << k;
    }
    chunks(keysByCode.keys(), [&](const QStringList &codes) {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT k.code, b.product_id FROM (VALUES %1) AS k(code) "
                                 "JOIN product_barcodes b ON b.barcode = k.code").arg(valuesList(codes.size())));
        for (const QString &c : codes)
            q.addBindValue(c);
        if (!q.exec())
            return;
        while (q.next())
            for (const QString &k : keysByCode.value(q.value(0).toString()))
                if (!ids->contains(k))
                    ids->insert(k, q.value(1).toInt());
    });

    // 2) артикул целиком, потом артикул на Маркете целиком; неоднозначное разберёт findExact
    QStringList rest;
    for (const QString &k : std::as_const(todo))
        if (!ids->contains(k))
            rest << k;
    QHash<QString, QList<int>> bySku, byMarket;
    chunks(rest, [&](const QStringList &part) {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT k.key, p.id, (lower(p.sku) = lower(k.key)) FROM (VALUES %1) AS k(key) "
                                 "JOIN products p ON lower(p.sku) = lower(k.key) OR lower(p.market_sku) = lower(k.key)")
                      .arg(valuesList(part.size())));
        for (const QString &k : part)
            q.addBindValue(k);
        if (!q.exec())
            return;
        while (q.next()) {
            const QString key = q.value(0).toString();
            (q.value(2).toBool() ? bySku : byMarket)[key] << q.value(1).toInt();
        }
    });
    for (const QString &k : std::as_const(rest)) {
        const QList<int> s = bySku.value(k), m = byMarket.value(k);
        if (s.size() == 1)
            ids->insert(k, s.first());
        else if (s.isEmpty() && m.size() == 1)
            ids->insert(k, m.first());
    }

    // 3) всё остальное - по одному, с точной причиной отказа
    for (const QString &k : std::as_const(todo)) {
        if (ids->contains(k))
            continue;
        QString err;
        const int id = findExact(k, &err);
        if (id >= 0)
            ids->insert(k, id);
        else
            errors->insert(k, err);
    }
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
