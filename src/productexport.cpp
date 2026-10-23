#include "productexport.h"
#include "productmarket.h"

#include <QDate>
#include <QFileDialog>
#include <QHash>
#include <QMap>
#include <QMessageBox>
#include <QObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <cmath>

namespace ProductExport {

namespace {

// 12990.5, 9000, 250.99 - без хвостовых нулей, точка как разделитель (так число пишется в xlsx).
QString price(double v)
{
    QString s = QString::number(std::round(v * 100.0) / 100.0, 'f', 2);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

// 30.5, 12, 1.25 - без хвостовых нулей
QString number(double v, int decimals)
{
    QString s = QString::number(v, 'f', decimals);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

struct Place {
    qint64 quantity = 0;
    QString location;
};

} // namespace

Xlsx::Sheet build(const QList<int> *onlyIds)
{
    QSet<int> wanted;
    if (onlyIds)
        wanted = QSet<int>(onlyIds->begin(), onlyIds->end());

    // Склады - колонками, в алфавитном порядке
    QList<int> warehouseIds;
    QStringList warehouseNames;
    {
        QSqlQuery q("SELECT id, name FROM warehouses ORDER BY name");
        while (q.next()) {
            warehouseIds << q.value(0).toInt();
            warehouseNames << q.value(1).toString();
        }
    }

    // Остатки и места хранения: product_id -> warehouse_id -> Place
    QHash<int, QHash<int, Place>> stock;
    {
        QSqlQuery q("SELECT s.product_id, s.warehouse_id, s.quantity, l.code "
                    "FROM stock s LEFT JOIN locations l ON l.id = s.location_id");
        while (q.next()) {
            Place p;
            p.quantity = q.value(2).toLongLong();
            p.location = q.value(3).toString();
            stock[q.value(0).toInt()].insert(q.value(1).toInt(), p);
        }
    }

    Xlsx::Sheet sheet;
    sheet.name = QObject::tr("Товары");
    QStringList head = {QObject::tr("Артикул"), QObject::tr("Название"), QObject::tr("Описание"), QObject::tr("Цена, ₽"),
                        QObject::tr("Артикул на Маркете"), QObject::tr("Свой код"), QObject::tr("Штрихкоды"),
                        QObject::tr("Всего на складах")};
    for (const QString &w : std::as_const(warehouseNames))
        head << QObject::tr("Остаток: %1").arg(w);
    head << QObject::tr("Места хранения") << QObject::tr("Фото, шт") << QObject::tr("Добавлен");
    // Параметры для карточки Яндекс Маркета - в конце, чтобы прежние колонки остались на своих местах.
    head << QObject::tr("Бренд") << QObject::tr("Страна производства") << QObject::tr("Длина, см") << QObject::tr("Ширина, см")
         << QObject::tr("Высота, см") << QObject::tr("Вес, кг") << QObject::tr("ТН ВЭД") << QObject::tr("Теги") << QObject::tr("18+")
         << QObject::tr("Категория Маркета (ID)") << QObject::tr("Категория Маркета");
    sheet.rows.append(head);

    const int n = int(warehouseIds.size());
    sheet.numericColumns = {3, 7};
    for (int i = 0; i < n; ++i)
        sheet.numericColumns << 8 + i;
    sheet.numericColumns << 9 + n;
    sheet.numericColumns << 13 + n << 14 + n << 15 + n << 16 + n << 20 + n; // длина, ширина, высота, вес, ID категории
    sheet.columnWidths = {22, 45, 40, 11, 22, 16, 30, 12};
    for (int i = 0; i < n; ++i)
        sheet.columnWidths << 14;
    sheet.columnWidths << 28 << 9 << 12 << 16 << 22 << 10 << 10 << 10 << 9 << 14 << 20 << 6 << 14 << 28;

    QSqlQuery q("SELECT p.id, p.sku, p.name, COALESCE(p.description, ''), p.price::float8, COALESCE(p.market_sku, ''), "
                "COALESCE(p.custom_code, ''), p.created_at, "
                "COALESCE((SELECT string_agg(b.barcode, '; ' ORDER BY b.created_at, b.barcode) "
                "         FROM product_barcodes b WHERE b.product_id = p.id), ''), "
                "(SELECT COUNT(*) FROM product_photos ph WHERE ph.product_id = p.id), " + ProductMarket::selectColumns(QStringLiteral("p.")) +
                " FROM products p ORDER BY p.sku");
    while (q.next()) {
        const int id = q.value(0).toInt();
        if (onlyIds && !wanted.contains(id))
            continue;
        const QHash<int, Place> own = stock.value(id);
        QStringList row = {q.value(1).toString(), q.value(2).toString(), q.value(3).toString(), price(q.value(4).toDouble()),
                           q.value(5).toString(), q.value(6).toString(), q.value(8).toString()};
        qint64 total = 0;
        QStringList perWarehouse, places;
        for (int i = 0; i < n; ++i) {
            const Place p = own.value(warehouseIds.at(i));
            total += p.quantity;
            perWarehouse << QString::number(p.quantity);
            if (!p.location.isEmpty())
                places << QStringLiteral("%1: %2").arg(warehouseNames.at(i), p.location);
        }
        row << QString::number(total) << perWarehouse << places.join(QStringLiteral("; "))
            << QString::number(q.value(9).toInt());
        const QDateTime created = q.value(7).toDateTime();
        row << (created.isValid() ? created.toLocalTime().date().toString(QStringLiteral("yyyy-MM-dd")) : QString());
        const ProductMarketInfo m = ProductMarket::fromRow(q, 10);
        row << m.vendor << m.countries.join(QStringLiteral("; ")) << number(m.lengthCm, 2) << number(m.widthCm, 2) << number(m.heightCm, 2)
            << number(m.weightKg, 3) << m.commodityCode << m.tags.join(QStringLiteral("; ")) << (m.adult ? QObject::tr("да") : QObject::tr("нет"))
            << (m.categoryId > 0 ? QString::number(m.categoryId) : QString()) << m.categoryName;
        sheet.rows.append(row);
    }
    return sheet;
}

bool exportToFile(QWidget *parent, const QList<int> *onlyIds, const QString &note)
{
    QString path = QFileDialog::getSaveFileName(
        parent, QObject::tr("Выгрузить товары в Excel"),
        QObject::tr("Товары_%1.xlsx").arg(QDate::currentDate().toString("yyyy-MM-dd")), QObject::tr("Excel (*.xlsx)"));
    if (path.isEmpty())
        return false;
    if (!path.endsWith(QLatin1String(".xlsx"), Qt::CaseInsensitive))
        path += QLatin1String(".xlsx");

    const Xlsx::Sheet sheet = build(onlyIds);
    QString err;
    if (!Xlsx::write(path, {sheet}, &err)) {
        QMessageBox::warning(parent, QObject::tr("Ошибка"), QObject::tr("Не удалось сохранить файл:\n%1").arg(err));
        return false;
    }
    QString text = QObject::tr("Выгружено товаров: %1").arg(sheet.rows.size() - 1);
    if (!note.isEmpty())
        text += QStringLiteral(" (%1)").arg(note);
    QMessageBox::information(parent, QObject::tr("Готово"),
                             text + QObject::tr(".\n\nВ файле: артикул, название, описание, цена, артикул на Маркете, свой код, "
                                                "штрихкоды, остатки по каждому складу, места хранения, число фото, дата добавления и параметры для карточки "
                                                "Яндекс Маркета (бренд, страна, габариты, вес, ТН ВЭД, теги, 18+, категория)."));
    return true;
}

} // namespace ProductExport
