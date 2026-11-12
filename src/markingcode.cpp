#include "markingcode.h"

#include "csvimport.h"

#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace MarkingCode {

namespace {

constexpr int kChunk = 10000;  // кодов в одном запросе (массивами: запрос один, размер - около мегабайта)
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

// Знаки, с которых начинаются видимые заменители разделителя GS («<GS>», «\\x1d», «^]», «␝»...). Нет ни одного - заменять нечего.
bool mayHavePlaceholder(const QString &t)
{
    for (const QChar c : t) {
        const ushort u = c.unicode();
        if (u == '<' || u == '{' || u == '[' || u == '\\' || u == '^' || u == 0x241D)
            return true;
    }
    return false;
}

bool printableWithoutGs(const QString &s)
{
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (u != 0x1D && (u < 0x20 || u > 0x7E))
            return false;
    }
    return true;
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

// Массивы PostgreSQL одним параметром: так запрос не зависит от числа элементов (нет предела в 65535 параметров, один круг до базы).
QString pgTextArray(const QStringList &v)
{
    QString out;
    out.reserve(int(v.size()) * 24 + 2);
    out += QLatin1Char('{');
    for (int i = 0; i < v.size(); ++i) {
        if (i)
            out += QLatin1Char(',');
        out += QLatin1Char('"');
        for (QChar c : v[i]) {
            if (c == QLatin1Char('"') || c == QLatin1Char('\\'))
                out += QLatin1Char('\\');
            out += c;
        }
        out += QLatin1Char('"');
    }
    out += QLatin1Char('}');
    return out;
}

QString pgIntArray(const QList<int> &v)
{
    QString out;
    out.reserve(int(v.size()) * 8 + 2);
    out += QLatin1Char('{');
    for (int i = 0; i < v.size(); ++i) {
        if (i)
            out += QLatin1Char(',');
        out += QString::number(v[i]);
    }
    out += QLatin1Char('}');
    return out;
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
    // Видимые заменители разделителя, которые оставляют программы и некоторые сканеры (у обычного кода их нет - тогда и искать нечего).
    if (mayHavePlaceholder(t)) {
        for (const QString &ph : {QStringLiteral("<GS>"), QStringLiteral("{GS}"), QStringLiteral("[GS]"), QStringLiteral("\\x1d"), QStringLiteral("\\u001d"),
                                  QStringLiteral("^]"), QStringLiteral("␝"), QStringLiteral("<FNC1>")})
            t.replace(ph, QString(kGs), Qt::CaseInsensitive);
    }

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
    if (!printableWithoutGs(t)) {
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
    const int firstGs = t.indexOf(kGs);
    if (firstGs > 18 && !t.contains(QString(2, kGs))) {
        // обычный код из базы или от сканера: группы уже разделены одним GS
        serial = t.mid(18, firstGs - 18);
        code = t;
    } else if (firstGs >= 0) {
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
    // Код из базы уже разобран и нормализован: для таблицы (сотни строк при каждой отрисовке) не разбираем его заново.
    if (code.size() >= 19 && code.startsWith(QLatin1String("01")) && code.at(16) == QLatin1Char('2') && code.at(17) == QLatin1Char('1')
        && !code.startsWith(QLatin1Char('(')) && !mayHavePlaceholder(code)) {
        bool digits = true;
        for (int i = 2; i < 16 && digits; ++i)
            digits = code.at(i).unicode() >= '0' && code.at(i).unicode() <= '9';
        const int gs = code.indexOf(kGs);
        if (digits && (gs < 0 || gs > 18)) {
            const QStringView serial = QStringView(code).mid(18, gs < 0 ? -1 : gs - 18);
            return QStringLiteral("(01)%1(21)%2%3").arg(QStringView(code).mid(2, 14), serial, gs >= 0 ? QStringLiteral(" …") : QString());
        }
    }
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
    // GTIN-14, а если он начинается с нуля - то же без одного нуля (EAN-13) и без двух (UPC-A): всё одним запросом, самый длинный штрихкод первым.
    const QVariant none = QVariant(QMetaType::fromType<QString>());   // NULL нужного типа (запись без «=» MSVC принимает за объявление функции)
    const bool shortened = gtin.size() == 14 && gtin.startsWith(QLatin1Char('0'));
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT product_id FROM product_barcodes WHERE barcode = ANY(ARRAY[?::text, ?::text, ?::text]) ORDER BY length(barcode) DESC LIMIT 1"));
    q.addBindValue(gtin);
    q.addBindValue(shortened ? QVariant(gtin.mid(1)) : none);
    q.addBindValue(shortened && gtin.startsWith(QLatin1String("00")) ? QVariant(gtin.mid(2)) : none);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
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

AddResult add(const QStringList &rawCodes, const QString &source, const QString &orderId, int forceProductId, const QString &status)
{
    AddResult res;
    struct Item {
        Parsed p;
        int productId = 0;   // 0 - искать по GTIN в самой базе
    };
    QList<Item> items;
    QSet<QString> seen;
    QHash<QString, int> forcedByGtin;   // только при заданном товаре: GTIN -> товар (-1: GTIN чужого товара)
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
        Item it;
        if (forceProductId > 0) {
            // Свой товар задан явно: его штрихкод становится GTIN кодов, если GTIN свободен или уже его.
            auto f = forcedByGtin.constFind(p.gtin);
            if (f == forcedByGtin.constEnd()) {
                if (linkGtin(p.gtin, forceProductId, &linkError)) {
                    f = forcedByGtin.insert(p.gtin, forceProductId);
                } else {
                    f = forcedByGtin.insert(p.gtin, -1); // GTIN чужого товара: такие коды не берём - этикетка вышла бы с чужим артикулом
                    if (res.problems.size() < kMaxProblems)
                        res.problems << linkError;
                }
            }
            if (f.value() < 0) {
                ++res.invalid;
                continue;
            }
            it.productId = f.value();
        }
        it.p = p;
        items << it;
    }

    // Товар по GTIN находит сама база прямо при вставке (штрихкод товара = GTIN, EAN-13 с нулём впереди или UPC-A): один запрос на пачку кодов,
    // а не по запросу на каждый GTIN и на каждую пачку по 400 кодов, как было раньше. Из ответа видно, какие коды вставились и с каким товаром.
    const QString sourceValue = nn(source), orderValue = nn(orderId), statusValue = status == QLatin1String("printed") ? QStringLiteral("printed") : QStringLiteral("new");
    int added = 0, failed = 0;
    QSet<QString> unknown;
    for (int start = 0; start < items.size(); start += kChunk) {
        const int end = qMin(int(items.size()), start + kChunk);
        QStringList codes;
        codes.reserve(end - start);
        for (int i = start; i < end; ++i)
            codes << items[i].p.code;
        // Передаём только сами коды: GTIN (14 знаков после «01») и серийный номер (до первого GS после «21») база берёт из кода сама - так по сети
        // уходит на четверть меньше. Товар у всех кодов один, если он задан явно, иначе его ищет база по GTIN.
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "INSERT INTO marking_codes (code, gtin, serial, product_id, source, order_id, status) "
            "SELECT u.code, g.gtin, split_part(substr(u.code, 19), chr(29), 1), COALESCE(?::int, (SELECT b.product_id FROM product_barcodes b WHERE b.barcode = ANY(ARRAY["
            "g.gtin, CASE WHEN g.gtin LIKE '0%' THEN substr(g.gtin, 2) END, CASE WHEN g.gtin LIKE '00%' THEN substr(g.gtin, 3) END]) "
            "ORDER BY length(b.barcode) DESC LIMIT 1)), ?::text, ?::text, ?::text "
            "FROM unnest(?::text[]) AS u(code) CROSS JOIN LATERAL (SELECT substr(u.code, 3, 14) AS gtin) g "
            "ON CONFLICT (code) DO NOTHING RETURNING gtin, product_id"));
        q.addBindValue(forceProductId > 0 ? QVariant(forceProductId) : nullInt());
        q.addBindValue(sourceValue);
        q.addBindValue(orderValue);
        q.addBindValue(statusValue);
        q.addBindValue(pgTextArray(codes));
        if (!q.exec()) {
            if (res.problems.size() < kMaxProblems)
                res.problems << QStringLiteral("не удалось записать: %1").arg(q.lastError().text());
            res.invalid += end - start;
            failed += end - start;
            continue;
        }
        while (q.next()) {
            ++added;
            if (q.value(1).isNull()) {
                ++res.unlinked;
                unknown.insert(q.value(0).toString());
            } else {
                ++res.linked;
            }
        }
    }
    res.added = added;
    res.duplicates += int(items.size()) - added - failed;   // не вставились из-за ON CONFLICT: такой код уже есть в базе
    res.unknownGtins = QStringList(unknown.begin(), unknown.end());
    res.unknownGtins.sort();
    return res;
}

bool hasCryptoTail(const QString &code)
{
    // Без выделения памяти (для каждой строки таблицы): после первой группы ищем группы 91, 92 или 93 - в начале каждой после GS.
    int first = 0;
    while (first < code.size() && code.at(first) == kGs)
        ++first;
    for (int gs = code.indexOf(kGs, first); gs >= 0 && gs + 2 < code.size(); gs = code.indexOf(kGs, gs + 1)) {
        if (code.at(gs + 1) == QLatin1Char('9')) {
            const QChar b = code.at(gs + 2);
            if (b == QLatin1Char('1') || b == QLatin1Char('2') || b == QLatin1Char('3'))
                return true;
        }
    }
    return false;
}

bool readRegistryCsv(const QString &path, QList<RegistryRow> *rows, QString *error)
{
    rows->clear();
    if (!path.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive))
        return false;
    const CsvImport::Table t = CsvImport::readFile(path);
    if (!t.ok) {
        if (error)
            *error = t.error;
        return false;
    }
    // Заголовок - строка, где есть «Код» и «GTIN» (перед ним может быть строка «Фильтр(...)»).
    int head = -1;
    QHash<QString, int> col;
    for (int r = 0; r < qMin(int(t.rows.size()), 6) && head < 0; ++r) {
        QHash<QString, int> c;
        for (int i = 0; i < t.rows[r].size(); ++i)
            c.insert(t.rows[r][i].trimmed(), i);
        if (c.contains(QStringLiteral("Код")) && c.contains(QStringLiteral("GTIN"))) {
            head = r;
            col = c;
        }
    }
    if (head < 0)
        return false;
    auto cell = [&](const QStringList &row, const char *name) {
        const int i = col.value(QString::fromUtf8(name), -1);
        return i >= 0 && i < row.size() ? row[i].trimmed() : QString();
    };
    for (int r = head + 1; r < t.rows.size(); ++r) {
        const QStringList &row = t.rows[r];
        RegistryRow x;
        x.code = cell(row, "Код");
        if (x.code.isEmpty())
            continue;
        x.gtin = cell(row, "GTIN");
        x.name = cell(row, "Наименование товара");
        x.brand = cell(row, "Бренд");
        x.status = cell(row, "Статус кода").toUpper();
        x.method = cell(row, "Способ ввода в оборот");
        x.orderId = cell(row, "Номер заказа в СУЗ");
        x.owner = cell(row, "Владелец");
        x.appliedAt = cell(row, "Дата нанесения");
        x.emittedAt = cell(row, "Дата эмиссии");
        rows->append(x);
    }
    return true;
}

