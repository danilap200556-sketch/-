#pragma once

#include <QImage>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QStringList>

// Штрихкоды товаров (EAN-13 с коробок и т.п.): проверка, генерация своих
// кодов и отрисовка EAN-13 для печати на этикетках.
namespace Barcode {

// Только цифры, длина 8/12/13/14 (EAN-8, UPC-A, EAN-13, GTIN-14) и верная
// контрольная цифра. В *error - понятная причина, если код не подходит.
bool isValid(const QString &code, QString *error = nullptr);

// Приводит к виду, в котором код хранится в базе: без пробелов/дефисов.
QString normalize(const QString &input);

// EAN-13 можно нарисовать для кодов EAN-13 и UPC-A (UPC-A = EAN-13 с ведущим 0).
bool canRenderEan13(const QString &code);

// Новый EAN-13 из диапазона 20..29 (для внутреннего использования - не
// пересекается с кодами производителей), ещё не привязанный ни к одному товару. Номера берутся из последовательности
// в базе (next_internal_ean13): коды не повторяются и не кончаются.
QString generateInternalEan13();
// Присваивает свой EAN-13 товарам из списка, у которых нет ни одного штрихкода. Возвращает, скольким присвоено
// (-1 - ошибка, текст в *error). Новые товары получают штрихкод автоматически - так их сразу можно печатать на этикетках.
int ensureOwn(const QList<int> &productIds, QString *error = nullptr);
// То же для всех товаров без штрихкода.
int ensureOwnForAll(QString *error = nullptr);
int countWithoutBarcode();

QImage renderEan13(const QString &code, int moduleWidth = 3);
QPixmap renderLabel(const QString &code, const QStringList &captionLines);

// --- Хранение в базе (таблица product_barcodes) ---
QStringList forProduct(int productId);
int productIdFor(const QString &code); // -1 - не найден
// Код, который уже привязан к другому товару (из списка codes), и артикул того товара.
bool findConflict(int productId, const QStringList &codes, QString *code, QString *otherSku);
bool setForProduct(int productId, const QStringList &codes, QString *error = nullptr);
bool attach(int productId, const QString &code, QString *error = nullptr);

} // namespace Barcode
