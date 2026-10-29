#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

// «Фото из Excel»: файл (xlsx или csv), где у каждого товара есть артикул и ссылки на фото. Подходит выгрузка «все товары» с
// добавленными колонками «Фото 1», «Фото 2»... или любая таблица: колонка артикула (заголовок «Артикул», «Артикул на Маркете»,
// «Штрихкод», «SKU») и одна или несколько колонок со ссылками (заголовок начинается с «Фото» / «Ссылк», содержит photo или image).
// В ячейке может быть несколько ссылок через пробел, перевод строки, «;» или «|». Ссылки, которые не http/https, пропускаются.
namespace PhotoExcel {

struct Parsed {
    bool ok = false;
    QString error;
    QString keyHeader;                 // колонка, по которой искали товар
    QStringList photoHeaders;          // колонки со ссылками
    int rows = 0;                      // строк с непустым ключом
    int linkCount = 0;                 // ссылок в строках, где нашёлся товар
    int withoutLinks = 0;              // строк, где ссылок нет
    int unknownCount = 0;              // ключей, которых нет в базе
    QStringList unknown;               // первые из них (для отчёта)
    QHash<int, QStringList> links;     // товар -> ссылки по порядку (без повторов)
};

// Разбирает таблицу (первая строка - заголовки; заголовки ищутся в первых 10 строках).
Parsed parseRows(const QVector<QStringList> &rows);
// Читает файл .xlsx или .csv и разбирает его.
Parsed parseFile(const QString &path);

// Ссылки из текста ячейки.
QStringList linksIn(const QString &cell);

} // namespace PhotoExcel
