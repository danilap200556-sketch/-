#include "stocktab.h"
#include "batchdialog.h"
#include "cancelmovementsdialog.h"
#include "database.h"
#include "movementdialog.h"
#include "producteditor.h"
#include "productfinder.h"
#include "photostore.h"

#include <QGroupBox>
#include <QLabel>
#include <QMenu>
#include <QSet>
#include <QBrush>
#include <QLineEdit>
#include <QTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QScrollBar>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QTableView>
#include <QVBoxLayout>

namespace {

// Остатки с миниатюрой фото рядом с артикулом; id товара - последняя (скрытая) колонка.
class StockModel : public QSqlQueryModel
{
public:
    using QSqlQueryModel::QSqlQueryModel;

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if (idx.isValid() && idx.column() == 0 && role == Qt::DecorationRole)
            return PhotoCache::thumb(QSqlQueryModel::data(index(idx.row(), columnCount() - 1)).toInt());
        return QSqlQueryModel::data(idx, role);
    }
};

// Журнал: отменённые записи и сами отмены - серым, чтобы действующие операции были видны сразу.
class MovementModel : public QSqlQueryModel
{
public:
    using QSqlQueryModel::QSqlQueryModel;

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if (idx.isValid() && role == Qt::ForegroundRole) {
            const QString status = QSqlQueryModel::data(index(idx.row(), columnCount() - 1)).toString();
            if (!status.isEmpty())
                return QBrush(Qt::gray);
        }
        return QSqlQueryModel::data(idx, role);
    }
};

} // namespace

