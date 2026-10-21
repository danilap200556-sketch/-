#pragma once

#include "xlsx.h"

#include <QList>
#include <QString>

class QWidget;

// Выгрузка полных данных товаров в Excel: по строке на товар, остатки по складам - отдельными колонками.
// Колонки и их порядок те же, что у выгрузки на сайте (web/src/export.js):
//   Артикул | Название | Описание | Цена, ₽ | Артикул на Маркете | Свой код | Штрихкоды | Всего на складах |
//   Остаток: <склад> ... | Места хранения | Фото, шт | Добавлен
namespace ProductExport {

// onlyIds == nullptr - все товары, иначе только перечисленные (по артикулу).
Xlsx::Sheet build(const QList<int> *onlyIds = nullptr);

// Спрашивает, куда сохранить, пишет книгу и сообщает результат. note - что именно выгружено
// ("найденные по поиску «nike»"), показывается в итоговом сообщении. true - файл записан.
bool exportToFile(QWidget *parent, const QList<int> *onlyIds, const QString &note = QString());

} // namespace ProductExport
