#include "documentstab.h"
#include "cancelmovementsdialog.h"
#include "database.h"
#include "theme.h"

#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
enum DocCol { DColId = 0, DColDate, DColKind, DColWarehouse, DColPositions, DColUnits, DColAuthor, DColComment, DColStatus, DColCount };
enum LineCol { LColSku = 0, LColName, LColWarehouse, LColDelta, LColJournal, LColState, LColCount };

QTableWidgetItem *numberItem(qint64 v)
{
    auto *it = new QTableWidgetItem;
    it->setData(Qt::DisplayRole, v);
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return it;
}
} // namespace

DocumentsTab::DocumentsTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *top = new QHBoxLayout();
    m_kind = new QComboBox(this);
    m_kind->addItem(tr("Все документы"), QString());
    for (const char *k : {"writeoff", "receipt", "transfer", "inventory", "cancel"})
        m_kind->addItem(Documents::kindTitle(QString::fromLatin1(k)) + QStringLiteral("…").left(0), QString::fromLatin1(k));
    m_kind->setItemText(1, tr("Списания"));
    m_kind->setItemText(2, tr("Приходы"));
    m_kind->setItemText(3, tr("Перемещения"));
    m_kind->setItemText(4, tr("Инвентаризации"));
    m_kind->setItemText(5, tr("Отмены"));
    top->addWidget(m_kind);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Найти: номер документа, комментарий, автор, артикул или название товара"));
    m_search->setClearButtonEnabled(true);
    top->addWidget(m_search, 1);
    auto *refreshBtn = new QPushButton(tr("Обновить"), this);
    top->addWidget(refreshBtn);
    layout->addLayout(top);

    auto *buttons = new QHBoxLayout();
    m_cancelBtn = new QPushButton(tr("Отменить документ целиком…"), this);
    m_lastBtn = new QPushButton(tr("Отменить последний документ…"), this);
    m_exportBtn = new QPushButton(tr("Выгрузить документ в Excel…"), this);
    buttons->addWidget(m_cancelBtn);
    buttons->addWidget(m_lastBtn);
    buttons->addWidget(m_exportBtn);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto *hint = new QLabel(tr("Каждое списание, приход, перемещение и инвентаризация - отдельный документ: внутри одна позиция или много. "
                               "Ошиблись - откройте документ и отмените его целиком: остатки вернутся, в журнале появится документ «Отмена»."), this);
    hint->setWordWrap(true);
    hint->setObjectName("muted");
    layout->addWidget(hint);

    auto *split = new QSplitter(Qt::Vertical, this);
    layout->addWidget(split, 1);

    m_docs = new QTableWidget(0, DColCount, split);
    m_docs->setHorizontalHeaderLabels({tr("№"), tr("Дата"), tr("Вид"), tr("Склад"), tr("Позиций"), tr("Штук"), tr("Автор"),
                                       tr("Комментарий"), tr("Состояние")});
    m_docs->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_docs->setSelectionMode(QAbstractItemView::SingleSelection);
    m_docs->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_docs->verticalHeader()->setVisible(false);
    m_docs->horizontalHeader()->setStretchLastSection(true);
    m_docs->setColumnWidth(DColId, 60);
    m_docs->setColumnWidth(DColDate, 130);
    m_docs->setColumnWidth(DColKind, 140);
    m_docs->setColumnWidth(DColWarehouse, 170);
    m_docs->setColumnWidth(DColPositions, 80);
    m_docs->setColumnWidth(DColUnits, 70);
    m_docs->setColumnWidth(DColAuthor, 110);
    m_docs->setColumnWidth(DColComment, 260);
    split->addWidget(m_docs);

    auto *bottom = new QWidget(split);
    auto *bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(0, 6, 0, 0);
    m_header = new QLabel(bottom);
    QFont f = m_header->font();
    f.setBold(true);
    m_header->setFont(f);
    bl->addWidget(m_header);
    m_lines = new QTableWidget(0, LColCount, bottom);
    m_lines->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("Склад"), tr("Изменение, шт"), tr("Запись журнала"), tr("Отмена")});
    m_lines->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lines->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_lines->verticalHeader()->setVisible(false);
    m_lines->horizontalHeader()->setStretchLastSection(true);
    m_lines->setColumnWidth(LColSku, 150);
    m_lines->setColumnWidth(LColName, 300);
    m_lines->setColumnWidth(LColWarehouse, 150);
    m_lines->setColumnWidth(LColDelta, 130);
    m_lines->setColumnWidth(LColJournal, 130);
    bl->addWidget(m_lines, 1);
    split->addWidget(bottom);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(350);
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_searchTimer, &QTimer::timeout, this, &DocumentsTab::refresh);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &DocumentsTab::refresh);
    connect(refreshBtn, &QPushButton::clicked, this, &DocumentsTab::refresh);
    connect(m_docs, &QTableWidget::itemSelectionChanged, this, &DocumentsTab::documentChanged);
    connect(m_cancelBtn, &QPushButton::clicked, this, &DocumentsTab::cancelSelected);
    connect(m_lastBtn, &QPushButton::clicked, this, &DocumentsTab::cancelLast);
    connect(m_exportBtn, &QPushButton::clicked, this, &DocumentsTab::exportSelected);
    updateButtons();
}

