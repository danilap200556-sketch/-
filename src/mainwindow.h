#pragma once

#include <QMainWindow>

class ProductsTab;
class WarehousesTab;
class StockTab;
class LabelsTab;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    ProductsTab *m_productsTab;
    WarehousesTab *m_warehousesTab;
    StockTab *m_stockTab;
    LabelsTab *m_labelsTab;
};
