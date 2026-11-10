#include "cabinetxml.h"

#include "xls.h"
#include "xlsx.h"

#include <QDir>
#include <QMap>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QXmlStreamWriter>

namespace CabinetXml {

namespace {

const QChar kGs(0x1D);

bool gtinChecksumOk(const QString &gtin)
{
    int sum = 0;
    for (int i = 0; i < gtin.size() - 1; ++i)
        sum += gtin[gtin.size() - 2 - i].digitValue() * (i % 2 == 0 ? 3 : 1);
    return (10 - sum % 10) % 10 == gtin.back().digitValue();
}

bool innOk(const QString &inn)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{10}|\\d{12})$"));
    return re.match(inn).hasMatch();
}

int templateFor(const OrderOptions &o)
{
    return o.templateId > 0 ? o.templateId : defaultTemplateId(o.group);
}

QString trimmed(const QString &s) { return s.trimmed(); }

} // namespace

QStringList groups() { return {QStringLiteral("shoes"), QStringLiteral("lp")}; }

QString groupTitle(const QString &group)
{
    if (group == QLatin1String("shoes"))
        return QStringLiteral("Обувь (shoes)");
    if (group == QLatin1String("lp"))
        return QStringLiteral("Лёгкая промышленность (lp)");
    return group;
}

QStringList releaseMethods(const QString &group)
{
    // Список из схемы: у обуви нет «REMAINS» (маркировка остатков), у лёгкой промышленности есть.
    if (group == QLatin1String("lp"))
        return {QStringLiteral("PRODUCTION"), QStringLiteral("IMPORT"), QStringLiteral("REMARK"), QStringLiteral("REMAINS"), QStringLiteral("CROSSBORDER"), QStringLiteral("COMMISSION")};
    return {QStringLiteral("PRODUCTION"), QStringLiteral("IMPORT"), QStringLiteral("REMARK"), QStringLiteral("CROSSBORDER"), QStringLiteral("COMMISSION")};
}

QString releaseMethodTitle(const QString &code)
{
    static const QMap<QString, QString> titles = {
        {QStringLiteral("PRODUCTION"), QStringLiteral("производство в РФ")},
        {QStringLiteral("IMPORT"), QStringLiteral("ввоз в РФ (импорт)")},
        {QStringLiteral("REMARK"), QStringLiteral("перемаркировка")},
        {QStringLiteral("REMAINS"), QStringLiteral("маркировка остатков")},
        {QStringLiteral("COMMISSION"), QStringLiteral("принят на комиссию от физического лица")},
    };
    return titles.value(code, QStringLiteral("по схеме кабинета"));
}

QString createMethodTitle(const QString &code)
{
    static const QMap<QString, QString> titles = {
        {QStringLiteral("SELF_MADE"), QStringLiteral("самостоятельно")},
        {QStringLiteral("CEM"), QStringLiteral("ЦЭМ (только через сервис-провайдера)")},
        {QStringLiteral("CM"), QStringLiteral("контрактное производство (только через сервис-провайдера)")},
        {QStringLiteral("CL"), QStringLiteral("логистический склад (только через сервис-провайдера)")},
        {QStringLiteral("CA"), QStringLiteral("комиссионная площадка (только через сервис-провайдера)")},
    };
    return titles.value(code);
}

QStringList createMethods() { return {QStringLiteral("SELF_MADE"), QStringLiteral("CEM"), QStringLiteral("CM"), QStringLiteral("CL"), QStringLiteral("CA")}; }

QList<int> templateIds(const QString &group)
{
    return group == QLatin1String("lp") ? QList<int>{10, 35} : QList<int>{1, 35};
}

int defaultTemplateId(const QString &group) { return group == QLatin1String("lp") ? 10 : 1; }