StockTab::StockTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *toolbar = new QHBoxLayout();
    auto *receiptBtn = new QPushButton(tr("Приход"), this);
    auto *writeOffBtn = new QPushButton(tr("Списание"), this);
    auto *transferBtn = new QPushButton(tr("Перемещение"), this);
    auto *inventoryBtn = new QPushButton(tr("Инвентаризация"), this);
    toolbar->addWidget(receiptBtn);
    toolbar->addWidget(writeOffBtn);
    toolbar->addWidget(transferBtn);
    toolbar->addWidget(inventoryBtn);
    auto *batchBtn = new QPushButton(tr("Много позиций сразу"), this);
    batchBtn->setToolTip(tr("Приход или списание списком: сканируйте штрихкоды или вставьте список из Excel, "
                            "всё проводится одним документом"));
    toolbar->addWidget(batchBtn);
    m_editWidgets << receiptBtn << writeOffBtn << transferBtn << inventoryBtn << batchBtn;
    toolbar->addSpacing(16);
    auto *cardBtn = new QPushButton(tr("Карточка / быстрая правка"), this);
    cardBtn->setToolTip(tr("Открыть карточку выбранного товара: название, цену, штрихкоды и остатки можно поправить сразу"));
    toolbar->addWidget(cardBtn);
    toolbar->addSpacing(16);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Поиск: артикул, название или штрихкод (можно сканером)"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(300);
    toolbar->addWidget(m_search, 1);
    layout->addLayout(toolbar);
    m_searchStatus = new QLabel(this);
    m_searchStatus->setStyleSheet("color: gray;");
    layout->addWidget(m_searchStatus);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(250);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    layout->addWidget(splitter);

    auto *stockBox = new QGroupBox(tr("Остатки по складам"), this);
    auto *stockLayout = new QVBoxLayout(stockBox);
    m_stockModel = new StockModel(this);
    m_stockTable = new QTableView(stockBox);
    m_stockTable->setModel(m_stockModel);
    m_stockTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_stockTable->horizontalHeader()->setStretchLastSection(true);
    m_stockTable->setIconSize(QSize(PhotoCache::kIconSize, PhotoCache::kIconSize));
    m_stockTable->verticalHeader()->setDefaultSectionSize(PhotoCache::kIconSize + 6);
    connect(m_stockTable->verticalScrollBar(), &QScrollBar::valueChanged, this, &StockTab::prefetchVisible);
    stockLayout->addWidget(m_stockTable);
    splitter->addWidget(stockBox);

    auto *movementBox = new QGroupBox(tr("Журнал движений (последние 500)"), this);
    auto *movementLayout = new QVBoxLayout(movementBox);
    auto *cancelRow = new QHBoxLayout();
    auto *cancelBtn = new QPushButton(tr("Отменить операцию..."), movementBox);
    cancelBtn->setToolTip(tr("Отменить выбранную запись журнала (или несколько): остаток вернётся как был, в журнале появится запись «Отмена»"));
    auto *cancelHint = new QLabel(tr("Ошиблись? Выберите запись в журнале (правая кнопка мыши - то же самое). "
                                     "Приход, списание, инвентаризацию и перемещение можно отменить."), movementBox);
    cancelHint->setStyleSheet("color: gray;");
    cancelRow->addWidget(cancelBtn);
    cancelRow->addWidget(cancelHint, 1);
    movementLayout->addLayout(cancelRow);
    m_editWidgets << cancelBtn << cancelHint;
    m_movementModel = new MovementModel(this);
    m_movementTable = new QTableView(movementBox);
    m_movementTable->setModel(m_movementModel);
    m_movementTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_movementTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_movementTable->setSelectionMode(QAbstractItemView::ExtendedSelection); // Ctrl/Shift - несколько записей
    m_movementTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_movementTable->horizontalHeader()->setStretchLastSection(true);
    movementLayout->addWidget(m_movementTable);
    splitter->addWidget(movementBox);

    refresh();

    connect(receiptBtn, &QPushButton::clicked, this, &StockTab::doReceipt);
    connect(writeOffBtn, &QPushButton::clicked, this, &StockTab::doWriteOff);
    connect(transferBtn, &QPushButton::clicked, this, &StockTab::doTransfer);
    connect(inventoryBtn, &QPushButton::clicked, this, &StockTab::doInventoryAdjust);
    connect(batchBtn, &QPushButton::clicked, this, &StockTab::doBatch);
    connect(cancelBtn, &QPushButton::clicked, this, &StockTab::cancelSelectedMovements);
    connect(m_movementTable, &QTableView::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (m_readOnly || !m_movementTable->indexAt(pos).isValid())
            return;
        QMenu menu(this);
        QAction *act = menu.addAction(tr("Отменить операцию..."));
        if (menu.exec(m_movementTable->viewport()->mapToGlobal(pos)) == act)
            cancelSelectedMovements();
    });
    connect(cardBtn, &QPushButton::clicked, this, &StockTab::editSelectedProduct);
    connect(m_stockTable, &QTableView::doubleClicked, this, &StockTab::editSelectedProduct);
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_searchTimer, &QTimer::timeout, this, [this]() {
        refreshStock();
        refreshMovements();
    });
    // Сканер заканчивает код Enter'ом: сразу показываем результат, не дожидаясь паузы.
    connect(m_search, &QLineEdit::returnPressed, this, [this]() {
        m_searchTimer->stop();
        refreshStock();
        refreshMovements();
    });
}

QString StockTab::searchTerm() const
{
    return m_search->text().trimmed();
}

void StockTab::editSelectedProduct()
{
    const QModelIndex cur = m_stockTable->currentIndex();
    if (!cur.isValid()) {
        QMessageBox::information(this, tr("Выберите строку"), tr("Выберите товар в таблице остатков."));
        return;
    }
    const int idCol = m_stockModel->columnCount() - 1;
    const int productId = m_stockModel->data(m_stockModel->index(cur.row(), idCol)).toInt();
    if (ProductEditor::edit(productId, this, m_readOnly)) {
        refresh();
        emit productEdited();
    }
}

void StockTab::refresh()
{
    PhotoCache::clear(); // фото могли поменять на другом компьютере
    refreshStock();
    refreshMovements();
}

void StockTab::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    for (QWidget *w : m_editWidgets)
        w->setVisible(!readOnly);
}

