#include "barcode.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace Barcode {

namespace {

int checkDigit(QStringView digitsWithoutCheck)
{
    int sum = 0;
    int weight = 3;
    for (qsizetype i = digitsWithoutCheck.size() - 1; i >= 0; --i) {
        sum += (digitsWithoutCheck[i].unicode() - '0') * weight;
        weight = weight == 3 ? 1 : 3;
    }
    return (10 - sum % 10) % 10;
}

const char *const kL[10] = {"0001101", "0011001", "0010011", "0111101", "0100011",
                            "0110001", "0101111", "0111011", "0110111", "0001011"};
const char *const kG[10] = {"0100111", "0110011", "0011011", "0100001", "0011101",
                            "0111001", "0000101", "0010001", "0001001", "0010111"};
const char *const kR[10] = {"1110010", "1100110", "1101100", "1000010", "1011100",
                            "1001110", "1010000", "1000100", "1001000", "1110100"};
// Набор L/G для левой половины задаётся первой цифрой кода.
const char *const kParity[10] = {"LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG",
                                 "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"};

} // namespace

QString normalize(const QString &input)
{
    QString s;
    for (QChar ch : input)
        if (!ch.isSpace() && ch != QLatin1Char('-'))
            s += ch;
    return s;
}

bool isValid(const QString &code, QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (code.isEmpty())
        return fail(QStringLiteral("пустой штрихкод"));
    for (QChar ch : code)
        if (!ch.isDigit() || ch.unicode() > '9')
            return fail(QStringLiteral("штрихкод должен состоять только из цифр"));
    const int len = code.size();
    if (len != 8 && len != 12 && len != 13 && len != 14)
        return fail(QStringLiteral("нужно 13 цифр (EAN-13), либо 8, 12 или 14 - а здесь %1").arg(len));
    if (checkDigit(QStringView(code).left(len - 1)) != code.back().unicode() - '0')
        return fail(QStringLiteral("неверная контрольная (последняя) цифра - проверьте, не ошибка ли в коде"));
    return true;
}

bool canRenderEan13(const QString &code)
{
    return isValid(code) && (code.size() == 13 || code.size() == 12);
}

QString generateInternalEan13()
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        QString body = QStringLiteral("2");
        for (int i = 0; i < 11; ++i)
            body += QChar('0' + QRandomGenerator::global()->bounded(10));
        const QString code = body + QChar('0' + checkDigit(body));
        if (productIdFor(code) < 0)
            return code;
    }
    return QString();
}

