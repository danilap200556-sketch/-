#include "markettab.h"
#include "productfinder.h"
#include "marketaccountdialog.h"
#include "marketautosync.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {

enum StockCol { SColOffer = 0, SColName, SColLocal, SColFit, SColAvailable, SColDiff, SColStatus, SColCount };
enum PriceCol { PColOffer = 0, PColName, PColLocal, PColMarket, PColDiscount, PColDiff, PColStatus, PColCount };

QTableWidget *makeTable(const QStringList &headers, QWidget *parent)
{
    auto *t = new QTableWidget(0, headers.size(), parent);
    t->setHorizontalHeaderLabels(headers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->verticalHeader()->setVisible(false);
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    return t;
}

// Числовая ячейка, которая сортируется как число, а не как строка.
QTableWidgetItem *numberItem(double value, int decimals = 0)
{
    auto *item = new QTableWidgetItem;
    const double scale = std::pow(10.0, decimals);
    if (decimals == 0)
        item->setData(Qt::DisplayRole, qint64(value));
    else
        item->setData(Qt::DisplayRole, std::round(value * scale) / scale);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

QString money(double v)
{
    return v > 0 ? QString::number(v, 'f', 2) : QStringLiteral("—");
}

} // namespace

MarketTab::MarketTab(bool isAdmin, QWidget *parent)
    : QWidget(parent), m_isAdmin(isAdmin)
{
    auto *layout = new QVBoxLayout(this);

    // --- Выбор кабинета ---
    auto *accountRow = new QHBoxLayout();
    accountRow->addWidget(new QLabel(tr("Кабинет:"), this));
    m_accountCombo = new QComboBox(this);
    m_accountCombo->setMinimumWidth(280);
    accountRow->addWidget(m_accountCombo);
    auto *addBtn = new QPushButton(tr("Добавить кабинет"), this);
    auto *editBtn = new QPushButton(tr("Настроить"), this);
    auto *delBtn = new QPushButton(tr("Удалить"), this);
    for (auto *b : {addBtn, editBtn, delBtn}) {
        b->setVisible(m_isAdmin);
        accountRow->addWidget(b);
    }
    accountRow->addStretch();
    layout->addLayout(accountRow);

    m_accountInfo = new QLabel(this);
    m_accountInfo->setStyleSheet("color: gray;");
    m_accountInfo->setWordWrap(true);
    layout->addWidget(m_accountInfo);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    layout->addWidget(splitter, 1);

    auto *pages = new QTabWidget(splitter);
    m_pages = pages;

    // --- Остатки ---
    auto *stockPage = new QWidget(pages);
    auto *stockLayout = new QVBoxLayout(stockPage);
    auto *stockButtons = new QHBoxLayout();
    auto *syncStocksBtn = new QPushButton(tr("1. Сверить остатки"), stockPage);
    auto *pushStocksBtn = new QPushButton(tr("2. Отправить остатки на Маркет"), stockPage);
    m_zeroMissing = new QCheckBox(tr("обнулять на Маркете товары, которых нет в приложении"), stockPage);
    stockButtons->addWidget(syncStocksBtn);
    stockButtons->addWidget(pushStocksBtn);
    stockButtons->addWidget(m_zeroMissing);
    stockButtons->addStretch();
    stockLayout->addLayout(stockButtons);
    m_stockTable = makeTable({tr("Артикул на Маркете"), tr("Товар"), tr("У нас, шт"), tr("На Маркете: годный"),
                              tr("На Маркете: доступно к заказу"), tr("Разница"), tr("Статус")},
                             stockPage);
    m_stockTable->horizontalHeaderItem(SColFit)->setToolTip(
        tr("«Годный» на Маркете - товар в наличии, включая зарезервированный под заказы. "
           "Именно это число мы передаём и сравниваем с нашим остатком."));
    m_stockTable->horizontalHeaderItem(SColAvailable)->setToolTip(
        tr("«Доступный к заказу» - годный минус резерв под текущие заказы. Только для справки."));
    stockLayout->addWidget(m_stockTable);
    m_stockSummary = new QLabel(stockPage);
    stockLayout->addWidget(m_stockSummary);
    pages->addTab(stockPage, tr("Остатки"));

    // --- Цены ---
    auto *pricePage = new QWidget(pages);
    auto *priceLayout = new QVBoxLayout(pricePage);
    auto *priceButtons = new QHBoxLayout();
    auto *syncPricesBtn = new QPushButton(tr("1. Сверить цены"), pricePage);
    auto *pushPricesBtn = new QPushButton(tr("2. Отправить наши цены на Маркет"), pricePage);
    auto *pullPricesBtn = new QPushButton(tr("Взять цены с Маркета в приложение"), pricePage);
    priceButtons->addWidget(syncPricesBtn);
    priceButtons->addWidget(pushPricesBtn);
    priceButtons->addWidget(pullPricesBtn);
    priceButtons->addStretch();
    priceLayout->addLayout(priceButtons);
    m_priceTable = makeTable({tr("Артикул на Маркете"), tr("Товар"), tr("Наша цена"), tr("Цена на Маркете"),
                              tr("Зачёркнутая на Маркете"), tr("Разница, %"), tr("Статус")},
                             pricePage);
    priceLayout->addWidget(m_priceTable);
    m_priceSummary = new QLabel(pricePage);
    priceLayout->addWidget(m_priceSummary);
    pages->addTab(pricePage, tr("Цены"));

    m_onlyDiffs = new QCheckBox(tr("Показывать только расхождения"), this);
    m_onlyDiffs->setChecked(true);
    layout->insertWidget(2, m_onlyDiffs);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Найти в таблицах: штрихкод (сканером), артикул или название"));
    m_search->setClearButtonEnabled(true);
    layout->insertWidget(3, m_search);

    // --- Журнал ---
    auto *logBox = new QGroupBox(tr("Журнал обмена с Маркетом"), splitter);
    auto *logLayout = new QVBoxLayout(logBox);
    m_log = new QPlainTextEdit(logBox);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    logLayout->addWidget(m_log);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);

    connect(addBtn, &QPushButton::clicked, this, &MarketTab::addAccount);
    connect(editBtn, &QPushButton::clicked, this, &MarketTab::editAccount);
    connect(delBtn, &QPushButton::clicked, this, &MarketTab::deleteAccount);
    connect(syncStocksBtn, &QPushButton::clicked, this, &MarketTab::syncStocks);
    connect(pushStocksBtn, &QPushButton::clicked, this, &MarketTab::pushStocks);
    connect(syncPricesBtn, &QPushButton::clicked, this, &MarketTab::syncPrices);
    connect(pushPricesBtn, &QPushButton::clicked, this, &MarketTab::pushPrices);
    connect(pullPricesBtn, &QPushButton::clicked, this, &MarketTab::pullPrices);
    connect(m_onlyDiffs, &QCheckBox::toggled, this, &MarketTab::applyFilter);
    connect(m_search, &QLineEdit::textChanged, this, &MarketTab::applyFilter);
    connect(m_accountCombo, &QComboBox::currentIndexChanged, this, &MarketTab::updateAccountInfo);

}