void StockTab::prefetchVisible()
{
    const int rows = m_stockModel->rowCount();
    if (rows == 0)
        return;
    const int first = qMax(0, m_stockTable->rowAt(0));
    int last = m_stockTable->rowAt(m_stockTable->viewport()->height());
    if (last < 0)
        last = qMin(rows - 1, first + 40);
    const int idCol = m_stockModel->columnCount() - 1;
    QList<int> ids;
    for (int r = qMax(0, first - 10); r <= qMin(rows - 1, last + 20); ++r)
        ids << m_stockModel->data(m_stockModel->index(r, idCol)).toInt();
    PhotoCache::prefetch(ids);
    m_stockTable->viewport()->update();
}

namespace {

// Условие поиска по артикулу, названию, артикулу на Маркете и штрихкоду; binds - 4 одинаковых значения.
const char *kSearchWhere =
    " (p.sku ILIKE ? OR p.name ILIKE ? OR p.market_sku ILIKE ? "
    "OR EXISTS (SELECT 1 FROM product_barcodes b WHERE b.product_id = p.id AND b.barcode ILIKE ?)) ";

QString likeOf(const QString &term)
{
    QString t = term;
    t.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('%'), QLatin1String("\\%"))
        .replace(QLatin1Char('_'), QLatin1String("\\_"));
    return QLatin1Char('%') + t + QLatin1Char('%');
}

} // namespace

void StockTab::refreshStock()
{
    const QString term = searchTerm();
    QSqlQuery q;
    q.prepare(QStringLiteral(
        "SELECT p.sku AS \"Артикул\", p.name AS \"Название\", w.name AS \"Склад\", "
        "       l.code AS \"Место хранения\", s.quantity AS \"Остаток\", p.id AS \"id\" "
        "FROM stock s "
        "JOIN products p ON p.id = s.product_id "
        "JOIN warehouses w ON w.id = s.warehouse_id "
        "LEFT JOIN locations l ON l.id = s.location_id %1"
        "ORDER BY p.sku, w.name").arg(term.isEmpty() ? QStringLiteral(" ") : QStringLiteral(" WHERE") + QLatin1String(kSearchWhere)));
    if (!term.isEmpty())
        for (int i = 0; i < 4; ++i)
            q.addBindValue(likeOf(term));
    q.exec();
    m_stockModel->setQuery(std::move(q));
    if (m_stockModel->lastError().isValid())
        qWarning("Остатки: %s", qPrintable(m_stockModel->lastError().text()));
    m_stockTable->setColumnHidden(m_stockModel->columnCount() - 1, true);
    m_searchStatus->setText(term.isEmpty() ? QString() : tr("Найдено строк остатков: %1").arg(m_stockModel->rowCount()));
    prefetchVisible();
}

void StockTab::refreshMovements()
{
    const QString term = searchTerm();
    QSqlQuery q;
    // Последняя колонка - статус отмены (по ней же серый цвет строки, см. MovementModel).
    q.prepare(QStringLiteral(
        "SELECT m.id AS \"№\", m.created_at AS \"Дата\", p.sku AS \"Артикул\", w.name AS \"Склад\", "
        "       CASE m.type WHEN 'receipt' THEN 'Приход' WHEN 'writeoff' THEN 'Списание' "
        "                   WHEN 'transfer_out' THEN 'Перемещение (расход)' WHEN 'transfer_in' THEN 'Перемещение (приход)' "
        "                   WHEN 'inventory_adjust' THEN 'Инвентаризация' WHEN 'cancel' THEN 'Отмена' ELSE m.type END AS \"Тип\", "
        "       m.delta AS \"Изменение\", m.comment AS \"Комментарий\", "
        "       CASE WHEN rv.id IS NOT NULL THEN 'отменена записью №' || rv.id "
        "            WHEN m.reverts_id IS NOT NULL THEN 'отменяет запись №' || m.reverts_id ELSE '' END AS \"Отмена\" "
        "FROM stock_movements m "
        "JOIN products p ON p.id = m.product_id "
        "JOIN warehouses w ON w.id = m.warehouse_id "
        "LEFT JOIN stock_movements rv ON rv.reverts_id = m.id %1"
        "ORDER BY m.id DESC LIMIT 500").arg(term.isEmpty() ? QStringLiteral(" ") : QStringLiteral(" WHERE") + QLatin1String(kSearchWhere)));
    if (!term.isEmpty())
        for (int i = 0; i < 4; ++i)
            q.addBindValue(likeOf(term));
    q.exec();
    m_movementModel->setQuery(std::move(q));
    m_movementTable->setColumnWidth(0, 60);
    m_movementTable->setColumnWidth(1, 150);
}

