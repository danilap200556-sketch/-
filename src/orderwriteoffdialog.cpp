#include "orderwriteoffdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

using namespace OrderWriteoff;

namespace {

enum Col { ColCheck = 0, ColSource, ColOrder, ColStatus, ColDate, ColItems, ColWarehouse, ColResult };

QString humanStatus(const Order &o)
{
    if (o.source == QLatin1String("site"))
        return o.status == QLatin1String("cancel_requested") ? QStringLiteral("отменён сайтом") : QStringLiteral("новый");
    static const QHash<QString, QString> names = {
        {"PROCESSING", "в обработке"}, {"DELIVERY", "в доставке"},  {"PICKUP", "в пункте выдачи"}, {"DELIVERED", "доставлен"},
        {"CANCELLED", "отменён"},      {"UNPAID", "не оплачен"},   {"PENDING", "ожидает подтверждения"},
        {"RETURNED", "возвращён"},     {"PARTIALLY_RETURNED", "частично возвращён"},
    };
    if (o.status == QLatin1String("PROCESSING")) {
        if (o.substatus == QLatin1String("STARTED"))
            return QStringLiteral("в сборке");
        if (o.substatus == QLatin1String("READY_TO_SHIP"))
            return QStringLiteral("готов к отгрузке");
    }
    return names.value(o.status, o.status);
}

// «1 заказ», «2 заказа», «5 заказов»
QString ordersCount(int n)
{
    const int m100 = n % 100, m10 = n % 10;
    const QString word = (m100 >= 11 && m100 <= 14) ? QStringLiteral("заказов") : m10 == 1 ? QStringLiteral("заказ") : (m10 >= 2 && m10 <= 4) ? QStringLiteral("заказа") : QStringLiteral("заказов");
    return QStringLiteral("%1 %2").arg(n).arg(word);
}

QString itemsText(const Planned &p)
{
    QStringList parts;
    if (!p.items.isEmpty()) {
        for (const Item &it : p.items)
            parts << QStringLiteral("%1 × %2").arg(it.sku).arg(it.qty);
    } else {
        for (const Line &l : p.order.lines)
            parts << QStringLiteral("%1 × %2").arg(l.key).arg(l.qty);
    }
    return parts.join(QStringLiteral("; "));
}

} // namespace

OrderWriteoffDialog::OrderWriteoffDialog(const QList<Order> &marketOrders, QWidget *parent)
    : QDialog(parent), m_market(marketOrders)
{
    setWindowTitle(tr("Списать заказы со склада"));
    resize(1100, 680);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("За каждый отмеченный заказ со склада списывается столько пар, сколько заказано, - остаток уменьшается в приложении, "
                                "на сайте и (при следующей отправке остатков) на Маркете. Каждый заказ - отдельный документ в разделе «Документы». "
                                "Один заказ списывается один раз. Если заказ потом отменили, его можно вернуть на склад тем же количеством. "
                                "Склад списания выбирается в настройках кабинета (меню «Маркет» → «Кабинеты»)."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    auto *topRow = new QHBoxLayout();
    auto *allBtn = new QPushButton(tr("Отметить все"), this);
    auto *noneBtn = new QPushButton(tr("Снять все"), this);
    m_showAll = new QCheckBox(tr("Показывать и те, что уже списаны или пропущены"), this);
    m_siteBtn = new QPushButton(tr("Заказы сайта..."), this);
    topRow->addWidget(allBtn);
    topRow->addWidget(noneBtn);
    topRow->addSpacing(16);
    topRow->addWidget(m_showAll);
    topRow->addStretch();
    topRow->addWidget(m_siteBtn);
    layout->addLayout(topRow);

    m_table = new QTableWidget(0, 8, this);
    m_table->setHorizontalHeaderLabels({QString(), tr("Кабинет"), tr("№ заказа"), tr("Статус"), tr("Дата"), tr("Товары"), tr("Склад"), tr("Что будет сделано")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 1);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(130);
    layout->addWidget(m_log);

    auto *buttons = new QHBoxLayout();
    m_applyBtn = new QPushButton(tr("Списать отмеченные"), this);
    m_applyBtn->setDefault(true);
    m_closeBtn = new QPushButton(tr("Закрыть"), this);
    buttons->addStretch();
    buttons->addWidget(m_applyBtn);
    buttons->addWidget(m_closeBtn);
    layout->addLayout(buttons);

    connect(allBtn, &QPushButton::clicked, this, [this]() { checkAll(true); });
    connect(noneBtn, &QPushButton::clicked, this, [this]() { checkAll(false); });
    connect(m_showAll, &QCheckBox::toggled, this, [this]() { fillTable(); });
    connect(m_siteBtn, &QPushButton::clicked, this, &OrderWriteoffDialog::editSiteOrders);
    connect(m_applyBtn, &QPushButton::clicked, this, &OrderWriteoffDialog::applyChecked);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::itemChanged, this, &OrderWriteoffDialog::updateButtons);
    reload();
}

void OrderWriteoffDialog::log(const QString &line)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss"), line));
    QApplication::processEvents();
}

