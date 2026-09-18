#include "qrlabel.h"

#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QPrinter>
#include <QPageSize>

#include <qrencode.h>

namespace QrLabel {

QImage renderMatrix(const QString &content, int moduleSize, int marginModules)
{
    const QByteArray utf8 = content.toUtf8();
    QRcode *qr = QRcode_encodeString(utf8.constData(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!qr)
        return QImage();

    const int size = qr->width;
    const int imgSize = (size + marginModules * 2) * moduleSize;
    QImage img(imgSize, imgSize, QImage::Format_RGB32);
    img.fill(Qt::white);

    QPainter painter(&img);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const unsigned char v = qr->data[y * size + x];
            if (v & 1) {
                const int px = (x + marginModules) * moduleSize;
                const int py = (y + marginModules) * moduleSize;
                painter.drawRect(px, py, moduleSize, moduleSize);
            }
        }
    }
    painter.end();

    QRcode_free(qr);
    return img;
}

QPixmap renderLabel(const QString &content, const QStringList &captionLines, int moduleSize)
{
    const QImage qr = renderMatrix(content, moduleSize, 2);
    if (qr.isNull())
        return QPixmap();

    QFont font;
    font.setPointSize(9);
    QFontMetrics fm(font);
    const int lineHeight = fm.height() + 2;
    const int textBlockHeight = lineHeight * captionLines.size() + 6;

    int textWidth = 0;
    for (const QString &line : captionLines)
        textWidth = qMax(textWidth, fm.horizontalAdvance(line));

    const int width = qMax(qr.width(), textWidth) + 16;
    const int height = qr.height() + textBlockHeight + 8;

    QPixmap pixmap(width, height);
    pixmap.fill(Qt::white);

    QPainter painter(&pixmap);
    painter.setFont(font);
    const int qrX = (width - qr.width()) / 2;
    painter.drawImage(qrX, 4, qr);

    int y = qr.height() + 8;
    for (const QString &line : captionLines) {
        const QRect rect(0, y, width, lineHeight);
        painter.drawText(rect, Qt::AlignHCenter | Qt::AlignVCenter, line);
        y += lineHeight;
    }
    painter.setPen(Qt::gray);
    painter.drawRect(0, 0, width - 1, height - 1);
    painter.end();

    return pixmap;
}

bool savePngSheet(const QString &filePath, const QList<QPixmap> &labels, int columns, int spacing)
{
    if (labels.isEmpty() || columns <= 0)
        return false;

    int cellW = 0, cellH = 0;
    for (const QPixmap &p : labels) {
        cellW = qMax(cellW, p.width());
        cellH = qMax(cellH, p.height());
    }

    const int rows = (labels.size() + columns - 1) / columns;
    const int sheetW = columns * cellW + (columns + 1) * spacing;
    const int sheetH = rows * cellH + (rows + 1) * spacing;

    QImage sheet(sheetW, sheetH, QImage::Format_RGB32);
    sheet.fill(Qt::white);
    QPainter painter(&sheet);

    for (int i = 0; i < labels.size(); ++i) {
        const int col = i % columns;
        const int row = i / columns;
        const int x = spacing + col * (cellW + spacing);
        const int y = spacing + row * (cellH + spacing);
        painter.drawPixmap(x, y, labels[i]);
    }
    painter.end();

    return sheet.save(filePath, "PNG");
}

bool exportPdf(const QString &filePath, const QList<QPixmap> &labels, int columns, int rows)
{
    if (labels.isEmpty() || columns <= 0 || rows <= 0)
        return false;

    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(filePath);
    printer.setPageSize(QPageSize(QPageSize::A4));
    printer.setPageMargins(QMarginsF(10, 10, 10, 10), QPageLayout::Millimeter);

    QPainter painter;
    if (!painter.begin(&printer))
        return false;

    const QRect pageRect = printer.pageRect(QPrinter::DevicePixel).toRect();
    const int cellW = pageRect.width() / columns;
    const int cellH = pageRect.height() / rows;
    const int perPage = columns * rows;

    for (int i = 0; i < labels.size(); ++i) {
        if (i > 0 && i % perPage == 0)
            printer.newPage();

        const int indexOnPage = i % perPage;
        const int col = indexOnPage % columns;
        const int row = indexOnPage / columns;

        const QPixmap &label = labels[i];
        const QSize scaled = label.size().scaled(cellW - 8, cellH - 8, Qt::KeepAspectRatio);
        const int x = pageRect.left() + col * cellW + (cellW - scaled.width()) / 2;
        const int y = pageRect.top() + row * cellH + (cellH - scaled.height()) / 2;
        painter.drawPixmap(QRect(x, y, scaled.width(), scaled.height()), label);
    }

    painter.end();
    return true;
}

} // namespace QrLabel