QString checkOrder(const QList<OrderItem> &items, const OrderOptions &o)
{
    if (!groups().contains(o.group))
        return QStringLiteral("товарная группа «%1» не поддерживается (доступны обувь и лёгкая промышленность)").arg(o.group);
    if (!releaseMethods(o.group).contains(o.releaseMethod))
        return QStringLiteral("способ выпуска в оборот «%1» не подходит для группы %2 (допустимо: %3)").arg(o.releaseMethod, o.group, releaseMethods(o.group).join(QStringLiteral(", ")));
    if (!createMethods().contains(o.createMethod))
        return QStringLiteral("способ изготовления «%1» не предусмотрен схемой (допустимо: %2)").arg(o.createMethod, createMethods().join(QStringLiteral(", ")));
    if (!templateIds(o.group).contains(templateFor(o)))
        return QStringLiteral("шаблон КМ %1 не подходит для группы %2 (допустимо: %3)").arg(templateFor(o)).arg(o.group, [&]() {
            QStringList l;
            for (int t : templateIds(o.group))
                l << QString::number(t);
            return l.join(QStringLiteral(", "));
        }());
    if (o.contactPerson.trimmed().size() > 128)
        return QStringLiteral("контактное лицо длиннее 128 знаков");
    if (o.productionOrderId.trimmed().size() > 256)
        return QStringLiteral("идентификатор производственного заказа длиннее 256 знаков");
    if (!o.producerInn.trimmed().isEmpty() && !innOk(o.producerInn.trimmed()))
        return QStringLiteral("ИНН владельца карточки товара - 10 или 12 цифр");
    if (!o.producerInn.trimmed().isEmpty()) {
        // По документации СУЗ API 3.0: ИНН владельца карточки товара указывается только при перемаркировке (для лёгкой промышленности - ещё при маркировке остатков).
        const bool allowed = o.releaseMethod == QLatin1String("REMARK") || (o.group == QLatin1String("lp") && o.releaseMethod == QLatin1String("REMAINS"));
        if (!allowed)
            return QStringLiteral("ИНН владельца карточки товара указывается только при перемаркировке (REMARK)%1: уберите ИНН или выберите другой способ выпуска").arg(o.group == QLatin1String("lp") ? QStringLiteral(" или маркировке остатков (REMAINS)") : QString());
    }
    if (o.ownSerials && !o.producerInn.trimmed().isEmpty())
        return QStringLiteral("свои серийные номера нельзя заказывать, если указан ИНН владельца карточки товара: уберите ИНН или выберите «серийные номера присваивает система»");
    if (o.group == QLatin1String("lp") && !QStringList({QStringLiteral("UNIT"), QStringLiteral("BUNDLE"), QStringLiteral("SET")}).contains(o.cisType))
        return QStringLiteral("тип КМ «%1» не предусмотрен схемой (UNIT, BUNDLE, SET)").arg(o.cisType);
    if (items.isEmpty())
        return QStringLiteral("в списке нет ни одной позиции");

    QMap<QString, qint64> total;
    for (const OrderItem &it : items) {
        const QString gtin = it.gtin.trimmed();
        static const QRegularExpression re(QStringLiteral("^[0-9]{14}$"));
        if (!re.match(gtin).hasMatch())
            return QStringLiteral("GTIN «%1» - нужно 14 цифр").arg(gtin);
        if (!gtinChecksumOk(gtin))
            return QStringLiteral("GTIN %1: не сходится контрольная цифра (кабинет такой код не примет)").arg(gtin);
        if (it.quantity < 1)
            return QStringLiteral("GTIN %1: количество кодов должно быть не меньше 1").arg(gtin);
        total[gtin] += it.quantity;
        if (total[gtin] > kMaxQuantity)
            return QStringLiteral("GTIN %1: больше %2 кодов на одну позицию в заказе нельзя").arg(gtin).arg(kMaxQuantity);
    }
    return {};
}

QString randomSerial()
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    QString s;
    s.reserve(kSerialLength);
    for (int i = 0; i < kSerialLength; ++i)
        s += QLatin1Char(alphabet[QRandomGenerator::system()->bounded(int(sizeof(alphabet) - 1))]);
    return s;
}