void MarketTab::updateAccountInfo()
{
    MarketAccount acc;
    if (!currentAccount(&acc)) {
        m_accountInfo->setText(m_isAdmin ? tr("Кабинетов пока нет - добавьте первый кнопкой «Добавить кабинет».")
                                         : tr("Кабинетов пока нет - их настраивает администратор."));
        return;
    }
    QStringList local;
    QSqlQuery q("SELECT id, name FROM warehouses ORDER BY name");
    while (q.next())
        if (acc.localWarehouseIds.contains(q.value(0).toInt()))
            local << q.value(1).toString();
    m_accountInfo->setText(
        tr("Магазин ID %1 · %2 · наши склады: %3")
            .arg(acc.campaignId)
            .arg(acc.warehouseGroups ? tr("группы складов") : tr("склад Маркета ID %1").arg(acc.marketWarehouseId))
            .arg(local.isEmpty() ? tr("все") : local.join(", ")));
}

void MarketTab::refresh()
{
    const int previous = m_accountCombo->currentData().toInt();
    m_accounts = MarketAccount::loadAll();
    m_accountCombo->blockSignals(true);
    m_accountCombo->clear();
    for (const auto &a : m_accounts) {
        m_accountCombo->addItem(a.name, a.id);
        if (a.id == previous)
            m_accountCombo->setCurrentIndex(m_accountCombo->count() - 1);
    }
    m_accountCombo->blockSignals(false);
    updateAccountInfo();
    fillAutoAccounts();
}

