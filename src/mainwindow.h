#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QSet>

#include "theme.h"

class ProductsTab;
class WarehousesTab;
class StockTab;
class StocktakeTab;
class MarkingTab;
class LabelsTab;
class ImportTab;
class MarketTab;
class CardsTab;
class DocumentsTab;
class OrdersTab;
class SitePricesTab;
class UsersTab;
class UpdatesTab;
class Updater;
class QTimer;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QToolButton;
class StockBar;
class TopNav;
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
    // Раздел: вкладка в верхнем меню + страница под ним.
    // group - группа разделов в верхней полосе (СКЛАД, МАРКЕТПЛЕЙС...): ставится у первого раздела группы.
    void addPage(const QString &title, Theme::Icon icon, QWidget *page, const QString &group = QString());
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
    // Обновления программы: проверка при запуске и раз в несколько часов, предложение установить, итог после перезапуска.
    void setupUpdates();
    void autoCheckUpdates();
    void updateChecked(bool ok);
    void offerUpdate();
    void installUpdate();
    void showUpdateResult();

    TopNav *m_nav = nullptr;          // верхнее меню: группы разделов и вкладки
    QWidget *m_content = nullptr;     // заголовок, полоса остатков и страница: эту область проявляет переход
    QStackedWidget *m_pages = nullptr;
    QLabel *m_pageTitle = nullptr;
    StockBar *m_stockBar = nullptr; // «Всего пар» и пары по складам: над каждой страницей
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
    StocktakeTab *m_stocktakeTab;
    MarkingTab *m_markingTab = nullptr;
    LabelsTab *m_labelsTab;
    ImportTab *m_importTab;
    MarketTab *m_marketTab;
    CardsTab *m_cardsTab;
    DocumentsTab *m_documentsTab;
    OrdersTab *m_ordersTab;
    SitePricesTab *m_sitePricesTab = nullptr;
    UsersTab *m_usersTab = nullptr;
    Updater *m_updater = nullptr;
    UpdatesTab *m_updatesTab = nullptr;
    int m_updatesIndex = -1;
    QTimer *m_updateTimer = nullptr;
    bool m_autoChecking = false;
    int m_offeredBuild = 0;
};
