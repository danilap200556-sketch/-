#include "mainwindow.h"
#include "productstab.h"
#include "stockbar.h"
#include "warehousestab.h"
#include "stocktab.h"
#include "markingtab.h"
#include "stocktaketab.h"
#include "labelstab.h"
#include "importtab.h"
#include "connectiondialog.h"
#include "markettab.h"
#include "cardstab.h"
#include "documentstab.h"
#include "documents.h"
#include "marketautosync.h"
#include "ordertab.h"
#include "sitepricestab.h"
#include "userstab.h"
#include "applog.h"
#include "authservice.h"
#include "database.h"
#include "producteditor.h"
#include "productfinder.h"
#include "productsuggest.h"
#include "appversion.h"
#include "updater.h"
#include "updatestab.h"
#include "topnav.h"
#include "pagetransition.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QDebug>
#include <QLabel>
#include <QLineEdit>
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
#include <QPushButton>

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
    // Переходы между страницами, линия у вкладок и тени кнопок при наведении: кому мешает движение - выключает.
    auto *animAction = viewMenu->addAction(tr("Анимации интерфейса"));
    animAction->setCheckable(true);
    animAction->setChecked(Theme::animations());
    connect(animAction, &QAction::toggled, this, [](bool on) { Theme::setAnimations(on); });

    // Каркас окна: сверху тёмно-зелёная полоса с группами разделов, строкой сканера и пользователем, под ней вкладки
    // раздела; ниже - заголовок страницы, полоса «пар на складах» и сама страница.
    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_nav = new TopNav(central);
    rootLayout->addWidget(m_nav);

    // Строка сканера: штрихкод (или артикул) -> карточка товара, где можно сразу поправить всё, что не так.
    m_scan = new QLineEdit(m_nav->header());
    m_scan->setPlaceholderText(tr("Сканер / поиск товара (Ctrl+F)"));
    m_scan->setClearButtonEnabled(true);
    m_scan->setMinimumWidth(300);
    m_scan->setMaximumWidth(440);
    m_scan->addAction(Theme::icon(Theme::Icon::Search, QColor("#cfe0d6"), 18), QLineEdit::LeadingPosition);
    QPalette scanPalette = m_scan->palette();
    scanPalette.setColor(QPalette::PlaceholderText, QColor("#cfe0d6"));
    m_scan->setPalette(scanPalette);
    m_nav->headerTrailing()->addWidget(m_scan, 1);

    auto *userBox = new QVBoxLayout();
    userBox->setSpacing(0);
    auto *userName = new QLabel(username, m_nav->header());
    QFont uf = userName->font();
    uf.setBold(true);
    userName->setFont(uf);
    auto *userRole = new QLabel(isAdmin ? tr("Администратор") : (canEdit ? tr("Сотрудник") : tr("Только просмотр")), m_nav->header());
    userRole->setObjectName("headerMuted");
    userBox->addWidget(userName);
    userBox->addWidget(userRole);
    m_nav->headerTrailing()->addSpacing(6);
    m_nav->headerTrailing()->addLayout(userBox);
    m_themeButton = new QToolButton(m_nav->header());
    m_themeButton->setAutoRaise(true);
    m_themeButton->setCursor(Qt::PointingHandCursor);
    m_themeButton->setIconSize(QSize(20, 20));
    m_themeButton->setFixedSize(38, 38);
    m_themeButton->setToolTip(tr("Светлая / тёмная тема (Ctrl+T)"));
    connect(m_themeButton, &QToolButton::clicked, this, &MainWindow::toggleTheme);
    m_nav->headerTrailing()->addWidget(m_themeButton);

    m_content = new QWidget(central);
    m_content->setObjectName("content");
    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(24, 18, 24, 16);
    contentLayout->setSpacing(12);

    m_pageTitle = new QLabel(m_content);
    m_pageTitle->setObjectName("pageTitle");
    contentLayout->addWidget(m_pageTitle);
    // Сколько пар лежит на складах: на виду на любой странице.
    m_stockBar = new StockBar(m_content);
    contentLayout->addWidget(m_stockBar);
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

    m_pages = new QStackedWidget(m_content);
    contentLayout->addWidget(m_pages, 1);
    rootLayout->addWidget(m_content, 1);
    setCentralWidget(central);
    updateThemeButton();

    m_productsTab = new ProductsTab(this);
    m_warehousesTab = new WarehousesTab(this);
    m_stockTab = new StockTab(this);
    m_stocktakeTab = new StocktakeTab(this);
    m_labelsTab = new LabelsTab(this);
    m_importTab = new ImportTab(this);
    m_marketTab = new MarketTab(isAdmin, this);
    m_cardsTab = new CardsTab(this);
    m_documentsTab = new DocumentsTab(this);
    m_ordersTab = new OrdersTab(this);

    addPage(tr("Товары"), Theme::Icon::Products, m_productsTab, tr("СКЛАД"));
    addPage(tr("Склады"), Theme::Icon::Warehouses, m_warehousesTab);
    addPage(tr("Остатки и движения"), Theme::Icon::Stock, m_stockTab);
    if (canEdit)
        addPage(tr("Инвентаризация"), Theme::Icon::Stocktake, m_stocktakeTab);
    addPage(tr("Документы"), Theme::Icon::Documents, m_documentsTab);
    addPage(tr("Этикетки"), Theme::Icon::Labels, m_labelsTab);
    addPage(canEdit ? tr("Импорт") : tr("Экспорт"), Theme::Icon::Import, m_importTab);
    if (canEdit) {
        // Яндекс Маркет (ключи кабинетов, отправка остатков) и заказы - только для тех, кто может редактировать.
        addPage(tr("Яндекс Маркет"), Theme::Icon::Market, m_marketTab, tr("МАРКЕТПЛЕЙС"));
        addPage(tr("Карточки Маркета"), Theme::Icon::Cards, m_cardsTab);
        addPage(tr("Заказы Маркета"), Theme::Icon::Orders, m_ordersTab);
        // Свои цены и скидки для сайта магазина (проект web): не зависят от цены склада.
        m_sitePricesTab = new SitePricesTab(this);
        addPage(tr("Цены сайта"), Theme::Icon::Tag, m_sitePricesTab);
        // Коды маркировки «Честного знака»: только в программе, на сайте этой страницы нет.
        m_markingTab = new MarkingTab(this);
        addPage(tr("Честный знак"), Theme::Icon::Marking, m_markingTab, tr("МАРКИРОВКА"));
        // Автообновление остатков по расписанию и значок в трее, пока оно включено.
        m_autoSync = new MarketAutoSync(this);
        m_marketTab->setAutoSync(m_autoSync);
        setupTray();
    } else {
        setWindowTitle(windowTitle() + tr(" (только просмотр)"));
        m_productsTab->setReadOnly(true);
        m_warehousesTab->setReadOnly(true);
        m_stockTab->setReadOnly(true);
        m_stocktakeTab->hide();
        m_documentsTab->setReadOnly(true);
        m_importTab->setReadOnly(true);
        m_marketTab->hide();
        m_cardsTab->hide();
        m_ordersTab->hide();
    }
    m_documentsTab->setAdmin(isAdmin);
    connect(m_documentsTab, &DocumentsTab::stockChanged, this, [this]() {
        markDirty(m_stockTab);
        markDirty(m_labelsTab);
        markDirty(m_importTab);
    });
    if (isAdmin) {
        m_usersTab = new UsersTab(username, this);
        addPage(tr("Пользователи"), Theme::Icon::Users, m_usersTab, tr("СИСТЕМА"));
    }
    // Обновления программы - у всех: новая версия ставится на этот компьютер.
    m_updater = new Updater(this);
    m_updatesTab = new UpdatesTab(m_updater, this);
    addPage(tr("Обновления"), Theme::Icon::Update, m_updatesTab, tr("СИСТЕМА"));
    m_updatesIndex = m_nav->count() - 1;
    connect(m_updatesTab, &UpdatesTab::installRequested, this, &MainWindow::installUpdate);
    connect(m_updater, &Updater::checkFinished, this, [this](bool ok, const QString &) { updateChecked(ok); });

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
    connect(m_stocktakeTab, &StocktakeTab::stockChanged, this, [this]() {
        markDirty(m_stockTab);
        markDirty(m_labelsTab);
        markDirty(m_importTab);
    });
    connect(m_stocktakeTab, &StocktakeTab::documentCreated, this, [this](int id) {
        if (id <= 0)
            return;
        markDirty(m_documentsTab);
        const Documents::Info d = Documents::get(id);
        statusBar()->showMessage(tr("Создан документ: %1. Открыть и при необходимости отменить целиком - раздел «Документы».")
                                     .arg(Documents::title(d)), 15000);
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
        markDirty(m_documentsTab);
        markDirty(m_labelsTab);
    });

    m_clock.start();
    connect(m_nav, &TopNav::currentChanged, this, &MainWindow::pageChanged);
    m_nav->setCurrentIndex(0);
    // Окно сначала рисуется, и только потом подгружается список товаров.
    QTimer::singleShot(0, this, [this]() { ensureLoaded(m_pages->currentWidget()); });
    // Остатки правят и с других компьютеров: сводку «пар на складах» перечитываем раз в полминуты.
    auto *stockTimer = new QTimer(this);
    stockTimer->setInterval(30000);
    connect(stockTimer, &QTimer::timeout, m_stockBar, &StockBar::refresh);
    stockTimer->start();
    setupUpdates();
}

