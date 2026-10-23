#include "productpickdialog.h"
#include "marketcards.h"
#include "productfinder.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum Col { ColSku = 0, ColName, ColOffer, ColPhotos, ColState, ColCount };
}

ProductPickDialog::ProductPickDialog(const QSet<QString> &existing, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Товары для карточек Маркета"));
    resize(860, 560);
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Отметьте товары (Ctrl или Shift + клик, Ctrl+A - все), для которых нужно создать карточки. "
                               "Название, описание, штрихкоды, цена и фото подставятся из базы."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Поиск: артикул, название или штрихкод (можно сканером)"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("Артикул на Маркете"), tr("Фото"), tr("Карточка")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 1);

    QSqlQuery q("SELECT p.id, p.sku, p.name, COALESCE(p.market_sku, ''), (SELECT COUNT(*) FROM product_photos f WHERE f.product_id = p.id) "
                "FROM products p ORDER BY p.sku");
    while (q.next()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QString offer = MarketCards::offerIdOf(q.value(1).toString(), q.value(3).toString());
        const bool has = existing.contains(offer);
        auto put = [&](int col, const QString &text) {
            auto *it = new QTableWidgetItem(text);
            if (has)
                it->setFlags(Qt::NoItemFlags);
            m_table->setItem(row, col, it);
            return it;
        };
        auto *sku = put(ColSku, q.value(1).toString());
        sku->setData(Qt::UserRole, q.value(0).toInt());
        put(ColName, q.value(2).toString());
        put(ColOffer, offer);
        put(ColPhotos, q.value(4).toInt() > 0 ? q.value(4).toString() : tr("нет"));
        put(ColState, has ? tr("уже есть") : QString());
    }
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(ColName, 300);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(tr("Создать карточки"));
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, [this]() {
        if (!selectedProductIds().isEmpty())
            accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_search, &QLineEdit::textChanged, this, &ProductPickDialog::applyFilter);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &ProductPickDialog::updateSummary);
    updateSummary();
    m_search->setFocus();
}

void ProductPickDialog::applyFilter()
{
    const QString text = m_search->text().trimmed();
    QSet<int> byCode;
    // Штрихкод ищется в базе - только когда введённое похоже на код (не на каждую букву названия).
    static const QRegularExpression codeLike(QStringLiteral("^[0-9]{6,}$"));
    if (codeLike.match(text).hasMatch()) {
        const auto r = ProductFinder::find(text);
        for (int id : r.ids)
            byCode.insert(id);
    }
    for (int r = 0; r < m_table->rowCount(); ++r) {
        bool show = text.isEmpty() || byCode.contains(m_table->item(r, ColSku)->data(Qt::UserRole).toInt());
        for (int c = 0; c < ColCount && !show; ++c)
            show = m_table->item(r, c)->text().contains(text, Qt::CaseInsensitive);
        m_table->setRowHidden(r, !show);
    }
}

QList<int> ProductPickDialog::selectedProductIds() const
{
    QList<int> ids;
    for (const QModelIndex &i : m_table->selectionModel()->selectedRows())
        if (!m_table->isRowHidden(i.row()) && (m_table->item(i.row(), ColSku)->flags() & Qt::ItemIsEnabled))
            ids << m_table->item(i.row(), ColSku)->data(Qt::UserRole).toInt();
    return ids;
}

void ProductPickDialog::updateSummary()
{
    m_summary->setText(tr("Выбрано товаров: %1").arg(selectedProductIds().size()));
}
