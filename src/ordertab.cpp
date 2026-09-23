#include "ordertab.h"
#include "xlsx.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Col { ColAccount = 0, ColOrder, ColStatus, ColShipment, ColOffer, ColName, ColCount, ColLocation };

struct StatusFilter {
    const char *label;
    QStringList statuses;
    QStringList substatuses;
};

const QList<StatusFilter> &statusFilters()
{
    static const QList<StatusFilter> filters = {
        {"В обработке — ждут сборки и отгрузки", {"PROCESSING"}, {}},
        {"Готовы к отгрузке", {"PROCESSING"}, {"READY_TO_SHIP"}},
        {"В сборке", {"PROCESSING"}, {"STARTED"}},
        {"Переданы в доставку", {"DELIVERY"}, {}},
        {"Все заказы за последние 30 дней", {}, {}},
    };
    return filters;
}

QString humanStatus(const QString &status, const QString &substatus)
{
    if (status == QLatin1String("PROCESSING")) {
        if (substatus == QLatin1String("STARTED"))
            return QStringLiteral("в сборке");
        if (substatus == QLatin1String("READY_TO_SHIP"))
            return QStringLiteral("готов к отгрузке");
        if (substatus == QLatin1String("SHIPPED"))
            return QStringLiteral("отгружен");
        return QStringLiteral("в обработке");
    }
    static const QHash<QString, QString> names = {
        {"DELIVERY", "в доставке"}, {"PICKUP", "в пункте выдачи"}, {"DELIVERED", "доставлен"},
        {"CANCELLED", "отменён"},   {"UNPAID", "не оплачен"},      {"PENDING", "ожидает подтверждения"},
        {"RETURNED", "возвращён"},  {"PARTIALLY_RETURNED", "частично возвращён"},
    };
    return names.value(status, status);
}

QString humanDate(const QString &raw, bool withTime)
{
    if (raw.isEmpty())
        return QString();
    QDateTime dt = QDateTime::fromString(raw, Qt::ISODate);
    if (!dt.isValid())
        dt = QDateTime(QDate::fromString(raw, Qt::ISODate), QTime());
    if (!dt.isValid())
        dt = QDateTime(QDate::fromString(raw, "dd-MM-yyyy"), QTime());
    if (!dt.isValid())
        return raw;
    return withTime ? dt.toLocalTime().toString("dd.MM.yyyy HH:mm") : dt.date().toString("dd.MM.yyyy");
}

QString safeFileName(QString s)
{
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (QChar &ch : s)
        if (forbidden.contains(ch) || ch.unicode() < 0x20)
            ch = QLatin1Char('_');
    return s.simplified().left(80);
}

} // namespace

OrdersTab::OrdersTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel(tr("Кабинет:"), this));
    m_accountCombo = new QComboBox(this);
    m_accountCombo->setMinimumWidth(220);
    filterRow->addWidget(m_accountCombo);
    filterRow->addWidget(new QLabel(tr("Заказы:"), this));
    m_statusCombo = new QComboBox(this);
    for (const auto &f : statusFilters())
        m_statusCombo->addItem(QString::fromUtf8(f.label));
    filterRow->addWidget(m_statusCombo);
    auto *loadBtn = new QPushButton(tr("Загрузить заказы"), this);
    filterRow->addWidget(loadBtn);
    filterRow->addStretch();
    layout->addLayout(filterRow);

    auto *actionRow = new QHBoxLayout();
    auto *excelBtn = new QPushButton(tr("Сохранить список в Excel..."), this);
    actionRow->addWidget(excelBtn);
    actionRow->addSpacing(20);
    actionRow->addWidget(new QLabel(tr("Формат ярлыков:"), this));
    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItem(tr("A7 — 75×120 мм"), "A7");
    m_formatCombo->addItem(tr("A4 — лист, формат из настроек кабинета"), "A4");
    m_formatCombo->addItem(tr("A9 — 58×40 мм, горизонтально"), "A9_HORIZONTALLY");
    m_formatCombo->addItem(tr("A9 — 40×58 мм"), "A9");
    actionRow->addWidget(m_formatCombo);
    auto *labelsBtn = new QPushButton(tr("Скачать ярлыки (PDF)..."), this);
    actionRow->addWidget(labelsBtn);
    actionRow->addStretch();
    layout->addLayout(actionRow);

    auto *hint = new QLabel(tr("Ярлыки и Excel делаются по выделенным в таблице заказам, а если ничего не "
                               "выделено - по всем загруженным. Ярлыки есть только у заказов в обработке."),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    layout->addWidget(splitter, 1);

    auto *tableBox = new QWidget(splitter);
    auto *tableLayout = new QVBoxLayout(tableBox);
    tableLayout->setContentsMargins(0, 0, 0, 0);
    m_table = new QTableWidget(0, 8, tableBox);
    m_table->setHorizontalHeaderLabels({tr("Кабинет"), tr("№ заказа"), tr("Статус"), tr("Отгрузка"),
                                        tr("Артикул"), tr("Товар"), tr("Кол-во"), tr("Где лежит у нас")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    tableLayout->addWidget(m_table);
    m_summary = new QLabel(tableBox);
    tableLayout->addWidget(m_summary);

    auto *logBox = new QGroupBox(tr("Журнал"), splitter);
    auto *logLayout = new QVBoxLayout(logBox);
    m_log = new QPlainTextEdit(logBox);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    logLayout->addWidget(m_log);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);

    connect(loadBtn, &QPushButton::clicked, this, &OrdersTab::loadOrders);
    connect(excelBtn, &QPushButton::clicked, this, &OrdersTab::exportExcel);
    connect(labelsBtn, &QPushButton::clicked, this, &OrdersTab::downloadLabels);

    refresh();
}

void OrdersTab::refresh()
{
    const int previous = m_accountCombo->currentData().toInt();
    m_accounts = MarketAccount::loadAll();
    m_accountCombo->clear();
    m_accountCombo->addItem(tr("Все кабинеты"), 0);
    for (const auto &a : m_accounts) {
        m_accountCombo->addItem(a.name, a.id);
        if (a.id == previous)
            m_accountCombo->setCurrentIndex(m_accountCombo->count() - 1);
    }
}

void OrdersTab::log(const QString &text)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss"), text));
    QApplication::processEvents();
}