void MainWindow::addPage(const QString &title, Theme::Icon icon, QWidget *page, const QString &group)
{
    Q_UNUSED(icon) // во вкладках верхнего меню значков нет: строже и читается быстрее
    m_nav->addPage(title, group);
    m_pages->addWidget(page);
}

void MainWindow::showPage(QWidget *page)
{
    const int i = m_pages->indexOf(page);
    if (i >= 0)
        m_nav->setCurrentIndex(i);
}

void MainWindow::pageChanged(int row)
{
    if (row < 0)
        return;
    // Уходим со страницы карточек Маркета - сохраняем то, что в ней правили.
    if (m_pages->currentWidget() == m_cardsTab && m_pages->widget(row) != m_cardsTab && !m_cardsTab->saveIfDirty()) {
        const QSignalBlocker block(m_nav);
        m_nav->setCurrentIndex(m_pages->currentIndex());
        return;
    }
    m_pages->setCurrentIndex(row);
    m_stockBar->scheduleRefresh(); // после перерисовки страницы: сводка не задерживает переключение
    m_pageTitle->setText(m_nav->title(row));
    ensureLoaded(m_pages->widget(row));
    // Страница уже с данными: проявляем её плавно (снимок поверх, ~0,26 с). При запуске окна ещё не видно - без анимации.
    PageTransition::play(m_content);
}