void OrderWriteoffDialog::reload()
{
    QString err;
    QList<Order> all = m_market;
    int site = 0;
    for (const Order &o : OrderWriteoff::siteQueue(&err)) {
        all.append(o);
        ++site;
    }
    if (!err.isEmpty())
        log(tr("Очередь заказов сайта не прочиталась: %1").arg(err));
    err.clear();
    m_plan = OrderWriteoff::plan(all, &err);
    if (!err.isEmpty())
        log(tr("Ошибка чтения базы: %1").arg(err));
    m_summary->setText(tr("Заказов Маркета в списке: %1, заказов сайта в очереди: %2.").arg(m_market.size()).arg(site));
    fillTable();
}

void OrderWriteoffDialog::fillTable()
{
    m_table->blockSignals(true);
    m_table->setRowCount(0);
    for (int i = 0; i < m_plan.size(); ++i) {
        const Planned &p = m_plan[i];
        const bool quiet = p.action == Action::Done || p.action == Action::Skip;
        if (quiet && !m_showAll->isChecked())
            continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *check = new QTableWidgetItem;
        check->setData(Qt::UserRole, i);
        // Заказы, которые уже едут к покупателю или доставлены, по умолчанию НЕ отмечены: скорее всего, пара давно ушла со склада
        // другим способом (старые заказы за 30 дней не должны списаться по неосторожности). Отметить их можно самому.
        const bool late = p.action == Action::WriteOff && p.order.source == QLatin1String("market") && p.order.status != QLatin1String("PROCESSING");
        if (p.actionable()) {
            check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
            check->setCheckState(late ? Qt::Unchecked : Qt::Checked);
        } else {
            check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        m_table->setItem(row, ColCheck, check);
        m_table->setItem(row, ColSource, new QTableWidgetItem(p.order.source == QLatin1String("site") ? tr("Сайт") : p.order.accountName));
        m_table->setItem(row, ColOrder, new QTableWidgetItem(p.order.ref));
        m_table->setItem(row, ColStatus, new QTableWidgetItem(humanStatus(p.order)));
        m_table->setItem(row, ColDate, new QTableWidgetItem(p.order.shipment));
        auto *items = new QTableWidgetItem(itemsText(p));
        items->setToolTip(itemsText(p));
        m_table->setItem(row, ColItems, items);
        m_table->setItem(row, ColWarehouse, new QTableWidgetItem(p.warehouseName));
        QString text = p.reason;
        if (p.action == Action::WriteOff && text.isEmpty())
            text = tr("будет списано");
        if (late)
            text += tr(" - заказ уже в пути или доставлен: отметьте сами, если пара ещё числится на складе");
        auto *result = new QTableWidgetItem(text);
        result->setToolTip(text);
        QColor color;
        switch (p.action) {
        case Action::WriteOff: color = QColor("#2e7d32"); break;
        case Action::Return: color = QColor("#1565c0"); break;
        case Action::Problem: color = QColor("#c62828"); break;
        default: color = QColor("#757575"); break;
        }
        result->setForeground(color);
        m_table->setItem(row, ColResult, result);
    }
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(ColItems, qMin(m_table->columnWidth(ColItems), 320));
    m_table->blockSignals(false);
    updateButtons();
}

void OrderWriteoffDialog::updateButtons()
{
    int write = 0, ret = 0, units = 0;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto *c = m_table->item(row, ColCheck);
        if (!c || !(c->flags() & Qt::ItemIsUserCheckable) || c->checkState() != Qt::Checked)
            continue;
        const Planned &p = m_plan[c->data(Qt::UserRole).toInt()];
        if (p.action == Action::WriteOff) {
            ++write;
            for (const Item &it : p.items)
                units += it.qty;
        } else if (p.action == Action::Return) {
            ++ret;
        }
    }
    m_applyBtn->setEnabled(!m_busy && write + ret > 0);
    m_applyBtn->setText(write + ret == 0 ? tr("Списать отмеченные")
                                        : ret == 0 ? tr("Списать отмеченные: %1, %2 шт").arg(ordersCount(write)).arg(units)
                                                   : tr("Выполнить отмеченные: списать %1, вернуть %2").arg(write).arg(ret));
}

