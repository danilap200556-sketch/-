#include "productstab.h"
#include "productdialog.h"

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
    toolbarLayout->addStretch();
    layout->addWidget(toolbar);

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
}

void ProductsTab::refresh()
{
    m_model->select();
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

    ProductDialog dlg(this);
    ProductDialog::ProductData d;
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
