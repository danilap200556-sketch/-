#include "stocktab.h"
#include "database.h"
#include "movementdialog.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QSqlQueryModel>
#include <QTableView>
#include <QVBoxLayout>

StockTab::StockTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *toolbar = new QHBoxLayout();
    auto *receiptBtn = new QPushButton(tr("Приход"), this);
    auto *writeOffBtn = new QPushButton(tr("Списание"), this);
    auto *transferBtn = new QPushButton(tr("Перемещение"), this);
    auto *inventoryBtn = new QPushButton(tr("Инвентаризация"), this);
    toolbar->addWidget(receiptBtn);
    toolbar->addWidget(writeOffBtn);
    toolbar->addWidget(transferBtn);
    toolbar->addWidget(inventoryBtn);
    toolbar->addStretch();
    layout->addLayout(toolbar);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    layout->addWidget(splitter);

    auto *stockBox = new QGroupBox(tr("Остатки по складам"), this);
    auto *stockLayout = new QVBoxLayout(stockBox);
    m_stockModel = new QSqlQueryModel(this);
    m_stockTable = new QTableView(stockBox);
    m_stockTable->setModel(m_stockModel);
    m_stockTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_stockTable->horizontalHeader()->setStretchLastSection(true);
    stockLayout->addWidget(m_stockTable);
    splitter->addWidget(stockBox);

    auto *movementBox = new QGroupBox(tr("Журнал движений (последние 500)"), this);
    auto *movementLayout = new QVBoxLayout(movementBox);
    m_movementModel = new QSqlQueryModel(this);
    m_movementTable = new QTableView(movementBox);
    m_movementTable->setModel(m_movementModel);
    m_movementTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_movementTable->horizontalHeader()->setStretchLastSection(true);
    movementLayout->addWidget(m_movementTable);
    splitter->addWidget(movementBox);

    refresh();

    connect(receiptBtn, &QPushButton::clicked, this, &StockTab::doReceipt);
    connect(writeOffBtn, &QPushButton::clicked, this, &StockTab::doWriteOff);
    connect(transferBtn, &QPushButton::clicked, this, &StockTab::doTransfer);
    connect(inventoryBtn, &QPushButton::clicked, this, &StockTab::doInventoryAdjust);
}

void StockTab::refresh()
{
    refreshStock();
    refreshMovements();
}

void StockTab::refreshStock()
{
    m_stockModel->setQuery(
        "SELECT p.sku AS 'Артикул', p.name AS 'Название', w.name AS 'Склад', "
        "       l.code AS 'Место хранения', s.quantity AS 'Остаток' "
        "FROM stock s "
        "JOIN products p ON p.id = s.product_id "
        "JOIN warehouses w ON w.id = s.warehouse_id "
        "LEFT JOIN locations l ON l.id = s.location_id "
        "ORDER BY p.sku, w.name");
}

void StockTab::refreshMovements()
{
    m_movementModel->setQuery(
        "SELECT m.created_at AS 'Дата', p.sku AS 'Артикул', w.name AS 'Склад', "
        "       m.type AS 'Тип', m.delta AS 'Изменение', m.comment AS 'Комментарий' "
        "FROM stock_movements m "
        "JOIN products p ON p.id = m.product_id "
        "JOIN warehouses w ON w.id = m.warehouse_id "
        "ORDER BY m.id DESC LIMIT 500");
}

void StockTab::doReceipt()
{
    MovementDialog dlg(MovementDialog::Kind::Receipt, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!Database::adjustStock(dlg.productId(), dlg.warehouseId(), dlg.quantity(),
                                Database::MovementType::Receipt, dlg.comment(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

void StockTab::doWriteOff()
{
    MovementDialog dlg(MovementDialog::Kind::WriteOff, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!Database::adjustStock(dlg.productId(), dlg.warehouseId(), -dlg.quantity(),
                                Database::MovementType::WriteOff, dlg.comment(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

void StockTab::doTransfer()
{
    MovementDialog dlg(MovementDialog::Kind::Transfer, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!Database::transferStock(dlg.productId(), dlg.warehouseId(), dlg.destWarehouseId(),
                                  dlg.quantity(), dlg.comment(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

void StockTab::doInventoryAdjust()
{
    MovementDialog dlg(MovementDialog::Kind::InventoryAdjust, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!Database::inventoryAdjust(dlg.productId(), dlg.warehouseId(), dlg.quantity(), dlg.comment(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}
