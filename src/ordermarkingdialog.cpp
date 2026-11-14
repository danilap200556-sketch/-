#include "ordermarkingdialog.h"

#include "markingorderdialog.h"
#include "markingprintdialog.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum Col { ColOrder, ColSku, ColName, ColOwner, ColNeed, ColIssued, ColPicked, ColMissing, ColNote, ColCount };
}

OrderMarkingDialog::OrderMarkingDialog(const QList<OrderMarking::OrderInput> &orders, QWidget *parent)
    : QDialog(parent)
    , m_orders(orders)
{
    setWindowTitle(tr("Коды «Честного знака» к заказам Маркета"));
    resize(1100, 640);
    auto *l = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("На каждую пару в заказе подбирается один свободный код этого товара того ИП, которому принадлежит склад списания кабинета "
                               "(склад - на вкладке «Склады», склад списания - в настройках кабинета). Подобранные коды печатаются на термоэтикетках и "
                               "записываются на свой заказ: повторно для того же заказа новые коды не берутся."),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    l->addWidget(hint);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    l->addWidget(m_summary);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Заказ"), tr("Артикул"), tr("Название"), tr("ИП"), tr("Нужно"), tr("Уже выдано"), tr("Подобрано"), tr("Не хватает"), tr("Примечание")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    QHeaderView *h = m_table->horizontalHeader();
    h->setResizeContentsPrecision(40);
    h->setSectionResizeMode(ColName, QHeaderView::Stretch);
    h->setSectionResizeMode(ColNote, QHeaderView::Stretch);
    for (int c : {ColOrder, ColSku, ColOwner, ColNeed, ColIssued, ColPicked, ColMissing})
        h->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    l->addWidget(m_table, 1);

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("noticeBanner"));
    m_note->setWordWrap(true);
    m_note->setVisible(false);
    l->addWidget(m_note);

    auto *row = new QHBoxLayout();
    m_printBtn = new QPushButton(this);
    m_printBtn->setProperty("primary", true);
    m_reprintBtn = new QPushButton(this);
    m_reprintBtn->setToolTip(tr("Напечатать ещё раз коды, которые уже выданы под эти заказы (например, после замятия этикетки)"));
    m_orderBtn = new QPushButton(tr("Заказать недостающие…"), this);
    m_orderBtn->setToolTip(tr("Открыть заказ кодов в СУЗ с недостающими позициями (под кабинетом нужного ИП)"));
    auto *close = new QPushButton(tr("Закрыть"), this);
    for (QPushButton *b : {m_printBtn, m_reprintBtn, m_orderBtn})
        row->addWidget(b);
    row->addStretch(1);
    row->addWidget(close);
    for (QPushButton *b : {m_printBtn, m_reprintBtn, m_orderBtn, close})
        b->setAutoDefault(false);
    l->addLayout(row);

    connect(m_printBtn, &QPushButton::clicked, this, [this]() { printNew(); });
    connect(m_reprintBtn, &QPushButton::clicked, this, [this]() { printIssued(); });
    connect(m_orderBtn, &QPushButton::clicked, this, [this]() { orderMissing(); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    rebuild();
}

void OrderMarkingDialog::rebuild()
{
    m_plan = OrderMarking::build(m_orders);
    m_table->setRowCount(int(m_plan.lines.size()));
    for (int r = 0; r < m_plan.lines.size(); ++r) {
        const OrderMarking::Line &ln = m_plan.lines[r];
        const int missing = ln.missing();
        auto put = [&](int col, const QString &text) {
            auto *it = new QTableWidgetItem(text);
            if (col >= ColNeed && col <= ColMissing)
                it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_table->setItem(r, col, it);
            return it;
        };
        put(ColOrder, tr("%1 · №%2").arg(ln.accountName, ln.orderNumber));
        put(ColSku, ln.sku.isEmpty() ? ln.offerId : ln.sku);
        put(ColName, ln.name);
        put(ColOwner, ln.ownerName.isEmpty() ? tr("без ИП") : ln.ownerName);
        put(ColNeed, QString::number(ln.need));
        put(ColIssued, QString::number(ln.issued));
        put(ColPicked, QString::number(ln.picked.size()));
        QTableWidgetItem *miss = put(ColMissing, missing > 0 ? QString::number(missing) : QString());
        if (missing > 0 || !ln.problem.isEmpty()) {
            miss->setForeground(QColor(0xd6, 0x45, 0x45));
            QFont f = miss->font();
            f.setBold(true);
            miss->setFont(f);
        }
        put(ColNote, ln.problem);
    }
    const int picked = m_plan.pickedCount(), issued = m_plan.issuedCount(), missing = m_plan.missingCount();
    m_summary->setText(tr("Заказов: <b>%1</b>%2. Пар: <b>%3</b>. Уже выдано кодов: <b>%4</b>. Подобрано новых: <b>%5</b>. Не хватает: <b>%6</b>.")
                           .arg(m_plan.ordersCount)
                           .arg(m_plan.skippedOrders > 0 ? tr(" (отменённые и пустые пропущены: %1)").arg(m_plan.skippedOrders) : QString())
                           .arg(m_plan.need()).arg(issued).arg(picked).arg(missing));
    m_printBtn->setText(tr("Печатать ЧЗ (%1)…").arg(picked));
    m_printBtn->setEnabled(picked > 0);
    m_reprintBtn->setText(tr("Повторно напечатать выданные (%1)…").arg(issued));
    m_reprintBtn->setEnabled(issued > 0);
    m_orderBtn->setEnabled(missing > 0);
}

void OrderMarkingDialog::printNew()
{
    const QList<int> ids = m_plan.pickedIds();
    if (ids.isEmpty())
        return;
    MarkingPrintDialog dlg(ids, this);
    dlg.exec();
    const int assigned = OrderMarking::markIssued(m_plan);
    m_printedTotal += dlg.printedCount();
    if (dlg.printedCount() > 0) {
        m_note->setText(tr("Напечатано этикеток: %1; кодов записано на заказы: %2.").arg(dlg.printedCount()).arg(assigned));
        m_note->setVisible(true);
    }
    rebuild();
}

void OrderMarkingDialog::printIssued()
{
    const QList<int> ids = m_plan.issuedIds();
    if (ids.isEmpty())
        return;
    MarkingPrintDialog dlg(ids, this);
    dlg.exec();
    if (dlg.printedCount() > 0) {
        m_note->setText(tr("Повторно напечатано этикеток: %1.").arg(dlg.printedCount()));
        m_note->setVisible(true);
    }
}

// Недостающее заказывается по ИП: под кабинетом каждого ИП - свой заказ в СУЗ.
void OrderMarkingDialog::orderMissing()
{
    const QList<int> owners = m_plan.ownersWithMissing();
    for (int owner : owners) {
        QHash<int, int> byProduct;
        for (const OrderMarking::Line &ln : m_plan.lines)
            if (ln.ownerId == owner && ln.missing() > 0)
                byProduct[ln.productId] += ln.missing();
        MarkingOrderDialog dlg(this, owner);
        for (auto it = byProduct.cbegin(); it != byProduct.cend(); ++it)
            dlg.addProduct(it.key(), it.value());
        dlg.exec();
    }
    rebuild();
}
