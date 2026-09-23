#include "productstab.h"
#include "productdialog.h"
#include "barcode.h"

#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSqlQuery>

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlError>
#include <QSqlRecord>
#include <QSqlTableModel>
#include <QTableView>
#include <QUrl>
#include <QVBoxLayout>

namespace {
// market_sku добавлен в таблицу через ALTER TABLE, поэтому всегда последний.
enum Column { ColId = 0, ColSku, ColName, ColDescription, ColPhotoPath, ColPrice, ColCustomCode, ColCreatedAt,
              ColMarketSku };
}

ProductsTab::ProductsTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *toolbar = new QWidget(this);
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    auto *addBtn = new QPushButton(tr("Добавить"), toolbar);
    auto *editBtn = new QPushButton(tr("Изменить"), toolbar);
    auto *deleteBtn = new QPushButton(tr("Удалить"), toolbar);
    auto *photoBtn = new QPushButton(tr("Открыть фото"), toolbar);
    toolbarLayout->addWidget(addBtn);
    toolbarLayout->addWidget(editBtn);
    toolbarLayout->addWidget(deleteBtn);
    toolbarLayout->addWidget(photoBtn);
    toolbarLayout->addSpacing(20);
    m_search = new QLineEdit(toolbar);
    m_search->setPlaceholderText(tr("Поиск: артикул, название или штрихкод (можно сканером)"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(320);
    toolbarLayout->addWidget(m_search, 1);
    layout->addWidget(toolbar);
    m_searchStatus = new QLabel(this);
    m_searchStatus->setStyleSheet("color: gray;");
    layout->addWidget(m_searchStatus);

    m_model = new QSqlTableModel(this);
    m_model->setTable("products");
    m_model->setEditStrategy(QSqlTableModel::OnManualSubmit);
    m_model->setSort(ColSku, Qt::AscendingOrder);

    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table);

    refresh();

    connect(addBtn, &QPushButton::clicked, this, &ProductsTab::addProduct);
    connect(editBtn, &QPushButton::clicked, this, &ProductsTab::editProduct);
    connect(deleteBtn, &QPushButton::clicked, this, &ProductsTab::deleteProduct);
    connect(photoBtn, &QPushButton::clicked, this, &ProductsTab::openSelectedPhoto);
    connect(m_table, &QTableView::doubleClicked, this, &ProductsTab::editProduct);
    connect(m_search, &QLineEdit::textChanged, this, &ProductsTab::applySearch);
    connect(m_search, &QLineEdit::returnPressed, this, &ProductsTab::onSearchEntered);
}

void ProductsTab::refresh()
{
    m_model->select();
    while (m_model->canFetchMore())
        m_model->fetchMore();
    m_model->setHeaderData(ColSku, Qt::Horizontal, tr("Артикул"));
    m_model->setHeaderData(ColName, Qt::Horizontal, tr("Название"));
    m_model->setHeaderData(ColPrice, Qt::Horizontal, tr("Цена"));
    m_model->setHeaderData(ColCustomCode, Qt::Horizontal, tr("Свой код"));
    m_model->setHeaderData(ColMarketSku, Qt::Horizontal, tr("Артикул на Маркете"));
    m_table->setColumnHidden(ColId, true);
    m_table->setColumnHidden(ColDescription, true);
    m_table->setColumnHidden(ColPhotoPath, true);
    m_table->setColumnHidden(ColCreatedAt, true);
}

int ProductsTab::selectedProductId() const
{
    const auto sel = m_table->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return -1;
    return m_model->record(sel.first().row()).value("id").toInt();
}

void ProductsTab::addProduct()
{
    ProductDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const auto d = dlg.data();
    QSqlRecord rec = m_model->record();
    rec.setValue("sku", d.sku);
    rec.setValue("name", d.name);
    rec.setValue("description", d.description);
    rec.setValue("photo_path", d.photoPath);
    rec.setValue("price", d.price);
    rec.setValue("custom_code", d.customCode);
    rec.setValue("market_sku", d.marketSku);
    rec.remove(rec.indexOf("id"));
    rec.remove(rec.indexOf("created_at"));

    if (!m_model->insertRecord(-1, rec) || !m_model->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сохранить товар:\n%1")
                                                       .arg(m_model->lastError().text()));
        m_model->revertAll();
        return;
    }
    QSqlQuery idQuery;
    idQuery.prepare("SELECT id FROM products WHERE sku = ?");
    idQuery.addBindValue(d.sku);
    if (idQuery.exec() && idQuery.next())
        saveBarcodes(idQuery.value(0).toInt(), d.barcodes);
    refresh();
    emit productsChanged();
}

