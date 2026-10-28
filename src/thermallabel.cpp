#include "thermallabel.h"

#include "barcode.h"

#include <QFont>
#include <QFontMetrics>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPrinter>
#include <QPrinterInfo>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace ThermalLabel {

namespace {

int px(double mm, int dpi) { return int(std::lround(mm * dpi / 25.4)); }

QFont labelFont(int pixelSize, bool bold)
{
    QFont f(QStringLiteral("Arial"));
    f.setPixelSize(qMax(6, pixelSize));
    f.setBold(bold);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

} // namespace

Settings loadSettings()
{
    QSettings q;
    Settings s;
    s.widthMm = qBound(20.0, q.value(QStringLiteral("thermal/widthMm"), 58.0).toDouble(), 120.0);
    s.heightMm = qBound(15.0, q.value(QStringLiteral("thermal/heightMm"), 40.0).toDouble(), 120.0);
    s.rotate = q.value(QStringLiteral("thermal/rotate"), false).toBool();
    s.printer = q.value(QStringLiteral("thermal/printer")).toString();
    return s;
}

void saveSettings(const Settings &s)
{
    QSettings q;
    q.setValue(QStringLiteral("thermal/widthMm"), s.widthMm);
    q.setValue(QStringLiteral("thermal/heightMm"), s.heightMm);
    q.setValue(QStringLiteral("thermal/rotate"), s.rotate);
    q.setValue(QStringLiteral("thermal/printer"), s.printer);
}

QStringList wrapText(const QString &text, const QFontMetrics &fm, int maxWidth, int maxLines)
{
    QStringList lines;
    const QStringList words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString cur;
    for (int i = 0; i < words.size(); ++i) {
        QString word = words[i];
        // слово длиннее строки режем по символам
        while (fm.horizontalAdvance(word) > maxWidth && word.size() > 1) {
            int cut = word.size() - 1;
            while (cut > 1 && fm.horizontalAdvance(word.left(cut)) > maxWidth)
                --cut;
            if (!cur.isEmpty()) {
                lines << cur;
                cur.clear();
            }
            lines << word.left(cut);
            word = word.mid(cut);
        }
        const QString candidate = cur.isEmpty() ? word : cur + QLatin1Char(' ') + word;
        if (fm.horizontalAdvance(candidate) <= maxWidth) {
            cur = candidate;
        } else {
            lines << cur;
            cur = word;
        }
    }
    if (!cur.isEmpty())
        lines << cur;
    if (lines.size() > maxLines) {
        lines = lines.mid(0, maxLines);
        lines.last() = fm.elidedText(lines.last() + QStringLiteral(" …"), Qt::ElideRight, maxWidth);
    }
    return lines;
}

QImage renderEan13(const Ean13Label &label, const Settings &s, int dpi, bool monochrome)
{
    const int W = px(s.widthMm, dpi), H = px(s.heightMm, dpi);
    QImage img(W, H, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing, !monochrome);
    p.setPen(Qt::black);

    const int margin = px(1.6, dpi);
    const int inner = W - 2 * margin;

    // --- Штрихкод внизу. Модуль - целое число точек (около 0,38 мм; если не помещается - меньше).
    const QString modules = Barcode::ean13Modules(label.code);
    const QString code = label.code.size() == 12 ? QStringLiteral("0") + label.code : label.code;
    constexpr int kQuietLeft = 11, kQuietRight = 7, kTotal = 95 + kQuietLeft + kQuietRight;
    int module = qMax(1, int(std::floor(0.38 * dpi / 25.4)));
    while (module > 1 && kTotal * module > inner)
        --module;
    const int digitsH = px(2.8, dpi);
    const int guard = px(1.1, dpi);
    int barH = px(11.5, dpi);
    // Штрихкод не должен занимать больше половины высоты, чтобы остались артикул и описание.
    barH = qMin(barH, int((H - 2 * margin) * 0.46) - guard - digitsH);
    barH = qMax(barH, px(6, dpi));
    const int blockH = barH + guard + digitsH;
    const int barTop = H - margin - blockH;

    if (!modules.isEmpty()) {
        const int barsW = kTotal * module;
        const int x0 = (W - barsW) / 2 + kQuietLeft * module;
        for (int i = 0; i < modules.size(); ++i) {
            if (modules[i] != QLatin1Char('1'))
                continue;
            const bool longBar = i < 3 || (i >= 45 && i < 50) || i >= 92;
            p.fillRect(x0 + i * module, barTop, module, barH + (longBar ? guard : 0), Qt::black);
        }
        // цифры: первая слева от полос, по шесть под каждой половиной
        p.setFont(labelFont(int(digitsH * 0.95), false));
        const int textTop = barTop + barH;
        const int textH = guard + digitsH;
        p.drawText(QRect(x0 - kQuietLeft * module, textTop, kQuietLeft * module - module, textH), Qt::AlignRight | Qt::AlignVCenter, code.left(1));
        p.drawText(QRect(x0 + 3 * module, textTop, 42 * module, textH), Qt::AlignCenter, code.mid(1, 6));
        p.drawText(QRect(x0 + 50 * module, textTop, 42 * module, textH), Qt::AlignCenter, code.mid(7, 6));
    } else {
        p.setFont(labelFont(px(3.2, dpi), false));
        p.drawText(QRect(margin, barTop, inner, blockH), Qt::AlignCenter | Qt::TextWordWrap, QObject::tr("нет штрихкода EAN-13"));
    }

    // --- Артикул сверху: жирно, одной строкой; длинный сжимается, но не мельче ~3 мм.
    int articlePx = px(5.6, dpi);
    const QString article = label.article.simplified();
    QFont af = labelFont(articlePx, true);
    while (articlePx > px(3.0, dpi) && QFontMetrics(af).horizontalAdvance(article) > inner) {
        articlePx -= qMax(1, px(0.2, dpi));
        af = labelFont(articlePx, true);
    }
    p.setFont(af);
    const QFontMetrics afm(af);
    const int articleH = afm.height();
    p.drawText(QRect(margin, margin, inner, articleH), Qt::AlignHCenter | Qt::AlignVCenter,
               afm.elidedText(article, Qt::ElideRight, inner));

    // --- Описание посередине: мелко, по центру, до трёх строк в оставшемся месте.
    const int descTop = margin + articleH + px(1.2, dpi);
    const int descBottom = barTop - px(1.2, dpi);
    if (descBottom > descTop && !label.description.trimmed().isEmpty()) {
        QFont df = labelFont(px(2.7, dpi), false);
        QFontMetrics dfm(df);
        const int lineH = dfm.lineSpacing();
        const int maxLines = qBound(1, (descBottom - descTop) / qMax(1, lineH), 3);
        const QStringList lines = wrapText(label.description, dfm, inner, maxLines);
        p.setFont(df);
        const int blockTop = descTop + ((descBottom - descTop) - int(lines.size()) * lineH) / 2; // по центру свободного места
        for (int i = 0; i < lines.size(); ++i)
            p.drawText(QRect(margin, blockTop + i * lineH, inner, lineH), Qt::AlignHCenter | Qt::AlignVCenter, lines[i]);
    }
    p.end();
    return monochrome ? img.convertToFormat(QImage::Format_Mono, Qt::ThresholdDither | Qt::MonoOnly) : img;
}

bool printPages(const Settings &s, int count, const std::function<QImage(int, int)> &render, QString *error, const QString &pdfPath)
{
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (count <= 0)
        return fail(QObject::tr("Нечего печатать"));
    const QString pdf = !pdfPath.isEmpty() ? pdfPath : qEnvironmentVariable("INVENTORY_PRINT_PDF");
    QPrinter printer(pdf.isEmpty() ? QPrinter::HighResolution : QPrinter::ScreenResolution);
    if (!pdf.isEmpty()) {
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(pdf);
        printer.setResolution(300);
    } else {
        QString name = s.printer;
        if (name.isEmpty())
            name = QPrinterInfo::defaultPrinterName();
        if (name.isEmpty())
            return fail(QObject::tr("В системе не найден принтер. Установите драйвер термопринтера или сохраните этикетки в PDF."));
        if (!QPrinterInfo::availablePrinterNames().contains(name))
            return fail(QObject::tr("Принтер «%1» не найден в системе.").arg(name));
        printer.setPrinterName(name);
    }
    // Бумага: ширина x длина этикетки. «Повернуть на 90°» меняет их местами и поворачивает рисунок.
    const double pageW = s.rotate ? s.heightMm : s.widthMm;
    const double pageH = s.rotate ? s.widthMm : s.heightMm;
    printer.setPageLayout(QPageLayout(QPageSize(QSizeF(pageW, pageH), QPageSize::Millimeter, QStringLiteral("Label")),
                                      QPageLayout::Portrait, QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter));
    printer.setFullPage(true);
    const int dpi = qMax(96, printer.resolution());

    QPainter painter;
    if (!painter.begin(&printer))
        return fail(QObject::tr("Не удалось начать печать: принтер недоступен или занят."));
    for (int i = 0; i < count; ++i) {
        QImage img = render(i, dpi);
        if (s.rotate)
            img = img.transformed(QTransform().rotate(90));
        if (i > 0 && !printer.newPage()) {
            painter.end();
            return fail(QObject::tr("Принтер не принял страницу %1.").arg(i + 1));
        }
        painter.drawImage(QPoint(0, 0), img);
    }
    painter.end();
    return true;
}

} // namespace ThermalLabel
