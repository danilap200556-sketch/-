#include "mainwindow.h"
#include "productstab.h"
#include "warehousestab.h"
#include "stocktab.h"
#include "labelstab.h"
#include "importtab.h"
#include "connectiondialog.h"
#include "markettab.h"
#include "ordertab.h"
#include "userstab.h"
#include "authservice.h"

#include <QMenuBar>
#include <QMessageBox>
#include <QTabWidget>

MainWindow::MainWindow(const QString &username, QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Управление складом — %1").arg(username));
    resize(1200, 800);

    auto *fileMenu = menuBar()->addMenu(tr("Файл"));
    auto *connectionAction = fileMenu->addAction(tr("Настройки подключения к серверу..."));
    connect(connectionAction, &QAction::triggered, this, [this]() {
        ConnectionDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            dlg.config().save();
            QMessageBox::information(this, tr("Сохранено"),
                                      tr("Настройки сохранены. Перезапустите приложение, чтобы применить их."));
        }
    });

    const bool isAdmin = AuthService::isAdmin(username);

    auto *accountMenu = menuBar()->addMenu(tr("Учётная запись"));
    auto *passwordAction = accountMenu->addAction(tr("Сменить мой пароль..."));
    connect(passwordAction, &QAction::triggered, this, [this, username]() {
        for (const auto &u : AuthService::listUsers()) {
            if (u.username == username) {
                UsersTab::changePassword(this, u.id, username);
                return;
            }
        }
    });

    auto *tabs = new QTabWidget(this);
    setCentralWidget(tabs);

    m_productsTab = new ProductsTab(this);
    m_warehousesTab = new WarehousesTab(this);
    m_stockTab = new StockTab(this);
    m_labelsTab = new LabelsTab(this);
    m_importTab = new ImportTab(this);
    m_marketTab = new MarketTab(isAdmin, this);
    m_ordersTab = new OrdersTab(this);

    tabs->addTab(m_productsTab, tr("Товары"));
    tabs->addTab(m_warehousesTab, tr("Склады"));
    tabs->addTab(m_stockTab, tr("Остатки и движения"));
    tabs->addTab(m_labelsTab, tr("Этикетки"));
    tabs->addTab(m_importTab, tr("Импорт"));
    tabs->addTab(m_marketTab, tr("Яндекс Маркет"));
    tabs->addTab(m_ordersTab, tr("Заказы Маркета"));
    if (isAdmin) {
        m_usersTab = new UsersTab(username, this);
        tabs->addTab(m_usersTab, tr("Пользователи"));
    }

    // Изменения товаров/складов должны сразу отражаться там, где на них
    // ссылаются другие вкладки (комбобоксы, сводные таблицы).
    connect(m_productsTab, &ProductsTab::productsChanged, m_stockTab, &StockTab::refresh);
    connect(m_productsTab, &ProductsTab::productsChanged, m_labelsTab, &LabelsTab::refresh);
    connect(m_warehousesTab, &WarehousesTab::warehousesChanged, m_stockTab, &StockTab::refresh);
    connect(m_warehousesTab, &WarehousesTab::warehousesChanged, m_labelsTab, &LabelsTab::refresh);
    connect(m_warehousesTab, &WarehousesTab::warehousesChanged, m_importTab, &ImportTab::refresh);
    connect(m_importTab, &ImportTab::dataImported, m_stockTab, &StockTab::refresh);
    connect(m_importTab, &ImportTab::dataImported, m_productsTab, &ProductsTab::refresh);
    connect(m_importTab, &ImportTab::dataImported, m_labelsTab, &LabelsTab::refresh);
    connect(m_warehousesTab, &WarehousesTab::warehousesChanged, m_marketTab, &MarketTab::refresh);

    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index) {
        QWidget *w = tabs->widget(index);
        if (w == m_stockTab)
            m_stockTab->refresh();
        else if (w == m_labelsTab)
            m_labelsTab->refresh();
        else if (w == m_importTab)
            m_importTab->refresh();
        else if (w == m_ordersTab)
            m_ordersTab->refresh();
        else if (w == m_usersTab)
            m_usersTab->refresh();
    });
}