void ProductsTab::editProduct()
{
    const auto sel = m_table->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    const int row = sel.first().row();
    const QSqlRecord rec = m_model->record(row);

    const int productId = rec.value("id").toInt();
    ProductDialog dlg(this);
    dlg.setProductId(productId);
    ProductDialog::ProductData d;
    d.barcodes = Barcode::forProduct(productId);
    d.sku = rec.value("sku").toString();
    d.name = rec.value("name").toString();
    d.description = rec.value("description").toString();
    d.photoPath = rec.value("photo_path").toString();
    d.price = rec.value("price").toDouble();
    d.customCode = rec.value("custom_code").toString();
    d.marketSku = rec.value("market_sku").toString();
    dlg.setData(d);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const auto nd = dlg.data();
    m_model->setData(m_model->index(row, ColSku), nd.sku);
    m_model->setData(m_model->index(row, ColName), nd.name);
    m_model->setData(m_model->index(row, ColDescription), nd.description);
    m_model->setData(m_model->index(row, ColPhotoPath), nd.photoPath);
    m_model->setData(m_model->index(row, ColPrice), nd.price);
    m_model->setData(m_model->index(row, ColCustomCode), nd.customCode);
    m_model->setData(m_model->index(row, ColMarketSku), nd.marketSku);

    if (!m_model->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сохранить изменения:\n%1")
                                                       .arg(m_model->lastError().text()));
        m_model->revertAll();
        return;
    }
    saveBarcodes(productId, nd.barcodes);
    refresh();
    emit productsChanged();
}

void ProductsTab::deleteProduct()
{
    const auto sel = m_table->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Удалить товар"),
                               tr("Удалить выбранный товар и все связанные остатки/движения?"))
        != QMessageBox::Yes)
        return;

    m_model->removeRow(sel.first().row());
    if (!m_model->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось удалить товар:\n%1")
                                                       .arg(m_model->lastError().text()));
        m_model->revertAll();
        return;
    }
    refresh();
    emit productsChanged();
}

void ProductsTab::openSelectedPhoto()
{
    const auto sel = m_table->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    const QString path = m_model->record(sel.first().row()).value("photo_path").toString();
    if (path.isEmpty()) {
        QMessageBox::information(this, tr("Нет фото"), tr("У этого товара не указано фото."));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void ProductsTab::saveBarcodes(int productId, const QStringList &codes)
{
    QString err;
    if (!Barcode::setForProduct(productId, codes, &err))
        QMessageBox::warning(this, tr("Штрихкоды не сохранены"), err);
}

void ProductsTab::applySearch()
{
    const QString term = m_search->text().trimmed();
    if (term.isEmpty()) {
        m_model->setFilter(QString());
        m_searchStatus->clear();
    } else {
        // Фильтр QSqlTableModel - это кусок SQL, поэтому экранируем кавычки и
        // спецсимволы LIKE, чтобы введённый текст искался буквально.
        QString t = term;
        t.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_").replace('\'', "''");
        m_model->setFilter(QStringLiteral("sku ILIKE '%%1%' OR name ILIKE '%%1%' OR market_sku ILIKE '%%1%' "
                                          "OR id IN (SELECT product_id FROM product_barcodes WHERE barcode LIKE '%%1%')")
                               .arg(t));
    }
    m_model->select();
    // Модель подгружает строки порциями - для поиска и выделения нужны все.
    while (m_model->canFetchMore())
        m_model->fetchMore();
    if (!term.isEmpty())
        m_searchStatus->setText(tr("Найдено товаров: %1").arg(m_model->rowCount()));
}

void ProductsTab::selectProduct(int productId)
{
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->record(row).value("id").toInt() == productId) {
            m_table->selectRow(row);
            m_table->scrollTo(m_model->index(row, ColSku));
            return;
        }
    }
}

void ProductsTab::onSearchEntered()
{
    const QString code = Barcode::normalize(m_search->text());
    if (!Barcode::isValid(code))
        return; // обычный текстовый поиск - уже отфильтровано

    const int productId = Barcode::productIdFor(code);
    if (productId >= 0) {
        m_search->setText(code);
        selectProduct(productId);
        m_searchStatus->setText(tr("Штрихкод %1 - найден товар").arg(code));
        m_search->selectAll();
        return;
    }

    // Незнакомый штрихкод с коробки - предлагаем сразу привязать его к товару.
    QStringList items;
    QList<int> ids;
    QSqlQuery q("SELECT id, sku, name FROM products ORDER BY sku");
    while (q.next()) {
        ids << q.value(0).toInt();
        items << QStringLiteral("%1 — %2").arg(q.value(1).toString(), q.value(2).toString());
    }
    if (items.isEmpty()) {
        QMessageBox::information(this, tr("Штрихкод не найден"), tr("Штрихкод %1 не привязан ни к одному товару, "
                                                                   "а товаров ещё нет.").arg(code));
        return;
    }
    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, tr("Новый штрихкод"),
        tr("Штрихкод %1 ещё не привязан ни к одному товару.\nК какому товару его привязать?").arg(code), items, 0,
        false, &ok);
    if (!ok)
        return;
    const int chosenId = ids.value(items.indexOf(choice), -1);
    QString err;
    if (!Barcode::attach(chosenId, code, &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    m_search->setText(code);
    selectProduct(chosenId);
    m_searchStatus->setText(tr("Штрихкод %1 привязан к товару %2").arg(code, choice));
    m_search->selectAll();
}