void DocumentsTab::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    m_cancelBtn->setVisible(!readOnly);
    m_lastBtn->setVisible(!readOnly);
}

void DocumentsTab::refresh()
{
    const int keep = selectedDocumentId();
    QString err;
    m_list = Documents::list(m_kind->currentData().toString(), m_search->text(), 500, &err);
    fillDocuments();
    if (!err.isEmpty())
        m_header->setText(tr("Не удалось прочитать документы: %1").arg(err));
    if (keep > 0)
        showDocument(keep);
    else if (m_docs->rowCount() > 0)
        m_docs->selectRow(0);
    else
        documentChanged();
}

void DocumentsTab::fillDocuments()
{
    m_filling = true;
    m_docs->setRowCount(0);
    const auto &colors = Theme::colors();
    for (const Documents::Info &d : std::as_const(m_list)) {
        const int row = m_docs->rowCount();
        m_docs->insertRow(row);
        auto *id = numberItem(d.id);
        id->setData(Qt::UserRole, d.id);
        m_docs->setItem(row, DColId, id);
        m_docs->setItem(row, DColDate, new QTableWidgetItem(d.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))));
        m_docs->setItem(row, DColKind, new QTableWidgetItem(Documents::kindTitle(d.kind)));
        m_docs->setItem(row, DColWarehouse, new QTableWidgetItem(d.warehouses));
        m_docs->setItem(row, DColPositions, numberItem(d.positions));
        m_docs->setItem(row, DColUnits, numberItem(d.units));
        m_docs->setItem(row, DColAuthor, new QTableWidgetItem(d.username));
        m_docs->setItem(row, DColComment, new QTableWidgetItem(d.comment));
        auto *st = new QTableWidgetItem(Documents::statusText(d));
        if (Documents::isCancelled(d) || d.reverted > 0)
            st->setForeground(colors.muted);
        m_docs->setItem(row, DColStatus, st);
        if (Documents::isCancelled(d))
            for (int c = 0; c < DColCount - 1; ++c)
                m_docs->item(row, c)->setForeground(colors.muted);
    }
    m_filling = false;
}

int DocumentsTab::selectedDocumentId() const
{
    const auto rows = m_docs->selectionModel()->selectedRows();
    return rows.isEmpty() ? 0 : m_docs->item(rows.first().row(), DColId)->data(Qt::UserRole).toInt();
}

void DocumentsTab::showDocument(int id)
{
    for (int r = 0; r < m_docs->rowCount(); ++r) {
        if (m_docs->item(r, DColId)->data(Qt::UserRole).toInt() == id) {
            m_docs->selectRow(r);
            m_docs->scrollToItem(m_docs->item(r, DColId));
            return;
        }
    }
    // Документа нет в списке (другой фильтр или он старый) - открываем отдельно.
    const Documents::Info d = Documents::get(id);
    if (d.id > 0) {
        m_docs->clearSelection();
        fillLines(d);
    }
}

void DocumentsTab::documentChanged()
{
    if (m_filling)
        return;
    const int id = selectedDocumentId();
    for (const Documents::Info &d : std::as_const(m_list)) {
        if (d.id == id) {
            fillLines(d);
            updateButtons();
            return;
        }
    }
    m_lines->setRowCount(0);
    m_header->setText(m_list.isEmpty() ? tr("Документов пока нет.") : QString());
    updateButtons();
}

