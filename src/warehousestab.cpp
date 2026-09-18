#include "warehousestab.h"
#include "formdialog.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QSqlError>
#include <QSqlRecord>
#include <QSqlTableModel>
#include <QTableView>
#include <QVBoxLayout>

namespace {
enum WCol { WColId = 0, WColName, WColAddress };
enum LCol { LColId = 0, LColWarehouseId, LColCode, LColDescription };
}

WarehousesTab::WarehousesTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *splitter = new QSplitter(this);
    layout->addWidget(splitter);

    // --- Левая панель: склады ---
    auto *warehouseBox = new QGroupBox(tr("Склады"), this);
    auto *warehouseLayout = new QVBoxLayout(warehouseBox);
    auto *warehouseToolbar = new QHBoxLayout();
    auto *addWhBtn = new QPushButton(tr("Добавить"), warehouseBox);
    auto *editWhBtn = new QPushButton(tr("Изменить"), warehouseBox);
    auto *delWhBtn = new QPushButton(tr("Удалить"), warehouseBox);
    warehouseToolbar->addWidget(addWhBtn);
    warehouseToolbar->addWidget(editWhBtn);
    warehouseToolbar->addWidget(delWhBtn);
    warehouseToolbar->addStretch();
    warehouseLayout->addLayout(warehouseToolbar);

    m_warehouseModel = new QSqlTableModel(this);
    m_warehouseModel->setTable("warehouses");
    m_warehouseModel->setEditStrategy(QSqlTableModel::OnManualSubmit);
    m_warehouseModel->setSort(WColName, Qt::AscendingOrder);

    m_warehouseTable = new QTableView(warehouseBox);
    m_warehouseTable->setModel(m_warehouseModel);
    m_warehouseTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_warehouseTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_warehouseTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_warehouseTable->horizontalHeader()->setStretchLastSection(true);
    warehouseLayout->addWidget(m_warehouseTable);
    splitter->addWidget(warehouseBox);

    // --- Правая панель: места хранения выбранного склада ---
    auto *locationBox = new QGroupBox(tr("Места хранения (полки/зоны)"), this);
    auto *locationLayout = new QVBoxLayout(locationBox);
    auto *locationToolbar = new QHBoxLayout();
    auto *addLocBtn = new QPushButton(tr("Добавить"), locationBox);
    auto *editLocBtn = new QPushButton(tr("Изменить"), locationBox);
    auto *delLocBtn = new QPushButton(tr("Удалить"), locationBox);
    locationToolbar->addWidget(addLocBtn);
    locationToolbar->addWidget(editLocBtn);
    locationToolbar->addWidget(delLocBtn);
    locationToolbar->addStretch();
    locationLayout->addLayout(locationToolbar);

    m_locationModel = new QSqlTableModel(this);
    m_locationModel->setTable("locations");
    m_locationModel->setEditStrategy(QSqlTableModel::OnManualSubmit);
    m_locationModel->setSort(LColCode, Qt::AscendingOrder);

    m_locationTable = new QTableView(locationBox);
    m_locationTable->setModel(m_locationModel);
    m_locationTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_locationTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_locationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_locationTable->horizontalHeader()->setStretchLastSection(true);
    locationLayout->addWidget(m_locationTable);
    splitter->addWidget(locationBox);

    refresh();

    connect(addWhBtn, &QPushButton::clicked, this, &WarehousesTab::addWarehouse);
    connect(editWhBtn, &QPushButton::clicked, this, &WarehousesTab::editWarehouse);
    connect(delWhBtn, &QPushButton::clicked, this, &WarehousesTab::deleteWarehouse);
    connect(addLocBtn, &QPushButton::clicked, this, &WarehousesTab::addLocation);
    connect(editLocBtn, &QPushButton::clicked, this, &WarehousesTab::editLocation);
    connect(delLocBtn, &QPushButton::clicked, this, &WarehousesTab::deleteLocation);
    connect(m_warehouseTable->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &WarehousesTab::onWarehouseSelectionChanged);
}

void WarehousesTab::refresh()
{
    m_warehouseModel->select();
    m_warehouseModel->setHeaderData(WColName, Qt::Horizontal, tr("Название"));
    m_warehouseModel->setHeaderData(WColAddress, Qt::Horizontal, tr("Адрес"));
    m_warehouseTable->setColumnHidden(WColId, true);
    refreshLocations();
}

int WarehousesTab::selectedWarehouseId() const
{
    const auto sel = m_warehouseTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return -1;
    return m_warehouseModel->record(sel.first().row()).value("id").toInt();
}