QImage renderEan13(const QString &input, int m)
{
    if (!canRenderEan13(input))
        return QImage();
    const QString code = input.size() == 12 ? QStringLiteral("0") + input : input;
    auto digit = [&](int i) { return code[i].unicode() - '0'; };

    QString bits = QStringLiteral("101");
    const char *parity = kParity[digit(0)];
    for (int i = 1; i <= 6; ++i)
        bits += QLatin1String(parity[i - 1] == 'L' ? kL[digit(i)] : kG[digit(i)]);
    bits += QStringLiteral("01010");
    for (int i = 7; i <= 12; ++i)
        bits += QLatin1String(kR[digit(i)]);
    bits += QStringLiteral("101");

    constexpr int quietLeft = 11, quietRight = 7, barHeight = 60, guardExtra = 5, textHeight = 11;
    const int width = (quietLeft + bits.size() + quietRight) * m;
    const int height = (barHeight + guardExtra + textHeight) * m;
    QImage img(width, height, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    for (int i = 0; i < bits.size(); ++i) {
        if (bits[i] != QLatin1Char('1'))
            continue;
        const bool guard = i < 3 || (i >= 45 && i < 50) || i >= 92;
        p.fillRect((quietLeft + i) * m, 0, m, (barHeight + (guard ? guardExtra : 0)) * m, Qt::black);
    }
    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(9 * m);
    p.setFont(font);
    p.setPen(Qt::black);
    const int textTop = barHeight * m;
    const int textH = (guardExtra + textHeight) * m;
    p.drawText(QRect(0, textTop, quietLeft * m - m, textH), Qt::AlignRight | Qt::AlignVCenter, code.left(1));
    p.drawText(QRect((quietLeft + 3) * m, textTop, 42 * m, textH), Qt::AlignCenter, code.mid(1, 6));
    p.drawText(QRect((quietLeft + 50) * m, textTop, 42 * m, textH), Qt::AlignCenter, code.mid(7, 6));
    p.end();
    return img;
}

QPixmap renderLabel(const QString &code, const QStringList &captionLines)
{
    const QImage bar = renderEan13(code, 3);
    if (bar.isNull())
        return QPixmap();
    QFont font;
    font.setPointSize(9);
    const QFontMetrics fm(font);
    const int lineHeight = fm.height() + 2;
    int textWidth = 0;
    for (const QString &line : captionLines)
        textWidth = qMax(textWidth, fm.horizontalAdvance(line));
    const int width = qMax(bar.width(), textWidth) + 16;
    const int height = 4 + lineHeight * int(captionLines.size()) + 4 + bar.height() + 6;

    QPixmap pixmap(width, height);
    pixmap.fill(Qt::white);
    QPainter p(&pixmap);
    p.setFont(font);
    int y = 4;
    for (const QString &line : captionLines) {
        p.drawText(QRect(0, y, width, lineHeight), Qt::AlignHCenter | Qt::AlignVCenter, line);
        y += lineHeight;
    }
    p.drawImage((width - bar.width()) / 2, y + 4, bar);
    p.setPen(Qt::gray);
    p.drawRect(0, 0, width - 1, height - 1);
    p.end();
    return pixmap;
}

// ---------------------------------------------------------------------------

QStringList forProduct(int productId)
{
    QStringList codes;
    QSqlQuery q;
    q.prepare("SELECT barcode FROM product_barcodes WHERE product_id = ? ORDER BY created_at, barcode");
    q.addBindValue(productId);
    if (q.exec())
        while (q.next())
            codes << q.value(0).toString();
    return codes;
}

int productIdFor(const QString &code)
{
    QSqlQuery q;
    q.prepare("SELECT product_id FROM product_barcodes WHERE barcode = ?");
    q.addBindValue(normalize(code));
    return q.exec() && q.next() ? q.value(0).toInt() : -1;
}

bool findConflict(int productId, const QStringList &codes, QString *code, QString *otherSku)
{
    for (const QString &c : codes) {
        QSqlQuery q;
        q.prepare("SELECT p.sku FROM product_barcodes b JOIN products p ON p.id = b.product_id "
                  "WHERE b.barcode = ? AND b.product_id <> ?");
        q.addBindValue(c);
        q.addBindValue(productId);
        if (q.exec() && q.next()) {
            *code = c;
            *otherSku = q.value(0).toString();
            return true;
        }
    }
    return false;
}

bool setForProduct(int productId, const QStringList &codes, QString *error)
{
    QString conflictCode, otherSku;
    if (findConflict(productId, codes, &conflictCode, &otherSku)) {
        if (error)
            *error = QStringLiteral("штрихкод %1 уже привязан к товару %2").arg(conflictCode, otherSku);
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database();
    db.transaction();
    QSqlQuery del;
    del.prepare("DELETE FROM product_barcodes WHERE product_id = ?");
    del.addBindValue(productId);
    bool ok = del.exec();
    QString err = del.lastError().text();
    for (const QString &c : codes) {
        if (!ok)
            break;
        QSqlQuery ins;
        ins.prepare("INSERT INTO product_barcodes (barcode, product_id) VALUES (?, ?)");
        ins.addBindValue(c);
        ins.addBindValue(productId);
        ok = ins.exec();
        err = ins.lastError().text();
    }
    if (!ok || !db.commit()) {
        db.rollback();
        if (error)
            *error = err.isEmpty() ? db.lastError().text() : err;
        return false;
    }
    return true;
}

bool attach(int productId, const QString &input, QString *error)
{
    const QString code = normalize(input);
    if (!isValid(code, error))
        return false;
    const int owner = productIdFor(code);
    if (owner == productId)
        return true;
    if (owner >= 0) {
        QString c, sku;
        findConflict(productId, {code}, &c, &sku);
        if (error)
            *error = QStringLiteral("штрихкод %1 уже привязан к товару %2").arg(code, sku);
        return false;
    }
    QSqlQuery q;
    q.prepare("INSERT INTO product_barcodes (barcode, product_id) VALUES (?, ?)");
    q.addBindValue(code);
    q.addBindValue(productId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

} // namespace Barcode
