#pragma once

#include <QImage>
#include <QList>
#include <QString>

#include <functional>

class QPrinter;

// Термоэтикетки (по умолчанию 58x40 мм): каждая этикетка - отдельная страница нужного размера. Рисуется сразу в разрешении
// принтера (203/300 dpi), штрихкод - целым числом точек на модуль, поэтому полосы чёткие и сканер читает их без проблем.
namespace ThermalLabel {

struct Settings {
    double widthMm = 58;
    double heightMm = 40;
    bool rotate = false; // повернуть этикетку на 90°: нужно, если драйвер принтера считает бумагу «повёрнутой»
    QString printer;     // имя принтера; пусто - принтер по умолчанию
};

Settings loadSettings();
void saveSettings(const Settings &s);

// Этикетка EAN-13: артикул сверху, краткое описание посередине (мелко, до 3 строк), штрихкод с цифрами снизу.
struct Ean13Label {
    QString article;
    QString description;
    QString code; // EAN-13 или UPC-A (12 цифр)
};

// Этикетка с кодом маркировки «Честного знака» (Data Matrix GS1): слева квадрат с кодом, справа артикул и краткое
// описание, внизу читаемые (01) GTIN и (21) серийный номер.
struct MarkingLabel {
    QString article;
    QString description;
    QString code; // нормализованный КМ с символами GS (см. MarkingCode::parse)
};

// Рисует этикетку в изображение размером widthMm x heightMm при заданном разрешении. monochrome - чёрно-белое
// (для печати: драйверу термопринтера не нужно делать полутона), иначе с сглаживанием (для просмотра).
QImage renderEan13(const Ean13Label &label, const Settings &s, int dpi = 203, bool monochrome = false);

// То же для кода маркировки. Модуль Data Matrix - целое число точек, символ занимает не больше 30 мм.
QImage renderMarking(const MarkingLabel &label, const Settings &s, int dpi = 203, bool monochrome = false);

// Печатает count этикеток: страница i рисуется функцией render(i, dpi). Возвращает false и причину в *error, если принтер
// не принял задание. Если задана переменная окружения INVENTORY_PRINT_PDF, вместо принтера пишется PDF (для проверок).
bool printPages(const Settings &s, int count, const std::function<QImage(int index, int dpi)> &render, QString *error = nullptr,
                const QString &pdfPath = QString());

// Переносит текст по словам в строки шириной не больше maxWidth (в пикселях шрифта fm), не больше maxLines строк;
// последняя строка при нехватке места заканчивается многоточием.
QStringList wrapText(const QString &text, const QFontMetrics &fm, int maxWidth, int maxLines);

} // namespace ThermalLabel