QList<QByteArray> orderFiles(const QList<OrderItem> &items, const OrderOptions &o, QString *error)
{
    const QString problem = checkOrder(items, o);
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return {};
    }
    // Один GTIN - одна позиция заказа.
    QStringList order;
    QMap<QString, int> qty;
    for (const OrderItem &it : items) {
        const QString gtin = it.gtin.trimmed();
        if (!qty.contains(gtin))
            order << gtin;
        qty[gtin] += it.quantity;
    }
    QSet<QString> usedSerials;
    QList<QByteArray> files;
    for (int from = 0; from < order.size(); from += kProductsPerOrderFile) {
        QByteArray out;
        QXmlStreamWriter w(&out);
        w.setAutoFormatting(true);
        w.setAutoFormattingIndent(2);
        w.writeStartDocument(QStringLiteral("1.0"));
        w.writeStartElement(QStringLiteral("order"));
        w.writeDefaultNamespace(QStringLiteral("urn:oms.order"));
        w.writeStartElement(o.group);
        w.writeTextElement(QStringLiteral("productGroup"), o.group);
        if (!o.contactPerson.trimmed().isEmpty())
            w.writeTextElement(QStringLiteral("contactPerson"), o.contactPerson.trimmed());
        w.writeTextElement(QStringLiteral("releaseMethodType"), o.releaseMethod);
        w.writeTextElement(QStringLiteral("createMethodType"), o.createMethod);
        if (!o.productionOrderId.trimmed().isEmpty())
            w.writeTextElement(QStringLiteral("productionOrderId"), o.productionOrderId.trimmed());
        w.writeStartElement(QStringLiteral("products"));
        for (int i = from; i < qMin(from + kProductsPerOrderFile, int(order.size())); ++i) {
            const QString &gtin = order[i];
            w.writeStartElement(QStringLiteral("product"));
            w.writeTextElement(QStringLiteral("gtin"), gtin);
            w.writeTextElement(QStringLiteral("quantity"), QString::number(qty[gtin]));
            w.writeTextElement(QStringLiteral("serialNumberType"), o.ownSerials ? QStringLiteral("SELF_MADE") : QStringLiteral("OPERATOR"));
            if (o.ownSerials) {
                w.writeStartElement(QStringLiteral("serialNumbers"));
                for (int n = 0; n < qty[gtin]; ++n) {
                    QString serial;
                    do
                        serial = randomSerial();
                    while (usedSerials.contains(serial));
                    usedSerials.insert(serial);
                    w.writeTextElement(QStringLiteral("serialNumber"), serial);
                }
                w.writeEndElement(); // serialNumbers
            }
            if (o.group == QLatin1String("lp"))
                w.writeTextElement(QStringLiteral("cisType"), o.cisType);
            w.writeTextElement(QStringLiteral("templateId"), QString::number(templateFor(o)));
            if (!o.producerInn.trimmed().isEmpty())
                w.writeTextElement(QStringLiteral("producer"), o.producerInn.trimmed());
            w.writeEndElement(); // product
        }
        w.writeEndElement(); // products
        w.writeEndElement(); // shoes / lp
        w.writeEndElement(); // order
        w.writeEndDocument();
        files << out;
    }
    return files;
}

QString identificationCode(const QString &code)
{
    QString ki = code.trimmed();
    const int gs = ki.indexOf(kGs);
    if (gs >= 0)
        ki = ki.left(gs);
    static const QRegularExpression re(QStringLiteral("^01[0-9]{14}21[^\\s]+$"));
    return re.match(ki).hasMatch() ? ki : QString();
}

QString checkIntro(const QStringList &codes, const IntroOptions &o)
{
    if (!innOk(o.inn.trimmed()))
        return QStringLiteral("ИНН участника оборота - 10 или 12 цифр");
    if (!o.date.isValid())
        return QStringLiteral("не указана дата ввода в оборот");
    if (codes.isEmpty())
        return QStringLiteral("нет ни одного кода");
    return {};
}

QStringList introCodes(const QStringList &codes, int *skipped)
{
    QStringList kis;
    QSet<QString> seen;
    int bad = 0;
    for (const QString &c : codes) {
        const QString ki = identificationCode(c);
        if (ki.isEmpty()) {
            ++bad;
            continue;
        }
        if (seen.contains(ki))
            continue;
        seen.insert(ki);
        kis << ki;
    }
    if (skipped)
        *skipped = bad;
    return kis;
}

QString introCodeWithBrackets(const QString &ki)
{
    static const QRegularExpression re(QStringLiteral("^01([0-9]{14})21(.+)$"));
    const QRegularExpressionMatch m = re.match(ki);
    return m.hasMatch() ? QStringLiteral("(01)%1(21)%2").arg(m.captured(1), m.captured(2)) : QString();
}

QString introFormatTitle(IntroFormat format)
{
    switch (format) {
    case IntroFormat::Xls: return QStringLiteral("Excel .xls (как в шаблоне кабинета)");
    case IntroFormat::Xlsx: return QStringLiteral("Excel .xlsx");
    case IntroFormat::Xml: return QStringLiteral("XML (vvod_individuals)");
    }
    return {};
}

QString introFormatExtension(IntroFormat format)
{
    switch (format) {
    case IntroFormat::Xls: return QStringLiteral("xls");
    case IntroFormat::Xlsx: return QStringLiteral("xlsx");
    case IntroFormat::Xml: break;
    }
    return QStringLiteral("xml");
}

