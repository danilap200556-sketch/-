#include "photoreviewdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
constexpr int kIconSide = 150;
constexpr int kCheckPad = 28; // слева от снимка место под галочку, чтобы она его не закрывала
constexpr int kRowHeight = 215;
}

PhotoReviewDialog::PhotoReviewDialog(const QList<PhotoFetch::Proposal> &proposals, int perProduct, bool replaceExisting, QWidget *parent)
    : QDialog(parent), m_proposals(proposals), m_perProduct(qMax(1, perProduct))
{
    setWindowTitle(tr("Найденные фото"));
    resize(1100, 720);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Отметьте галочкой фото, которые нужно сохранить в карточках. «Проверено» значит, что артикул товара нашёлся в названии, "
                                "подписи или адресе страницы; остальное - только похожие снимки, смотрите внимательно. Двойной щелчок по фото - "
                                "крупный просмотр."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *tools = new QHBoxLayout();
    auto *markBtn = new QPushButton(tr("Отметить проверенные"), this);
    markBtn->setToolTip(tr("Для каждого товара отметить первые %1 проверенных фото, остальное снять").arg(m_perProduct));
    auto *clearBtn = new QPushButton(tr("Снять все отметки"), this);
    m_replace = new QCheckBox(tr("Заменить прежние фото: у товара с отмеченными фото старые будут удалены"), this);
    m_replace->setChecked(replaceExisting);
    tools->addWidget(markBtn);
    tools->addWidget(clearBtn);
    tools->addSpacing(16);
    tools->addWidget(m_replace, 1);
    layout->addLayout(tools);

    m_table = new QTableWidget(m_proposals.size(), 2, this);
    m_table->setHorizontalHeaderLabels({tr("Товар"), tr("Найденные фото")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 230);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(kRowHeight);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    layout->addWidget(m_table, 1);

    for (int row = 0; row < m_proposals.size(); ++row) {
        const PhotoFetch::Proposal &p = m_proposals[row];
        auto *info = new QTableWidgetItem(tr("%1\n%2\n\nФото сейчас: %3").arg(p.sku, p.name).arg(p.existingPhotos));
        info->setToolTip(p.name);
        m_table->setItem(row, 0, info);

        auto *list = new QListWidget(m_table);
        list->setViewMode(QListView::IconMode);
        list->setFlow(QListView::LeftToRight);
        list->setWrapping(false);
        list->setMovement(QListView::Static);
        list->setResizeMode(QListView::Adjust);
        list->setIconSize(QSize(kIconSide + kCheckPad, kIconSide));
        list->setSpacing(6);
        list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setSelectionMode(QAbstractItemView::NoSelection);
        int verifiedTaken = 0;
        for (int i = 0; i < p.candidates.size(); ++i) {
            const PhotoFetch::Candidate &c = p.candidates[i];
            QPixmap pix;
            pix.loadFromData(c.photo.thumb.isEmpty() ? c.photo.data : c.photo.thumb);
            const QPixmap scaled = pix.scaled(kIconSide, kIconSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPixmap padded(kIconSide + kCheckPad, kIconSide);
            padded.fill(Qt::transparent);
            {
                QPainter painter(&padded);
                painter.drawPixmap(kCheckPad + (kIconSide - scaled.width()) / 2, (kIconSide - scaled.height()) / 2, scaled);
            }
            auto *item = new QListWidgetItem(QIcon(padded),
                                             tr("%1\n%2").arg(c.source, c.verified ? tr("проверено") : tr("не проверено")), list);
            item->setToolTip(tr("%1\n%2\n%3").arg(c.source, c.origin, c.title));
            item->setSizeHint(QSize(kIconSide + kCheckPad + 12, kIconSide + 44));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            const bool take = c.verified && verifiedTaken < m_perProduct;
            if (take)
                ++verifiedTaken;
            item->setCheckState(take ? Qt::Checked : Qt::Unchecked);
        }
        connect(list, &QListWidget::itemChanged, this, [this]() { updateSummary(); });
        connect(list, &QListWidget::itemDoubleClicked, this, [this, list](QListWidgetItem *item) { showFull(list, list->row(item)); });
        m_table->setCellWidget(row, 1, list);
        m_lists << list;
    }

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);
    auto *buttons = new QDialogButtonBox(this);
    auto *saveBtn = buttons->addButton(tr("Сохранить выбранное"), QDialogButtonBox::AcceptRole);
    buttons->addButton(tr("Отмена"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    Q_UNUSED(saveBtn)

    connect(markBtn, &QPushButton::clicked, this, &PhotoReviewDialog::markVerified);
    connect(clearBtn, &QPushButton::clicked, this, &PhotoReviewDialog::clearMarks);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateSummary();
}

QList<PhotoFetch::Choice> PhotoReviewDialog::choices() const
{
    QList<PhotoFetch::Choice> out;
    for (int row = 0; row < m_proposals.size(); ++row) {
        PhotoFetch::Choice ch;
        ch.productId = m_proposals[row].productId;
        ch.sku = m_proposals[row].sku;
        for (int i = 0; i < m_lists[row]->count(); ++i)
            if (m_lists[row]->item(i)->checkState() == Qt::Checked)
                ch.chosen << m_proposals[row].candidates[i];
        if (!ch.chosen.isEmpty())
            out << ch;
    }
    return out;
}

bool PhotoReviewDialog::replaceExisting() const
{
    return m_replace->isChecked();
}

int PhotoReviewDialog::selectedCount() const
{
    int n = 0;
    for (const QListWidget *list : m_lists)
        for (int i = 0; i < list->count(); ++i)
            n += list->item(i)->checkState() == Qt::Checked;
    return n;
}

void PhotoReviewDialog::updateSummary()
{
    int photos = 0, products = 0;
    for (const QListWidget *list : m_lists) {
        int here = 0;
        for (int i = 0; i < list->count(); ++i)
            here += list->item(i)->checkState() == Qt::Checked;
        photos += here;
        products += here > 0;
    }
    m_summary->setText(tr("Отмечено фото: %1, товаров: %2 из %3").arg(photos).arg(products).arg(m_lists.size()));
}

void PhotoReviewDialog::markVerified()
{
    for (int row = 0; row < m_lists.size(); ++row) {
        int taken = 0;
        for (int i = 0; i < m_lists[row]->count(); ++i) {
            const bool take = m_proposals[row].candidates[i].verified && taken < m_perProduct;
            taken += take;
            m_lists[row]->item(i)->setCheckState(take ? Qt::Checked : Qt::Unchecked);
        }
    }
}

void PhotoReviewDialog::clearMarks()
{
    for (QListWidget *list : std::as_const(m_lists))
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setCheckState(Qt::Unchecked);
}

void PhotoReviewDialog::showFull(QListWidget *list, int index)
{
    const int row = m_lists.indexOf(list);
    if (row < 0 || index < 0 || index >= m_proposals[row].candidates.size())
        return;
    const PhotoFetch::Candidate &c = m_proposals[row].candidates[index];
    QDialog dlg(this);
    dlg.setWindowTitle(tr("%1 - %2").arg(m_proposals[row].sku, c.source));
    auto *l = new QVBoxLayout(&dlg);
    auto *label = new QLabel(&dlg);
    QPixmap pix;
    pix.loadFromData(c.photo.data);
    label->setPixmap(pix.scaled(780, 780, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    label->setAlignment(Qt::AlignCenter);
    l->addWidget(label);
    auto *src = new QLabel(tr("%1%2").arg(c.origin, c.verified ? tr("\nАртикул подтверждён.") : tr("\nАртикул не подтверждён.")), &dlg);
    src->setTextInteractionFlags(Qt::TextSelectableByMouse);
    src->setWordWrap(true);
    l->addWidget(src);
    dlg.exec();
}
