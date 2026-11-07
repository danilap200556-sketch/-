#include "marketbarcodes.h"

#include "markingcode.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace MarketBarcodes {

namespace {

constexpr int kBatch = 100;

QString issueText(const MarketOfferIssue &i)
{
    return i.message.isEmpty() ? i.type : i.message;
}

QStringList uniqueKeepOrder(const QStringList &list)
{
    QStringList out;
    QSet<QString> seen;
    for (const QString &s : list)
        if (!s.isEmpty() && !seen.contains(s)) {
            seen.insert(s);
            out << s;
        }
    return out;
}

struct Local {
    QString name;
    QList<int> productIds;
    QStringList barcodes;
};

QHash<QString, Local> loadLocal()
{
    QHash<QString, Local> out;
    QHash<int, QString> offerOf;
    QSqlQuery q(QStringLiteral("SELECT p.id, COALESCE(NULLIF(TRIM(p.market_sku), ''), p.sku), p.name FROM products p"));
    while (q.next()) {
        const QString offer = q.value(1).toString().trimmed();
        if (offer.isEmpty())
            continue;
        Local &l = out[offer];
        l.productIds << q.value(0).toInt();
        if (l.name.isEmpty())
            l.name = q.value(2).toString();
        offerOf.insert(q.value(0).toInt(), offer);
    }
    QSqlQuery b(QStringLiteral("SELECT product_id, barcode FROM product_barcodes ORDER BY product_id, created_at, barcode"));
    while (b.next()) {
        const auto it = offerOf.constFind(b.value(0).toInt());
        if (it != offerOf.cend())
            out[*it].barcodes << b.value(1).toString().trimmed();
    }
    return out;
}

} // namespace

Code classify(const QString &raw)
{
    Code c;
    QString d = raw.trimmed();
    c.digits = d;
    if (d.isEmpty()) {
        c.note = QStringLiteral("пустой штрихкод");
        return c;
    }
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    if (!digits.match(d).hasMatch()) {
        c.note = QStringLiteral("штрихкод не из цифр");
        return c;
    }
    if (d.size() == 14) {
        if (!d.startsWith(QLatin1Char('0'))) {
            c.kind = Kind::Unsupported;
            c.note = QStringLiteral("GTIN-14 с признаком упаковки: Маркет принимает EAN-13, EAN-8 и UPC-A");
            return c;
        }
        d = MarkingCode::barcodeFromGtin(d);
        c.digits = d;
    }
    if (d.size() != 8 && d.size() != 12 && d.size() != 13) {
        c.note = QStringLiteral("нужно 8, 12 или 13 цифр, а здесь %1").arg(d.size());
        return c;
    }
    if (!MarkingCode::gtinChecksumOk(d)) {
        c.note = QStringLiteral("неверная контрольная цифра");
        return c;
    }
    if (d.startsWith(QLatin1Char('2')) || d.startsWith(QLatin1String("02"))) {
        c.kind = Kind::Internal;
        c.note = QStringLiteral("внутренний код (начинается на 2): Маркет не считает его GTIN");
        return c;
    }
    c.kind = Kind::Gtin;
    return c;
}

QString statusText(Status status)
{
    switch (status) {
    case Status::Same: return QStringLiteral("совпадает");
    case Status::Change: return QStringLiteral("будет отправлено");
    case Status::NoGtin: return QStringLiteral("нет подходящего штрихкода");
    case Status::Archived: return QStringLiteral("в архиве Маркета");
    }
    return QString();
}

QStringList defaultExcludePrefixes()
{
    return {QStringLiteral("20")};
}

QStringList parsePrefixes(const QString &text)
{
    QStringList out;
    static const QRegularExpression sep(QStringLiteral("[,;\\s]+"));
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    for (const QString &part : text.split(sep, Qt::SkipEmptyParts))
        if (digits.match(part).hasMatch() && !out.contains(part))
            out << part;
    return out;
}