void DocumentsTab::fillLines(const Documents::Info &d)
{
    m_header->setText(tr("%1 · %2 · %3%4 · %5")
                          .arg(Documents::title(d), d.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")),
                               d.username.isEmpty() ? tr("автор неизвестен") : d.username,
                               d.comment.isEmpty() ? QString() : QStringLiteral(" · «%1»").arg(d.comment), Documents::statusText(d)));
    const auto lines = Documents::lines(d.key);
    m_lines->setRowCount(0);
    const auto &colors = Theme::colors();
    for (const Documents::Line &l : lines) {
        const int row = m_lines->rowCount();
        m_lines->insertRow(row);
        m_lines->setItem(row, LColSku, new QTableWidgetItem(l.sku));
        m_lines->setItem(row, LColName, new QTableWidgetItem(l.name));
        m_lines->setItem(row, LColWarehouse, new QTableWidgetItem(l.warehouse));
        auto *delta = numberItem(l.delta);
        delta->setText((l.delta > 0 ? QStringLiteral("+") : QString()) + QString::number(l.delta));
        delta->setForeground(l.delta < 0 ? colors.danger : colors.ok);
        m_lines->setItem(row, LColDelta, delta);
        m_lines->setItem(row, LColJournal, numberItem(l.id));
        m_lines->setItem(row, LColState, new QTableWidgetItem(l.revertedBy ? tr("отменена записью №%1").arg(l.revertedBy) : QString()));
        if (l.revertedBy)
            for (int c = 0; c < LColCount; ++c)
                m_lines->item(row, c)->setForeground(colors.muted);
    }
}

void DocumentsTab::updateButtons()
{
    const int id = selectedDocumentId();
    bool can = false;
    for (const Documents::Info &d : std::as_const(m_list))
        if (d.id == id)
            can = d.kind != QLatin1String("cancel") && d.reverted < d.lines;
    m_cancelBtn->setEnabled(can);
    m_exportBtn->setEnabled(id > 0);
}

void DocumentsTab::cancelSelected()
{
    cancelDocument(selectedDocumentId());
}

void DocumentsTab::cancelLast()
{
    const int id = Documents::lastCancellable();
    if (id <= 0) {
        QMessageBox::information(this, tr("Отменять нечего"), tr("Нет документов, которые можно отменить."));
        return;
    }
    cancelDocument(id);
}

void DocumentsTab::cancelDocument(int id)
{
    if (id <= 0)
        return;
    const Documents::Info d = Documents::get(id);
    if (d.id == 0)
        return;
    if (d.kind == QLatin1String("cancel")) {
        QMessageBox::information(this, tr("Отмену отменить нельзя"),
                                 tr("Это документ-отмена. Если нужно вернуть как было, проведите операцию заново."));
        return;
    }
    const QList<int> ids = Documents::cancellableIds(d.key);
    if (ids.isEmpty()) {
        QMessageBox::information(this, tr("Уже отменён"), tr("%1 уже отменён.").arg(Documents::title(d)));
        return;
    }
    if (QMessageBox::question(this, tr("Отменить документ"),
                              tr("Отменить %1 целиком (%2 поз., %3 шт, %4)?\n\nДальше покажем, что изменится, и спросим ещё раз.")
                                  .arg(Documents::title(d)).arg(d.positions).arg(d.units).arg(d.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")))) != QMessageBox::Yes)
        return;
    CancelMovementsDialog dlg(ids, this);
    dlg.setReason(tr("документ №%1").arg(d.id));
    dlg.exec();
    if (dlg.cancelled()) {
        refresh();
        showDocument(id);
    }
}

void DocumentsTab::exportSelected()
{
    const int id = selectedDocumentId();
    if (id <= 0)
        return;
    const Documents::Info d = Documents::get(id);
    const QString path = QFileDialog::getSaveFileName(this, tr("Выгрузить документ в Excel"),
                                                      QStringLiteral("%1.xlsx").arg(Documents::title(d).replace(QLatin1Char(' '), QLatin1Char('_'))),
                                                      tr("Excel (*.xlsx)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!Xlsx::write(path.endsWith(QLatin1String(".xlsx"), Qt::CaseInsensitive) ? path : path + QStringLiteral(".xlsx"),
                     {Documents::toSheet(d, Documents::lines(d.key))}, &err))
        QMessageBox::warning(this, tr("Не удалось сохранить"), err);
}
