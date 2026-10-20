#pragma once

#include "productdialog.h"

#include <QList>
#include <QString>

// Правка карточки товара из любого места программы: список товаров, остатки, заказы,
// строка сканера. Одна форма (ProductDialog) и одно сохранение: название, цена, артикулы,
// штрихкоды, исправление остатков и мест хранения - все изменения применяются одной
// транзакцией (либо всё, либо ничего); фото - следом, отдельно.
namespace ProductEditor {

struct Changes {
    ProductDialog::ProductData data; // data.barcodes - итоговый список штрихкодов
    QList<ProductDialog::StockEdit> stock;
    QString stockComment;
};

// Проверяет и сохраняет. В *error - понятная причина отказа (занятый артикул/штрихкод и т.п.).
bool save(int productId, const Changes &changes, QString *error);

// Открывает карточку; readOnly - только посмотреть. true - что-то сохранено.
bool edit(int productId, QWidget *parent, bool readOnly = false);

// Читает карточку из базы (без штрихкодов).
bool load(int productId, ProductDialog::ProductData *out);

} // namespace ProductEditor