bool load(const MarketAccount &acc, MarketApi &api, const QStringList &onlyOfferIds, Source *out, QString *error)
{
    *out = Source();
    const QHash<QString, Local> local = loadLocal();
    QSet<QString> only;
    for (const QString &id : onlyOfferIds)
        only.insert(id.trimmed());

    QHash<QString, MarketOffer> catalog;
    if (!api.catalog(acc, &catalog, error))
        return false;
    QStringList candidates;
    for (auto it = local.cbegin(); it != local.cend(); ++it) {
        if (!only.isEmpty() && !only.contains(it.key()))
            continue;
        ++out->ourOffers;
        if (catalog.contains(it.key()))
            candidates << it.key();
        else
            ++out->notOnMarket;
    }
    std::sort(candidates.begin(), candidates.end());

    QHash<QString, QStringList> onMarket;
    if (!api.offerBarcodes(acc, candidates, &onMarket, error))
        return false;

    for (const QString &offer : std::as_const(candidates)) {
        const Local &l = local.value(offer);
        Source::Item item;
        item.name = l.name;
        item.productIds = l.productIds;
        item.barcodes = l.barcodes;
        item.market = onMarket.value(offer);
        item.archived = catalog.value(offer).archived;
        out->items.insert(offer, item);
    }
    return true;
}

Compared build(const Source &source, const Options &opt)
{
    Compared out;
    out.ourOffers = source.ourOffers;
    out.notOnMarket = source.notOnMarket;
    QStringList prefixes;
    for (const QString &p : opt.excludePrefixes)
        if (!p.trimmed().isEmpty())
            prefixes << p.trimmed();
    QStringList offers = source.items.keys();
    std::sort(offers.begin(), offers.end());

    for (const QString &offer : std::as_const(offers)) {
        const Source::Item &l = source.items.value(offer);
        Row r;
        r.offerId = offer;
        r.name = l.name;
        r.productIds = l.productIds;
        r.market = l.market;
        QStringList notes, notGtin;
        for (const QString &raw : uniqueKeepOrder(l.barcodes)) {
            const Code c = classify(raw);
            if (c.kind != Kind::Gtin && c.kind != Kind::Internal) {
                r.skipped << QStringLiteral("%1 - %2").arg(raw, c.note);
                continue;
            }
            QString hit;
            for (const QString &p : std::as_const(prefixes))
                if (c.digits.startsWith(p)) {
                    hit = p;
                    break;
                }
            if (!hit.isEmpty()) {
                ++r.skippedByPrefix;
                r.skipped << QStringLiteral("%1 - начинается на %2: по вашему списку не передаётся").arg(raw, hit);
                continue;
            }
            r.ours << c.digits;
            if (c.kind == Kind::Internal)
                notGtin << c.digits;
        }
        r.ours = uniqueKeepOrder(r.ours);
        if (!r.skipped.isEmpty())
            notes << QStringLiteral("не передаём: %1").arg(r.skipped.join(QStringLiteral("; ")));
        if (!notGtin.isEmpty())
            notes << QStringLiteral("начинается на 2, Маркет может не принять: %1").arg(notGtin.join(QStringLiteral(", ")));
        if (l.archived) {
            r.status = Status::Archived;
        } else if (r.ours.isEmpty()) {
            r.status = Status::NoGtin;
            r.result = r.market; // ничего не меняем
        } else {
            if (opt.mode == Mode::Replace) {
                r.result = r.ours;
                QStringList gone;
                for (const QString &m : std::as_const(r.market))
                    if (!r.ours.contains(m))
                        gone << m;
                if (!gone.isEmpty())
                    notes << QStringLiteral("уйдут с Маркета: %1").arg(gone.join(QStringLiteral(", ")));
            } else {
                r.result = uniqueKeepOrder(r.market + r.ours);
            }
            r.status = QSet<QString>(r.result.cbegin(), r.result.cend()) == QSet<QString>(r.market.cbegin(), r.market.cend()) ? Status::Same : Status::Change;
        }
        if (l.productIds.size() > 1)
            notes << QStringLiteral("на этот артикул Маркета привязано товаров: %1").arg(l.productIds.size());
        r.note = notes.join(QStringLiteral("; "));
        out.rows << r;
    }
    auto rank = [](Status s) { return s == Status::Change ? 0 : s == Status::NoGtin ? 1 : s == Status::Archived ? 2 : 3; };
    std::stable_sort(out.rows.begin(), out.rows.end(), [&](const Row &a, const Row &b) { return rank(a.status) < rank(b.status); });
    return out;
}