bool writeIntroTables(const QString &directory, const QString &prefix, const QStringList &codes, IntroFormat format, bool brackets,
                      QStringList *paths, QString *error, int *skipped)
{
    if (paths)
        paths->clear();
    if (skipped)
        *skipped = 0;
    auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (format == IntroFormat::Xml)
        return fail(QStringLiteral("для XML используйте introFiles"));
    int bad = 0;
    QStringList kis = introCodes(codes, &bad);
    if (skipped)
        *skipped = bad;
    if (kis.isEmpty())
        return fail(codes.isEmpty() ? QStringLiteral("нет ни одного кода") : QStringLiteral("ни один из кодов не похож на код GS1 (01…21…)"));
    if (brackets) {
        for (QString &ki : kis)
            ki = introCodeWithBrackets(ki);
    }
    const QString dir = trimmed(directory);
    if (dir.isEmpty())
        return fail(QStringLiteral("не выбрана папка для файлов"));
    if (!QDir().mkpath(dir))
        return fail(QStringLiteral("не удалось создать папку %1").arg(dir));

    const int fileCount = (int(kis.size()) + kCodesPerIntroFile - 1) / kCodesPerIntroFile;
    const QString extension = introFormatExtension(format);
    const QString sheetName = QStringLiteral("Шаблон");
    for (int i = 0; i < fileCount; ++i) {
        const QStringList part = kis.mid(i * kCodesPerIntroFile, kCodesPerIntroFile);
        const QString name = fileCount == 1 ? QStringLiteral("%1.%2").arg(prefix, extension)
                                            : QStringLiteral("%1-%2.%3").arg(prefix).arg(i + 1).arg(extension);
        const QString path = QDir(dir).filePath(name);
        QString problem;
        bool ok = false;
        if (format == IntroFormat::Xls) {
            ok = Xls::writeColumn(path, sheetName, part, &problem);
        } else {
            Xlsx::Sheet sheet;
            sheet.name = sheetName;
            sheet.header = false;
            int longest = 0;
            for (const QString &ki : part) {
                sheet.rows.append(QStringList{ki});
                longest = qMax(longest, int(ki.size()));
            }
            sheet.columnWidths = {qBound(12, longest + 3, 60)};
            ok = Xlsx::write(path, {sheet}, &problem);
        }
        if (!ok)
            return fail(QStringLiteral("не удалось записать файл %1: %2").arg(path, problem));
        if (paths)
            paths->append(path);
    }
    return true;
}

QList<QByteArray> introFiles(const QStringList &codes, const IntroOptions &o, QString *error, int *skipped)
{
    if (skipped)
        *skipped = 0;
    const QString problem = checkIntro(codes, o);
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return {};
    }
    int bad = 0;
    const QStringList kis = introCodes(codes, &bad);
    if (skipped)
        *skipped = bad;
    if (kis.isEmpty()) {
        if (error)
            *error = QStringLiteral("ни один из кодов не похож на код GS1 (01…21…)");
        return {};
    }
    const QString date = o.date.toString(QStringLiteral("dd.MM.yyyy"));
    QList<QByteArray> files;
    for (int from = 0; from < kis.size(); from += kCodesPerIntroFile) {
        QByteArray out;
        QXmlStreamWriter w(&out);
        w.setAutoFormatting(true);
        w.setAutoFormattingIndent(4);
        w.writeStartDocument(QStringLiteral("1.0"));
        w.writeStartElement(QStringLiteral("vvod_individuals"));
        w.writeAttribute(QStringLiteral("action_id"), QStringLiteral("5.3"));
        w.writeAttribute(QStringLiteral("version"), QStringLiteral("3"));
        w.writeTextElement(QStringLiteral("trade_participant_inn"), o.inn.trimmed());
        w.writeTextElement(QStringLiteral("product_receiving_date"), date);
        w.writeStartElement(QStringLiteral("products_list"));
        for (int i = from; i < qMin(from + kCodesPerIntroFile, int(kis.size())); ++i) {
            w.writeStartElement(QStringLiteral("product"));
            w.writeTextElement(QStringLiteral("kit"), kis[i]);
            w.writeTextElement(QStringLiteral("product_receiving_date"), date);
            w.writeEndElement();
        }
        w.writeEndElement(); // products_list
        w.writeEndElement(); // vvod_individuals
        w.writeEndDocument();
        files << out;
    }
    return files;
}

bool writeFiles(const QString &directory, const QString &prefix, const QList<QByteArray> &files, QStringList *paths, QString *error,
                const QString &extension)
{
    if (paths)
        paths->clear();
    const QString dir = trimmed(directory);
    if (dir.isEmpty()) {
        if (error)
            *error = QStringLiteral("не выбрана папка для файлов");
        return false;
    }
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("не удалось создать папку %1").arg(dir);
        return false;
    }
    for (int i = 0; i < files.size(); ++i) {
        const QString name = files.size() == 1 ? QStringLiteral("%1.%2").arg(prefix, extension) : QStringLiteral("%1-%2.%3").arg(prefix).arg(i + 1).arg(extension);
        const QString path = QDir(dir).filePath(name);
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(files[i]) != files[i].size() || !f.commit()) {
            if (error)
                *error = QStringLiteral("не удалось записать файл %1: %2").arg(path, f.errorString());
            return false;
        }
        if (paths)
            paths->append(path);
    }
    return true;
}

} // namespace CabinetXml
