#pragma once

#include <QWidget>

class QTableView;
class QSqlTableModel;

// Вкладка "Склады": список складов слева, места хранения (полки/зоны)
// выбранного склада - справа. Поддерживает несколько складов, на которых
// может храниться один и тот же артикул.
class WarehousesTab : public QWidget
{
    Q_OBJECT
public:
    explicit WarehousesTab(QWidget *parent = nullptr);

    void refresh();

signals:
    void warehousesChanged();

private slots:
    void addWarehouse();
    void editWarehouse();
    void deleteWarehouse();
    void addLocation();
    void editLocation();
    void deleteLocation();
    void onWarehouseSelectionChanged();

private:
    int selectedWarehouseId() const;
    void refreshLocations();

    QTableView *m_warehouseTable;
    QSqlTableModel *m_warehouseModel;

    QTableView *m_locationTable;
    QSqlTableModel *m_locationModel;
};