bool compare(const MarketAccount &acc, MarketApi &api, const QStringList &onlyOfferIds, const Options &opt, Compared *out, QString *error)
{
    Source source;
    if (!load(acc, api, onlyOfferIds, &source, error)) {
        *out = Compared();
        return false;
    }
    *out = build(source, opt);
    return true;
}

Report push(MarketApi &api, const MarketAccount &acc, const QList<Row> &rows, QString *error)
{
    Report report;
    QList<const Row *> todo;
    for (const Row &r : rows)
        if (r.status == Status::Change && !r.result.isEmpty())
            todo << &r;

    auto accept = [&](const Row *r, const MarketOfferResult *res) {
        QStringList warns;
        if (res)
            for (const MarketOfferIssue &w : res->warnings) {
                const QString t = issueText(w);
                // Предупреждение «не переданы габариты» к штрихкодам отношения не имеет: габаритов мы и не передаём.
                if (!t.isEmpty() && !t.contains(QStringLiteral("габарит"), Qt::CaseInsensitive) && !w.type.contains(QStringLiteral("DIMENSIONS")))
                    warns << t;
            }
        ++report.accepted;
        report.acceptedOffers << r->offerId;
        report.lines << QStringLiteral("%1: принято (%2)%3").arg(r->offerId, r->result.join(QStringLiteral(", ")),
                                                                warns.isEmpty() ? QString() : QStringLiteral(", предупреждения: %1").arg(warns.join(QStringLiteral("; "))));
        QSqlQuery q; // карточка в нашей базе (вкладка «Карточки Маркета») - в тех же штрихкодах
        q.prepare(QStringLiteral("UPDATE market_cards SET barcodes = ?, updated_at = NOW() WHERE account_id = ? AND offer_id = ?"));
        q.addBindValue(r->result.join(QStringLiteral(";")));
        q.addBindValue(acc.id);
        q.addBindValue(r->offerId);
        q.exec();
    };
    auto reject = [&](const Row *r, const QString &why) {
        ++report.rejected;
        report.rejectedOffers << r->offerId;
        report.lines << QStringLiteral("%1: ОТКАЗ - %2").arg(r->offerId, why);
    };

    for (int start = 0; start < todo.size(); start += kBatch) {
        QList<const Row *> pending = todo.mid(start, kBatch);
        for (int pass = 0; pass < 2 && !pending.isEmpty(); ++pass) {
            QJsonArray arr;
            for (const Row *r : std::as_const(pending)) {
                QJsonArray codes;
                for (const QString &c : r->result)
                    codes.append(c);
                arr.append(QJsonObject{{QStringLiteral("offer"), QJsonObject{{QStringLiteral("offerId"), r->offerId}, {QStringLiteral("barcodes"), codes}}}});
            }
            QList<MarketOfferResult> results;
            QString err;
            const bool ok = api.updateOfferMappings(acc.businessId, arr, &results, &err);
            QHash<QString, const MarketOfferResult *> byId;
            for (const MarketOfferResult &res : std::as_const(results))
                byId.insert(res.offerId, &res);
            if (!ok && results.isEmpty()) {
                // Вся пачка не принята по общей причине (ключ, права, связь).
                for (const Row *r : std::as_const(pending))
                    reject(r, err);
                if (error)
                    *error = err;
                pending.clear();
                break;
            }
            QList<const Row *> retry;
            bool anyFailed = false;
            for (const Row *r : std::as_const(pending)) {
                const MarketOfferResult *res = byId.value(r->offerId, nullptr);
                if (res && !res->errors.isEmpty()) {
                    anyFailed = true;
                    QStringList errs;
                    for (const MarketOfferIssue &i : res->errors)
                        errs << issueText(i);
                    reject(r, errs.join(QStringLiteral("; ")));
                } else if (ok) {
                    accept(r, res);
                } else if (pass == 0) {
                    retry << r; // своих ошибок нет, но пачка отклонена из-за соседей
                } else {
                    reject(r, err);
                }
            }
            pending = (anyFailed && pass == 0) ? retry : QList<const Row *>();
        }
    }
    return report;
}

} // namespace MarketBarcodes
