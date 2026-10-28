#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>

// Кодировщик Data Matrix ECC 200 (ISO/IEC 16022) для кодов маркировки «Честного знака»: GS1 DataMatrix - первым идёт
// символ FNC1, а разделитель групп (байт 0x1D) кодируется тоже как FNC1. Размеры только квадратные (10x10 ... 144x144).
namespace DataMatrix {

struct Symbol {
    int size = 0;               // сторона в модулях, без свободной зоны; 0 - не удалось закодировать
    QVector<bool> modules;      // size*size, построчно, true - тёмный модуль
    bool isNull() const { return size == 0; }
    bool dark(int row, int col) const { return modules[row * size + col]; }
};

// gs1 == true: данные в формате GS1 (FNC1 в начале). Символ с кодом 0x1D в тексте - разделитель групп.
// error (если задан) получает причину, когда данных слишком много.
Symbol encode(const QByteArray &text, bool gs1 = true, QString *error = nullptr);

// Рисует символ: модуль = modulepx пикселей, вокруг свободная зона quietModules модулей (по стандарту не меньше 1).
QImage render(const Symbol &symbol, int modulepx, int quietModules = 1);

} // namespace DataMatrix
