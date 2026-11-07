#pragma once

#include <QFont>
#include <QString>
#include <QStringList>

// Шрифты этикеток. Встроен Montserrat (SIL OFL, кириллица; четыре насыщенности) - он близок к фирменному начертанию «Честного знака».
// Свой шрифт (например, фирменный) кладут в папку fonts рядом с программой: файлы .ttf / .otf подхватываются при запуске и появляются в
// списке. Любой установленный в системе шрифт тоже можно выбрать по имени.
namespace LabelFonts {

// Регистрирует встроенные шрифты и шрифты из папки fonts (один раз; дальше ничего не делает).
void ensureLoaded();

// Что предложить в списке «Шрифт»: Montserrat, шрифты из папки fonts, Arial.
QStringList families();
// Шрифт по умолчанию: Montserrat, а если он не загрузился - Arial.
QString defaultFamily();

// Шрифт семейства family нужной насыщенности (400 обычный, 500, 600 полужирный, 700 жирный, 800 сверхжирный) и размера в пикселях.
// Подбирается ближайшее начертание семейства; нет такого семейства - Arial.
QFont font(const QString &family, int weight, int pixelSize);

} // namespace LabelFonts
