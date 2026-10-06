#include "productstab.h"
#include "productdialog.h"
#include "barcode.h"
#include "bulkeditdialog.h"
#include "bulkphotosdialog.h"
#include "photostore.h"
#include "photoviewer.h"

#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSqlQuery>

#include <QBuffer>
#include <QScrollBar>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlRecord>
#include <QSqlTableModel>
#include <QTableView>
#include <QVBoxLayout>

namespace {
// market_sku добавлен в таблицу через ALTER TABLE, поэтому всегда последний.
enum Column { ColId = 0, ColSku, ColName, ColDescription, ColPhotoPath, ColPrice, ColCustomCode, ColCreatedAt,
              ColMarketSku };
}

namespace {

// Таблица товаров с миниатюрой фото слева от артикула и крупной подсказкой при наведении.
class ProductsModel : public QSqlTableModel
{
public:
    using QSqlTableModel::QSqlTableModel;

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if (idx.isValid() && idx.column() == ColSku && (role == Qt::DecorationRole || role == Qt::ToolTipRole)) {
            const int id = QSqlTableModel::data(index(idx.row(), ColId)).toInt();
            if (role == Qt::DecorationRole)
                return PhotoCache::thumb(id);
            if (!PhotoCache::hasPhoto(id))
                return QVariant();
            const QByteArray jpeg = PhotoCache::coverJpeg(id);
            return QStringLiteral("<img src=\"data:image/jpeg;base64,%1\" width=\"320\">").arg(QString::fromLatin1(jpeg.toBase64()));
        }
        return QSqlTableModel::data(idx, role);
    }
};

} // namespace

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
    auto *photoBtn = new QPushButton(tr("Фото..."), toolbar);
    auto *bulkEditBtn = new QPushButton(tr("Массовое редактирование..."), toolbar);
    auto *bulkPhotoBtn = new QPushButton(tr("Фото пачкой..."), toolbar);
    toolbarLayout->addWidget(addBtn);
    toolbarLayout->addWidget(editBtn);
    toolbarLayout->addWidget(deleteBtn);
    toolbarLayout->addWidget(photoBtn);
    toolbarLayout->addWidget(bulkEditBtn);
    toolbarLayout->addWidget(bulkPhotoBtn);
    m_editWidgets << addBtn << deleteBtn << bulkEditBtn << bulkPhotoBtn;
    editBtn->setToolTip(tr("Открыть карточку товара"));
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

    m_model = new ProductsModel(this);
    m_model->setTable("products");
    m_model->setEditStrategy(QSqlTableModel::OnManualSubmit);
    m_model->setSort(ColSku, Qt::AscendingOrder);

    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection); // Ctrl/Shift - несколько товаров
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setIconSize(QSize(PhotoCache::kIconSize, PhotoCache::kIconSize));
    m_table->verticalHeader()->setDefaultSectionSize(PhotoCache::kIconSize + 6);
    layout->addWidget(m_table);

    refresh();

    connect(addBtn, &QPushButton::clicked, this, &ProductsTab::addProduct);
    connect(editBtn, &QPushButton::clicked, this, &ProductsTab::editProduct);
    connect(deleteBtn, &QPushButton::clicked, this, &ProductsTab::deleteProduct);
    connect(photoBtn, &QPushButton::clicked, this, &ProductsTab::openSelectedPhoto);
    connect(bulkEditBtn, &QPushButton::clicked, this, &ProductsTab::bulkEdit);
    connect(bulkPhotoBtn, &QPushButton::clicked, this, &ProductsTab::bulkPhotos);
    connect(m_table->verticalScrollBar(), &QScrollBar::valueChanged, this, &ProductsTab::prefetchVisible);
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
    m_table->setColumnWidth(ColSku, 210);
    m_table->setColumnWidth(ColName, 330);
    m_table->setColumnWidth(ColPrice, 90);
    prefetchVisible();
}

void ProductsTab::prefetchVisible()
{
    if (m_model->rowCount() == 0)
        return;
    const int first = qMax(0, m_table->rowAt(0));
    int last = m_table->rowAt(m_table->viewport()->height());
    if (last < 0)
        last = qMin(m_model->rowCount() - 1, first + 40);
    QList<int> ids;
    for (int r = qMax(0, first - 10); r <= qMin(m_model->rowCount() - 1, last + 20); ++r)
        ids << m_model->data(m_model->index(r, ColId)).toInt();
    PhotoCache::prefetch(ids);
    m_table->viewport()->update();
}

