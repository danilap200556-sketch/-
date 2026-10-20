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
#include "producteditor.h"
#include "productfinder.h"

#include <QDesktopServices>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenuBar>
#include <QSettings>
#include <QShortcut>
#include <QUrl>
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
    const bool canEdit = AuthService::canEdit(username); // false - роль "только просмотр"
    m_canEdit = canEdit;

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

    // Сайт склада работает с той же базой: адрес хранится на этом компьютере.
    auto *siteMenu = menuBar()->addMenu(tr("Сайт"));
    connect(siteMenu->addAction(tr("Открыть сайт склада")), &QAction::triggered, this, &MainWindow::openSite);
    connect(siteMenu->addAction(tr("Адрес сайта...")), &QAction::triggered, this, &MainWindow::editSiteUrl);

    // Строка сканера: штрихкод (или артикул) -> карточка товара, где можно сразу поправить всё, что не так.
    m_scan = new QLineEdit(this);
    m_scan->setPlaceholderText(tr("Сканер / поиск товара (Ctrl+F): штрихкод или артикул"));
    m_scan->setClearButtonEnabled(true);
    m_scan->setMinimumWidth(340);
    menuBar()->setCornerWidget(m_scan, Qt::TopRightCorner);
    connect(m_scan, &QLineEdit::returnPressed, this, &MainWindow::scanEntered);
    auto *focusScan = new QShortcut(QKeySequence::Find, this);
    connect(focusScan, &QShortcut::activated, this, [this]() {
        m_scan->setFocus();
        m_scan->selectAll();
    });

    auto *tabs = new QTabWidget(this);
    m_tabs = tabs;
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
    tabs->addTab(m_importTab, canEdit ? tr("Импорт") : tr("Экспорт"));
    if (canEdit) {
        // Яндекс Маркет (ключи кабинетов, отправка остатков) и заказы - только для тех, кто может редактировать.
        tabs->addTab(m_marketTab, tr("Яндекс Маркет"));
        tabs->addTab(m_ordersTab, tr("Заказы Маркета"));
    } else {
        setWindowTitle(windowTitle() + tr(" (только просмотр)"));
        m_productsTab->setReadOnly(true);
        m_warehousesTab->setReadOnly(true);
        m_stockTab->setReadOnly(true);
        m_importTab->setReadOnly(true);
        m_marketTab->hide();
        m_ordersTab->hide();
    }
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
    connect(m_stockTab, &StockTab::productEdited, this, [this]() {
        m_productsTab->refresh();
        m_labelsTab->refresh();
    });
    connect(m_ordersTab, &OrdersTab::productEdited, this, [this]() {
        m_productsTab->refresh();
        m_stockTab->refresh();
        m_labelsTab->refresh();
    });

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

void MainWindow::refreshAfterProductEdit()
{
    m_productsTab->refresh();
    m_stockTab->refresh();
    m_labelsTab->refresh();
}

void MainWindow::scanEntered()
{
    const QString text = m_scan->text().trimmed();
    if (text.isEmpty())
        return;
    const ProductFinder::Result r = ProductFinder::find(text);
    if (r.ids.size() == 1) {
        // Нашёлся ровно один товар - сразу его карточка (для "только просмотр" - без правки).
        if (ProductEditor::edit(r.ids.first(), this, !m_canEdit))
            refreshAfterProductEdit();
    } else {
        // Ничего или несколько: показываем список товаров с этим поиском; незнакомый штрихкод
        // там же можно привязать к нужному товару.
        m_tabs->setCurrentWidget(m_productsTab);
        m_productsTab->searchFor(text);
    }
    m_scan->selectAll();
}

void MainWindow::editSiteUrl()
{
    QSettings settings;
    bool ok = false;
    const QString url = QInputDialog::getText(
        this, tr("Адрес сайта"),
        tr("Адрес сайта склада (например, https://sklad.example.ru). Хранится на этом компьютере, "
           "нужен только для пункта «Открыть сайт склада»:"),
        QLineEdit::Normal, settings.value("siteUrl").toString(), &ok).trimmed();
    if (!ok)
        return;
    const QUrl parsed(url);
    if (!url.isEmpty() && (!parsed.isValid() || (parsed.scheme() != "https" && parsed.scheme() != "http") ||
                           parsed.host().isEmpty())) {
        QMessageBox::warning(this, tr("Неверный адрес"), tr("Адрес должен начинаться с https:// (или http://)."));
        return;
    }
    settings.setValue("siteUrl", url);
}

void MainWindow::openSite()
{
    const QString url = QSettings().value("siteUrl").toString();
    if (url.isEmpty()) {
        editSiteUrl();
        return;
    }
    QDesktopServices::openUrl(QUrl(url));
}
