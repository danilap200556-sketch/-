#pragma once

#include <QMainWindow>

class ProductsTab;
class WarehousesTab;
class StockTab;
class LabelsTab;
class ImportTab;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QString &username, QWidget *parent = nullptr);

private:
    ProductsTab *m_productsTab;
    WarehousesTab *m_warehousesTab;
    StockTab *m_stockTab;
    LabelsTab *m_labelsTab;
    ImportTab *m_importTab;
};
