#include "markingcode.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace MarkingCode {

namespace {

constexpr int kChunk = 400;  // кодов в одном запросе
constexpr int kMaxProblems = 30;

bool printable(const QString &s)
{
    for (const QChar c : s)
        if (c.unicode() < 0x20 || c.unicode() > 0x7E)
            return false;
    return true;
}

QString stripGs(QString s)
{
    while (s.startsWith(kGs))
        s.remove(0, 1);
    while (s.endsWith(kGs))
        s.chop(1);
    return s;
}

bool allDigits(const QString &s)
{
    if (s.isEmpty())
        return false;
    for (const QChar c : s)
        if (!c.isDigit() || c.unicode() > 0x7F)
            return false;
    return true;
}

// Пустая строка, а не NULL: драйвер передаёт QString() как NULL, а в таблице столбцы NOT NULL.
QString nn(const QString &s)
{
    return s.isNull() ? QStringLiteral("") : s;
}

QVariant nullInt()
{
    return QVariant(QMetaType(QMetaType::Int));
}

QString likeEscape(const QString &term)
{
    QString t = term;
    t.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('%'), QLatin1String("\\%")).replace(QLatin1Char('_'), QLatin1String("\\_"));
    return QLatin1Char('%') + t + QLatin1Char('%');
}

} // namespace

bool gtinChecksumOk(const QString &digits)
{
    if (!allDigits(digits) || (digits.size() != 8 && digits.size() != 12 && digits.size() != 13 && digits.size() != 14))
        return false;
    int sum = 0;
    // веса 3,1,3,1... считая справа от контрольной цифры
    for (int i = 0; i < digits.size() - 1; ++i) {
        const int d = digits[digits.size() - 2 - i].digitValue();
        sum += d * (i % 2 == 0 ? 3 : 1);
    }
    return (10 - sum % 10) % 10 == digits.back().digitValue();
}

QString barcodeFromGtin(const QString &gtin)
{
    if (gtin.size() == 14 && gtin.startsWith(QLatin1Char('0'))) {
        if (gtin.startsWith(QLatin1String("00")))
            return gtin.mid(2); // UPC-A
        return gtin.mid(1);     // EAN-13
    }
    return gtin;
}

QString gtinFromBarcode(const QString &barcode)
{
    const QString b = barcode.trimmed();
    if (!allDigits(b) || (b.size() != 12 && b.size() != 13 && b.size() != 14))
        return {};
    return QString(14 - b.size(), QLatin1Char('0')) + b;
}

