#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QFrame;
class QHBoxLayout;
class QLabel;
class QTimer;

// Полоса «сколько пар на складах»: всегда на виду над страницей. Слева крупная карточка «Всего пар», правее - по карточке на склад
// (число пар и подпись). Данные - один запрос сводки по таблице stock; перечитывается при смене страницы, после изменения остатков
// и раз в полминуты (остатки правят и с других компьютеров).
class StockBar : public QWidget
{
    Q_OBJECT
public:
    struct Item {
        int warehouseId = 0;
        QString name;
        qint64 pairs = 0;      // пар на складе
        qint64 positions = 0;  // сколько артикулов (размеров) в наличии
    };

    explicit StockBar(QWidget *parent = nullptr);

    // Перечитывает сводку из базы. Если базы нет или запрос не удался, показывает то, что было.
    void refresh();
    // То же, но без задержки интерфейса и не чаще раза в четверть секунды: первый запрос уходит сразу после возврата в цикл событий,
    // а если за четверть секунды их набежало несколько (закрытие окна, смена страницы), делается ещё один - последний.
    void scheduleRefresh();
    // Показывает готовые данные (для проверок и для случая, когда сводка уже посчитана).
    void setItems(const QList<Item> &items);

    qint64 totalPairs() const { return m_total; }
    QList<Item> items() const { return m_items; }

    // «1 пара», «2 пары», «5 пар», «21 пара», «12 пар».
    static QString pairsText(qint64 count);
    // Число с неразрывным пробелом между тысячами: 12345 -> «12 345».
    static QString groupDigits(qint64 value);

private:
    struct Card {
        QFrame *frame = nullptr;
        QLabel *caption = nullptr, *value = nullptr;
    };
    void rebuild();
    // Карточки не пересоздаются: у существующих меняется только текст. Лишние окна и скачки под курсором не нужны.
    void setCard(int index, const QString &caption, const QString &value, bool total, const QString &tip);

    QHBoxLayout *m_layout = nullptr;
    QList<Card> m_cards;
    QTimer *m_refreshTimer = nullptr;
    bool m_refreshPending = false;
    QList<Item> m_items;
    qint64 m_total = 0;
};
