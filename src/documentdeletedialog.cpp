#include "documentdeletedialog.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum Col { ColSku = 0, ColName, ColWarehouse, ColCurrent, ColAfter, ColCount };
}

DocumentDeleteDialog::DocumentDeleteDialog(int documentId, QWidget *parent)
    : QDialog(parent), m_id(documentId), m_info(Documents::get(documentId))
{
    setWindowTitle(tr("Удалить документ №%1").arg(documentId));
    resize(820, 560);
    auto *layout = new QVBoxLayout(this);

    auto *head = new QLabel(this);
    head->setWordWrap(true);
    head->setText(m_info.id > 0
                      ? tr("<b>%1</b> · %2 · %3 поз., %4 шт · %5 · %6")
                            .arg(Documents::title(m_info).toHtmlEscaped(), m_info.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")))
                            .arg(m_info.positions).arg(m_info.units)
                            .arg(m_info.username.isEmpty() ? tr("автор неизвестен") : m_info.username.toHtmlEscaped(), Documents::statusText(m_info).toHtmlEscaped())
                      : tr("Документ не найден (возможно, его уже удалили)."));
    layout->addWidget(head);

    const bool isCancel = m_info.kind == QLatin1String("cancel");
    m_restore = new QRadioButton(isCancel ? tr("Вернуть остатки: отменённые документы снова станут действующими")
                                          : tr("Вернуть остатки, как будто документа не было (приход убирается с остатка, списание возвращается)"), this);
    m_keep = new QRadioButton(tr("Оставить остатки как есть: документ просто пропадает из истории, остатки не меняются"), this);
    m_restore->setChecked(true);
    if (isCancel) {
        m_keep->setEnabled(false);
        m_keep->setToolTip(tr("Документ-отмену можно удалить только с возвратом остатков."));
    }
    layout->addWidget(m_restore);
    layout->addWidget(m_keep);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("Склад"), tr("Сейчас"), tr("Станет")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    layout->addWidget(m_table, 1);

    m_problems = new QLabel(this);
    m_problems->setWordWrap(true);
    m_problems->setTextFormat(Qt::PlainText);
    m_problems->setStyleSheet(QStringLiteral("color: #d64545;"));
    m_problems->hide();
    layout->addWidget(m_problems);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    m_deleteBtn = new QPushButton(tr("Удалить документ"), this);
    m_deleteBtn->setMinimumWidth(190);
    auto *close = new QPushButton(tr("Отмена"), this);
    for (QPushButton *b : {m_deleteBtn, close}) {
        b->setAutoDefault(false);
        buttons->addWidget(b);
    }
    layout->addLayout(buttons);

    connect(m_restore, &QRadioButton::toggled, this, [this]() { reload(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_deleteBtn, &QPushButton::clicked, this, [this]() {
        const QString what = m_restore->isChecked() ? tr("Остатки будут возвращены.") : tr("Остатки не изменятся.");
        if (QMessageBox::warning(this, tr("Удалить документ"),
                                 tr("Удалить %1 навсегда? %2\n\nЭто действие нельзя отменить.").arg(Documents::title(m_info), what),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        if (apply()) {
            accept();
            return;
        }
        QMessageBox::warning(this, tr("Не удалено"), tr("Ничего не изменилось.\n\n%1").arg(m_plan.problems.mid(0, 10).join(QLatin1Char('\n'))));
    });
    reload();
}

void DocumentDeleteDialog::setRestoreStock(bool restore)
{
    (restore ? m_restore : m_keep)->setChecked(true);
}

int DocumentDeleteDialog::rowCount() const
{
    return m_table->rowCount();
}

void DocumentDeleteDialog::reload()
{
    m_plan = Documents::planDelete(m_id, m_restore->isChecked());
    m_table->setRowCount(0);
    for (const Documents::DeleteRow &r : std::as_const(m_plan.rows)) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, ColSku, new QTableWidgetItem(r.sku));
        m_table->setItem(row, ColName, new QTableWidgetItem(r.name));
        m_table->setItem(row, ColWarehouse, new QTableWidgetItem(r.warehouse));
        for (int c : {int(ColCurrent), int(ColAfter)}) {
            auto *it = new QTableWidgetItem(QString::number(c == ColCurrent ? r.current : r.after));
            it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (c == ColAfter && r.after < 0) {
                it->setBackground(QColor(255, 205, 205));
                it->setForeground(Qt::black);
            }
            m_table->setItem(row, c, it);
        }
    }
    QString text = tr("Из журнала исчезнет записей: %1").arg(m_plan.movements);
    if (m_plan.cancelDocuments > 0)
        text += tr(" (вместе с документами-отменами: %1)").arg(m_plan.cancelDocuments);
    text += QLatin1Char('.');
    if (!m_restore->isChecked())
        text += tr(" Остатки не меняются.");
    else if (m_plan.rows.isEmpty())
        text += tr(" Остатки не изменятся: документ уже отменён или у него нет записей.");
    else
        text += tr(" Остатки изменятся так, как в таблице ниже.");
    m_summary->setText(text);
    m_problems->setText(m_plan.problems.isEmpty() ? QString() : tr("Удалить нельзя:\n") + m_plan.problems.mid(0, 8).join(QLatin1Char('\n')));
    m_problems->setVisible(!m_plan.problems.isEmpty());
    m_deleteBtn->setEnabled(m_info.id > 0 && m_plan.ok());
}

bool DocumentDeleteDialog::apply()
{
    QStringList problems;
    QString err;
    if (!Documents::remove(m_id, m_restore->isChecked(), &problems, &err)) {
        m_plan.problems = problems.isEmpty() ? QStringList{err} : problems;
        return false;
    }
    m_deleted = true;
    return true;
}
