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
    QColor warn;                                          // предупреждение (янтарный)
    QColor accentSoft, okSoft, dangerSoft, warnSoft;      // мягкие подложки для баннеров и «чипов» статуса
};

Mode saved();
Mode current();
// Роли кнопок: главная (заливка акцентом), опасная (красная) и обычная. Кнопке роль можно задать свойством primary / danger = true;
// остальным «по смыслу подписи» её назначает сама тема («Добавить», «Сохранить», «Создать», «Печать» - главные; «Удалить» - опасная).
// Баннер: QLabel со свойством banner = "ok" | "error" | "warn" | "info" рисуется цветной плашкой (после смены свойства нужен unpolish/polish).
// Применяет тему ко всему приложению (стиль Fusion + палитра + таблица стилей) и запоминает выбор.
void apply(Mode mode);
const Colors &colors();
QString stylesheet(Mode mode);

enum class Icon { Products, Warehouses, Stock, Labels, Import, Market, Cards, Documents, Stocktake, Marking, Update, Orders, Users, Tag, Search, Eye, EyeOff, Close, Sun, Moon, Logout };
// Значок нужного цвета (по умолчанию - цвет боковой панели).
QIcon icon(Icon which, const QColor &color, int size = 22);
// Логотип приложения: скруглённый квадрат с градиентом и коробкой.
QPixmap logo(int size);

} // namespace Theme
