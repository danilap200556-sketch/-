#pragma once

#include <QMainWindow>

class ProductsTab;
class WarehousesTab;
class StockTab;
class LabelsTab;
class ImportTab;
class MarketTab;
class OrdersTab;
class UsersTab;
class QLineEdit;
class QTabWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QString &username, QWidget *parent = nullptr);

private slots:
    void scanEntered();
    void openSite();
    void editSiteUrl();

private:
    void refreshAfterProductEdit();

    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_scan = nullptr;
    bool m_canEdit = true;
    ProductsTab *m_productsTab;
    WarehousesTab *m_warehousesTab;
    StockTab *m_stockTab;
    LabelsTab *m_labelsTab;
    ImportTab *m_importTab;
    MarketTab *m_marketTab;
    OrdersTab *m_ordersTab;
    UsersTab *m_usersTab = nullptr;
};
