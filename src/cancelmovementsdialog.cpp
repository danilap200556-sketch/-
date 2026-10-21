#include "cancelmovementsdialog.h"

#include <QBrush>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum Column { ColId, ColSku, ColWarehouse, ColType, ColWas, ColUndo, ColNow, ColAfter, ColCount };

QString signedText(qint64 v) { return (v > 0 ? QStringLiteral("+") : QString()) + QString::number(v); }
} // namespace

CancelMovementsDialog::CancelMovementsDialog(const QList<int> &movementIds, QWidget *parent)
    : QDialog(parent), m_ids(movementIds)
{
    setWindowTitle(tr("Отмена операции"));
    resize(820, 460);

    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Журнал не стирается: в него добавятся записи «Отмена» с противоположным изменением, "
                               "остатки вернутся к прежним значениям. Каждую запись можно отменить один раз."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("№"), tr("Артикул"), tr("Склад"), tr("Запись"), tr("Было"), tr("Отмена даст"),
                                        tr("Сейчас"), tr("Станет")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setSectionResizeMode(ColSku, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColWarehouse, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColType, QHeaderView::ResizeToContents);
    layout->addWidget(m_table, 1);

    m_problems = new QLabel(this);
    m_problems->setWordWrap(true);
    m_problems->setTextFormat(Qt::PlainText);
    m_problems->setStyleSheet("color: #b00020;");
    m_problems->hide();
    layout->addWidget(m_problems);

    m_reason = new QLineEdit(this);
    m_reason->setMaxLength(200);
    m_reason->setPlaceholderText(tr("Причина (необязательно, попадёт в журнал): например, ошиблись артикулом"));
    layout->addWidget(m_reason);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    m_apply = new QPushButton(tr("Отменить операцию"), this);
    auto *close = new QPushButton(tr("Закрыть"), this);
    buttons->addWidget(m_apply);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_apply, &QPushButton::clicked, this, [this]() {
        if (apply()) {
            QMessageBox::information(this, windowTitle(),
                                     tr("Отменено записей: %1. Остатки возвращены, в журнале появились записи «Отмена».")
                                         .arg(m_plan.rows.size()));
            accept();
        } else {
            QMessageBox::warning(this, tr("Не отменено"),
                                 tr("Ничего не изменилось.\n\n%1").arg(m_plan.problems.mid(0, 10).join(QLatin1Char('\n'))));
        }
    });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    reload();
}

void CancelMovementsDialog::reload()
{
    m_plan = Database::planCancel(m_ids);
    m_table->setRowCount(0);
    for (const Database::CancelRow &r : std::as_const(m_plan.rows)) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const bool bad = r.after < 0;
        const QStringList cells = {QString::number(r.id), r.sku, r.warehouse, Database::movementTypeTitle(r.type),
                                   signedText(r.delta), signedText(-r.delta), QString::number(r.current), QString::number(r.after)};
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = new QTableWidgetItem(cells.at(c));
            if (c >= ColWas)
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (bad) {
                item->setBackground(QColor(255, 205, 205));
                item->setForeground(QBrush(Qt::black));
            }
            m_table->setItem(row, c, item);
        }
    }
    if (m_plan.problems.isEmpty()) {
        m_problems->hide();
    } else {
        QStringList shown = m_plan.problems.mid(0, 8);
        if (m_plan.problems.size() > shown.size())
            shown << tr("...и ещё %1").arg(m_plan.problems.size() - shown.size());
        m_problems->setText(tr("Отменить нельзя:\n") + shown.join(QLatin1Char('\n')));
        m_problems->show();
    }
    m_apply->setEnabled(m_plan.ok());
}

int CancelMovementsDialog::rowCount() const { return m_table->rowCount(); }

void CancelMovementsDialog::setReason(const QString &reason) { m_reason->setText(reason); }

bool CancelMovementsDialog::apply()
{
    QStringList problems;
    QString err;
    if (!Database::cancelMovements(m_ids, m_reason->text(), &problems, &err)) {
        reload(); // остатки могли измениться - показываем актуальное состояние
        if (m_plan.problems.isEmpty())
            m_plan.problems = problems.isEmpty() ? QStringList{err} : problems;
        return false;
    }
    m_cancelled = true;
    return true;
}