QHash<QString, QString> OrdersTab::loadLocations() const
{
    QHash<QString, QStringList> parts;
    QSqlQuery q("SELECT COALESCE(NULLIF(TRIM(p.market_sku), ''), p.sku), w.name, l.code, s.quantity "
                "FROM stock s JOIN products p ON p.id = s.product_id "
                "JOIN warehouses w ON w.id = s.warehouse_id "
                "LEFT JOIN locations l ON l.id = s.location_id "
                "WHERE s.quantity > 0 ORDER BY w.name");
    while (q.next()) {
        const QString place = q.value(2).toString().isEmpty()
                                  ? q.value(1).toString()
                                  : QStringLiteral("%1: %2").arg(q.value(1).toString(), q.value(2).toString());
        parts[q.value(0).toString()] << QStringLiteral("%1 (%2 шт)").arg(place).arg(q.value(3).toInt());
    }
    QHash<QString, QString> result;
    for (auto it = parts.cbegin(); it != parts.cend(); ++it)
        result.insert(it.key(), it.value().join("; "));
    return result;
}

void OrdersTab::loadOrders()
{
    refresh();
    if (m_accounts.isEmpty()) {
        QMessageBox::information(this, tr("Нет кабинетов"),
                                 tr("Сначала добавьте кабинет на вкладке «Яндекс Маркет»."));
        return;
    }
    const int accountId = m_accountCombo->currentData().toInt();
    const StatusFilter &filter = statusFilters()[m_statusCombo->currentIndex()];

    // Магазины с одним ключом и кабинетом запрашиваются одним запросом.
    struct Group { QString apiKey; qint64 businessId; QHash<qint64, QString> nameByCampaign; };
    QList<Group> groups;
    for (const auto &a : m_accounts) {
        if (accountId && a.id != accountId)
            continue;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const Group &g) {
            return g.apiKey == a.apiKey && g.businessId == a.businessId;
        });
        if (it == groups.end()) {
            groups.append({a.apiKey, a.businessId, {}});
            it = groups.end() - 1;
        }
        it->nameByCampaign.insert(a.campaignId, a.name);
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_orders.clear();
    QStringList failed;
    for (const Group &g : groups) {
        const QString names = QStringList(g.nameByCampaign.values()).join(", ");
        log(tr("«%1»: загружаю заказы...").arg(names));
        MarketApi api(g.apiKey, [this](const QString &s) { log(s); });
        QList<MarketOrder> orders;
        QString err;
        if (!api.orders(g.businessId, g.nameByCampaign.keys(), filter.statuses, filter.substatuses, &orders, &err)) {
            log(tr("«%1»: ошибка - %2").arg(names, err));
            failed << QStringLiteral("%1: %2").arg(names, err);
            continue;
        }
        for (const auto &o : orders)
            m_orders.append({o, g.nameByCampaign.value(o.campaignId, tr("магазин %1").arg(o.campaignId)), g.apiKey,
                             g.businessId});
        log(tr("«%1»: заказов %2").arg(names).arg(orders.size()));
    }
    QApplication::restoreOverrideCursor();

    std::sort(m_orders.begin(), m_orders.end(), [](const LoadedOrder &a, const LoadedOrder &b) {
        if (a.order.shipmentDate != b.order.shipmentDate)
            return a.order.shipmentDate < b.order.shipmentDate;
        return a.order.id < b.order.id;
    });

    m_locations = loadLocations();
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    int items = 0;
    for (int i = 0; i < m_orders.size(); ++i) {
        const auto &lo = m_orders[i];
        for (const auto &item : lo.order.items) {
            const int row = m_table->rowCount();
            m_table->insertRow(row);
            auto *acc = new QTableWidgetItem(lo.accountName);
            acc->setData(Qt::UserRole, i);
            m_table->setItem(row, ColAccount, acc);
            auto *id = new QTableWidgetItem;
            id->setData(Qt::DisplayRole, lo.order.id);
            m_table->setItem(row, ColOrder, id);
            m_table->setItem(row, ColStatus, new QTableWidgetItem(humanStatus(lo.order.status, lo.order.substatus)));
            m_table->setItem(row, ColShipment, new QTableWidgetItem(humanDate(lo.order.shipmentDate, false)));
            m_table->setItem(row, ColOffer, new QTableWidgetItem(item.offerId));
            m_table->setItem(row, ColName, new QTableWidgetItem(item.name));
            auto *count = new QTableWidgetItem;
            count->setData(Qt::DisplayRole, item.count);
            m_table->setItem(row, ColCount, count);
            const QString where = m_locations.value(item.offerId);
            auto *loc = new QTableWidgetItem(where.isEmpty() ? tr("нет в наличии у нас") : where);
            if (where.isEmpty())
                loc->setForeground(QColor("#c62828"));
            m_table->setItem(row, ColLocation, loc);
            items += item.count;
        }
    }
    m_table->setSortingEnabled(true);
    m_table->resizeColumnsToContents();
    m_summary->setText(tr("Заказов: %1, товаров: %2 шт").arg(m_orders.size()).arg(items));
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Маркет"), tr("Не удалось загрузить заказы:\n%1").arg(failed.join('\n')));
}