AddResult addRegistry(const QList<RegistryRow> &rows)
{
    // Коды одного заказа и статуса - одной группой (так у кода остаётся номер заказа).
    QMap<QString, QStringList> groups; // «статус|заказ» -> коды
    for (const RegistryRow &r : rows)
        groups[(r.status == QLatin1String("EMITTED") ? QStringLiteral("new") : QStringLiteral("printed")) + QLatin1Char('|') + r.orderId] << r.code;
    AddResult total;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const QStringList key = it.key().split(QLatin1Char('|'));
        const AddResult part = add(it.value(), QStringLiteral("registry"), key.value(1), 0, key.value(0));
        total.added += part.added;
        total.duplicates += part.duplicates;
        total.invalid += part.invalid;
        total.linked += part.linked;
        total.unlinked += part.unlinked;
        for (const QString &g : part.unknownGtins)
            if (!total.unknownGtins.contains(g))
                total.unknownGtins << g;
        for (const QString &pr : part.problems)
            if (total.problems.size() < kMaxProblems)
                total.problems << pr;
    }
    total.unknownGtins.sort();
    return total;
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

namespace {

// Условия списка. Поиск: артикул, название, GTIN и серийный номер; целиком вставленный код ищется точно (по подстроке в длинных кодах база
// перебирала бы весь текст каждой строки).
void listWhere(const Filter &f, QStringList *where, QVariantList *binds)
{
    if (!f.status.isEmpty()) {
        *where << QStringLiteral("m.status = ?");
        *binds << f.status;
    }
    if (f.onlyUnlinked)
        *where << QStringLiteral("m.product_id IS NULL");
    if (f.productId > 0) {
        *where << QStringLiteral("m.product_id = ?");
        *binds << f.productId;
    }
    const QString term = f.text.trimmed();
    if (!term.isEmpty()) {
        const Parsed whole = term.size() >= 20 ? parse(term) : Parsed();
        if (whole.ok) {
            *where << QStringLiteral("(m.code = ? OR (m.gtin = ? AND m.serial = ?))");
            *binds << whole.code << whole.gtin << whole.serial;
        } else {
            *where << QStringLiteral("(p.sku ILIKE ? OR p.name ILIKE ? OR m.gtin ILIKE ? OR m.serial ILIKE ?)");
            for (int i = 0; i < 4; ++i)
                *binds << likeEscape(term);
        }
    }
}

const char kCountsJoin[] = "CROSS JOIN (SELECT count(*) AS total, count(*) FILTER (WHERE status = 'new') AS fresh, count(*) FILTER (WHERE status = 'printed') AS printed, "
                           "count(*) FILTER (WHERE product_id IS NULL) AS unlinked FROM marking_codes) c ";

} // namespace