Parsed parse(const QString &raw)
{
    Parsed r;
    QString t = raw.trimmed();
    t = stripGs(t);
    for (const QString &prefix : {QStringLiteral("]d2"), QStringLiteral("]C1"), QStringLiteral("]Q3")})
        if (t.startsWith(prefix)) {
            t.remove(0, prefix.size());
            break;
        }
    // Видимые заменители разделителя, которые оставляют программы и некоторые сканеры.
    for (const QString &ph : {QStringLiteral("<GS>"), QStringLiteral("{GS}"), QStringLiteral("[GS]"), QStringLiteral("\\x1d"), QStringLiteral("\\u001d"),
                              QStringLiteral("^]"), QStringLiteral("␝"), QStringLiteral("<FNC1>")})
        t.replace(ph, QString(kGs), Qt::CaseInsensitive);

    // Скобочная запись «(01)...(21)...(91)...(92)...»
    if (t.startsWith(QLatin1Char('('))) {
        static const QRegularExpression group(QStringLiteral("\\((\\d{2,4})\\)([^()]*)"));
        QString joined;
        int pos = 0;
        QList<QPair<QString, QString>> groups;
        for (auto it = group.globalMatch(t); it.hasNext();) {
            const auto m = it.next();
            if (m.capturedStart() != pos) {
                r.error = QStringLiteral("лишние символы вне скобок");
                return r;
            }
            pos = m.capturedEnd();
            groups.append({m.captured(1), m.captured(2)});
        }
        if (groups.isEmpty() || pos != t.size()) {
            r.error = QStringLiteral("не удалось разобрать запись со скобками");
            return r;
        }
        for (int i = 0; i < groups.size(); ++i) {
            joined += groups[i].first + groups[i].second;
            if (i + 1 < groups.size() && groups[i].first != QLatin1String("01"))
                joined += kGs;
        }
        t = joined;
    }

    if (t.isEmpty()) {
        r.error = QStringLiteral("пусто");
        return r;
    }
    if (!printable(QString(t).remove(kGs))) {
        r.error = QStringLiteral("в коде недопустимые символы");
        return r;
    }
    if (t.size() < 19 || !t.startsWith(QLatin1String("01")) || !allDigits(t.mid(2, 14))) {
        r.error = QStringLiteral("код должен начинаться с 01 и GTIN из 14 цифр");
        return r;
    }
    const QString gtin = t.mid(2, 14);
    if (!gtinChecksumOk(gtin)) {
        r.error = QStringLiteral("у GTIN %1 неверная контрольная цифра - код искажён").arg(gtin);
        return r;
    }
    if (t.mid(16, 2) != QLatin1String("21")) {
        r.error = QStringLiteral("после GTIN должна идти группа 21 (серийный номер)");
        return r;
    }

    QString code, serial;
    if (t.contains(kGs)) {
        QStringList groups = t.split(kGs, Qt::SkipEmptyParts);
        serial = groups.first().mid(18);
        code = groups.join(kGs);
    } else {
        // Слитно, без разделителей: серийный номер 13 знаков (обувь, одежда), ключ проверки 4, подпись 44 или 88.
        static const QRegularExpression full(QStringLiteral("^01(\\d{14})21(.{13})91(.{4})92(.{44}|.{88})$"));
        const auto m = full.match(t);
        if (m.hasMatch()) {
            serial = m.captured(2);
            code = QStringLiteral("01") + m.captured(1) + QStringLiteral("21") + serial + kGs + QStringLiteral("91") + m.captured(3) + kGs + QStringLiteral("92") + m.captured(4);
        } else if (t.size() - 18 >= 1 && t.size() - 18 <= 20) {
            serial = t.mid(18);   // код без криптохвоста
            code = t;
        } else {
            r.error = QStringLiteral("в коде нет разделителей групп (GS), а по длине он не похож на код обуви или одежды "
                                     "(серийный номер 13 знаков, ключ 4, подпись 44 или 88) - вставьте код вместе с GS или сканируйте сканером");
            return r;
        }
    }
    if (serial.isEmpty() || serial.size() > 20) {
        r.error = QStringLiteral("серийный номер должен быть от 1 до 20 знаков");
        return r;
    }
    r.ok = true;
    r.code = code;
    r.gtin = gtin;
    r.serial = serial;
    return r;
}

QString humanReadable(const QString &code)
{
    const Parsed p = parse(code);
    if (!p.ok)
        return code;
    return QStringLiteral("(01)%1(21)%2").arg(p.gtin, p.serial);
}

QString shortText(const QString &code)
{
    const Parsed p = parse(code);
    if (!p.ok)
        return code;
    return QStringLiteral("(01)%1(21)%2%3").arg(p.gtin, p.serial, p.code.contains(kGs) ? QStringLiteral(" …") : QString());
}

QStringList splitText(const QString &text)
{
    QStringList out;
    for (const QString &lineRaw : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        const QString line = lineRaw.trimmed();
        if (line.isEmpty())
            continue;
        // В строке может быть несколько кодов подряд (вставили слитно): режем перед каждым «01<14 цифр>21»
        // только если в строке есть GS либо скобки, иначе строка считается одним кодом.
        out << line;
    }
    return out;
}

int productIdForGtin(const QString &gtin)
{
    QStringList candidates{gtin};
    if (gtin.size() == 14 && gtin.startsWith(QLatin1Char('0'))) {
        candidates << gtin.mid(1);
        if (gtin.startsWith(QLatin1String("00")))
            candidates << gtin.mid(2);
    }
    QSqlQuery q;
    for (const QString &c : std::as_const(candidates)) {
        q.prepare(QStringLiteral("SELECT product_id FROM product_barcodes WHERE barcode = ?"));
        q.addBindValue(c);
        if (q.exec() && q.next())
            return q.value(0).toInt();
    }
    return 0;
}

