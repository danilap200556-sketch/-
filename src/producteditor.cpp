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
    ProductDialog::Card card;
    if (!ProductDialog::loadCard(productId, &card))
        return false;
    *out = card.data;
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

    // Что добавить и что убрать - по разнице с тем, с чем карточку открыли.
    QStringList have;
    if (c.hasOriginal) {
        for (const QString &raw : c.originalBarcodes)
            have << Barcode::normalize(raw);
    } else {
        have = Barcode::forProduct(productId);
    }
    QStringList toAdd, toRemove;
    for (const QString &code : codes)
        if (!have.contains(code))
            toAdd << code;
    for (const QString &h : have)
        if (!codes.contains(h))
            toRemove << h;
    bool stockWrites = false;
    for (const auto &e : c.stock)
        stockWrites = stockWrites || e.setCount || e.setLocation;

    QSqlQuery up;
    up.prepare(QStringLiteral("UPDATE products SET sku = ?, name = ?, description = ?, price = ?, custom_code = ?, market_sku = ?, %1 "
                              "WHERE id = ?").arg(ProductMarket::updateAssignments()));
    up.addBindValue(d.sku.trimmed());
    up.addBindValue(d.name.trimmed());
    up.addBindValue(d.description);
    up.addBindValue(d.price);
    up.addBindValue(d.customCode);
    up.addBindValue(d.marketSku);
    ProductMarket::bind(up, d.market);
    up.addBindValue(productId);
    const auto updateError = [&]() {
        return up.lastError().nativeErrorCode() == QLatin1String("23505")
                   ? QStringLiteral("Товар с артикулом %1 уже есть.").arg(d.sku.trimmed())
                   : up.lastError().text();
    };

    // Правятся только поля товара: одна команда, она и так выполняется целиком (без BEGIN/COMMIT - два лишних
    // обращения к удалённой базе).
    if (toAdd.isEmpty() && toRemove.isEmpty() && !stockWrites) {
        if (!up.exec())
            return fail(updateError());
        if (up.numRowsAffected() == 0)
            return fail(QStringLiteral("Товар уже удалён (возможно, с другого компьютера)."));
        return true;
    }

    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction())
        return fail(db.lastError().text());
    auto rollback = [&](const QString &msg) {
        db.rollback();
        return fail(msg);
    };
    if (!up.exec())
        return rollback(updateError());
    if (up.numRowsAffected() == 0)
        return rollback(QStringLiteral("Товар уже удалён (возможно, с другого компьютера)."));

    // Штрихкоды: убрать лишние, добавить новые. Если код успели занять другим товаром - откат.
    if (!toRemove.isEmpty()) {
        QSqlQuery del;
        del.prepare(QStringLiteral("DELETE FROM product_barcodes WHERE product_id = ? AND barcode IN (%1)")
                        .arg(QStringList(toRemove.size(), QStringLiteral("?")).join(QLatin1Char(','))));
        del.addBindValue(productId);
        for (const QString &h : toRemove)
            del.addBindValue(h);
        if (!del.exec())
            return rollback(del.lastError().text());
    }
    if (!toAdd.isEmpty()) {
        QSqlQuery ins;
        ins.prepare(QStringLiteral("INSERT INTO product_barcodes (barcode, product_id) VALUES %1 "
                                   "ON CONFLICT (barcode) DO NOTHING")
                        .arg(QStringList(toAdd.size(), QStringLiteral("(?, ?)")).join(QLatin1Char(','))));
        for (const QString &code : toAdd) {
            ins.addBindValue(code);
            ins.addBindValue(productId);
        }
        if (!ins.exec())
            return rollback(ins.lastError().text());
        if (ins.numRowsAffected() < toAdd.size()) {
            db.rollback();
            QString conflictCode, otherSku;
            if (Barcode::findConflict(productId, toAdd, &conflictCode, &otherSku))
                return fail(QStringLiteral("Штрихкод %1 уже привязан к товару %2. Один штрихкод может быть только у "
                                           "одного товара.").arg(conflictCode, otherSku));
            return fail(QStringLiteral("Штрихкод только что привязали к другому товару."));
        }
    }

    // Остатки: пересчёт (запись в журнал) и места хранения - в той же транзакции.
    const QString comment = c.stockComment.isEmpty() ? QStringLiteral("правка карточки") : c.stockComment;
    Database::Document doc(QStringLiteral("inventory"), comment); // все пересчёты карточки - один документ: отменяются разом
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
    ProductDialog::Card card;
    if (!ProductDialog::loadCard(productId, &card)) {
        QMessageBox::warning(parent, QObject::tr("Товар не найден"),
                             QObject::tr("Этого товара уже нет в базе (возможно, его удалили с другого компьютера)."));
        return false;
    }
    ProductDialog dlg(parent);
    dlg.setCard(productId, card);
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
        c.hasOriginal = true;
        c.originalBarcodes = dlg.originalBarcodes();
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