QList<Row> list(const Filter &f)
{
    QStringList where;
    QVariantList binds;
    listWhere(f, &where, &binds);
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

QList<Row> listWithCounts(const Filter &f, Counts *counts)
{
    QStringList where;
    QVariantList binds;
    listWhere(f, &where, &binds);
    // Список и счётчики одним запросом: на удалённой базе каждый запрос - это ожидание ответа (десятки миллисекунд).
    QString sql = QString::fromLatin1(kSelect);
    sql.replace(QLatin1String("FROM marking_codes m LEFT JOIN products p ON p.id = m.product_id "),
                QLatin1String("FROM marking_codes m LEFT JOIN products p ON p.id = m.product_id ") + QString::fromLatin1(kCountsJoin));
    sql.replace(QLatin1String("m.source, m.order_id, m.created_at, m.printed_at FROM"), QLatin1String("m.source, m.order_id, m.created_at, m.printed_at, c.total, c.fresh, c.printed, c.unlinked FROM"));
    QSqlQuery q;
    q.prepare(sql + (where.isEmpty() ? QString() : QStringLiteral("WHERE ") + where.join(QStringLiteral(" AND ")))
              + QStringLiteral(" ORDER BY m.id DESC LIMIT %1").arg(qBound(1, f.limit, 100000)));
    for (const QVariant &b : std::as_const(binds))
        q.addBindValue(b);
    QList<Row> out;
    bool haveCounts = false;
    if (q.exec()) {
        while (q.next()) {
            out << rowFrom(q);
            if (!haveCounts && counts) {
                counts->total = q.value(13).toInt();
                counts->fresh = q.value(14).toInt();
                counts->printed = q.value(15).toInt();
                counts->unlinked = q.value(16).toInt();
                haveCounts = true;
            }
        }
    }
    if (counts && !haveCounts)   // ничего не нашлось - счётчиков в ответе нет
        *counts = MarkingCode::counts();
    return out;
}

QList<Row> getMany(const QList<int> &ids)
{
    QList<Row> out;
    if (ids.isEmpty())
        return out;
    QSqlQuery q;
    q.prepare(QString::fromLatin1(kSelect) + QStringLiteral("WHERE m.id = ANY(?::int[])"));
    q.addBindValue(pgIntArray(ids));
    QHash<int, Row> byId;
    if (q.exec())
        while (q.next()) {
            const Row r = rowFrom(q);
            byId.insert(r.id, r);
        }
    out.reserve(ids.size());
    for (int id : ids) {
        const auto it = byId.constFind(id);
        if (it != byId.constEnd())
            out << it.value();
    }
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

int countForOrder(const QString &orderId, const QString &gtin)
{
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT count(*) FROM marking_codes WHERE order_id = ? AND gtin = ? AND source <> 'registry'"));
    q.addBindValue(orderId);
    q.addBindValue(gtin);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

QStringList codesByIds(const QList<int> &ids)
{
    QStringList out;
    if (ids.isEmpty())
        return out;
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT code FROM marking_codes WHERE id = ANY(?::int[]) ORDER BY id"));
    q.addBindValue(pgIntArray(ids));
    if (!q.exec())
        return out;
    out.reserve(ids.size());
    while (q.next())
        out << q.value(0).toString();
    return out;
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
    QSqlQuery q;
    q.prepare(QStringLiteral("UPDATE marking_codes SET status = 'printed', print_count = print_count + 1, printed_at = NOW() WHERE id = ANY(?::int[])"));
    q.addBindValue(pgIntArray(ids));
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
    QSqlQuery q;
    q.prepare(QStringLiteral("DELETE FROM marking_codes WHERE id = ANY(?::int[])%1").arg(onlyNew ? QStringLiteral(" AND status = 'new'") : QString()));
    q.addBindValue(pgIntArray(ids));
    return q.exec() ? q.numRowsAffected() : 0;
}

} // namespace MarkingCode
