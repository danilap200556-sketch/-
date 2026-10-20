#include "stocktab.h"
#include "database.h"
#include "movementdialog.h"
#include "producteditor.h"
#include "productfinder.h"
#include "photostore.h"

#include <QGroupBox>
#include <QLabel>
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
    m_editWidgets << receiptBtn << writeOffBtn << transferBtn << inventoryBtn;
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
    m_movementModel = new QSqlQueryModel(this);
    m_movementTable = new QTableView(movementBox);
    m_movementTable->setModel(m_movementModel);
    m_movementTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_movementTable->horizontalHeader()->setStretchLastSection(true);
    movementLayout->addWidget(m_movementTable);
    splitter->addWidget(movementBox);

    refresh();

    connect(receiptBtn, &QPushButton::clicked, this, &StockTab::doReceipt);
    connect(writeOffBtn, &QPushButton::clicked, this, &StockTab::doWriteOff);
    connect(transferBtn, &QPushButton::clicked, this, &StockTab::doTransfer);
    connect(inventoryBtn, &QPushButton::clicked, this, &StockTab::doInventoryAdjust);
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
    q.prepare(QStringLiteral(
        "SELECT m.created_at AS \"Дата\", p.sku AS \"Артикул\", w.name AS \"Склад\", "
        "       m.type AS \"Тип\", m.delta AS \"Изменение\", m.comment AS \"Комментарий\" "
        "FROM stock_movements m "
        "JOIN products p ON p.id = m.product_id "
        "JOIN warehouses w ON w.id = m.warehouse_id %1"
        "ORDER BY m.id DESC LIMIT 500").arg(term.isEmpty() ? QStringLiteral(" ") : QStringLiteral(" WHERE") + QLatin1String(kSearchWhere)));
    if (!term.isEmpty())
        for (int i = 0; i < 4; ++i)
            q.addBindValue(likeOf(term));
    q.exec();
    m_movementModel->setQuery(std::move(q));
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
