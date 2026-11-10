#include "stockbar.h"

#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVBoxLayout>

namespace {

constexpr int kMaxCards = 6;     // больше складов в полосе не помещается: остальные - в карточке «Ещё»
constexpr int kNameChars = 24;

QFrame *makeCard(QWidget *parent, const QString &caption, const QString &value, bool total, const QString &tip)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("statCard"));
    card->setProperty("total", total);
    card->setMinimumWidth(total ? 150 : 128);
    card->setToolTip(tip);
    auto *l = new QVBoxLayout(card);
    l->setContentsMargins(14, 8, 14, 8);
    l->setSpacing(0);
    auto *cap = new QLabel(caption, card);
    cap->setObjectName(QStringLiteral("statCaption"));
    auto *val = new QLabel(value, card);
    val->setObjectName(QStringLiteral("statValue"));
    l->addWidget(cap);
    l->addWidget(val);
    return card;
}

} // namespace

StockBar::StockBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("stockBar"));
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(10);
    rebuild();
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

void StockBar::rebuild()
{
    while (QLayoutItem *li = m_layout->takeAt(0)) {
        if (QWidget *w = li->widget()) {
            w->setParent(nullptr); // сразу убираем из дерева виджетов, удаляется позже
            w->deleteLater();
        }
        delete li;
    }
    // Тонкий список для подсказки: каждый склад - число пар и сколько артикулов в наличии.
    QStringList lines;
    for (const Item &it : std::as_const(m_items))
        lines << QObject::tr("%1: %2 (артикулов в наличии: %3)").arg(it.name, pairsText(it.pairs)).arg(it.positions);
    const QString allTip = lines.isEmpty() ? QObject::tr("Складов пока нет") : lines.join(QLatin1Char('\n'));

    m_layout->addWidget(makeCard(this, QObject::tr("ВСЕГО ПАР"), groupDigits(m_total), true, allTip));
    const int shown = qMin(int(m_items.size()), m_items.size() > kMaxCards ? kMaxCards - 1 : kMaxCards);
    for (int i = 0; i < shown; ++i) {
        const Item &it = m_items.at(i);
        QString name = it.name;
        if (name.size() > kNameChars)
            name = name.left(kNameChars - 1) + QChar(0x2026);
        m_layout->addWidget(makeCard(this, name, groupDigits(it.pairs), false,
                                     QObject::tr("%1: %2\nАртикулов в наличии: %3").arg(it.name, pairsText(it.pairs)).arg(it.positions)));
    }
    if (m_items.size() > shown) {
        qint64 restPairs = 0;
        QStringList restLines;
        for (int i = shown; i < m_items.size(); ++i) {
            restPairs += m_items[i].pairs;
            restLines << QObject::tr("%1: %2").arg(m_items[i].name, pairsText(m_items[i].pairs));
        }
        m_layout->addWidget(makeCard(this, QObject::tr("Ещё складов: %1").arg(m_items.size() - shown), groupDigits(restPairs), false, restLines.join(QLatin1Char('\n'))));
    }
    m_layout->addStretch(1);
}