void WarehousesTab::refreshLocations()
{
    const int whId = selectedWarehouseId();
    if (whId < 0) {
        m_locationModel->setFilter("0 = 1");
    } else {
        m_locationModel->setFilter(QStringLiteral("warehouse_id = %1").arg(whId));
    }
    m_locationModel->select();
    m_locationModel->setHeaderData(LColCode, Qt::Horizontal, tr("Код"));
    m_locationModel->setHeaderData(LColDescription, Qt::Horizontal, tr("Описание"));
    m_locationTable->setColumnHidden(LColId, true);
    m_locationTable->setColumnHidden(LColWarehouseId, true);
}

void WarehousesTab::onWarehouseSelectionChanged()
{
    refreshLocations();
}

void WarehousesTab::addWarehouse()
{
    FormDialog dlg(tr("Новый склад"), {{tr("Название"), "", false}, {tr("Адрес"), "", false}}, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const auto v = dlg.values();
    if (v[0].trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Название склада обязательно."));
        return;
    }

    QSqlRecord rec = m_warehouseModel->record();
    rec.setValue("name", v[0].trimmed());
    rec.setValue("address", v[1]);
    rec.remove(rec.indexOf("id"));
    if (!m_warehouseModel->insertRecord(-1, rec) || !m_warehouseModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_warehouseModel->lastError().text());
        m_warehouseModel->revertAll();
        return;
    }
    refresh();
    emit warehousesChanged();
}

void WarehousesTab::editWarehouse()
{
    const auto sel = m_warehouseTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    const int row = sel.first().row();
    const QSqlRecord rec = m_warehouseModel->record(row);

    FormDialog dlg(tr("Изменить склад"),
                    {{tr("Название"), rec.value("name").toString(), false},
                     {tr("Адрес"), rec.value("address").toString(), false}},
                    this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const auto v = dlg.values();

    m_warehouseModel->setData(m_warehouseModel->index(row, WColName), v[0].trimmed());
    m_warehouseModel->setData(m_warehouseModel->index(row, WColAddress), v[1]);
    if (!m_warehouseModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_warehouseModel->lastError().text());
        m_warehouseModel->revertAll();
        return;
    }
    refresh();
    emit warehousesChanged();
}

void WarehousesTab::deleteWarehouse()
{
    const auto sel = m_warehouseTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Удалить склад"),
                               tr("Удалить склад со всеми его местами хранения, остатками и историей движений?"))
        != QMessageBox::Yes)
        return;
    m_warehouseModel->removeRow(sel.first().row());
    if (!m_warehouseModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_warehouseModel->lastError().text());
        m_warehouseModel->revertAll();
        return;
    }
    refresh();
    emit warehousesChanged();
}

void WarehousesTab::addLocation()
{
    const int whId = selectedWarehouseId();
    if (whId < 0) {
        QMessageBox::information(this, tr("Выберите склад"), tr("Сначала выберите склад слева."));
        return;
    }
    FormDialog dlg(tr("Новое место хранения"),
                    {{tr("Код (напр. A1-03)"), "", false}, {tr("Описание"), "", false}}, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const auto v = dlg.values();
    if (v[0].trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Код места хранения обязателен."));
        return;
    }

    QSqlRecord rec = m_locationModel->record();
    rec.setValue("warehouse_id", whId);
    rec.setValue("code", v[0].trimmed());
    rec.setValue("description", v[1]);
    rec.remove(rec.indexOf("id"));
    if (!m_locationModel->insertRecord(-1, rec) || !m_locationModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_locationModel->lastError().text());
        m_locationModel->revertAll();
        return;
    }
    refreshLocations();
}

void WarehousesTab::editLocation()
{
    const auto sel = m_locationTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    const int row = sel.first().row();
    const QSqlRecord rec = m_locationModel->record(row);

    FormDialog dlg(tr("Изменить место хранения"),
                    {{tr("Код"), rec.value("code").toString(), false},
                     {tr("Описание"), rec.value("description").toString(), false}},
                    this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const auto v = dlg.values();

    m_locationModel->setData(m_locationModel->index(row, LColCode), v[0].trimmed());
    m_locationModel->setData(m_locationModel->index(row, LColDescription), v[1]);
    if (!m_locationModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_locationModel->lastError().text());
        m_locationModel->revertAll();
        return;
    }
    refreshLocations();
}

void WarehousesTab::deleteLocation()
{
    const auto sel = m_locationTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Удалить место хранения"), tr("Удалить выбранное место хранения?"))
        != QMessageBox::Yes)
        return;
    m_locationModel->removeRow(sel.first().row());
    if (!m_locationModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_locationModel->lastError().text());
        m_locationModel->revertAll();
        return;
    }
    refreshLocations();
}
