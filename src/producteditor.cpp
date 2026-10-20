#include "producteditor.h"
#include "barcode.h"
#include "database.h"
#include "photostore.h"

#include <QMessageBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ProductEditor {

bool load(int productId, ProductDialog::ProductData *out)
{
    QSqlQuery q;
    q.prepare("SELECT sku, name, COALESCE(description, ''), COALESCE(photo_path, ''), price, "
              "COALESCE(custom_code, ''), COALESCE(market_sku, '') FROM products WHERE id = ?");
    q.addBindValue(productId);
    if (!q.exec() || !q.next())
        return false;
    out->sku = q.value(0).toString();
    out->name = q.value(1).toString();
    out->description = q.value(2).toString();
    out->photoPath = q.value(3).toString();
    out->price = q.value(4).toDouble();
    out->customCode = q.value(5).toString();
    out->marketSku = q.value(6).toString();
    out->barcodes = Barcode::forProduct(productId);
    return true;
}

bool save(int productId, const Changes &c, QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    const auto &d = c.data;
    if (d.sku.trimmed().isEmpty() || d.name.trimmed().isEmpty())
        return fail(QStringLiteral("Артикул и название обязательны."));

    QStringList codes;
    for (const QString &raw : d.barcodes) {
        const QString code = Barcode::normalize(raw);
        QString why;
        if (!Barcode::isValid(code, &why))
            return fail(QStringLiteral("Штрихкод %1: %2").arg(code, why));
        if (!codes.contains(code))
            codes << code;
    }
    QString conflictCode, otherSku;
    if (Barcode::findConflict(productId, codes, &conflictCode, &otherSku))
        return fail(QStringLiteral("Штрихкод %1 уже привязан к товару %2. Один штрихкод может быть только у одного "
                                   "товара.").arg(conflictCode, otherSku));

    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction())
        return fail(db.lastError().text());
    auto rollback = [&](const QString &msg) {
        db.rollback();
        return fail(msg);
    };

    QSqlQuery up;
    up.prepare("UPDATE products SET sku = ?, name = ?, description = ?, price = ?, custom_code = ?, market_sku = ? "
               "WHERE id = ?");
    up.addBindValue(d.sku.trimmed());
    up.addBindValue(d.name.trimmed());
    up.addBindValue(d.description);
    up.addBindValue(d.price);
    up.addBindValue(d.customCode);
    up.addBindValue(d.marketSku);
    up.addBindValue(productId);
    if (!up.exec()) {
        return rollback(up.lastError().nativeErrorCode() == QLatin1String("23505")
                            ? QStringLiteral("Товар с артикулом %1 уже есть.").arg(d.sku.trimmed())
                            : up.lastError().text());
    }
    if (up.numRowsAffected() == 0)
        return rollback(QStringLiteral("Товар уже удалён (возможно, с другого компьютера)."));

    // Штрихкоды: убрать лишние, добавить новые. Если за это время код занял другой товар - откат.
    QSqlQuery existing;
    existing.prepare("SELECT barcode FROM product_barcodes WHERE product_id = ?");
    existing.addBindValue(productId);
    QStringList have;
    if (existing.exec())
        while (existing.next())
            have << existing.value(0).toString();
    for (const QString &h : have) {
        if (codes.contains(h))
            continue;
        QSqlQuery del;
        del.prepare("DELETE FROM product_barcodes WHERE product_id = ? AND barcode = ?");
        del.addBindValue(productId);
        del.addBindValue(h);
        if (!del.exec())
            return rollback(del.lastError().text());
    }
    for (const QString &code : codes) {
        if (have.contains(code))
            continue;
        QSqlQuery ins;
        ins.prepare("INSERT INTO product_barcodes (barcode, product_id) VALUES (?, ?) ON CONFLICT (barcode) DO NOTHING");
        ins.addBindValue(code);
        ins.addBindValue(productId);
        if (!ins.exec())
            return rollback(ins.lastError().text());
        if (ins.numRowsAffected() == 0)
            return rollback(QStringLiteral("Штрихкод %1 только что привязали к другому товару.").arg(code));
    }

    // Остатки: пересчёт (запись в журнал) и места хранения - в той же транзакции.
    const QString comment = c.stockComment.isEmpty() ? QStringLiteral("правка карточки") : c.stockComment;
    for (const auto &e : c.stock) {
        if (e.setCount) {
            QString err;
            if (!Database::inventorySetInTransaction(productId, e.warehouseId, e.counted, comment, &err))
                return rollback(err);
        }
        if (e.setLocation) {
            QString err;
            if (!Database::setStockLocation(productId, e.warehouseId,
                                            e.locationId > 0 ? std::optional<int>(e.locationId) : std::nullopt, &err))
                return rollback(err);
        }
    }
    if (!db.commit()) {
        const QString err = db.lastError().text();
        return rollback(err);
    }
    return true;
}

bool edit(int productId, QWidget *parent, bool readOnly)
{
    ProductDialog::ProductData d;
    if (!load(productId, &d)) {
        QMessageBox::warning(parent, QObject::tr("Товар не найден"),
                             QObject::tr("Этого товара уже нет в базе (возможно, его удалили с другого компьютера)."));
        return false;
    }
    ProductDialog dlg(parent);
    dlg.setProductId(productId);
    dlg.setData(d);
    if (readOnly) {
        dlg.setReadOnly(true);
        dlg.exec();
        return false;
    }
    while (true) {
        if (dlg.exec() != QDialog::Accepted)
            return false;
        Changes c;
        c.data = dlg.data();
        c.stock = dlg.stockEdits();
        c.stockComment = dlg.stockComment();
        QString err;
        if (save(productId, c, &err))
            break;
        // Окно не закрываем и введённое не теряем - человек поправит и сохранит ещё раз.
        QMessageBox::warning(parent, QObject::tr("Не сохранено"), err);
    }
    QString err;
    if (!PhotoStore::apply(productId, dlg.photoChanges(), &err))
        QMessageBox::warning(parent, QObject::tr("Фото не сохранены"), err);
    return true;
}

} // namespace ProductEditor