void MainWindow::refreshPage(QWidget *w)
{
    if (w == m_productsTab)
        m_productsTab->refresh();
    else if (w == m_warehousesTab)
        m_warehousesTab->refresh();
    else if (w == m_stockTab)
        m_stockTab->refresh();
    else if (w == m_stocktakeTab)
        m_stocktakeTab->refresh();
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
    else if (w == m_sitePricesTab && m_sitePricesTab)
        m_sitePricesTab->refresh();
    else if (w == m_markingTab && m_markingTab)
        m_markingTab->refresh();
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
    if (m_stockBar)
        m_stockBar->scheduleRefresh(); // изменилось что-то, что может менять остатки
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
    m_themeButton->setIcon(Theme::icon(dark ? Theme::Icon::Sun : Theme::Icon::Moon, QColor("#ffffff"), 20));
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

void MainWindow::setupUpdates()
{
    Updater::cleanupOld(); // скачанное раньше и уже установленное
    qInfo().noquote() << "Версия программы:" << AppVersion::text();
    const int env = qEnvironmentVariableIntValue("INVENTORY_UPDATE_CHECK_DELAY_MS");
    QTimer::singleShot(env > 0 ? env : 6000, this, &MainWindow::autoCheckUpdates);
    m_updateTimer = new QTimer(this);
    m_updateTimer->setInterval(6 * 60 * 60 * 1000); // программа может работать в трее сутками
    connect(m_updateTimer, &QTimer::timeout, this, &MainWindow::autoCheckUpdates);
    m_updateTimer->start();
    QTimer::singleShot(1200, this, &MainWindow::showUpdateResult);
}

void MainWindow::autoCheckUpdates()
{
    if (!UpdatesTab::autoCheckEnabled() || AppVersion::build() <= 0 || m_updater->state() != Updater::State::Idle)
        return;
    m_autoChecking = true;
    m_updater->check();
}

void MainWindow::updateChecked(bool ok)
{
    const bool automatic = m_autoChecking;
    m_autoChecking = false;
    const bool available = ok && m_updater->updateAvailable();
    m_nav->setBadge(m_updatesIndex, available);
    if (!available)
        return;
    statusBar()->showMessage(tr("Доступна новая версия программы (сборка %1) - раздел «Обновления».").arg(m_updater->latest().build), 20000);
    if (automatic && m_offeredBuild != m_updater->latest().build) {
        m_offeredBuild = m_updater->latest().build;
        QTimer::singleShot(0, this, &MainWindow::offerUpdate);
    }
}

void MainWindow::offerUpdate()
{
    if (!m_updater->updateAvailable() || m_updater->state() != Updater::State::Idle)
        return;
    // Не перебиваем человека, который сейчас в диалоге; а если окно свёрнуто в трей - только напоминание у значка.
    if (QApplication::activeModalWidget()) {
        QTimer::singleShot(30000, this, &MainWindow::offerUpdate);
        return;
    }
    const Updater::Release &r = m_updater->latest();
    if (!isVisible() || isMinimized()) {
        if (m_tray && m_tray->isVisible())
            m_tray->showMessage(tr("Доступна новая версия программы"), tr("Сборка %1. Откройте окно: раздел «Обновления».").arg(r.build),
                                QSystemTrayIcon::Information, 10000);
        return;
    }
    QString notes = r.history.isEmpty() ? r.notes : r.history.first().notes;
    if (notes.size() > 700)
        notes = notes.left(700) + QStringLiteral("…");
    QMessageBox box(QMessageBox::Question, tr("Доступна новая версия"),
                    tr("Вышла новая версия программы: сборка %1 (у вас сборка %2).\n\nСкачать и установить сейчас? Программа закроется и "
                       "запустится заново через несколько секунд.").arg(r.build).arg(AppVersion::build()),
                    QMessageBox::NoButton, this);
    if (!notes.isEmpty())
        box.setDetailedText(notes);
    QPushButton *yes = box.addButton(tr("Скачать и установить"), QMessageBox::AcceptRole);
    box.addButton(tr("Позже"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == yes) {
        showPage(m_updatesTab);
        m_updatesTab->startDownload();
    }
}

void MainWindow::installUpdate()
{
    if (m_canEdit && !m_cardsTab->saveIfDirty())
        return; // открытая карточка Маркета не сохранилась - не закрываем программу, чтобы не потерять правки
    QString err;
    if (!m_updater->install(&err)) {
        QMessageBox::warning(this, tr("Обновление не установлено"), err);
        return;
    }
    quitApp();
}

void MainWindow::showUpdateResult()
{
    const Updater::Result r = Updater::takeResult();
    if (!r.present)
        return;
    if (r.ok) {
        qInfo().noquote() << "Обновление установлено: сборка" << r.build;
        statusBar()->showMessage(tr("Программа обновлена: сборка %1. Что изменилось - раздел «Обновления».").arg(r.build), 30000);
        return;
    }
    QString why = r.message;
    if (why.startsWith(QLatin1String("copy failed")))
        why = tr("не удалось заменить файлы (их могут держать антивирус или другая программа). Возвращена прежняя версия.");
    else if (why.startsWith(QLatin1String("backup failed")))
        why = tr("не удалось сделать резервную копию текущих файлов, ничего не менялось.");
    else if (why.startsWith(QLatin1String("the program did not close")))
        why = tr("программа не закрылась за отведённое время, файлы не менялись.");
    qWarning().noquote() << "Обновление не установлено: сборка" << r.build << r.message;
    QMessageBox::warning(this, tr("Обновление не установлено"),
                         tr("Установить сборку %1 не удалось: %2\n\nМожно повторить на вкладке «Обновления».").arg(r.build).arg(why));
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
