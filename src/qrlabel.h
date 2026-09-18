#pragma once

#include <QImage>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QList>

// Генерация QR-кодов (libqrencode) и сборка этикеток с подписями
// для обклейки склада/полок/товаров, плюс экспорт листа этикеток в PDF/PNG.
namespace QrLabel {

// Растеризует QR-код заданного текста. moduleSize - размер одного модуля
// в пикселях, marginModules - белая рамка вокруг кода (в модулях).
QImage renderMatrix(const QString &content, int moduleSize = 6, int marginModules = 2);

// Собирает этикетку: QR-код сверху + строки подписи под ним (например
// артикул, название, склад/место), чтобы на складе не путать позиции.
QPixmap renderLabel(const QString &content, const QStringList &captionLines, int moduleSize = 6);

// Сохраняет один лист (сетка columns x N) в PNG. Подходит для небольшого
// количества этикеток - для больших тиражей используйте exportPdf.
bool savePngSheet(const QString &filePath, const QList<QPixmap> &labels, int columns = 3, int spacing = 12);

// Экспортирует этикетки в PDF, разбивая на страницы по columns x rows штук.
bool exportPdf(const QString &filePath, const QList<QPixmap> &labels, int columns = 3, int rows = 4);

} // namespace QrLabel
