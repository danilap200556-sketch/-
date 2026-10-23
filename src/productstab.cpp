#include "productstab.h"
#include "productdialog.h"
#include "producteditor.h"
#include "productfinder.h"
#include "barcode.h"
#include "bulkeditdialog.h"
#include "bulkphotosdialog.h"
#include "marketphotosdialog.h"
#include "photostore.h"
#include "photoviewer.h"
#include "productexport.h"
#include "productimportdialog.h"

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

protected:
    // Описание и старый путь к фото в списке не показываются (колонки скрыты), а описания бывают длинными:
    // не тянем их с сервера на каждом обновлении списка.
    QString selectStatement() const override
    {
        QString s = QSqlTableModel::selectStatement();
        const int from = s.indexOf(QLatin1String(" FROM "));
        if (from > 0) {
            QString head = s.left(from);
            head.replace(QLatin1String("\"description\""), QLatin1String("NULL AS \"description\""));
            head.replace(QLatin1String("\"photo_path\""), QLatin1String("NULL AS \"photo_path\""));
            s = head + s.mid(from);
        }
        return s;
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
    auto *marketPhotoBtn = new QPushButton(tr("Фото с Маркета..."), toolbar);
    marketPhotoBtn->setToolTip(tr("Скачать фото товаров с Яндекс Маркета (по артикулу) и сохранить в карточки: "
                                  "выделенным товарам или всем, у кого ещё нет фото."));
    toolbarLayout->addWidget(addBtn);
    toolbarLayout->addWidget(editBtn);
    toolbarLayout->addWidget(deleteBtn);
    toolbarLayout->addWidget(photoBtn);
    toolbarLayout->addWidget(bulkEditBtn);
    toolbarLayout->addWidget(bulkPhotoBtn);
    toolbarLayout->addWidget(marketPhotoBtn);
    auto *updateBtn = new QPushButton(tr("Обновить из Excel..."), toolbar);
    updateBtn->setToolTip(tr("Загрузить изменения карточек из Excel: название, описание, цена, штрихкоды и т.д. - сразу для многих товаров. "
                             "Остатки не меняются."));
    toolbarLayout->addWidget(updateBtn);
    m_editWidgets << updateBtn;
    auto *exportBtn = new QPushButton(tr("Выгрузить в Excel..."), toolbar);
    exportBtn->setToolTip(tr("Полные данные товаров: артикул, название, описание, цена, штрихкоды, остатки по складам, места хранения.\n"
                             "Выгружается то, что сейчас в списке (с учётом поиска); если отмечено несколько товаров - можно выгрузить только их."));
    toolbarLayout->addWidget(exportBtn); // не в m_editWidgets: выгружать может и роль «только просмотр»
    m_editWidgets << addBtn << deleteBtn << bulkEditBtn << bulkPhotoBtn << marketPhotoBtn;
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


    connect(addBtn, &QPushButton::clicked, this, &ProductsTab::addProduct);
    connect(editBtn, &QPushButton::clicked, this, &ProductsTab::editProduct);
    connect(deleteBtn, &QPushButton::clicked, this, &ProductsTab::deleteProduct);
    connect(photoBtn, &QPushButton::clicked, this, &ProductsTab::openSelectedPhoto);
    connect(bulkEditBtn, &QPushButton::clicked, this, &ProductsTab::bulkEdit);
    connect(bulkPhotoBtn, &QPushButton::clicked, this, &ProductsTab::bulkPhotos);
    connect(marketPhotoBtn, &QPushButton::clicked, this, &ProductsTab::marketPhotos);
    connect(exportBtn, &QPushButton::clicked, this, &ProductsTab::exportExcel);
    connect(updateBtn, &QPushButton::clicked, this, &ProductsTab::updateFromExcel);
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

void ProductsTab::marketPhotos()
{
    MarketPhotosDialog dlg(selectedProductIds(), this);
    dlg.exec();
    if (dlg.changed()) {
        PhotoCache::clear();
        refresh();
    }
}

void ProductsTab::updateFromExcel()
{
    ProductImportDialog dlg(this);
    dlg.exec();
    if (dlg.applied()) {
        PhotoCache::clear();
        refresh();
        emit productsChanged();
    }
}

void ProductsTab::exportExcel()
{
    QList<int> shown;
    for (int r = 0; r < m_model->rowCount(); ++r)
        shown << m_model->data(m_model->index(r, ColId)).toInt();
    if (shown.isEmpty()) {
        QMessageBox::information(this, tr("Нечего выгружать"), tr("В списке нет товаров."));
        return;
    }
    const QString term = m_search->text().trimmed();
    QList<int> ids = shown;
    QString note = term.isEmpty() ? tr("все товары") : tr("найденные по поиску «%1»").arg(term);

    const QList<int> picked = selectedProductIds();
    if (picked.size() >= 2 && picked.size() < shown.size()) {
        QMessageBox box(QMessageBox::Question, tr("Что выгрузить?"),
                        tr("Отмечено товаров: %1, в списке: %2.").arg(picked.size()).arg(shown.size()), QMessageBox::NoButton, this);
        QAbstractButton *onlyPicked = box.addButton(tr("Только отмеченные (%1)").arg(picked.size()), QMessageBox::AcceptRole);
        box.addButton(tr("Все в списке (%1)").arg(shown.size()), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == nullptr || box.standardButton(box.clickedButton()) == QMessageBox::Cancel)
            return;
        if (box.clickedButton() == onlyPicked) {
            ids = picked;
            note = tr("отмеченные");
        }
    }
    // Без фильтра и без выбора - все товары одним запросом, без списка идентификаторов.
    ProductExport::exportToFile(this, (term.isEmpty() && ids.size() == shown.size()) ? nullptr : &ids, note);
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
    // Товар и его штрихкоды - одним запросом (а не вставка через модель с перечитыванием всей таблицы,
    // потом запрос id и отдельная запись штрихкодов).
    QStringList codes;
    for (const QString &raw : d.barcodes) {
        const QString c = Barcode::normalize(raw);
        if (!c.isEmpty() && !codes.contains(c))
            codes << c;
    }
    QString sql = QStringLiteral(
        "WITH p AS (INSERT INTO products (sku, name, description, photo_path, price, custom_code, market_sku, %1) "
        "           VALUES (?, ?, ?, ?, ?, ?, ?, %2) RETURNING id)").arg(ProductMarket::insertColumns(), ProductMarket::insertMarks());
    if (!codes.isEmpty())
        sql += QStringLiteral(", b AS (INSERT INTO product_barcodes (barcode, product_id) SELECT c.code, p.id FROM p, "
                              "(VALUES %1) c(code))").arg(QStringList(codes.size(), QStringLiteral("(?)")).join(QLatin1Char(',')));
    sql += QStringLiteral(" SELECT id FROM p");
    QSqlQuery ins;
    ins.prepare(sql);
    ins.addBindValue(d.sku);
    ins.addBindValue(d.name);
    ins.addBindValue(d.description);
    ins.addBindValue(d.photoPath);
    ins.addBindValue(d.price);
    ins.addBindValue(d.customCode);
    ins.addBindValue(d.marketSku);
    ProductMarket::bind(ins, d.market);
    for (const QString &c : codes)
        ins.addBindValue(c);
    if (!ins.exec() || !ins.next()) {
        const QString text = ins.lastError().text();
        const bool barcodeClash = ins.lastError().nativeErrorCode() == QLatin1String("23505") && text.contains(QLatin1String("product_barcodes"));
        QMessageBox::warning(this, tr("Ошибка"),
                             barcodeClash ? tr("Не удалось сохранить товар:\nодин из штрихкодов уже привязан к другому товару.")
                                          : tr("Не удалось сохранить товар:\n%1").arg(text));
        return;
    }
    savePhotos(ins.value(0).toInt(), dlg.photoChanges());
    refresh();
    emit productsChanged();
}

void ProductsTab::editProduct()
{
    const QModelIndex cur = m_table->currentIndex();
    if (!cur.isValid())
        return;
    const int productId = m_model->data(m_model->index(cur.row(), ColId)).toInt();
    const QString oldSku = m_model->data(m_model->index(cur.row(), ColSku)).toString();
    if (!ProductEditor::edit(productId, this, m_readOnly))
        return;
    // Перечитываем только правленую строку, а не весь список (в большом каталоге это секунды). Если артикул
    // поменялся, строка могла сменить место при сортировке - тогда обновляем весь список.
    PhotoCache::invalidate(productId);
    if (m_model->selectRow(cur.row()) && m_model->data(m_model->index(cur.row(), ColSku)).toString() == oldSku) {
        prefetchVisible();
    } else {
        refresh();
        selectProduct(productId);
    }
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

void ProductsTab::searchFor(const QString &text)
{
    m_search->setText(text);
    onSearchEntered();
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
