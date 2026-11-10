#include "stockbar.h"

#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kMaxCards = 6;     // больше складов в полосе не помещается: остальные - в карточке «Ещё»
constexpr int kNameChars = 24;

} // namespace

StockBar::StockBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("stockBar"));
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(10);
    m_layout->addStretch(1);
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(250);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        if (!m_refreshPending)
            return;
        m_refreshPending = false;
        refresh();
        m_refreshTimer->start();
    });
    rebuild();
}

void StockBar::scheduleRefresh()
{
    if (m_refreshTimer->isActive()) {
        m_refreshPending = true;
        return;
    }
    m_refreshTimer->start();
    QTimer::singleShot(0, this, &StockBar::refresh);
}

QString StockBar::pairsText(qint64 count)
{
    const qint64 n = count < 0 ? -count : count;
    const int last = int(n % 10);
    const int last2 = int(n % 100);
    QString word;
    if (last == 1 && last2 != 11)
        word = QStringLiteral("пара");
    else if (last >= 2 && last <= 4 && !(last2 >= 12 && last2 <= 14))
        word = QStringLiteral("пары");
    else
        word = QStringLiteral("пар");
    return QStringLiteral("%1\u00A0%2").arg(groupDigits(count), word); // неразрывный пробел: число и слово не расходятся по строкам
}

QString StockBar::groupDigits(qint64 value)
{
    QString digits = QString::number(value < 0 ? -value : value);
    QString out;
    for (int i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            out += QChar(0x00A0);
        out += digits[i];
    }
    return value < 0 ? QStringLiteral("-") + out : out;
}

void StockBar::refresh()
{
    if (!QSqlDatabase::database().isOpen())
        return;
    QSqlQuery q(QStringLiteral(
        "SELECT w.id, w.name, COALESCE(SUM(s.quantity), 0)::bigint, COUNT(s.product_id) FILTER (WHERE s.quantity > 0)::bigint "
        "FROM warehouses w LEFT JOIN stock s ON s.warehouse_id = w.id GROUP BY w.id, w.name ORDER BY w.name"));
    QList<Item> items;
    bool any = false;
    while (q.next()) {
        any = true;
        Item it;
        it.warehouseId = q.value(0).toInt();
        it.name = q.value(1).toString();
        it.pairs = q.value(2).toLongLong();
        it.positions = q.value(3).toLongLong();
        items << it;
    }
    if (any || q.lastError().type() == QSqlError::NoError)
        setItems(items);
}

void StockBar::setItems(const QList<Item> &items)
{
    m_items = items;
    m_total = 0;
    for (const Item &it : items)
        m_total += it.pairs;
    rebuild();
}

void StockBar::setCard(int index, const QString &caption, const QString &value, bool total, const QString &tip)
{
    if (index >= m_cards.size()) {
        Card c;
        c.frame = new QFrame(this);
        c.frame->setObjectName(QStringLiteral("statCard"));
        auto *l = new QVBoxLayout(c.frame);
        l->setContentsMargins(14, 8, 14, 8);
        l->setSpacing(0);
        c.caption = new QLabel(c.frame);
        c.caption->setObjectName(QStringLiteral("statCaption"));
        c.value = new QLabel(c.frame);
        c.value->setObjectName(QStringLiteral("statValue"));
        l->addWidget(c.caption);
        l->addWidget(c.value);
        m_layout->insertWidget(index, c.frame);
        m_cards << c;
    }
    Card &c = m_cards[index];
    if (c.frame->property("total").toBool() != total || !c.frame->property("total").isValid()) {
        c.frame->setProperty("total", total);
        c.frame->style()->unpolish(c.frame);
        c.frame->style()->polish(c.frame);
    }
    c.frame->setMinimumWidth(total ? 150 : 128);
    if (c.caption->text() != caption)
        c.caption->setText(caption);
    if (c.value->text() != value)
        c.value->setText(value);
    if (c.frame->toolTip() != tip)
        c.frame->setToolTip(tip);
}

void StockBar::rebuild()
{
    // Тонкий список для подсказки: каждый склад - число пар и сколько артикулов в наличии.
    QStringList lines;
    for (const Item &it : std::as_const(m_items))
        lines << QObject::tr("%1: %2 (артикулов в наличии: %3)").arg(it.name, pairsText(it.pairs)).arg(it.positions);
    const QString allTip = lines.isEmpty() ? QObject::tr("Складов пока нет") : lines.join(QLatin1Char('\n'));

    int index = 0;
    setCard(index++, QObject::tr("ВСЕГО ПАР"), groupDigits(m_total), true, allTip);
    const int shown = qMin(int(m_items.size()), m_items.size() > kMaxCards ? kMaxCards - 1 : kMaxCards);
    for (int i = 0; i < shown; ++i) {
        const Item &it = m_items.at(i);
        QString name = it.name;
        if (name.size() > kNameChars)
            name = name.left(kNameChars - 1) + QChar(0x2026);
        setCard(index++, name, groupDigits(it.pairs), false,
                QObject::tr("%1: %2\nАртикулов в наличии: %3").arg(it.name, pairsText(it.pairs)).arg(it.positions));
    }
    if (m_items.size() > shown) {
        qint64 restPairs = 0;
        QStringList restLines;
        for (int i = shown; i < m_items.size(); ++i) {
            restPairs += m_items[i].pairs;
            restLines << QObject::tr("%1: %2").arg(m_items[i].name, pairsText(m_items[i].pairs));
        }
        setCard(index++, QObject::tr("Ещё складов: %1").arg(m_items.size() - shown), groupDigits(restPairs), false, restLines.join(QLatin1Char('\n')));
    }
    // Лишние карточки (складов стало меньше) убираем из полосы целиком: они остаются её дочерними и никогда не становятся отдельными окнами.
    while (m_cards.size() > index) {
        Card c = m_cards.takeLast();
        m_layout->removeWidget(c.frame);
        c.frame->hide();
        c.frame->deleteLater();
    }
}