bool linkGtin(const QString &gtin, int productId, QString *error)
{
    const int owner = productIdForGtin(gtin);
    if (owner == productId)
        return true;
    if (owner > 0) {
        if (error)
            *error = QStringLiteral("GTIN %1 уже привязан к другому товару").arg(gtin);
        return false;
    }
    QSqlQuery q;
    q.prepare(QStringLiteral("INSERT INTO product_barcodes (barcode, product_id) VALUES (?, ?) ON CONFLICT (barcode) DO NOTHING"));
    q.addBindValue(barcodeFromGtin(gtin));
    q.addBindValue(productId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

AddResult add(const QStringList &rawCodes, const QString &source, const QString &orderId, int forceProductId)
{
    AddResult res;
    struct Item {
        Parsed p;
        int productId = 0;
    };
    QList<Item> items;
    QSet<QString> seen;
    QHash<QString, int> productByGtin;
    QString linkError;
    int lineNo = 0;
    for (const QString &raw : rawCodes) {
        ++lineNo;
        if (raw.trimmed().isEmpty())
            continue;
        Parsed p = parse(raw);
        if (!p.ok) {
            ++res.invalid;
            if (res.problems.size() < kMaxProblems)
                res.problems << QStringLiteral("строка %1: %2").arg(lineNo).arg(p.error);
            continue;
        }
        if (seen.contains(p.code)) {
            ++res.duplicates;
            continue;
        }
        seen.insert(p.code);
        if (!productByGtin.contains(p.gtin)) {
            if (forceProductId > 0) {
                // Свой товар задан явно: его штрихкод становится GTIN кодов, если GTIN свободен или уже его.
                if (linkGtin(p.gtin, forceProductId, &linkError)) {
                    productByGtin.insert(p.gtin, forceProductId);
                } else {
                    productByGtin.insert(p.gtin, -1); // GTIN чужого товара: такие коды не берём - этикетка вышла бы с чужим артикулом
                    if (res.problems.size() < kMaxProblems)
                        res.problems << linkError;
                }
            } else {
                productByGtin.insert(p.gtin, productIdForGtin(p.gtin));
            }
        }
        if (productByGtin.value(p.gtin) < 0) {
            ++res.invalid;
            continue;
        }
        Item it;
        it.p = p;
        it.productId = productByGtin.value(p.gtin);
        items << it;
    }

    QSqlDatabase db = QSqlDatabase::database();
    QSet<QString> insertedCodes;
    for (int start = 0; start < items.size(); start += kChunk) {
        const int end = qMin(int(items.size()), start + kChunk);
        QStringList rows;
        QVariantList binds;
        for (int i = start; i < end; ++i) {
            rows << (i == start ? QStringLiteral("(?::text, ?::text, ?::text, ?::int, ?::text, ?::text)") : QStringLiteral("(?, ?, ?, ?, ?, ?)"));
            binds << items[i].p.code << items[i].p.gtin << items[i].p.serial
                  << (items[i].productId > 0 ? QVariant(items[i].productId) : nullInt()) << nn(source) << nn(orderId);
        }
        QSqlQuery q;
        q.prepare(QStringLiteral("INSERT INTO marking_codes (code, gtin, serial, product_id, source, order_id) VALUES %1 "
                                 "ON CONFLICT (code) DO NOTHING RETURNING code").arg(rows.join(QLatin1Char(','))));
        for (const QVariant &b : std::as_const(binds))
            q.addBindValue(b);
        if (!q.exec()) {
            if (res.problems.size() < kMaxProblems)
                res.problems << QStringLiteral("не удалось записать: %1").arg(q.lastError().text());
            res.invalid += end - start;
            continue;
        }
        while (q.next())
            insertedCodes.insert(q.value(0).toString());
    }
    QSet<QString> unknown;
    for (const Item &it : std::as_const(items)) {
        if (!insertedCodes.contains(it.p.code)) {
            ++res.duplicates;
            continue;
        }
        ++res.added;
        if (it.productId > 0) {
            ++res.linked;
        } else {
            ++res.unlinked;
            unknown.insert(it.p.gtin);
        }
    }
    res.unknownGtins = QStringList(unknown.begin(), unknown.end());
    res.unknownGtins.sort();
    return res;
}

int relink()
{
    QSqlQuery q;
    // GTIN-14 -> EAN-13 (ведущий ноль отброшен) или UPC-A (два нуля) или как есть.
    const bool ok = q.exec(QStringLiteral(
        "UPDATE marking_codes m SET product_id = b.product_id FROM product_barcodes b "
        "WHERE m.product_id IS NULL AND (b.barcode = m.gtin OR b.barcode = substr(m.gtin, 2) OR b.barcode = substr(m.gtin, 3))"));
    return ok ? q.numRowsAffected() : 0;
}

namespace {

Row rowFrom(QSqlQuery &q)
{
    Row r;
    r.id = q.value(0).toInt();
    r.code = q.value(1).toString();
    r.gtin = q.value(2).toString();
    r.serial = q.value(3).toString();
    r.productId = q.value(4).isNull() ? 0 : q.value(4).toInt();
    r.sku = q.value(5).toString();
    r.name = q.value(6).toString();
    r.status = q.value(7).toString();
    r.printCount = q.value(8).toInt();
    r.source = q.value(9).toString();
    r.orderId = q.value(10).toString();
    r.createdAt = q.value(11).toDateTime();
    r.printedAt = q.value(12).toDateTime();
    return r;
}

const char kSelect[] = "SELECT m.id, m.code, m.gtin, m.serial, m.product_id, COALESCE(p.sku, ''), COALESCE(p.name, ''), m.status, m.print_count, "
                       "m.source, m.order_id, m.created_at, m.printed_at FROM marking_codes m LEFT JOIN products p ON p.id = m.product_id ";

} // namespace

QList<Row> list(const Filter &f)
{
    QStringList where;
    QVariantList binds;
    if (!f.status.isEmpty()) {
        where << QStringLiteral("m.status = ?");
        binds << f.status;
    }
    if (f.onlyUnlinked)
        where << QStringLiteral("m.product_id IS NULL");
    if (f.productId > 0) {
        where << QStringLiteral("m.product_id = ?");
        binds << f.productId;
    }
    const QString term = f.text.trimmed();
    if (!term.isEmpty()) {
        where << QStringLiteral("(p.sku ILIKE ? OR p.name ILIKE ? OR m.gtin ILIKE ? OR m.serial ILIKE ? OR m.code ILIKE ?)");
        for (int i = 0; i < 5; ++i)
            binds << likeEscape(term);
    }
    QSqlQuery q;
    q.prepare(QString::fromLatin1(kSelect) + (where.isEmpty() ? QString() : QStringLiteral("WHERE ") + where.join(QStringLiteral(" AND ")))
              + QStringLiteral(" ORDER BY m.id DESC LIMIT %1").arg(qBound(1, f.limit, 100000)));
    for (const QVariant &b : std::as_const(binds))
        q.addBindValue(b);
    QList<Row> out;
    if (q.exec())
        while (q.next())
            out << rowFrom(q);
    return out;
}

Row get(int id)
{
    QSqlQuery q;
    q.prepare(QString::fromLatin1(kSelect) + QStringLiteral("WHERE m.id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next())
        return rowFrom(q);
    return {};
}

Counts counts()
{
    Counts c;
    QSqlQuery q(QStringLiteral("SELECT count(*), count(*) FILTER (WHERE status = 'new'), count(*) FILTER (WHERE status = 'printed'), "
                               "count(*) FILTER (WHERE product_id IS NULL) FROM marking_codes"));
    if (q.next()) {
        c.total = q.value(0).toInt();
        c.fresh = q.value(1).toInt();
        c.printed = q.value(2).toInt();
        c.unlinked = q.value(3).toInt();
    }
    return c;
}

bool markPrinted(const QList<int> &ids, QString *error)
{
    if (ids.isEmpty())
        return true;
    QStringList marks;
    for (int i = 0; i < ids.size(); ++i)
        marks << QStringLiteral("?");
    QSqlQuery q;
    q.prepare(QStringLiteral("UPDATE marking_codes SET status = 'printed', print_count = print_count + 1, printed_at = NOW() WHERE id IN (%1)").arg(marks.join(QLatin1Char(','))));
    for (int id : ids)
        q.addBindValue(id);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

int remove(const QList<int> &ids, bool onlyNew)
{
    if (ids.isEmpty())
        return 0;
    QStringList marks;
    for (int i = 0; i < ids.size(); ++i)
        marks << QStringLiteral("?");
    QSqlQuery q;
    q.prepare(QStringLiteral("DELETE FROM marking_codes WHERE id IN (%1)%2").arg(marks.join(QLatin1Char(',')), onlyNew ? QStringLiteral(" AND status = 'new'") : QString()));
    for (int id : ids)
        q.addBindValue(id);
    return q.exec() ? q.numRowsAffected() : 0;
}

} // namespace MarkingCode
