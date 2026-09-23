#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

// Минимальная работа с файлами Excel (.xlsx): чтение первого листа как
// таблицы строк и запись простой книги из нескольких листов. Формулы не
// вычисляются - берётся значение, которое Excel сохранил в файле.
namespace Xlsx {

struct Table {
    QVector<QStringList> rows;
    bool ok = false;
    QString error;
};

Table readFirstSheet(const QString &path);

struct Sheet {
    QString name;
    QVector<QStringList> rows;   // первая строка - заголовки (жирным)
    QList<int> numericColumns;   // колонки, которые пишутся числами
    QList<int> columnWidths;     // ширина колонок в символах (необязательно)
};

bool write(const QString &path, const QList<Sheet> &sheets, QString *error = nullptr);

} // namespace Xlsx
