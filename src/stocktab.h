#pragma once

#include <QWidget>

class QTableView;
class QSqlQueryModel;

// Вкладка "Остатки и движения": сводный остаток по товару/складу
// (один артикул может числиться сразу на нескольких складах) и журнал
// приход/списание/перемещение/инвентаризации.
class StockTab : public QWidget
{
    Q_OBJECT
public:
    explicit StockTab(QWidget *parent = nullptr);

    void refresh();

private slots:
    void doReceipt();
    void doWriteOff();
    void doTransfer();
    void doInventoryAdjust();

private:
    void refreshStock();
    void refreshMovements();

    QTableView *m_stockTable;
    QSqlQueryModel *m_stockModel;

    QTableView *m_movementTable;
    QSqlQueryModel *m_movementModel;
};
