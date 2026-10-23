#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

// Оформление приложения в современном стиле: светлая и тёмная темы (цвета, шрифт, скруглённые поля
// и кнопки, тонкие полосы прокрутки), тёмное боковое меню, значки, нарисованные кодом (без файлов),
// и логотип. Тема запоминается на этом компьютере.
namespace Theme {

enum class Mode { Light, Dark };

struct Colors {
    QColor bg, surface, surface2, border, borderStrong, text, muted, accent, accentHover, accentText;
    QColor danger, ok, selection, scroll, sidebar;
};

Mode saved();
Mode current();
// Применяет тему ко всему приложению (стиль Fusion + палитра + таблица стилей) и запоминает выбор.
void apply(Mode mode);
const Colors &colors();
QString stylesheet(Mode mode);

enum class Icon { Products, Warehouses, Stock, Labels, Import, Market, Cards, Documents, Update, Orders, Users, Search, Eye, EyeOff, Close, Sun, Moon, Logout };
// Значок нужного цвета (по умолчанию - цвет боковой панели).
QIcon icon(Icon which, const QColor &color, int size = 22);
// Логотип приложения: скруглённый квадрат с градиентом и коробкой.
QPixmap logo(int size);

} // namespace Theme
