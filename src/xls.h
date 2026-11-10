#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Запись простого старого Excel-файла (.xls, BIFF8): один лист, один столбец текстовых ячеек (по строке на значение).
// Нужен для загрузки списка кодов в личный кабинет «Честного знака»: образец («шаблон») кабинета - именно .xls, один код в строке.
//
// Файл собран по образцу того, что пишет LibreOffice (книга Excel 97-2003 в контейнере OLE2), без формул, стилей и рисунков:
// все ячейки - текст, поэтому Excel не превращает код вида 0100…21… в число и не теряет ведущие нули.
namespace Xls {

constexpr int kMaxRows = 65536;       // предел формата .xls
constexpr int kMaxSheetName = 31;

// Файл целиком. Пусто и *error - если не получилось (слишком много строк, ячейка длиннее 4000 знаков, недопустимое имя листа).
QByteArray buildColumn(const QString &sheetName, const QStringList &values, QString *error = nullptr);

// Записывает файл на диск. false и причина в *error.
bool writeColumn(const QString &path, const QString &sheetName, const QStringList &values, QString *error = nullptr);

} // namespace Xls
