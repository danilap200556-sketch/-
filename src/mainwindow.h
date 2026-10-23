#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QSet>

#include "theme.h"

class ProductsTab;
class WarehousesTab;
class StockTab;
class LabelsTab;
class ImportTab;
class MarketTab;
class CardsTab;
class DocumentsTab;
class OrdersTab;
class UsersTab;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QToolButton;
class QSystemTrayIcon;
class MarketAutoSync;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QString &username, QWidget *parent = nullptr);

protected:
    // Если включено автообновление остатков Маркета, окно прячется в трей, а не закрывает программу.
    void closeEvent(QCloseEvent *event) override;

private slots:
    void scanEntered();
    void openSite();
    void editSiteUrl();

private:
    void refreshAfterProductEdit();
    // Раздел боковой панели: строка в списке + страница справа.
    void addPage(const QString &title, Theme::Icon icon, QWidget *page);
    void showPage(QWidget *page);
    void pageChanged(int row);
    // Данные страницы подгружаются при первом показе и потом, только если что-то изменилось (или
    // они давно не обновлялись): лишние запросы к удалённой базе - это ожидание у человека.
    void ensureLoaded(QWidget *page);
    void refreshPage(QWidget *page);
    void markDirty(QWidget *page);
    void toggleTheme();
    void updateThemeButton();
    void setupTray();
    void restoreFromTray();
    void quitApp();

    QListWidget *m_sidebar = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_pageTitle = nullptr;
    QSet<QWidget *> m_dirty;
    QHash<QWidget *, qint64> m_loadedAt;
    QElapsedTimer m_clock;
    MarketAutoSync *m_autoSync = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    bool m_quitting = false;
    bool m_trayHintShown = false;
    QToolButton *m_themeButton = nullptr;
    QLineEdit *m_scan = nullptr;
    bool m_canEdit = true;
    ProductsTab *m_productsTab;
    WarehousesTab *m_warehousesTab;
    StockTab *m_stockTab;
    LabelsTab *m_labelsTab;
    ImportTab *m_importTab;
    MarketTab *m_marketTab;
    CardsTab *m_cardsTab;
    DocumentsTab *m_documentsTab;
    OrdersTab *m_ordersTab;
    UsersTab *m_usersTab = nullptr;
};
