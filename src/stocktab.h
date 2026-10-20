#pragma once

#include <QList>
#include <QWidget>

class QTableView;
class QSqlQueryModel;
class QLabel;
class QLineEdit;
class QTimer;
class QWidget;

// Вкладка "Остатки и движения": сводный остаток по товару/складу
// (один артикул может числиться сразу на нескольких складах) и журнал
// приход/списание/перемещение/инвентаризации.
class StockTab : public QWidget
{
    Q_OBJECT
public:
    explicit StockTab(QWidget *parent = nullptr);

    void refresh();
    // Роль "только просмотр": приход/списание/перемещение/инвентаризация скрыты.
    void setReadOnly(bool readOnly);

signals:
    // Карточку товара поправили прямо отсюда - остальные вкладки должны обновиться.
    void productEdited();

private slots:
    void editSelectedProduct();
    void doReceipt();
    void doWriteOff();
    void doTransfer();
    void doInventoryAdjust();
    void doBatch();
    void prefetchVisible();

private:
    void refreshStock();
    void refreshMovements();
    QString searchTerm() const;

    QLineEdit *m_search = nullptr;
    QLabel *m_searchStatus = nullptr;
    QTimer *m_searchTimer = nullptr;
    bool m_readOnly = false;

    QList<QWidget *> m_editWidgets;
    QTableView *m_stockTable;
    QSqlQueryModel *m_stockModel;

    QTableView *m_movementTable;
    QSqlQueryModel *m_movementModel;
};