void OrderWriteoffDialog::checkAll(bool on)
{
    m_table->blockSignals(true);
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *c = m_table->item(row, ColCheck);
        if (c && (c->flags() & Qt::ItemIsUserCheckable))
            c->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    m_table->blockSignals(false);
    updateButtons();
}

QList<Planned> OrderWriteoffDialog::checkedRows() const
{
    QList<Planned> out;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto *c = m_table->item(row, ColCheck);
        if (c && (c->flags() & Qt::ItemIsUserCheckable) && c->checkState() == Qt::Checked)
            out << m_plan[c->data(Qt::UserRole).toInt()];
    }
    return out;
}

void OrderWriteoffDialog::applyChecked()
{
    // Ещё раз сверяем с базой: за время, что окно открыто, остатки и заказы могли измениться.
    const QList<Planned> chosen = checkedRows();
    QList<Order> orders;
    for (const Planned &p : chosen)
        orders << p.order;
    QString err;
    const QList<Planned> fresh = OrderWriteoff::plan(orders, &err);
    QList<Planned> todo;
    int units = 0, write = 0, ret = 0, stale = 0;
    for (const Planned &p : fresh) {
        if (!p.actionable()) {
            ++stale;
            log(tr("Заказ №%1 пропущен: %2").arg(p.order.ref, p.reason));
            continue;
        }
        todo << p;
        if (p.action == Action::WriteOff) {
            ++write;
            for (const Item &it : p.items)
                units += it.qty;
        } else {
            ++ret;
        }
    }
    if (todo.isEmpty()) {
        QMessageBox::information(this, tr("Нечего списывать"), tr("Ни один из отмеченных заказов сейчас списать нельзя - причины в журнале."));
        reload();
        return;
    }
    QStringList what;
    if (write)
        what << tr("списать %1 (%2 шт)").arg(ordersCount(write)).arg(units);
    if (ret)
        what << tr("вернуть на склад отменённые заказы: %1").arg(ret);
    if (QMessageBox::question(this, tr("Подтвердите"), tr("Будет сделано: %1.\n\nКаждый заказ - отдельный документ, его можно отменить в разделе «Документы». Продолжить?").arg(what.join(tr(" и "))))
        != QMessageBox::Yes)
        return;

    m_busy = true;
    updateButtons();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const Outcome r = OrderWriteoff::apply(todo, [this](int done, int total) {
        m_summary->setText(tr("Списываю: %1 из %2...").arg(done).arg(total));
        QApplication::processEvents();
        return true;
    });
    QApplication::restoreOverrideCursor();
    m_busy = false;
    if (r.written > 0 || r.returned > 0)
        m_changed = true;
    if (r.written)
        log(tr("Списано заказов: %1 (%2 шт)").arg(r.written).arg(r.units));
    if (r.returned)
        log(tr("Возвращено на склад заказов: %1 (%2 шт)").arg(r.returned).arg(r.unitsReturned));
    if (!r.documents.isEmpty()) {
        QStringList nums;
        for (int d : r.documents)
            nums << QString::number(d);
        log(tr("Документы: №%1").arg(nums.join(QStringLiteral(", №"))));
    }
    for (const QString &p : r.problems)
        log(tr("НЕ СДЕЛАНО: %1").arg(p));
    if (stale)
        log(tr("Пропущено по свежей проверке: %1").arg(stale));
    reload();
    if (!r.problems.isEmpty())
        QMessageBox::warning(this, tr("Часть заказов не обработана"), tr("Не обработано заказов: %1. Подробности - в журнале окна.").arg(r.problems.size()));
}