QList<const OrdersTab::LoadedOrder *> OrdersTab::selectedOrders() const
{
    QList<const LoadedOrder *> result;
    QSet<int> seen;
    const auto rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        for (const auto &o : m_orders)
            result.append(&o);
        return result;
    }
    for (const auto &idx : rows) {
        const int i = m_table->item(idx.row(), ColAccount)->data(Qt::UserRole).toInt();
        if (!seen.contains(i)) {
            seen.insert(i);
            result.append(&m_orders[i]);
        }
    }
    return result;
}

void OrdersTab::exportExcel()
{
    const auto orders = selectedOrders();
    if (orders.isEmpty()) {
        QMessageBox::information(this, tr("Нет заказов"), tr("Сначала загрузите заказы."));
        return;
    }
    QString path = QFileDialog::getSaveFileName(
        this, tr("Сохранить список заказов"),
        tr("Заказы_Маркет_%1.xlsx").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmm")),
        tr("Excel (*.xlsx)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".xlsx"), Qt::CaseInsensitive))
        path += QLatin1String(".xlsx");

    Xlsx::Sheet list;
    list.name = tr("Заказы");
    list.rows.append({tr("Кабинет"), tr("№ заказа"), tr("Статус"), tr("Оформлен"), tr("Отгрузка"),
                      tr("Служба доставки"), tr("Артикул"), tr("Товар"), tr("Кол-во"), tr("Где лежит у нас")});
    list.numericColumns = {1, 8};
    list.columnWidths = {20, 12, 16, 16, 11, 20, 20, 40, 8, 40};

    struct Pick { QString name; int total = 0; QSet<qint64> orders; QSet<QString> accounts; };
    QMap<QString, Pick> picks;
    for (const LoadedOrder *lo : orders) {
        for (const auto &item : lo->order.items) {
            list.rows.append({lo->accountName, QString::number(lo->order.id),
                              humanStatus(lo->order.status, lo->order.substatus),
                              humanDate(lo->order.creationDate, true), humanDate(lo->order.shipmentDate, false),
                              lo->order.deliveryService, item.offerId, item.name, QString::number(item.count),
                              m_locations.value(item.offerId)});
            Pick &p = picks[item.offerId];
            p.name = item.name;
            p.total += item.count;
            p.orders.insert(lo->order.id);
            p.accounts.insert(lo->accountName);
        }
    }

    Xlsx::Sheet picking;
    picking.name = tr("Сборка");
    picking.rows.append({tr("Артикул"), tr("Товар"), tr("Всего, шт"), tr("Заказов"), tr("Где лежит у нас"),
                         tr("Кабинеты")});
    picking.numericColumns = {2, 3};
    picking.columnWidths = {20, 40, 10, 9, 45, 30};
    for (auto it = picks.cbegin(); it != picks.cend(); ++it) {
        QStringList accounts(it->accounts.cbegin(), it->accounts.cend());
        accounts.sort();
        const QString where = m_locations.value(it.key());
        picking.rows.append({it.key(), it->name, QString::number(it->total), QString::number(it->orders.size()),
                             where.isEmpty() ? tr("нет в наличии у нас") : where, accounts.join(", ")});
    }

    QString err;
    if (!Xlsx::write(path, {list, picking}, &err)) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сохранить файл:\n%1").arg(err));
        return;
    }
    log(tr("Список сохранён: %1 (заказов %2, артикулов %3)").arg(path).arg(orders.size()).arg(picks.size()));
    QMessageBox::information(this, tr("Готово"),
                             tr("Сохранено: заказов %1.\nЛист «Заказы» - все товары по заказам, лист «Сборка» - "
                                "сколько каждого артикула собрать и где он лежит.")
                                 .arg(orders.size()));
}

void OrdersTab::downloadLabels()
{
    const auto orders = selectedOrders();
    if (orders.isEmpty()) {
        QMessageBox::information(this, tr("Нет заказов"), tr("Сначала загрузите заказы."));
        return;
    }

    // Один файл на кабинет (ключ + бизнес) - Маркет склеивает ярлыки только
    // в пределах одного кабинета.
    struct Batch { QString apiKey; qint64 businessId; QList<qint64> ids; QStringList names; };
    QList<Batch> batches;
    for (const LoadedOrder *lo : orders) {
        auto it = std::find_if(batches.begin(), batches.end(), [&](const Batch &b) {
            return b.apiKey == lo->apiKey && b.businessId == lo->businessId;
        });
        if (it == batches.end()) {
            batches.append({lo->apiKey, lo->businessId, {}, {}});
            it = batches.end() - 1;
        }
        it->ids.append(lo->order.id);
        if (!it->names.contains(lo->accountName))
            it->names.append(lo->accountName);
    }

    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmm");
    const QString format = m_formatCombo->currentData().toString();
    auto fileNameFor = [&](const Batch &b, int part, int parts) {
        QString name = tr("Ярлыки_%1_%2").arg(safeFileName(b.names.join("+")), stamp);
        if (parts > 1)
            name += tr("_часть%1").arg(part);
        return name + QStringLiteral(".pdf");
    };

    QString singlePath;
    QString dir;
    const bool single = batches.size() == 1 && batches.first().ids.size() <= 1000;
    if (single) {
        singlePath = QFileDialog::getSaveFileName(this, tr("Сохранить ярлыки"), fileNameFor(batches.first(), 1, 1),
                                                  tr("PDF (*.pdf)"));
        if (singlePath.isEmpty())
            return;
        if (!singlePath.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
            singlePath += QLatin1String(".pdf");
    } else {
        dir = QFileDialog::getExistingDirectory(this, tr("Папка для файлов с ярлыками (по файлу на кабинет)"));
        if (dir.isEmpty())
            return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList saved, problems;
    for (const Batch &b : batches) {
        MarketApi api(b.apiKey, [this](const QString &s) { log(s); });
        const int parts = int((b.ids.size() + 999) / 1000);
        for (int part = 1; part <= parts; ++part) {
            const QList<qint64> ids = b.ids.mid((part - 1) * 1000, 1000);
            const QString title = b.names.join(", ");
            log(tr("«%1»: запрашиваю ярлыки для %2 заказов...").arg(title).arg(ids.size()));
            QByteArray pdf;
            QString warning, err;
            if (!api.orderLabels(b.businessId, ids, format, &pdf, &warning, &err)) {
                log(tr("«%1»: ошибка - %2").arg(title, err));
                problems << QStringLiteral("%1: %2").arg(title, err);
                continue;
            }
            const QString path = single ? singlePath : QDir(dir).filePath(fileNameFor(b, part, parts));
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write(pdf) != pdf.size()) {
                problems << tr("%1: не удалось записать файл %2").arg(title, path);
                continue;
            }
            f.close();
            saved << path;
            if (!warning.isEmpty())
                problems << QStringLiteral("%1: %2").arg(title, warning);
            log(tr("«%1»: сохранено %2").arg(title, path));
        }
    }
    QApplication::restoreOverrideCursor();

    QString text = saved.isEmpty() ? tr("Ни одного файла с ярлыками не сохранено.")
                                   : tr("Сохранено файлов: %1\n%2").arg(saved.size()).arg(saved.join('\n'));
    if (!problems.isEmpty())
        text += tr("\n\nПроблемы:\n%1").arg(problems.join('\n'));
    if (saved.isEmpty())
        QMessageBox::warning(this, tr("Ярлыки"), text);
    else
        QMessageBox::information(this, tr("Ярлыки"), text);
}