void ProductsTab::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    for (QWidget *w : m_editWidgets)
        w->setVisible(!readOnly);
}

QList<int> ProductsTab::selectedProductIds() const
{
    QList<int> ids;
    for (const QModelIndex &idx : m_table->selectionModel()->selectedRows())
        ids << m_model->data(m_model->index(idx.row(), ColId)).toInt();
    return ids;
}

void ProductsTab::bulkEdit()
{
    const QList<int> ids = selectedProductIds();
    if (ids.isEmpty()) {
        QMessageBox::information(this, tr("Выделите товары"),
                                 tr("Выделите несколько товаров в списке (Ctrl или Shift + клик, Ctrl+A - все) "
                                    "и нажмите кнопку ещё раз."));
        return;
    }
    BulkEditDialog dlg(ids, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    refresh();
    emit productsChanged();
}

void ProductsTab::bulkPhotos()
{
    BulkPhotosDialog dlg(selectedProductIds(), this);
    dlg.exec();
    if (dlg.changed()) {
        PhotoCache::clear();
        refresh();
    }
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
    if (idQuery.exec() && idQuery.next()) {
        saveBarcodes(idQuery.value(0).toInt(), d.barcodes);
        savePhotos(idQuery.value(0).toInt(), dlg.photoChanges());
    }
    refresh();
    emit productsChanged();
}

void ProductsTab::editProduct()
{
    const auto sel = m_table->selectionModel()->selectedRows();
    if (sel.isEmpty() && !m_table->currentIndex().isValid())
        return;
    const int row = m_table->currentIndex().isValid() ? m_table->currentIndex().row() : sel.first().row();
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
    if (m_readOnly) {
        dlg.setReadOnly(true);
        dlg.exec();
        return;
    }

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
    savePhotos(productId, dlg.photoChanges());
    refresh();
    emit productsChanged();
}

void ProductsTab::savePhotos(int productId, const PhotoStore::Changes &changes)
{
    QString err;
    if (!PhotoStore::apply(productId, changes, &err))
        QMessageBox::warning(this, tr("Фото не сохранены"), err);
}

void ProductsTab::deleteProduct()
{
    const QList<int> ids = selectedProductIds();
    if (ids.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Удалить товары"),
                               ids.size() == 1 ? tr("Удалить выбранный товар и все связанные остатки/движения?")
                                               : tr("Удалить выбранные товары (%1 шт.) и все связанные остатки, "
                                                    "движения и фото?").arg(ids.size()))
        != QMessageBox::Yes)
        return;

    QSqlDatabase db = QSqlDatabase::database();
    db.transaction();
    bool ok = true;
    QString err;
    for (int i = 0; i < ids.size() && ok; i += 400) {
        const QList<int> chunk = ids.mid(i, 400);
        QStringList marks;
        for (int k = 0; k < chunk.size(); ++k)
            marks << QStringLiteral("?");
        QSqlQuery q;
        q.prepare(QStringLiteral("DELETE FROM products WHERE id IN (%1)").arg(marks.join(',')));
        for (int id : chunk)
            q.addBindValue(id);
        ok = q.exec();
        if (!ok)
            err = q.lastError().text();
    }
    if (!ok || !db.commit()) {
        db.rollback();
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось удалить товары:\n%1").arg(err));
        return;
    }
    for (int id : ids)
        PhotoCache::invalidate(id);
    refresh();
    emit productsChanged();
}

void ProductsTab::openSelectedPhoto()
{
    const int row = m_table->currentIndex().isValid() ? m_table->currentIndex().row() : -1;
    if (row < 0) {
        QMessageBox::information(this, tr("Выберите товар"), tr("Выберите товар в списке."));
        return;
    }
    const QSqlRecord rec = m_model->record(row);
    const QList<int> ids = PhotoStore::ids(rec.value("id").toInt());
    if (ids.isEmpty()) {
        QMessageBox::information(this, tr("Нет фото"), tr("У этого товара пока нет фото. Добавьте их в карточке "
                                                          "товара или кнопкой «Фото пачкой»."));
        return;
    }
    PhotoViewer viewer(ids, QStringLiteral("%1 — %2").arg(rec.value("sku").toString(), rec.value("name").toString()), this);
    viewer.exec();
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