void StockTab::doReceipt()
{
    MovementDialog dlg(MovementDialog::Kind::Receipt, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!Database::adjustStock(dlg.productId(), dlg.warehouseId(), dlg.quantity(),
                                Database::MovementType::Receipt, dlg.comment(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

void StockTab::doWriteOff()
{
    MovementDialog dlg(MovementDialog::Kind::WriteOff, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!Database::adjustStock(dlg.productId(), dlg.warehouseId(), -dlg.quantity(),
                                Database::MovementType::WriteOff, dlg.comment(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

void StockTab::doTransfer()
{
    MovementDialog dlg(MovementDialog::Kind::Transfer, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!Database::transferStock(dlg.productId(), dlg.warehouseId(), dlg.destWarehouseId(),
                                  dlg.quantity(), dlg.comment(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

void StockTab::doInventoryAdjust()
{
    MovementDialog dlg(MovementDialog::Kind::InventoryAdjust, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!Database::inventoryAdjust(dlg.productId(), dlg.warehouseId(), dlg.quantity(), dlg.comment(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

void StockTab::doBatch()
{
    BatchDialog dlg(BatchDialog::Kind::Receipt, this);
    dlg.exec();
    if (dlg.applied())
        refresh();
}

void StockTab::cancelSelectedMovements()
{
    QList<int> picked;
    for (const QModelIndex &idx : m_movementTable->selectionModel()->selectedRows())
        picked << m_movementModel->data(m_movementModel->index(idx.row(), 0)).toInt();
    if (picked.isEmpty()) {
        QMessageBox::information(this, tr("Выберите запись"),
                                 tr("Выберите в журнале запись, которую нужно отменить (Ctrl или Shift + клик - несколько)."));
        return;
    }

    // Запись может быть частью документа (пакет, импорт, правка карточки): предлагаем отменить документ целиком.
    // Перемещение (пара записей) отменяется целиком всегда - его отдельно предлагать не нужно.
    const Database::CancelPlan first = Database::planCancel(picked);
    QSet<int> covered(picked.begin(), picked.end());
    for (const auto &r : first.rows)
        covered.insert(r.id);
    QList<int> document;
    for (int id : std::as_const(picked))
        for (int d : Database::documentMovementIds(id))
            if (!document.contains(d))
                document << d;
    bool partial = false;
    for (int d : std::as_const(document))
        if (!covered.contains(d))
            partial = true;

    QList<int> ids = picked;
    if (partial) {
        QMessageBox box(QMessageBox::Question, tr("Что отменить?"),
                        tr("Выбрано записей: %1, а в этом документе их %2 (пакет, импорт или правка карточки).")
                            .arg(picked.size()).arg(document.size()), QMessageBox::NoButton, this);
        QAbstractButton *whole = box.addButton(tr("Весь документ (%1)").arg(document.size()), QMessageBox::AcceptRole);
        box.addButton(tr("Только выбранные (%1)").arg(picked.size()), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == nullptr || box.standardButton(box.clickedButton()) == QMessageBox::Cancel)
            return;
        if (box.clickedButton() == whole)
            ids = document;
    }

    CancelMovementsDialog dlg(ids, this);
    dlg.exec();
    if (dlg.cancelled())
        refresh();
}
