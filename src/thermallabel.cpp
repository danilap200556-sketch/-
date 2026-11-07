#include "thermallabel.h"

#include "barcode.h"
#include "datamatrix.h"
#include "labelfonts.h"
#include "markingcode.h"

#include <QFont>
#include <QFontMetrics>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPrinter>
#include <QPrinterInfo>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace ThermalLabel {

namespace {

int px(double mm, int dpi) { return int(std::lround(mm * dpi / 25.4)); }

// Шрифт текущей этикетки: его задаёт Settings::fontFamily в начале отрисовки.
QString g_family;

struct FamilyScope {
    explicit FamilyScope(const QString &family) : previous(g_family) { g_family = family; }
    ~FamilyScope() { g_family = previous; }
    QString previous;
};

QFont labelFontW(int pixelSize, int weight)
{
    return LabelFonts::font(g_family, weight, pixelSize);
}

QFont labelFont(int pixelSize, bool bold)
{
    return labelFontW(pixelSize, bold ? 700 : 400);
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
    s.fontFamily = q.value(QStringLiteral("thermal/fontFamily")).toString();
    return s;
}

void saveSettings(const Settings &s)
{
    QSettings q;
    q.setValue(QStringLiteral("thermal/widthMm"), s.widthMm);
    q.setValue(QStringLiteral("thermal/heightMm"), s.heightMm);
    q.setValue(QStringLiteral("thermal/rotate"), s.rotate);
    q.setValue(QStringLiteral("thermal/printer"), s.printer);
    q.setValue(QStringLiteral("thermal/fontFamily"), s.fontFamily);
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
    const FamilyScope fontScope(s.fontFamily);
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

namespace {

struct ArticleParts {
    QString model; // артикул без размера
    QString size;  // размер («42», «38.5», «M»); пусто - в артикуле его нет
};

// «DD1391-100_42» -> модель «DD1391-100», размер «42». Обувные размеры 30-52 (в том числе с половиной) отделяются и через дефис,
// буквенные (XS...XXXL, OS) - только через «_» (иначе «AD-S-40» превратился бы в размер S).
ArticleParts splitSize(const QString &article)
{
    static const QRegularExpression shoe(QStringLiteral("^(.*\\S)\\s*[-_/ ]\\s*((?:3\\d|4\\d|5[0-2])(?:[.,]5)?)$"));
    static const QRegularExpression letters(QStringLiteral("^(.*\\S)\\s*_\\s*(XXS|XS|S|M|L|XL|XXL|XXXL|OS)$"), QRegularExpression::CaseInsensitiveOption);
    for (const QRegularExpression *re : {&shoe, &letters}) {
        const auto m = re->match(article.trimmed());
        if (m.hasMatch() && m.captured(1).size() >= 3)
            return {m.captured(1), m.captured(2).replace(QLatin1Char(','), QLatin1Char('.')).toUpper()};
    }
    return {article.trimmed(), QString()};
}

} // namespace

namespace {

// Самый крупный шрифт (в пикселях) от maxPx до minPx, в котором text помещается в width.
QFont fitFont(const QString &text, int width, int maxPx, int minPx, int weight)
{
    int size = qMax(minPx, maxPx);
    QFont f = labelFontW(size, weight);
    while (size > minPx && QFontMetrics(f).horizontalAdvance(text) > width) {
        size = qMax(minPx, size - qMax(1, size / 20));
        f = labelFontW(size, weight);
    }
    return f;
}

// Перенос значения по местам, где можно переломить (пробел, дефис, «_», «/»), не больше maxLines строк. *fits = false, если так не
// поместилось (слишком длинное слово или строк больше): тогда последняя строка собирает остаток и сокращается многоточием.
QStringList chunkWrap(const QString &text, const QFontMetrics &fm, int width, int maxLines, bool *fits)
{
    QStringList chunks;
    QString cur;
    for (const QChar c : text) {
        cur += c;
        if (c == QLatin1Char(' ') || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('/')) {
            chunks << cur;
            cur.clear();
        }
    }
    if (!cur.isEmpty())
        chunks << cur;
    *fits = true;
    QStringList lines;
    QString line;
    for (const QString &chunk : std::as_const(chunks)) {
        if (fm.horizontalAdvance((line + chunk).trimmed()) <= width) {
            line += chunk;
            continue;
        }
        if (!line.trimmed().isEmpty())
            lines << line.trimmed();
        if (fm.horizontalAdvance(chunk.trimmed()) > width)
            *fits = false; // одно слово шире строки
        line = chunk;
    }
    if (!line.trimmed().isEmpty())
        lines << line.trimmed();
    if (lines.isEmpty())
        lines << QString();
    if (lines.size() > maxLines) {
        *fits = false;
        QString rest;
        for (int i = maxLines - 1; i < lines.size(); ++i)
            rest += (rest.isEmpty() ? QString() : QStringLiteral(" ")) + lines[i];
        lines = lines.mid(0, maxLines - 1);
        lines << fm.elidedText(rest, Qt::ElideRight, width);
    } else if (!*fits) {
        lines.last() = fm.elidedText(lines.last(), Qt::ElideRight, width);
    }
    return lines;
}

struct FittedText {
    QFont font;
    QStringList lines;
};

// Значение в прямоугольнике width x roomH: самый крупный шрифт от maxPx до minPx, при котором оно помещается в одну или несколько
// строк; в самом мелком, если не помещается, последняя строка сокращается многоточием.
FittedText fitLines(const QString &text, int width, int roomH, int maxPx, int minPx, int weight)
{
    FittedText out;
    for (int size = qMax(minPx, maxPx);; size = qMax(minPx, size - qMax(1, size / 16))) {
        out.font = labelFontW(size, weight);
        const QFontMetrics fm(out.font);
        bool fits = true;
        out.lines = chunkWrap(text, fm, width, qMax(1, roomH / qMax(1, fm.lineSpacing())), &fits);
        if (fits || size <= minPx)
            break;
    }
    return out;
}

// Логотип «Честный знак»: рамка-видоискатель с галочкой и подпись «ЧЕСТНЫЙ / ЗНАК». Рисуется в прямоугольнике r.
void drawLogo(QPainter &p, const QRect &r, int dpi)
{
    const int side = qMin(r.height(), r.width() / 3);
    const int line = qMax(1, px(0.38, dpi));
    QPen pen(Qt::black, line, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const int x0 = r.left(), y0 = r.top() + (r.height() - side) / 2;
    const int arm = qMax(2, side * 32 / 100);
    const int x1 = x0 + side - 1, y1 = y0 + side - 1;
    // Четыре угла рамки.
    p.drawLine(x0, y0, x0 + arm, y0); p.drawLine(x0, y0, x0, y0 + arm);
    p.drawLine(x1, y0, x1 - arm, y0); p.drawLine(x1, y0, x1, y0 + arm);
    p.drawLine(x0, y1, x0 + arm, y1); p.drawLine(x0, y1, x0, y1 - arm);
    p.drawLine(x1, y1, x1 - arm, y1); p.drawLine(x1, y1, x1, y1 - arm);
    // Галочка.
    QPen tick(Qt::black, qMax(line, side / 8), Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    p.setPen(tick);
    p.drawPolyline(QPolygon({QPoint(x0 + side * 27 / 100, y0 + side * 52 / 100), QPoint(x0 + side * 43 / 100, y0 + side * 68 / 100),
                             QPoint(x0 + side * 74 / 100, y0 + side * 33 / 100)}));
    p.setPen(Qt::black);
    const int tx = x0 + side + px(1.0, dpi);
    const int tw = r.right() - tx + 1;
    if (tw < px(4, dpi))
        return;
    const QString top = QObject::tr("ЧЕСТНЫЙ"), bottom = QObject::tr("ЗНАК");
    const QFont topFont = fitFont(top, tw, int(side * 0.36), px(1.2, dpi), 700);
    const QFont bottomFont = fitFont(bottom, tw, int(side * 0.62), px(1.6, dpi), 800);
    const QFontMetrics tfm(topFont), bfm(bottomFont);
    const int total = tfm.height() + bfm.height() - px(0.3, dpi);
    int y = y0 + (side - total) / 2;
    p.setFont(topFont);
    p.drawText(QRect(tx, y, tw, tfm.height()), Qt::AlignLeft | Qt::AlignVCenter, top);
    y += tfm.height() - px(0.3, dpi);
    p.setFont(bottomFont);
    p.drawText(QRect(tx, y, tw, bfm.height()), Qt::AlignLeft | Qt::AlignVCenter, bottom);
}

// Этикетка «как в личном кабинете Честного знака» (см. MarkingLabel::Layout::Cabinet).
QImage renderCabinet(const MarkingLabel &label, const Settings &s, int dpi, bool monochrome)
{
    const int W = px(s.widthMm, dpi), H = px(s.heightMm, dpi);
    QImage img(W, H, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing, !monochrome);
    p.setPen(Qt::black);
    const int m = px(2.0, dpi); // поле: у термопринтера по краям есть непропечатываемая полоска
    const int L = m, T = m, R = W - m, B = H - m;
    const int rule = qMax(1, px(0.3, dpi));
    const MarkingCode::Parsed parsed = MarkingCode::parse(label.code);
    const ArticleParts parts = splitSize(label.article);
    const QString model = label.model.isEmpty() ? parts.model : label.model.trimmed();
    const QString size = label.size.isEmpty() ? parts.size : label.size.trimmed();

    // --- Data Matrix: модуль - целое число точек; столбец справа ровно по коду (но не уже 15 мм, чтобы поместились адрес и логотип).
    QString dmError;
    const DataMatrix::Symbol sym = parsed.ok ? DataMatrix::encode(parsed.code.toLatin1(), true, &dmError) : DataMatrix::Symbol();
    const int symSize = sym.isNull() ? 24 : sym.size;
    const bool tall = s.heightMm >= 34;
    const int band = px(tall ? 6.5 : 5.0, dpi);   // верхняя полоса: бренд и адрес сайта
    const int ruleY = T + band;
    const int gap = px(0.9, dpi);
    const int colTop = ruleY + rule + px(1.0, dpi);
    const int logoH = px(tall ? 5.4 : 4.2, dpi);
    // Читаемый текст кода «01<GTIN>21<серийный номер>» - до двух строк под кодом.
    const QString readable = parsed.ok ? QStringLiteral("01") + parsed.gtin + QStringLiteral("21") + parsed.serial : label.code.left(40);
    int module = qMax(2, px(20.0, dpi) / symSize);
    int colW = qBound(px(15.0, dpi), module * symSize, int((R - L) * 0.46));
    module = qMax(1, qMin(module, colW / symSize));
    QFont cf = labelFontW(px(1.9, dpi), 600);
    QStringList codeLines;
    for (int size2 = px(1.9, dpi);; size2 -= qMax(1, px(0.05, dpi))) {
        cf = labelFontW(size2, 600);
        const QFontMetrics fm(cf);
        QString rest = readable;
        codeLines.clear();
        for (int i = 0; i < 2 && !rest.isEmpty(); ++i) {
            int n = rest.size();
            while (n > 1 && fm.horizontalAdvance(rest.left(n)) > colW)
                --n;
            codeLines << rest.left(n);
            rest = rest.mid(n);
        }
        if (rest.isEmpty() || size2 <= px(1.25, dpi)) {
            if (!rest.isEmpty())
                codeLines.last() = fm.elidedText(codeLines.last() + rest, Qt::ElideRight, colW);
            break;
        }
    }
    const int codeH = int(codeLines.size()) * QFontMetrics(cf).lineSpacing();
    const int maxSide = (B - colTop) - logoH - codeH - 2 * gap;
    module = qMax(1, qMin(module, maxSide / symSize));
    const int dmSide = module * symSize;
    colW = qMax(colW, qMin(dmSide, int((R - L) * 0.46)));
    const int colX = R - colW + 1;
    const int ruleX = colX - px(1.3, dpi);

    // --- Линии: под верхней полосой на всю ширину и вертикальная между колонками.
    p.fillRect(L, ruleY, R - L + 1, rule, Qt::black);
    p.fillRect(ruleX, ruleY, rule, B - ruleY + 1, Qt::black);

    // --- Верхняя полоса: бренд слева крупно, адрес сайта справа.
    {
        const QString brand = label.brand.trimmed().toUpper();
        if (!brand.isEmpty()) {
            const int w = ruleX - L - px(1.0, dpi), h = band - px(0.7, dpi);
            const FittedText fit = fitLines(brand, w, h, px(4.8, dpi), px(2.0, dpi), 800);
            const QFontMetrics bfm(fit.font);
            p.setFont(fit.font);
            // Прижато к нижней линии: у однострочного бренда как в образце, у двухстрочного - две строки над линией.
            int by = T + h - int(fit.lines.size()) * bfm.lineSpacing();
            for (const QString &line : fit.lines) {
                p.drawText(QRect(L, by, w, bfm.lineSpacing()), Qt::AlignLeft | Qt::AlignVCenter, line);
                by += bfm.lineSpacing();
            }
        }
        const QString url = QObject::tr("www.честныйзнак.рф");
        const QFont uf = fitFont(url, colW, px(1.9, dpi), px(1.1, dpi), 700);
        p.setFont(uf);
        p.drawText(QRect(colX, T, colW, band - px(0.7, dpi)), Qt::AlignHCenter | Qt::AlignBottom, url);
    }

    // --- Правая колонка: Data Matrix, текст кода, логотип.
    {
        const int xDm = colX + (colW - dmSide) / 2;
        if (!sym.isNull()) {
            for (int r = 0; r < sym.size; ++r)
                for (int c = 0; c < sym.size; ++c)
                    if (sym.dark(r, c))
                        p.fillRect(xDm + c * module, colTop + r * module, module, module, Qt::black);
        } else {
            p.setFont(labelFont(px(2.2, dpi), false));
            p.drawText(QRect(colX, colTop, colW, dmSide), Qt::AlignCenter | Qt::TextWordWrap, QObject::tr("Код не помещается в Data Matrix"));
        }
        p.setFont(cf);
        const QFontMetrics cfm(cf);
        int y = colTop + dmSide + gap;
        for (const QString &line : std::as_const(codeLines)) {
            p.drawText(QRect(colX, y, colW, cfm.lineSpacing()), Qt::AlignLeft | Qt::AlignVCenter, line);
            y += cfm.lineSpacing();
        }
        drawLogo(p, QRect(colX, B - logoH + 1, colW, logoH), dpi);
    }

    // --- Левая колонка: три строки с полями. Подпись мелко, значение жирно под ней.
    const int leftW = ruleX - L;
    const int rowsTop = ruleY + rule;
    const int rowsH = B - rowsTop + 1;
    const int y2 = rowsTop + rowsH * 33 / 100, y3 = y2 + rowsH * 30 / 100;
    p.fillRect(L, y2, leftW, rule, Qt::black);
    p.fillRect(L, y3, leftW, rule, Qt::black);
    QFont capFont = labelFontW(px(1.65, dpi), 500);
    capFont.setLetterSpacing(QFont::AbsoluteSpacing, qMax(0.0, px(0.07, dpi) * 1.0));
    const QFontMetrics capFm(capFont);
    const int pad = px(1.0, dpi), top = px(0.9, dpi);
    // Строка таблицы из нескольких ячеек (подпись мелко, под ней значение). Значения одной строки рисуются ОДНИМ размером шрифта - по самой
    // «тесной» ячейке: ряд выглядит ровно, как в образце. Значение переносится на несколько строк, если так крупнее.
    struct Cell {
        int x, w;
        QString caption, value;
    };
    auto row = [&](int y, int rowH, const QList<Cell> &cells, double maxMm, double minMm) {
        const int room = rowH - top - capFm.height() - px(0.5, dpi);
        const int minPx = px(minMm, dpi);
        const int maxPx = qMax(minPx, qMin(px(maxMm, dpi), int(room / 1.18)));
        QList<FittedText> fits;
        int common = maxPx;
        for (const Cell &c : cells) {
            fits << fitLines(c.value, c.w - pad - px(0.5, dpi), room, maxPx, minPx, 700);
            common = qMin(common, fits.last().font.pixelSize());
        }
        for (int i = 0; i < cells.size(); ++i) {
            const Cell &c = cells[i];
            const int cx = c.x + pad, cw = c.w - pad - px(0.5, dpi);
            p.setFont(capFont);
            p.drawText(QRect(cx, y + top, cw, capFm.height()), Qt::AlignLeft | Qt::AlignVCenter, c.caption);
            const FittedText fit = fits[i].font.pixelSize() == common ? fits[i] : fitLines(c.value, cw, room, common, qMin(common, minPx), 700);
            const QFontMetrics vfm(fit.font);
            p.setFont(fit.font);
            int vy = y + top + capFm.height();
            for (const QString &line : std::as_const(fit.lines)) {
                p.drawText(QRect(cx, vy, cw, vfm.lineSpacing()), Qt::AlignLeft | Qt::AlignVCenter, line);
                vy += vfm.lineSpacing();
            }
        }
    };
    const int h1 = y2 - rowsTop, h2 = y3 - y2, h3 = B - y3 + 1;
    row(rowsTop, h1, {{L, leftW, QObject::tr("АРТИКУЛ"), model}}, 4.2, 1.8);
    const int w1 = leftW * 40 / 100, w2 = leftW * 32 / 100, w3 = leftW - w1 - w2;
    row(y2 + rule, h2, {{L, w1, QObject::tr("ПРОДУКЦИЯ"), label.product.trimmed().toUpper()}, {L + w1, w2, QObject::tr("ЦВЕТ"), label.color.trimmed().toUpper()},
                         {L + w1 + w2, w3, QObject::tr("РАЗМЕР"), size}}, 3.0, 1.5);
    const int wm = leftW * 58 / 100;
    row(y3 + rule, h3, {{L, wm, QObject::tr("ИЗГОТОВИТЕЛЬ"), label.manufacturer.trimmed().toUpper()}, {L + wm, leftW - wm, QObject::tr("СТРАНА"), label.country.trimmed().toUpper()}}, 3.0, 1.5);
    p.fillRect(L + wm, y3, rule, B - y3 + 1, Qt::black);

    p.end();
    return monochrome ? img.convertToFormat(QImage::Format_Mono, Qt::ThresholdDither | Qt::MonoOnly) : img;
}

} // namespace

QImage renderMarking(const MarkingLabel &label, const Settings &s, int dpi, bool monochrome)
{
    const FamilyScope fontScope(s.fontFamily);
    if (label.layout == MarkingLabel::Layout::Cabinet && px(s.widthMm, dpi) * 5 >= px(s.heightMm, dpi) * 6)
        return renderCabinet(label, s, dpi, monochrome);
    const int W = px(s.widthMm, dpi), H = px(s.heightMm, dpi);
    QImage img(W, H, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing, !monochrome);
    p.setPen(Qt::black);
    // Поле 2 мм: у термопринтеров по краям бумаги есть непропечатываемая полоска, меньшее поле обрезает этикетку.
    const int margin = px(2.0, dpi);
    const int ruleGap = px(1.1, dpi);
    const bool wide = W >= H * 1.25; // горизонтальная: код слева, текст справа; иначе код сверху, текст под ним

    const MarkingCode::Parsed parsed = MarkingCode::parse(label.code);

    // --- Читаемый текст внизу: (01) GTIN и (21) серийный номер. Две строки (слева, под кодом); у низкой этикетки - одна.
    const QString gtinText = QStringLiteral("(01) %1").arg(parsed.gtin), serialText = QStringLiteral("(21) %1").arg(parsed.serial);
    QStringList humanLines;
    if (!parsed.ok)
        humanLines = {label.code.left(40)};
    else if (s.heightMm >= 34)
        humanLines = {gtinText, serialText};
    else
        humanLines = {gtinText + QStringLiteral("  ") + serialText};
    auto widest = [&](const QFont &f) {
        int w = 0;
        for (const QString &l : std::as_const(humanLines))
            w = qMax(w, QFontMetrics(f).horizontalAdvance(l));
        return w;
    };
    int humanPx = px(2.9, dpi);
    QFont hf = labelFont(humanPx, false);
    while (humanPx > px(1.9, dpi) && widest(hf) > W - 2 * margin) {
        humanPx -= qMax(1, px(0.1, dpi));
        hf = labelFont(humanPx, false);
    }
    if (humanLines.size() == 1 && parsed.ok && widest(hf) > W - 2 * margin) {
        humanLines = {gtinText, serialText};
        humanPx = px(2.6, dpi);
        hf = labelFont(humanPx, false);
        while (humanPx > px(1.9, dpi) && widest(hf) > W - 2 * margin) {
            humanPx -= qMax(1, px(0.1, dpi));
            hf = labelFont(humanPx, false);
        }
    }
    const QFontMetrics hfm(hf);
    const int humanH = int(humanLines.size()) * hfm.lineSpacing();

    // --- Data Matrix: не больше 27 мм (модуль - целое число точек, чтобы квадраты были чёткими).
    QString dmError;
    const DataMatrix::Symbol sym = parsed.ok ? DataMatrix::encode(parsed.code.toLatin1(), true, &dmError) : DataMatrix::Symbol();
    int maxSide = 0, ruleY = 0, yDm = 0, xDm = 0;
    if (wide) {
        // Горизонтальная: не больше половины ширины. Блок «код + линия + читаемый текст» стоит по центру по вертикали.
        maxSide = qMin(qMin(H - 2 * margin - humanH - 2 * ruleGap, px(27, dpi)), int(W * 0.5));
    } else {
        // Вертикальная: читаемый текст прижат к низу, под кодом остаётся место для артикула и названия (не меньше 11 мм).
        ruleY = H - margin - humanH - ruleGap;
        maxSide = qMin(qMin(W - 2 * margin, ruleY - ruleGap - margin - px(11, dpi)), px(27, dpi));
    }
    maxSide = qMax(maxSide, px(8, dpi));
    int module = 0, dmSide = maxSide;
    if (!sym.isNull()) {
        module = qMax(1, maxSide / sym.size);
        dmSide = module * sym.size;
    }
    if (wide) {
        const int stackH = dmSide + 2 * ruleGap + humanH;
        yDm = margin + qMax(0, (H - 2 * margin - stackH) / 2);
        xDm = margin;
        ruleY = yDm + dmSide + ruleGap;
    } else {
        yDm = margin;
        xDm = (W - dmSide) / 2;
    }
    if (!sym.isNull()) {
        for (int r = 0; r < sym.size; ++r)
            for (int c = 0; c < sym.size; ++c)
                if (sym.dark(r, c))
                    p.fillRect(xDm + c * module, yDm + r * module, module, module, Qt::black);
    } else {
        p.setFont(labelFont(px(2.8, dpi), false));
        p.drawText(QRect(xDm, yDm, dmSide, dmSide), Qt::AlignCenter | Qt::TextWordWrap, QObject::tr("Код не помещается в Data Matrix"));
    }

    // --- Тонкая линия и читаемый текст под ней, выровнены по левому краю кода.
    p.fillRect(margin, ruleY, W - 2 * margin, qMax(1, px(0.25, dpi)), Qt::black);
    p.setFont(hf);
    const int humanTop = ruleY + ruleGap;
    for (int i = 0; i < humanLines.size(); ++i)
        p.drawText(QRect(margin, humanTop + i * hfm.lineSpacing(), W - 2 * margin, hfm.lineSpacing()), (wide ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter,
                   humanLines[i]);

    // --- Текст: у горизонтальной этикетки справа от кода (верх и низ - по краям кода), у вертикальной - под кодом.
    const int colX = wide ? margin + dmSide + px(3.0, dpi) : margin;
    const int colW = W - margin - colX;
    const int textTop = wide ? yDm : yDm + dmSide + px(1.4, dpi);
    const int textBottom = wide ? yDm + dmSide : ruleY - px(1.0, dpi);
    if (colW >= px(10, dpi) && textBottom > textTop) {
        const Qt::Alignment hAlign = wide ? Qt::AlignLeft : Qt::AlignHCenter;
        const ArticleParts parts = splitSize(label.article);
        // Артикул: жирно. Сначала пробуем уместить в одну строку (шрифт сжимается до ~3,2 мм); если не вышло - две строки с
        // переносом по дефису, а не посреди слова.
        const QString model = wide ? parts.model : label.article.simplified();
        int articlePx = px(wide ? 4.6 : 3.8, dpi);
        const int minArticlePx = px(3.2, dpi);
        QFont af = labelFont(articlePx, true);
        QStringList articleLines;
        for (;;) {
            af = labelFont(articlePx, true);
            if (QFontMetrics(af).elidedText(model, Qt::ElideRight, colW) == model) { // «помещается» - по тому же измерению, которым потом рисуем
                articleLines = {model};
                break;
            }
            if (articlePx <= minArticlePx)
                break;
            articlePx = qMax(minArticlePx, articlePx - qMax(1, px(0.2, dpi)));
        }
        if (articleLines.isEmpty()) {
            af = labelFont(px(3.4, dpi), true);
            const QFontMetrics fm2(af);
            for (int i = model.size() - 2; i > 0 && articleLines.isEmpty(); --i) {
                const QChar c = model[i];
                if ((c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char(' ') || c == QLatin1Char('/'))
                    && fm2.elidedText(model.left(i + 1).trimmed(), Qt::ElideRight, colW) == model.left(i + 1).trimmed()
                    && fm2.elidedText(model.mid(i + 1).trimmed(), Qt::ElideRight, colW) == model.mid(i + 1).trimmed())
                    articleLines = {model.left(i + 1).trimmed(), model.mid(i + 1).trimmed()};
            }
            if (articleLines.isEmpty())
                articleLines = wrapText(model, fm2, colW, 2);
        }
        const QFontMetrics afm(af);
        p.setFont(af);
        int y = textTop;
        for (const QString &line : std::as_const(articleLines)) {
            p.drawText(QRect(colX, y, colW, afm.lineSpacing()), hAlign | Qt::AlignVCenter, afm.elidedText(line, Qt::ElideRight, colW));
            y += afm.lineSpacing();
        }
        y += px(1.0, dpi);

        // Размер крупно внизу правой колонки (только у горизонтальной этикетки и только если он есть в артикуле).
        int descBottom = textBottom;
        if (wide && !parts.size.isEmpty()) {
            int sizePx = px(7.5, dpi);
            QFont zf = labelFont(sizePx, true);
            while (sizePx > px(3.5, dpi) && QFontMetrics(zf).horizontalAdvance(parts.size) > colW) {
                sizePx -= qMax(1, px(0.3, dpi));
                zf = labelFont(sizePx, true);
            }
            const QFontMetrics zfm(zf);
            QFont cf = labelFont(px(2.3, dpi), false);
            const QFontMetrics cfm(cf);
            const int need = zfm.lineSpacing() + cfm.lineSpacing();
            if (textBottom - need - px(1.0, dpi) >= y) {
                p.setFont(cf);
                p.drawText(QRect(colX, textBottom - need, colW, cfm.lineSpacing()), Qt::AlignLeft | Qt::AlignVCenter, QObject::tr("размер"));
                p.setFont(zf);
                p.drawText(QRect(colX, textBottom - zfm.lineSpacing(), colW, zfm.lineSpacing()), Qt::AlignLeft | Qt::AlignVCenter, parts.size);
                descBottom = textBottom - need - px(1.0, dpi);
            } else if (textBottom - zfm.lineSpacing() - px(1.0, dpi) >= y) {
                p.setFont(zf);
                p.drawText(QRect(colX, textBottom - zfm.lineSpacing(), colW, zfm.lineSpacing()), Qt::AlignLeft | Qt::AlignVCenter, parts.size);
                descBottom = textBottom - zfm.lineSpacing() - px(1.0, dpi);
            }
        }
        // Название: мелко, в оставшемся месте.
        if (descBottom > y && !label.description.trimmed().isEmpty()) {
            QFont df = labelFont(px(2.5, dpi), false);
            const QFontMetrics dfm(df);
            const int maxLines = qBound(1, (descBottom - y) / qMax(1, dfm.lineSpacing()), 6);
            if (descBottom - y >= dfm.lineSpacing()) {
                p.setFont(df);
                const QStringList lines = wrapText(label.description, dfm, colW, maxLines);
                for (const QString &line : lines) {
                    p.drawText(QRect(colX, y, colW, dfm.lineSpacing()), hAlign | Qt::AlignVCenter, line);
                    y += dfm.lineSpacing();
                }
            }
        }
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
