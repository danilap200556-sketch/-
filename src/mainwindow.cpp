#include "mainwindow.h"
#include "productstab.h"
#include "warehousestab.h"
#include "stocktab.h"
#include "labelstab.h"
#include "importtab.h"
#include "connectiondialog.h"
#include "markettab.h"
#include "cardstab.h"
#include "documentstab.h"
#include "documents.h"
#include "marketautosync.h"
#include "ordertab.h"
#include "userstab.h"
#include "applog.h"
#include "authservice.h"
#include "database.h"
#include "producteditor.h"
#include "productfinder.h"
#include "productsuggest.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QMessageBox>

MainWindow::MainWindow(const QString &username, QWidget *parent)
    : QMainWindow(parent)
{
    Database::setCurrentUser(username); // автор документов остатков
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

    fileMenu->addAction(tr("Показать файл журнала (лог)"), this, []() {
        const QString path = AppLog::path();
        if (!path.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    bool isAdmin = false;
    bool canEdit = false; // false - роль "только просмотр"
    AuthService::permissions(username, &isAdmin, &canEdit);
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

    auto *viewMenu = menuBar()->addMenu(tr("Вид"));
    viewMenu->addAction(tr("Светлая / тёмная тема"), QKeySequence(Qt::CTRL | Qt::Key_T), this, &MainWindow::toggleTheme);

    // Каркас окна: слева тёмная боковая панель с разделами, справа заголовок раздела со строкой
    // сканера и сама страница.
    auto *central = new QWidget(this);
    auto *rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *sideFrame = new QFrame(central);
    sideFrame->setObjectName("sidebarFrame");
    sideFrame->setFixedWidth(236);
    auto *sideLayout = new QVBoxLayout(sideFrame);
    sideLayout->setContentsMargins(10, 18, 10, 12);
    sideLayout->setSpacing(4);

    auto *brand = new QHBoxLayout();
    brand->setContentsMargins(8, 0, 8, 0);
    brand->setSpacing(10);
    auto *logo = new QLabel(sideFrame);
    logo->setPixmap(Theme::logo(36));
    logo->setFixedSize(36, 36);
    brand->addWidget(logo);
    auto *brandText = new QVBoxLayout();
    brandText->setSpacing(0);
    auto *brandTitle = new QLabel(tr("Склад"), sideFrame);
    QFont bf = brandTitle->font();
    bf.setPointSizeF(13);
    bf.setBold(true);
    brandTitle->setFont(bf);
    auto *brandSub = new QLabel(tr("учёт и Маркет"), sideFrame);
    brandSub->setObjectName("sideMuted");
    brandText->addWidget(brandTitle);
    brandText->addWidget(brandSub);
    brand->addLayout(brandText);
    brand->addStretch(1);
    sideLayout->addLayout(brand);
    sideLayout->addSpacing(14);

    m_sidebar = new QListWidget(sideFrame);
    m_sidebar->setObjectName("sidebar");
    m_sidebar->setIconSize(QSize(20, 20));
    m_sidebar->setFocusPolicy(Qt::NoFocus);
    m_sidebar->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sidebar->setSelectionMode(QAbstractItemView::SingleSelection);
    sideLayout->addWidget(m_sidebar, 1);

    auto *userCard = new QFrame(sideFrame);
    userCard->setObjectName("userCard");
    auto *userLayout = new QHBoxLayout(userCard);
    userLayout->setContentsMargins(12, 8, 8, 8);
    userLayout->setSpacing(6);
    auto *userText = new QVBoxLayout();
    userText->setSpacing(0);
    auto *userName = new QLabel(username, userCard);
    QFont uf = userName->font();
    uf.setBold(true);
    userName->setFont(uf);
    auto *userRole = new QLabel(isAdmin ? tr("Администратор") : (canEdit ? tr("Сотрудник") : tr("Только просмотр")), userCard);
    userRole->setObjectName("sideMuted");
    userText->addWidget(userName);
    userText->addWidget(userRole);
    userLayout->addLayout(userText, 1);
    m_themeButton = new QToolButton(userCard);
    m_themeButton->setAutoRaise(true);
    m_themeButton->setCursor(Qt::PointingHandCursor);
    m_themeButton->setIconSize(QSize(20, 20));
    m_themeButton->setToolTip(tr("Светлая / тёмная тема (Ctrl+T)"));
    connect(m_themeButton, &QToolButton::clicked, this, &MainWindow::toggleTheme);
    userLayout->addWidget(m_themeButton);
    sideLayout->addWidget(userCard);
    rootLayout->addWidget(sideFrame);

    auto *content = new QWidget(central);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(22, 16, 22, 16);
    contentLayout->setSpacing(12);

    auto *header = new QHBoxLayout();
    m_pageTitle = new QLabel(content);
    m_pageTitle->setObjectName("pageTitle");
    header->addWidget(m_pageTitle);
    header->addStretch(1);
    // Строка сканера: штрихкод (или артикул) -> карточка товара, где можно сразу поправить всё, что не так.
    m_scan = new QLineEdit(content);
    m_scan->setPlaceholderText(tr("Сканер / поиск товара (Ctrl+F): штрихкод или артикул"));
    m_scan->setClearButtonEnabled(true);
    m_scan->setMinimumWidth(380);
    m_scan->addAction(Theme::icon(Theme::Icon::Search, Theme::colors().muted, 18), QLineEdit::LeadingPosition);
    header->addWidget(m_scan);
    contentLayout->addLayout(header);
    connect(m_scan, &QLineEdit::returnPressed, this, &MainWindow::scanEntered);
    // Подсказки по мере ввода (часть артикула, названия, штрихкода): выбранный товар открывается карточкой.
    auto *scanSuggest = new ProductSuggest(m_scan, this);
    connect(scanSuggest, &ProductSuggest::chosen, this, [this](int id, const QString &, const QString &) {
        m_scan->clear();
        if (ProductEditor::edit(id, this, !m_canEdit))
            refreshAfterProductEdit();
    });
    auto *focusScan = new QShortcut(QKeySequence::Find, this);
    connect(focusScan, &QShortcut::activated, this, [this]() {
        m_scan->setFocus();
        m_scan->selectAll();
    });

    m_pages = new QStackedWidget(content);
    contentLayout->addWidget(m_pages, 1);
    rootLayout->addWidget(content, 1);
    setCentralWidget(central);
    updateThemeButton();

    m_productsTab = new ProductsTab(this);
    m_warehousesTab = new WarehousesTab(this);
    m_stockTab = new StockTab(this);
    m_labelsTab = new LabelsTab(this);
    m_importTab = new ImportTab(this);
    m_marketTab = new MarketTab(isAdmin, this);
    m_cardsTab = new CardsTab(this);
    m_documentsTab = new DocumentsTab(this);
    m_ordersTab = new OrdersTab(this);

    addPage(tr("Товары"), Theme::Icon::Products, m_productsTab);
    addPage(tr("Склады"), Theme::Icon::Warehouses, m_warehousesTab);
    addPage(tr("Остатки и движения"), Theme::Icon::Stock, m_stockTab);
    addPage(tr("Документы"), Theme::Icon::Documents, m_documentsTab);
    addPage(tr("Этикетки"), Theme::Icon::Labels, m_labelsTab);
    addPage(canEdit ? tr("Импорт") : tr("Экспорт"), Theme::Icon::Import, m_importTab);
    if (canEdit) {
        // Яндекс Маркет (ключи кабинетов, отправка остатков) и заказы - только для тех, кто может редактировать.
        addPage(tr("Яндекс Маркет"), Theme::Icon::Market, m_marketTab);
        addPage(tr("Карточки Маркета"), Theme::Icon::Cards, m_cardsTab);
        addPage(tr("Заказы Маркета"), Theme::Icon::Orders, m_ordersTab);
        // Автообновление остатков по расписанию и значок в трее, пока оно включено.
        m_autoSync = new MarketAutoSync(this);
        m_marketTab->setAutoSync(m_autoSync);
        setupTray();
    } else {
        setWindowTitle(windowTitle() + tr(" (только просмотр)"));
        m_productsTab->setReadOnly(true);
        m_warehousesTab->setReadOnly(true);
        m_stockTab->setReadOnly(true);
        m_documentsTab->setReadOnly(true);
        m_importTab->setReadOnly(true);
        m_marketTab->hide();
        m_cardsTab->hide();
        m_ordersTab->hide();
    }
    if (isAdmin) {
        m_usersTab = new UsersTab(username, this);
        addPage(tr("Пользователи"), Theme::Icon::Users, m_usersTab);
    }

    // Изменения товаров/складов должны отражаться там, где на них ссылаются другие страницы
    // (комбобоксы, сводные таблицы): помечаем такие страницы устаревшими, а обновляются они,
    // когда на них перейдут (или сразу, если страница открыта).
    connect(m_productsTab, &ProductsTab::productsChanged, this, [this]() {
        markDirty(m_stockTab);
        markDirty(m_labelsTab);
    });
    connect(m_warehousesTab, &WarehousesTab::warehousesChanged, this, [this]() {
        markDirty(m_stockTab);
        markDirty(m_labelsTab);
        markDirty(m_importTab);
        markDirty(m_marketTab);
    });
    connect(m_importTab, &ImportTab::dataImported, this, [this]() {
        markDirty(m_stockTab);
        markDirty(m_productsTab);
        markDirty(m_labelsTab);
    });
    connect(m_stockTab, &StockTab::documentCreated, this, [this](int id) {
        if (id <= 0)
            return;
        markDirty(m_documentsTab);
        const Documents::Info d = Documents::get(id);
        statusBar()->showMessage(tr("Создан документ: %1. Открыть и при необходимости отменить целиком - раздел «Документы».")
                                     .arg(Documents::title(d)), 15000);
    });
    connect(m_stockTab, &StockTab::productEdited, this, [this]() {
        markDirty(m_productsTab);
        markDirty(m_labelsTab);
    });
    connect(m_ordersTab, &OrdersTab::productEdited, this, [this]() {
        markDirty(m_productsTab);
        markDirty(m_stockTab);
        markDirty(m_labelsTab);
    });

    m_clock.start();
    connect(m_sidebar, &QListWidget::currentRowChanged, this, &MainWindow::pageChanged);
    m_sidebar->setCurrentRow(0);
    // Окно сначала рисуется, и только потом подгружается список товаров.
    QTimer::singleShot(0, this, [this]() { ensureLoaded(m_pages->currentWidget()); });
}

void MainWindow::addPage(const QString &title, Theme::Icon icon, QWidget *page)
{
    // Значок бледный, а у выбранного раздела - белый.
    QIcon ic;
    const QPixmap normal = Theme::icon(icon, QColor("#9aa6c4"), 20).pixmap(QSize(20, 20));
    const QPixmap selected = Theme::icon(icon, QColor("#ffffff"), 20).pixmap(QSize(20, 20));
    ic.addPixmap(normal, QIcon::Normal, QIcon::Off);
    ic.addPixmap(selected, QIcon::Selected, QIcon::Off);
    ic.addPixmap(selected, QIcon::Active, QIcon::Off);
    auto *item = new QListWidgetItem(ic, title, m_sidebar);
    item->setSizeHint(QSize(0, 42));
    m_pages->addWidget(page);
}

void MainWindow::showPage(QWidget *page)
{
    const int i = m_pages->indexOf(page);
    if (i >= 0)
        m_sidebar->setCurrentRow(i);
}

void MainWindow::pageChanged(int row)
{
    if (row < 0)
        return;
    // Уходим со страницы карточек Маркета - сохраняем то, что в ней правили.
    if (m_pages->currentWidget() == m_cardsTab && m_pages->widget(row) != m_cardsTab && !m_cardsTab->saveIfDirty()) {
        const QSignalBlocker block(m_sidebar);
        m_sidebar->setCurrentRow(m_pages->currentIndex());
        return;
    }
    m_pages->setCurrentIndex(row);
    m_pageTitle->setText(m_sidebar->item(row)->text());
    ensureLoaded(m_pages->widget(row));
}

void MainWindow::refreshPage(QWidget *w)
{
    if (w == m_productsTab)
        m_productsTab->refresh();
    else if (w == m_warehousesTab)
        m_warehousesTab->refresh();
    else if (w == m_stockTab)
        m_stockTab->refresh();
    else if (w == m_documentsTab)
        m_documentsTab->refresh();
    else if (w == m_labelsTab)
        m_labelsTab->refresh();
    else if (w == m_importTab)
        m_importTab->refresh();
    else if (w == m_marketTab)
        m_marketTab->refresh();
    else if (w == m_cardsTab)
        m_cardsTab->refresh();
    else if (w == m_ordersTab)
        m_ordersTab->refresh();
    else if (w == m_usersTab)
        m_usersTab->refresh();
    m_dirty.remove(w);
    m_loadedAt[w] = m_clock.elapsed();
}

void MainWindow::ensureLoaded(QWidget *w)
{
    if (!w)
        return;
    // Списки, которые правят и с других компьютеров, перечитываем и без пометки, если им больше 20 секунд;
    // список товаров - только по пометке (иначе при возврате на него сбрасывалось бы выделение).
    constexpr qint64 kStaleMs = 20000;
    const bool never = !m_loadedAt.contains(w);
    // Документы перечитываем при каждом заходе: в них добавляются и записи с других компьютеров.
    const bool stale = w != m_productsTab && w != m_warehousesTab &&
                       (w == m_documentsTab || m_clock.elapsed() - m_loadedAt.value(w) > kStaleMs);
    if (never || stale || m_dirty.contains(w))
        refreshPage(w);
}

void MainWindow::markDirty(QWidget *w)
{
    m_dirty.insert(w);
    if (m_pages->currentWidget() == w)
        ensureLoaded(w);
}

void MainWindow::toggleTheme()
{
    Theme::apply(Theme::current() == Theme::Mode::Dark ? Theme::Mode::Light : Theme::Mode::Dark);
    updateThemeButton();
}

void MainWindow::updateThemeButton()
{
    // Показываем, на какую тему переключит нажатие.
    const bool dark = Theme::current() == Theme::Mode::Dark;
    m_themeButton->setIcon(Theme::icon(dark ? Theme::Icon::Sun : Theme::Icon::Moon, QColor("#c5cde0"), 20));
}

void MainWindow::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    QApplication::setQuitOnLastWindowClosed(false); // выходим сами (closeEvent / «Выход» в трее)
    m_tray = new QSystemTrayIcon(QIcon(Theme::logo(32)), this);
    auto *menu = new QMenu(this);
    menu->addAction(tr("Открыть окно"), this, &MainWindow::restoreFromTray);
    menu->addAction(tr("Обновить остатки на Маркете сейчас"), m_autoSync, &MarketAutoSync::runNow);
    menu->addSeparator();
    menu->addAction(tr("Выход"), this, &MainWindow::quitApp);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            restoreFromTray();
    });
    connect(m_autoSync, &MarketAutoSync::problem, this, [this](const QString &text) {
        if (m_tray->isVisible() && !isActiveWindow())
            m_tray->showMessage(tr("Маркет: остатки не обновились"), text, QSystemTrayIcon::Warning, 10000);
    });
    auto update = [this]() {
        // Значок в трее виден, пока включено автообновление.
        const bool on = m_autoSync->settings().enabled;
        m_tray->setToolTip(tr("Склад. %1").arg(m_autoSync->statusText()).left(250));
        if (m_tray->isVisible() != on)
            m_tray->setVisible(on);
    };
    connect(m_autoSync, &MarketAutoSync::statusChanged, this, update);
    update();
}

void MainWindow::restoreFromTray()
{
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::quitApp()
{
    m_quitting = true;
    if (m_tray)
        m_tray->hide();
    close();
    QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_canEdit && !m_cardsTab->saveIfDirty()) {
        event->ignore(); // карточка не сохранилась - окно не закрываем, чтобы не потерять правки
        return;
    }
    if (!m_quitting && m_tray && m_tray->isVisible() && m_autoSync && m_autoSync->settings().enabled) {
        event->ignore();
        hide();
        if (!m_trayHintShown) {
            m_trayHintShown = true;
            m_tray->showMessage(tr("Склад работает в фоне"),
                                tr("Остатки на Маркете обновляются по расписанию. Открыть окно - значок здесь, "
                                   "выйти совсем - правый щелчок → «Выход»."),
                                QSystemTrayIcon::Information, 8000);
        }
        return;
    }
    event->accept();
    QApplication::quit();
}

void MainWindow::refreshAfterProductEdit()
{
    markDirty(m_productsTab);
    markDirty(m_stockTab);
    markDirty(m_labelsTab);
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
        showPage(m_productsTab);
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