bool MarketTab::currentAccount(MarketAccount *acc)
{
    const int id = m_accountCombo->currentData().toInt();
    for (const auto &a : m_accounts) {
        if (a.id == id) {
            *acc = a;
            return true;
        }
    }
    return false;
}

void MarketTab::log(const QString &text)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss"), text));
    QApplication::processEvents();
}

// ---------------------------------------------------------------------------
// Кабинеты

void MarketTab::addAccount()
{
    MarketAccountDialog dlg(MarketAccount{}, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    MarketAccount acc = dlg.account();
    QString err;
    if (!acc.save(&err)) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сохранить кабинет:\n%1").arg(err));
        return;
    }
    refresh();
    m_accountCombo->setCurrentIndex(m_accountCombo->findData(acc.id));
    log(tr("Добавлен кабинет «%1».").arg(acc.name));
}

void MarketTab::editAccount()
{
    MarketAccount acc;
    if (!currentAccount(&acc))
        return;
    MarketAccountDialog dlg(acc, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    acc = dlg.account();
    QString err;
    if (!acc.save(&err)) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сохранить кабинет:\n%1").arg(err));
        return;
    }
    refresh();
    log(tr("Настройки кабинета «%1» сохранены.").arg(acc.name));
}

void MarketTab::deleteAccount()
{
    MarketAccount acc;
    if (!currentAccount(&acc))
        return;
    if (QMessageBox::question(this, tr("Удалить кабинет"),
                              tr("Удалить кабинет «%1» из приложения? На самом Маркете ничего не изменится.")
                                  .arg(acc.name))
        != QMessageBox::Yes)
        return;
    QString err;
    if (!MarketAccount::remove(acc.id, &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

// ---------------------------------------------------------------------------
// Наши данные

// ---------------------------------------------------------------------------
// Остатки

void MarketTab::syncStocks()
{
    MarketAccount acc;
    if (!currentAccount(&acc))
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    log(tr("«%1»: загружаю каталог и остатки с Маркета...").arg(acc.name));
    MarketApi api(acc.apiKey, [this](const QString &s) { log(s); });
    QString err;
    const bool ok = MarketSync::compareStocks(acc, api, &m_stockRows, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        m_stockRows.clear();
        log(tr("Ошибка: %1").arg(err));
        QMessageBox::warning(this, tr("Маркет"), tr("Не удалось получить данные с Маркета:\n%1").arg(err));
        return;
    }
    m_stockAccountId = acc.id;
    fillStockTable();
    log(tr("«%1»: сверка остатков готова - %2").arg(acc.name, m_stockSummary->text()));
}

void MarketTab::fillStockTable()
{
    m_stockTable->setSortingEnabled(false);
    m_stockTable->setRowCount(0);
    int match = 0, differ = 0, notInCatalog = 0, notInApp = 0, other = 0;
    for (const auto &r : m_stockRows) {
        const int row = m_stockTable->rowCount();
        m_stockTable->insertRow(row);
        m_stockTable->setItem(row, SColOffer, new QTableWidgetItem(r.offerId));
        m_stockTable->setItem(row, SColName, new QTableWidgetItem(r.name));
        m_stockTable->setItem(row, SColLocal, r.status == Status::NotInApp ? new QTableWidgetItem(tr("—"))
                                                                          : numberItem(r.local));
        const bool onMarket = r.status != Status::NotInCatalog;
        m_stockTable->setItem(row, SColFit, onMarket ? numberItem(r.marketFit) : new QTableWidgetItem(tr("—")));
        m_stockTable->setItem(row, SColAvailable,
                              onMarket ? numberItem(r.marketAvailable) : new QTableWidgetItem(tr("—")));
        m_stockTable->setItem(row, SColDiff,
                              r.status == Status::Differ || r.status == Status::NotInApp
                                  ? numberItem(double(r.local - r.marketFit))
                                  : new QTableWidgetItem());

        QString text;
        QColor color;
        switch (r.status) {
        case Status::Match: text = tr("совпадает"); color = QColor("#2e7d32"); ++match; break;
        case Status::Differ: text = tr("будет обновлено: %1 → %2").arg(r.marketFit).arg(r.local); color = QColor("#e65100"); ++differ; break;
        case Status::NotInCatalog: text = tr("нет в каталоге Маркета - не передаётся"); color = QColor("#757575"); ++notInCatalog; break;
        case Status::NotInCampaign: text = tr("не размещён в этом магазине - не передаётся"); color = QColor("#757575"); ++other; break;
        case Status::Archived: text = tr("в архиве на Маркете - не передаётся"); color = QColor("#757575"); ++other; break;
        case Status::NotInApp: text = tr("нет в приложении"); color = QColor("#c62828"); ++notInApp; break;
        default: break;
        }
        auto *statusItem = new QTableWidgetItem(text);
        statusItem->setForeground(color);
        statusItem->setData(Qt::UserRole, int(r.status));
        m_stockTable->setItem(row, SColStatus, statusItem);
    }
    m_stockTable->setSortingEnabled(true);
    m_stockTable->resizeColumnsToContents();
    m_stockSummary->setText(tr("совпадает: %1, к обновлению: %2, нет в каталоге Маркета: %3, "
                               "есть на Маркете но нет у нас: %4, прочее: %5")
                                .arg(match).arg(differ).arg(notInCatalog).arg(notInApp).arg(other));
    applyFilter();
}

void MarketTab::pushStocks()
{
    MarketAccount acc;
    if (!currentAccount(&acc))
        return;
    if (m_stockAccountId != acc.id) {
        QMessageBox::information(this, tr("Сначала сверка"),
                                 tr("Сначала нажмите «Сверить остатки» для этого кабинета - "
                                    "отправляется ровно то, что показано в таблице сверки."));
        return;
    }
    int zeroed = 0;
    const auto items = MarketSync::itemsToPush(m_stockRows, m_zeroMissing->isChecked(), &zeroed);
    if (items.isEmpty()) {
        QMessageBox::information(this, tr("Нечего отправлять"), tr("Все остатки уже совпадают с Маркетом."));
        return;
    }
    QString question = tr("Отправить остатки в кабинет «%1»?\n\nБудет обновлено позиций: %2")
                           .arg(acc.name).arg(items.size());
    if (zeroed)
        question += tr("\nИз них обнулить (нет в приложении): %1").arg(zeroed);
    if (QMessageBox::question(this, tr("Отправка остатков"), question) != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    log(tr("«%1»: отправляю остатки (%2 поз.)...").arg(acc.name).arg(items.size()));
    MarketApi api(acc.apiKey, [this](const QString &s) { log(s); });
    QString err;
    const bool ok = api.updateStocks(acc, items, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        log(tr("Ошибка: %1").arg(err));
        QMessageBox::warning(this, tr("Маркет"), tr("Не удалось отправить остатки:\n%1").arg(err));
        return;
    }
    log(tr("«%1»: остатки отправлены. На Маркете они обновятся в течение нескольких минут.").arg(acc.name));
    for (auto &r : m_stockRows) {
        if (r.status == Status::Differ || (r.status == Status::NotInApp && m_zeroMissing->isChecked())) {
            r.marketFit = r.status == Status::NotInApp ? 0 : r.local;
            r.status = Status::Match;
        }
    }
    fillStockTable();
    QMessageBox::information(this, tr("Готово"),
                             tr("Остатки отправлены (%1 поз.). На витрине Маркета они обновятся в течение "
                                "нескольких минут.").arg(items.size()));
}

// ---------------------------------------------------------------------------
// Цены

void MarketTab::syncPrices()
{
    MarketAccount acc;
    if (!currentAccount(&acc))
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    log(tr("«%1»: загружаю каталог и цены с Маркета...").arg(acc.name));
    MarketApi api(acc.apiKey, [this](const QString &s) { log(s); });
    QHash<QString, MarketOffer> catalog;
    QHash<QString, MarketPrice> prices;
    QString err;
    const bool ok = api.catalog(acc, &catalog, &err) && api.prices(acc.businessId, &prices, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        log(tr("Ошибка: %1").arg(err));
        QMessageBox::warning(this, tr("Маркет"), tr("Не удалось получить данные с Маркета:\n%1").arg(err));
        return;
    }

    const auto local = MarketSync::loadLocal(acc);
    m_priceRows.clear();
    for (auto it = local.cbegin(); it != local.cend(); ++it) {
        PriceRow row;
        row.offerId = it.key();
        row.name = it->name;
        row.productIds = it->productIds;
        row.local = it->price;
        const auto mp = prices.constFind(it.key());
        if (mp != prices.cend()) {
            row.market = mp->value;
            row.discountBase = mp->discountBase;
        }
        const auto cat = catalog.constFind(it.key());
        if (cat == catalog.cend())
            row.status = Status::NotInCatalog;
        else if (cat->archived)
            row.status = Status::Archived;
        else if (row.local <= 0)
            row.status = Status::NoLocalPrice;
        else if (row.market <= 0)
            row.status = Status::NoMarketPrice;
        else
            row.status = qAbs(row.local - row.market) < 0.005 ? Status::Match : Status::Differ;
        m_priceRows.append(row);
    }
    m_priceAccountId = acc.id;
    fillPriceTable();
    log(tr("«%1»: сверка цен готова - %2").arg(acc.name, m_priceSummary->text()));
}

void MarketTab::fillPriceTable()
{
    m_priceTable->setSortingEnabled(false);
    m_priceTable->setRowCount(0);
    int match = 0, differ = 0, noMarket = 0, noLocal = 0, other = 0;
    for (const auto &r : m_priceRows) {
        const int row = m_priceTable->rowCount();
        m_priceTable->insertRow(row);
        m_priceTable->setItem(row, PColOffer, new QTableWidgetItem(r.offerId));
        m_priceTable->setItem(row, PColName, new QTableWidgetItem(r.name));
        m_priceTable->setItem(row, PColLocal, r.local > 0 ? numberItem(r.local, 2) : new QTableWidgetItem(tr("—")));
        m_priceTable->setItem(row, PColMarket, r.market > 0 ? numberItem(r.market, 2) : new QTableWidgetItem(tr("—")));
        m_priceTable->setItem(row, PColDiscount,
                              r.discountBase > 0 ? numberItem(r.discountBase, 2) : new QTableWidgetItem());
        m_priceTable->setItem(row, PColDiff,
                              r.status == Status::Differ ? numberItem((r.local - r.market) / r.market * 100.0, 1)
                                                         : new QTableWidgetItem());
        QString text;
        QColor color;
        switch (r.status) {
        case Status::Match: text = tr("совпадает"); color = QColor("#2e7d32"); ++match; break;
        case Status::Differ: text = tr("отличается: на Маркете %1, у нас %2").arg(money(r.market), money(r.local)); color = QColor("#e65100"); ++differ; break;
        case Status::NoMarketPrice: text = tr("на Маркете цены нет - можно отправить нашу"); color = QColor("#e65100"); ++noMarket; break;
        case Status::NoLocalPrice: text = tr("у нас цена не указана"); color = QColor("#757575"); ++noLocal; break;
        case Status::NotInCatalog: text = tr("нет в каталоге Маркета"); color = QColor("#757575"); ++other; break;
        case Status::Archived: text = tr("в архиве на Маркете"); color = QColor("#757575"); ++other; break;
        default: break;
        }
        auto *statusItem = new QTableWidgetItem(text);
        statusItem->setForeground(color);
        statusItem->setData(Qt::UserRole, int(r.status));
        m_priceTable->setItem(row, PColStatus, statusItem);
    }
    m_priceTable->setSortingEnabled(true);
    m_priceTable->resizeColumnsToContents();
    m_priceSummary->setText(tr("совпадает: %1, отличается: %2, нет цены на Маркете: %3, нет нашей цены: %4, прочее: %5")
                                .arg(match).arg(differ).arg(noMarket).arg(noLocal).arg(other));
    applyFilter();
}

void MarketTab::pushPrices()
{
    MarketAccount acc;
    if (!currentAccount(&acc))
        return;
    if (m_priceAccountId != acc.id) {
        QMessageBox::information(this, tr("Сначала сверка"), tr("Сначала нажмите «Сверить цены» для этого кабинета."));
        return;
    }
    QList<MarketPriceUpdate> items;
    int bigDrops = 0;
    for (const auto &r : m_priceRows) {
        if (r.status != Status::Differ && r.status != Status::NoMarketPrice)
            continue;
        items.append({r.offerId, r.local, r.discountBase});
        if (r.market > 0 && r.local < r.market * 0.8)
            ++bigDrops;
    }
    if (items.isEmpty()) {
        QMessageBox::information(this, tr("Нечего отправлять"), tr("Все цены уже совпадают с Маркетом."));
        return;
    }
    QString question = tr("Отправить наши цены в кабинет «%1»?\n\nБудет изменено цен: %2\n\n"
                          "Внимание: цена меняется во всех магазинах этого кабинета.")
                           .arg(acc.name).arg(items.size());
    if (bigDrops)
        question += tr("\n\nУ %1 товаров цена снижается больше чем на 20%. Маркет может придержать такие цены "
                       "«в карантине» до подтверждения в личном кабинете.").arg(bigDrops);
    if (QMessageBox::question(this, tr("Отправка цен"), question) != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    log(tr("«%1»: отправляю цены (%2 поз.)...").arg(acc.name).arg(items.size()));
    MarketApi api(acc.apiKey, [this](const QString &s) { log(s); });
    QString err;
    const bool ok = api.updatePrices(acc.businessId, items, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        log(tr("Ошибка: %1").arg(err));
        QMessageBox::warning(this, tr("Маркет"), tr("Не удалось отправить цены:\n%1").arg(err));
        return;
    }
    for (auto &r : m_priceRows) {
        if (r.status == Status::Differ || r.status == Status::NoMarketPrice) {
            r.market = r.local;
            if (r.local > r.discountBase * 0.95)
                r.discountBase = 0;
            r.status = Status::Match;
        }
    }
    fillPriceTable();
    log(tr("«%1»: цены отправлены.").arg(acc.name));
    QMessageBox::information(this, tr("Готово"), tr("Цены отправлены (%1 поз.). На Маркете они обновятся в течение "
                                                    "нескольких минут.").arg(items.size()));
}

void MarketTab::pullPrices()
{
    MarketAccount acc;
    if (!currentAccount(&acc))
        return;
    if (m_priceAccountId != acc.id) {
        QMessageBox::information(this, tr("Сначала сверка"), tr("Сначала нажмите «Сверить цены» для этого кабинета."));
        return;
    }
    QList<const PriceRow *> rows;
    for (const auto &r : m_priceRows)
        if (r.market > 0 && (r.status == Status::Differ || r.status == Status::NoLocalPrice))
            rows.append(&r);
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Нечего обновлять"), tr("Цены в приложении уже совпадают с Маркетом."));
        return;
    }
    if (QMessageBox::question(this, tr("Цены с Маркета"),
                              tr("Записать в приложение цены с Маркета для %1 товаров? "
                                 "Наши текущие цены этих товаров будут заменены.").arg(rows.size()))
        != QMessageBox::Yes)
        return;

    QSqlDatabase db = QSqlDatabase::database();
    db.transaction();
    for (const PriceRow *r : rows) {
        for (int productId : r->productIds) {
            QSqlQuery q;
            q.prepare("UPDATE products SET price = ? WHERE id = ?");
            q.addBindValue(r->market);
            q.addBindValue(productId);
            if (!q.exec()) {
                const QString err = q.lastError().text();
                db.rollback();
                QMessageBox::warning(this, tr("Ошибка"), err);
                return;
            }
        }
    }
    if (!db.commit()) {
        const QString err = db.lastError().text();
        db.rollback();
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    for (auto &r : m_priceRows) {
        if (r.market > 0 && (r.status == Status::Differ || r.status == Status::NoLocalPrice)) {
            r.local = r.market;
            r.status = Status::Match;
        }
    }
    fillPriceTable();
    log(tr("«%1»: в приложение записаны цены с Маркета (%2 поз.).").arg(acc.name).arg(rows.size()));
}

void MarketTab::applyFilter()
{
    const bool only = m_onlyDiffs->isChecked();
    const QString term = m_search->text().trimmed();
    // Штрихкод или артикул превращаем в артикулы на Маркете (по ним строятся таблицы).
    QSet<QString> offers;
    if (!term.isEmpty())
        offers = ProductFinder::offerIds(term);
    auto apply = [&](QTableWidget *t, int statusCol, int offerCol, int nameCol) {
        for (int row = 0; row < t->rowCount(); ++row) {
            const auto status = Status(t->item(row, statusCol)->data(Qt::UserRole).toInt());
            bool match = term.isEmpty() || offers.contains(t->item(row, offerCol)->text())
                         || t->item(row, offerCol)->text().contains(term, Qt::CaseInsensitive)
                         || t->item(row, nameCol)->text().contains(term, Qt::CaseInsensitive);
            t->setRowHidden(row, (only && status == Status::Match) || !match);
        }
    };
    apply(m_stockTable, SColStatus, SColOffer, SColName);
    apply(m_priceTable, PColStatus, PColOffer, PColName);
}

// ---------------------------------------------------------------------------
// Автообновление остатков

void MarketTab::setAutoSync(MarketAutoSync *sync)
{
    m_auto = sync;
    auto *page = new QWidget(m_pages);
    auto *l = new QVBoxLayout(page);

    m_autoEnabled = new QCheckBox(tr("Автоматически обновлять остатки на Маркете"), page);
    QFont bold = m_autoEnabled->font();
    bold.setBold(true);
    m_autoEnabled->setFont(bold);
    l->addWidget(m_autoEnabled);

    auto *every = new QHBoxLayout();
    every->addWidget(new QLabel(tr("Раз в"), page));
    m_autoEvery = new QSpinBox(page);
    m_autoEvery->setRange(1, 999);
    every->addWidget(m_autoEvery);
    m_autoUnit = new QComboBox(page);
    m_autoUnit->addItem(tr("минут"), 1);
    m_autoUnit->addItem(tr("часов"), 60);
    every->addWidget(m_autoUnit);
    every->addWidget(new QLabel(tr("(не чаще, чем раз в %1 минут)").arg(MarketAutoSync::kMinIntervalMinutes), page));
    every->addStretch();
    l->addLayout(every);

    l->addWidget(new QLabel(tr("Какие кабинеты обновлять:"), page));
    m_autoAccounts = new QListWidget(page);
    m_autoAccounts->setMaximumHeight(110);
    l->addWidget(m_autoAccounts);

    m_autoZero = new QCheckBox(tr("обнулять на Маркете товары, которых нет в приложении"), page);
    l->addWidget(m_autoZero);

    auto *note = new QLabel(tr("Сверка и отправка идут сами, как кнопки «Сверить» и «Отправить» на вкладке «Остатки», "
                               "только без вопросов: отправляются расхождения. Работает, пока запущено приложение: "
                               "если закрыть окно, оно свернётся в значок у часов (трей) и продолжит работать, "
                               "выйти совсем - значок → «Выход». Расписание хранится на этом компьютере."), page);
    note->setWordWrap(true);
    note->setObjectName("muted");
    l->addWidget(note);

    m_autoStatus = new QLabel(page);
    m_autoStatus->setWordWrap(true);
    l->addWidget(m_autoStatus);

    auto *buttons = new QHBoxLayout();
    auto *saveBtn = new QPushButton(tr("Сохранить расписание"), page);
    saveBtn->setProperty("primary", true);
    m_autoRunBtn = new QPushButton(tr("Обновить остатки сейчас"), page);
    buttons->addWidget(saveBtn);
    buttons->addWidget(m_autoRunBtn);
    buttons->addStretch();
    l->addLayout(buttons);
    l->addStretch();
    m_pages->addTab(page, tr("Автообновление"));

    const auto s = sync->settings();
    m_autoEnabled->setChecked(s.enabled);
    const bool hours = s.intervalMinutes % 60 == 0 && s.intervalMinutes >= 60;
    m_autoUnit->setCurrentIndex(hours ? 1 : 0);
    m_autoEvery->setValue(hours ? s.intervalMinutes / 60 : s.intervalMinutes);
    m_autoZero->setChecked(s.zeroMissing);
    fillAutoAccounts();

    connect(saveBtn, &QPushButton::clicked, this, &MarketTab::saveAutoSettings);
    connect(m_autoRunBtn, &QPushButton::clicked, this, [this]() {
        if (m_auto->isRunning())
            return;
        m_auto->runNow();
        QMessageBox::information(this, tr("Маркет"), m_auto->lastResult().isEmpty() ? tr("Готово.") : m_auto->lastResult());
    });
    connect(sync, &MarketAutoSync::message, this, &MarketTab::log);
    connect(sync, &MarketAutoSync::statusChanged, this, &MarketTab::updateAutoStatus);
    updateAutoStatus();
}

void MarketTab::fillAutoAccounts()
{
    if (!m_auto)
        return;
    const auto chosen = m_auto->settings().accountIds;
    m_autoAccounts->clear();
    for (const MarketAccount &a : std::as_const(m_accounts)) {
        auto *item = new QListWidgetItem(a.name, m_autoAccounts);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(chosen.isEmpty() || chosen.contains(a.id) ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, a.id);
    }
}

void MarketTab::saveAutoSettings()
{
    if (!m_auto)
        return;
    MarketAutoSync::Settings s;
    s.enabled = m_autoEnabled->isChecked();
    s.intervalMinutes = m_autoEvery->value() * m_autoUnit->currentData().toInt();
    if (s.intervalMinutes < MarketAutoSync::kMinIntervalMinutes) {
        s.intervalMinutes = MarketAutoSync::kMinIntervalMinutes;
        m_autoUnit->setCurrentIndex(0);
        m_autoEvery->setValue(s.intervalMinutes);
        QMessageBox::information(this, tr("Маркет"),
                                 tr("Чаще, чем раз в %1 минут, остатки не обновляются (у Маркета есть лимиты на запросы). "
                                    "Поставлено %1.").arg(MarketAutoSync::kMinIntervalMinutes));
    }
    int checked = 0;
    for (int i = 0; i < m_autoAccounts->count(); ++i) {
        if (m_autoAccounts->item(i)->checkState() == Qt::Checked) {
            ++checked;
            s.accountIds << m_autoAccounts->item(i)->data(Qt::UserRole).toInt();
        }
    }
    if (s.enabled && m_autoAccounts->count() > 0 && checked == 0) {
        QMessageBox::warning(this, tr("Маркет"), tr("Отметьте хотя бы один кабинет."));
        return;
    }
    if (checked == m_autoAccounts->count())
        s.accountIds.clear(); // все (и те, что добавят позже)
    s.zeroMissing = m_autoZero->isChecked();
    m_auto->setSettings(s);
}

void MarketTab::updateAutoStatus()
{
    if (m_auto && m_autoStatus) {
        m_autoStatus->setText(m_auto->statusText());
        m_autoRunBtn->setEnabled(!m_auto->isRunning());
    }
}
