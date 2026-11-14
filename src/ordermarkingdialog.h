#pragma once

#include "ordermarking.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;

// «ЧЗ на эти пары»: по выбранным заказам Маркета показывает, сколько кодов «Честного знака» нужно, сколько уже выдано, сколько подобрано
// и чего не хватает (по ИП склада кабинета), и печатает подобранные коды на термоэтикетках. Недостающие можно сразу заказать.
class OrderMarkingDialog : public QDialog
{
    Q_OBJECT
public:
    OrderMarkingDialog(const QList<OrderMarking::OrderInput> &orders, QWidget *parent = nullptr);

    const OrderMarking::Plan &plan() const { return m_plan; }
    int printedTotal() const { return m_printedTotal; }

private:
    void rebuild();
    void printNew();
    void printIssued();
    void orderMissing();

    QList<OrderMarking::OrderInput> m_orders;
    OrderMarking::Plan m_plan;
    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_printBtn = nullptr;
    QPushButton *m_reprintBtn = nullptr;
    QPushButton *m_orderBtn = nullptr;
    int m_printedTotal = 0;
};