void OrderWriteoffDialog::editSiteOrders()
{
    SiteOrdersDialog dlg(this);
    dlg.exec();
    reload();
}

// ---------------------------------------------------------------- Заказы сайта

SiteOrdersDialog::SiteOrdersDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Заказы сайта"));
    resize(820, 520);
    auto *layout = new QVBoxLayout(this);
    auto *info = new QLabel(tr("Сайт передаёт каждый новый заказ в очередь (запрос POST /api/orders на сайте склада, см. web/README.md; тот же сайт, что привязан к этой базе). "
                               "Списывает заказ только кнопка «Списать отмеченные» в окне списания: пара уходит с выбранного ниже склада. "
                               "Если сайт отменил заказ после списания, пара возвращается тем же количеством."),
                            this);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto *form = new QFormLayout();
    auto *warehouse = new QComboBox(this);
    warehouse->addItem(tr("Не выбран (заказы сайта списывать нельзя)"), 0);
    QSqlQuery wq(QStringLiteral("SELECT id, name FROM warehouses ORDER BY name"));
    while (wq.next())
        warehouse->addItem(wq.value(1).toString(), wq.value(0).toInt());
    const int idx = warehouse->findData(OrderWriteoff::siteWarehouse());
    warehouse->setCurrentIndex(idx < 0 ? 0 : idx);
    form->addRow(tr("Склад списания заказов сайта"), warehouse);
    layout->addLayout(form);
    connect(warehouse, qOverload<int>(&QComboBox::currentIndexChanged), this, [warehouse]() { OrderWriteoff::setSiteWarehouse(warehouse->currentData().toInt()); });

    auto *table = new QTableWidget(0, 5, this);
    table->setHorizontalHeaderLabels({tr("№ заказа"), tr("Состояние"), tr("Получен"), tr("Товары"), tr("Документ")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    QSqlQuery q(QStringLiteral("SELECT order_ref, state, to_char(created_at AT TIME ZONE 'UTC', 'DD.MM.YYYY HH24:MI'), lines::text, COALESCE(doc_id, 0) FROM order_writeoffs "
                               "WHERE source = 'site' ORDER BY id DESC LIMIT 300"));
    static const QHash<QString, QString> states = {
        {"pending", "ждёт списания"}, {"writing", "списывается"},       {"written_off", "списан"},        {"cancel_requested", "отменён сайтом, ждёт возврата"},
        {"returning", "возвращается"}, {"returned", "возвращён на склад"}, {"cancelled", "отменён до списания"},
    };
    while (q.next()) {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(q.value(0).toString()));
        table->setItem(row, 1, new QTableWidgetItem(states.value(q.value(1).toString(), q.value(1).toString())));
        table->setItem(row, 2, new QTableWidgetItem(q.value(2).toString()));
        QStringList parts;
        const QString raw = q.value(3).toString();
        // Строка вида [{"key": "A", "qty": 1}, ...] - показываем «A × 1».
        static const QRegularExpression item(QStringLiteral("\"key\"\\s*:\\s*\"([^\"]*)\"[^}]*?\"qty\"\\s*:\\s*(\\d+)|\"qty\"\\s*:\\s*(\\d+)[^}]*?\"key\"\\s*:\\s*\"([^\"]*)\""));
        for (auto it = item.globalMatch(raw); it.hasNext();) {
            const auto m = it.next();
            parts << (m.captured(1).isEmpty() ? QStringLiteral("%1 × %2").arg(m.captured(4), m.captured(3)) : QStringLiteral("%1 × %2").arg(m.captured(1), m.captured(2)));
        }
        table->setItem(row, 3, new QTableWidgetItem(parts.join(QStringLiteral("; "))));
        table->setItem(row, 4, new QTableWidgetItem(q.value(4).toInt() > 0 ? QString::number(q.value(4).toInt()) : QString()));
    }
    table->resizeColumnsToContents();
    layout->addWidget(table, 1);
    if (table->rowCount() == 0) {
        auto *empty = new QLabel(tr("Заказов от сайта пока нет."), this);
        empty->setStyleSheet("color: gray;");
        layout->addWidget(empty);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}
