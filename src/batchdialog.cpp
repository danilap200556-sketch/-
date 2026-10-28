#include "batchdialog.h"
#include "photostore.h"
#include "productfinder.h"
#include "productsuggest.h"

#include <algorithm>
#include <functional>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Column { ColSku, ColName, ColCurrent, ColQty, ColAfter, ColDest, ColCount };

QString kindText(BatchDialog::Kind kind)
{
    switch (kind) {
    case BatchDialog::Kind::WriteOff: return QObject::tr("Списание");
    case BatchDialog::Kind::Transfer: return QObject::tr("Перемещение");
    default: return QObject::tr("Приход");
    }
}

} // namespace

BatchDialog::BatchDialog(Kind kind, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Приход / списание / перемещение списком"));
    resize(860, 600);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    layout->addLayout(form);

    m_kind = new QComboBox(this);
    m_kind->addItem(tr("Приход (добавить на склад)"), int(Kind::Receipt));
    m_kind->addItem(tr("Списание (убрать со склада)"), int(Kind::WriteOff));
    m_kind->addItem(tr("Перемещение (со склада на склад)"), int(Kind::Transfer));
    form->addRow(tr("Операция"), m_kind);

    m_warehouse = new QComboBox(this);
    m_warehouseTo = new QComboBox(this);
    QSqlQuery wq("SELECT id, name FROM warehouses ORDER BY name");
    while (wq.next()) {
        m_warehouse->addItem(wq.value(1).toString(), wq.value(0).toInt());
        m_warehouseTo->addItem(wq.value(1).toString(), wq.value(0).toInt());
    }
    m_warehouseLabel = new QLabel(tr("Склад"), this);
    m_warehouseToLabel = new QLabel(tr("На склад"), this);
    form->addRow(m_warehouseLabel, m_warehouse);
    form->addRow(m_warehouseToLabel, m_warehouseTo);

    m_comment = new QLineEdit(this);
    m_comment->setPlaceholderText(tr("Например: поставка №12, возврат, брак"));
    form->addRow(tr("Комментарий"), m_comment);

    auto *hint = new QLabel(tr("Отсканируйте штрихкоды (каждый скан - 1 шт) или введите «артикул количество» и нажмите Enter. "
                               "Список из Excel или файла можно вставить кнопкой ниже: колонки «артикул» и «количество»."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    auto *scanRow = new QHBoxLayout();
    m_scan = new QLineEdit(this);
    m_scan->setPlaceholderText(tr("Штрихкод, артикул [количество] или часть названия; Enter - добавить"));
    m_scan->setClearButtonEnabled(true);
    scanRow->addWidget(m_scan, 1);
    auto *pasteBtn = new QPushButton(tr("Вставить список из буфера"), this);
    auto *removeBtn = new QPushButton(tr("Убрать выбранные"), this);
    auto *clearBtn = new QPushButton(tr("Очистить"), this);
    scanRow->addWidget(pasteBtn);
    scanRow->addWidget(removeBtn);
    scanRow->addWidget(clearBtn);
    layout->addLayout(scanRow);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("Сейчас"), tr("Кол-во"), tr("Станет"), tr("У получателя")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setIconSize(QSize(PhotoCache::kIconSize, PhotoCache::kIconSize));
    m_table->verticalHeader()->setDefaultSectionSize(PhotoCache::kIconSize + 6);
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColSku, QHeaderView::ResizeToContents);
    layout->addWidget(m_table, 1);

    m_problemsLabel = new QLabel(this);
    m_problemsLabel->setWordWrap(true);
    m_problemsLabel->setTextFormat(Qt::PlainText);
    m_problemsLabel->setStyleSheet("color: #d64545;");
    m_problemsLabel->hide();
    layout->addWidget(m_problemsLabel);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    m_commitButton = new QPushButton(this);
    m_commitButton->setDefault(false);
    auto *closeBtn = new QPushButton(tr("Закрыть"), this);
    buttons->addWidget(m_commitButton);
    buttons->addWidget(closeBtn);
    layout->addLayout(buttons);

    connect(m_scan, &QLineEdit::returnPressed, this, &BatchDialog::scanEntered);
    // Подсказки по мере ввода: часть артикула, названия или штрихкода; выбранный товар добавляется на 1 шт.
    m_suggest = new ProductSuggest(m_scan, this);
    connect(m_suggest, &ProductSuggest::chosen, this, [this](int id, const QString &, const QString &) {
        addChosen(id, 1);
        m_scan->clear();
        m_scan->setFocus();
    });
    connect(pasteBtn, &QPushButton::clicked, this, &BatchDialog::pasteFromClipboard);
    connect(removeBtn, &QPushButton::clicked, this, &BatchDialog::removeSelectedRows);
    connect(clearBtn, &QPushButton::clicked, this, &BatchDialog::clearAll);
    connect(m_commitButton, &QPushButton::clicked, this, &BatchDialog::commit);
    connect(closeBtn, &QPushButton::clicked, this, &BatchDialog::reject);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &BatchDialog::recompute);
    connect(m_warehouse, &QComboBox::currentIndexChanged, this, &BatchDialog::recompute);
    connect(m_warehouseTo, &QComboBox::currentIndexChanged, this, &BatchDialog::recompute);
    auto *del = new QShortcut(QKeySequence::Delete, m_table);
    del->setContext(Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, &BatchDialog::removeSelectedRows);

    // Enter в строке сканера только добавляет позицию: ни «Вставить из буфера», ни другая кнопка не должна срабатывать.
    for (QPushButton *b : findChildren<QPushButton *>())
        b->setAutoDefault(false);

    setKind(kind);
    m_scan->setFocus();
}

BatchDialog::Kind BatchDialog::kind() const { return static_cast<Kind>(m_kind->currentData().toInt()); }
int BatchDialog::warehouseId() const { return m_warehouse->currentData().toInt(); }
int BatchDialog::warehouseToId() const { return m_warehouseTo->currentData().toInt(); }

void BatchDialog::setKind(Kind kind)
{
    m_kind->setCurrentIndex(m_kind->findData(int(kind)));
    recompute();
}

void BatchDialog::setWarehouseTo(int warehouseId)
{
    const int idx = m_warehouseTo->findData(warehouseId);
    if (idx >= 0)
        m_warehouseTo->setCurrentIndex(idx);
}

void BatchDialog::setWarehouse(int warehouseId)
{
    const int idx = m_warehouse->findData(warehouseId);
    if (idx >= 0)
        m_warehouse->setCurrentIndex(idx);
}

void BatchDialog::setComment(const QString &comment) { m_comment->setText(comment); }

int BatchDialog::rowCount() const { return m_table->rowCount(); }

qint64 BatchDialog::totalQty() const
{
    qint64 total = 0;
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (auto *spin = qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColQty)))
            total += spin->value();
    return total;
}

int BatchDialog::rowOf(int productId) const
{
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (m_table->item(r, ColSku)->data(Qt::UserRole).toInt() == productId)
            return r;
    return -1;
}

void BatchDialog::addQuantity(int productId, qint64 qty)
{
    int row = rowOf(productId);
    if (row < 0) {
        // Артикулы и названия уже прочитаны пачкой (см. addText); одиночный запрос - на всякий случай.
        if (!m_info.contains(productId)) {
            QSqlQuery q;
            q.prepare("SELECT sku, name FROM products WHERE id = ?");
            q.addBindValue(productId);
            if (!q.exec() || !q.next())
                return;
            m_info.insert(productId, {q.value(0).toString(), q.value(1).toString()});
        }
        const auto info = m_info.value(productId);
        row = m_table->rowCount();
        m_table->insertRow(row);
        auto *sku = new QTableWidgetItem(info.first);
        sku->setData(Qt::UserRole, productId);
        sku->setIcon(PhotoCache::thumb(productId));
        m_table->setItem(row, ColSku, sku);
        m_table->setItem(row, ColName, new QTableWidgetItem(info.second));
        for (int c : {ColCurrent, ColAfter, ColDest}) {
            auto *it = new QTableWidgetItem();
            it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_table->setItem(row, c, it);
        }
        auto *spin = new QSpinBox(m_table);
        spin->setRange(1, int(BatchParser::kMaxQty));
        spin->setValue(int(qMin<qint64>(qty, BatchParser::kMaxQty)));
        spin->setGroupSeparatorShown(true);
        connect(spin, &QSpinBox::valueChanged, this, &BatchDialog::recompute);
        m_table->setCellWidget(row, ColQty, spin);
        return;
    }
    auto *spin = qobject_cast<QSpinBox *>(m_table->cellWidget(row, ColQty));
    const qint64 sum = qint64(spin->value()) + qty;
    if (sum > BatchParser::kMaxQty)
        m_problems << tr("%1: слишком большое количество, оставлено %2")
                          .arg(m_table->item(row, ColSku)->text()).arg(BatchParser::kMaxQty);
    spin->setValue(int(qMin<qint64>(sum, BatchParser::kMaxQty)));
}

int BatchDialog::addText(const QString &text)
{
    m_problems.clear();
    const QList<BatchParser::Line> lines = BatchParser::parse(text);
    if (lines.isEmpty()) {
        m_problems << tr("Список пуст: впишите или отсканируйте хотя бы одну позицию");
    } else if (lines.size() > BatchParser::kMaxLines) {
        m_problems << tr("Слишком много строк (больше %1) - разбейте список на части").arg(BatchParser::kMaxLines);
    } else {
        QStringList keys;
        for (const BatchParser::Line &l : lines)
            if (l.error.isEmpty())
                keys << l.key;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QHash<QString, int> ids;
        QHash<QString, QString> errors;
        ProductFinder::findExactMany(keys, &ids, &errors);

        QList<int> order;
        QHash<int, qint64> qtyById;
        for (const BatchParser::Line &l : lines) {
            if (!l.error.isEmpty()) {
                m_problems << tr("строка %1: %2: %3").arg(l.n).arg(l.key.isEmpty() ? tr("(пусто)") : l.key, l.error);
            } else if (!ids.contains(l.key)) {
                m_problems << tr("строка %1: %2").arg(l.n).arg(errors.value(l.key));
            } else {
                const int id = ids.value(l.key);
                if (!qtyById.contains(id))
                    order << id;
                qtyById[id] += l.qty;
            }
        }
        PhotoCache::prefetch(order);
        {
            // Артикул и название всех новых строк - одним запросом, а не по запросу на строку.
            QStringList marks;
            for (int id : std::as_const(order))
                if (rowOf(id) < 0 && !m_info.contains(id))
                    marks << QString::number(id);
            if (!marks.isEmpty()) {
                QSqlQuery q;
                if (q.exec(QStringLiteral("SELECT id, sku, name FROM products WHERE id IN (%1)").arg(marks.join(QLatin1Char(',')))))
                    while (q.next())
                        m_info.insert(q.value(0).toInt(), {q.value(1).toString(), q.value(2).toString()});
            }
        }
        m_updating = true;
        for (int id : std::as_const(order))
            addQuantity(id, qtyById.value(id));
        m_updating = false;
        QApplication::restoreOverrideCursor();
        if (!order.isEmpty()) {
            const int row = rowOf(order.last());
            if (row >= 0) {
                m_table->selectRow(row);
                m_table->scrollToItem(m_table->item(row, ColSku));
            }
        }
    }
    recompute();
    return int(m_problems.size());
}

void BatchDialog::addChosen(int productId, qint64 qty)
{
    m_problems.clear();
    PhotoCache::prefetch({productId});
    m_updating = true;
    addQuantity(productId, qty);
    m_updating = false;
    const int row = rowOf(productId);
    if (row >= 0) {
        m_table->selectRow(row);
        m_table->scrollToItem(m_table->item(row, ColSku));
    }
    recompute();
}

void BatchDialog::scanEntered()
{
    const QString text = m_scan->text().trimmed();
    if (text.isEmpty())
        return;
    if (addText(text) == 0) {
        m_scan->clear();
        return;
    }
    // Точно (штрихкод, артикул целиком) не нашлось. Ввели часть артикула или названия: если подходит ровно один товар,
    // берём его, если несколько - показываем список, чтобы выбрать.
    const QList<BatchParser::Line> parsed = BatchParser::parse(text);
    if (parsed.size() == 1 && parsed.first().error.isEmpty()) {
        const QList<ProductFinder::Suggestion> found = ProductFinder::suggest(parsed.first().key, 2);
        if (found.size() == 1) {
            addChosen(found.first().id, parsed.first().qty);
            m_scan->clear();
            return;
        }
        if (found.size() > 1) {
            m_problems = {tr("«%1»: подходит несколько товаров - выберите нужный в списке под строкой (стрелки и Enter) или введите артикул точнее")
                              .arg(parsed.first().key)};
            showProblems();
            m_suggest->searchNow();
        }
    }
    m_scan->selectAll();
}

void BatchDialog::pasteFromClipboard()
{
    const QString text = QApplication::clipboard()->text();
    if (text.trimmed().isEmpty()) {
        m_problems = {tr("В буфере обмена нет текста: скопируйте колонки «артикул» и «количество» из Excel")};
        showProblems();
        return;
    }
    addText(text);
}

void BatchDialog::removeSelectedRows()
{
    QList<int> rows;
    for (const QModelIndex &i : m_table->selectionModel()->selectedRows())
        rows << i.row();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int r : std::as_const(rows))
        m_table->removeRow(r);
    m_problems.clear();
    recompute();
}

void BatchDialog::clearAll()
{
    m_table->setRowCount(0);
    m_problems.clear();
    recompute();
}

void BatchDialog::showProblems()
{
    if (m_problems.isEmpty()) {
        m_problemsLabel->hide();
        return;
    }
    QStringList shown = m_problems.mid(0, 8);
    if (m_problems.size() > shown.size())
        shown << tr("...и ещё %1").arg(m_problems.size() - shown.size());
    m_problemsLabel->setText(tr("Нужно исправить:\n") + shown.join(QLatin1Char('\n')));
    m_problemsLabel->show();
}

void BatchDialog::recompute()
{
    if (m_updating)
        return;
    const Kind k = kind();
    const bool out = takesOut();
    const bool transfer = k == Kind::Transfer;
    const int wh = warehouseId();
    const int whTo = warehouseToId();

    // Для перемещения видны оба склада и столбец «У получателя»; склад-получатель не должен совпадать с источником.
    m_warehouseLabel->setText(transfer ? tr("Со склада") : tr("Склад"));
    m_warehouseToLabel->setVisible(transfer);
    m_warehouseTo->setVisible(transfer);
    m_table->setColumnHidden(ColDest, !transfer);
    const bool sameWarehouse = transfer && wh == whTo;

    // Остатки всех строк одним запросом (на обоих складах для перемещения)
    QHash<int, qint64> have, haveTo;
    QStringList idList;
    for (int r = 0; r < m_table->rowCount(); ++r)
        idList << QString::number(m_table->item(r, ColSku)->data(Qt::UserRole).toInt());
    if (!idList.isEmpty()) {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT product_id, quantity, warehouse_id FROM stock WHERE warehouse_id IN (?, ?) AND product_id IN (%1)")
                      .arg(idList.join(QLatin1Char(','))));
        q.addBindValue(wh);
        q.addBindValue(transfer ? whTo : wh);
        if (q.exec())
            while (q.next())
                (q.value(2).toInt() == wh ? have : haveTo)[q.value(0).toInt()] = q.value(1).toLongLong();
    }

    int short_ = 0;
    qint64 total = 0;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const int id = m_table->item(r, ColSku)->data(Qt::UserRole).toInt();
        const qint64 now = have.value(id, 0);
        const qint64 qty = qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColQty))->value();
        const qint64 after = out ? now - qty : now + qty;
        total += qty;
        m_table->item(r, ColCurrent)->setText(QString::number(now));
        m_table->item(r, ColAfter)->setText(QString::number(after));
        if (auto *dest = m_table->item(r, ColDest)) {
            const qint64 dnow = haveTo.value(id, 0);
            dest->setText(transfer ? QStringLiteral("%1 → %2").arg(dnow).arg(dnow + qty) : QString());
        }
        const bool bad = after < 0;
        if (bad)
            ++short_;
        for (int c : {int(ColSku), int(ColName), int(ColCurrent), int(ColAfter)}) {
            m_table->item(r, c)->setBackground(bad ? QBrush(QColor(255, 205, 205)) : QBrush());
            m_table->item(r, c)->setForeground(bad ? QBrush(Qt::black) : QBrush());
        }
    }

    QString s = tr("Позиций: %1, всего %2 шт").arg(m_table->rowCount()).arg(total);
    if (short_)
        s += tr(" - не хватает остатка у позиций: %1 (красные строки)").arg(short_);
    if (sameWarehouse)
        s += tr(" - выберите разные склады: перемещать на тот же склад нельзя");
    m_summary->setText(s);
    m_summary->setStyleSheet(short_ || sameWarehouse ? "color: #d64545;" : QString());
    m_commitButton->setText(k == Kind::Transfer ? tr("Провести перемещение") : out ? tr("Провести списание") : tr("Провести приход"));
    m_commitButton->setEnabled(m_table->rowCount() > 0 && !sameWarehouse);
    showProblems();
}

QList<Database::BatchItem> BatchDialog::itemsFromTable() const
{
    QList<Database::BatchItem> items;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        Database::BatchItem it;
        it.productId = m_table->item(r, ColSku)->data(Qt::UserRole).toInt();
        it.qty = qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColQty))->value();
        items << it;
    }
    return items;
}

bool BatchDialog::apply()
{
    m_lastError.clear();
    if (m_warehouse->count() == 0) {
        m_lastError = tr("Сначала добавьте склад.");
        m_problems = {m_lastError};
        showProblems();
        return false;
    }
    if (m_table->rowCount() == 0) {
        m_lastError = tr("Список пуст");
        m_problems = {m_lastError};
        showProblems();
        return false;
    }
    QStringList problems;
    QString err;
    const bool ok = kind() == Kind::Transfer
                        ? Database::transferStockBatch(itemsFromTable(), warehouseId(), warehouseToId(), m_comment->text().trimmed(), &problems, &err)
                        : Database::adjustStockBatch(itemsFromTable(), warehouseId(), writeOff(), m_comment->text().trimmed(), &problems, &err);
    if (!ok) {
        m_lastError = err;
        m_problems = problems.isEmpty() ? QStringList{err} : problems;
        recompute();
        return false;
    }
    m_applied = true;
    return true;
}

void BatchDialog::commit()
{
    // Красные строки (не хватает остатка) видно сразу - не отправляем заведомо неудачный документ.
    if (takesOut()) {
        QStringList lacking;
        for (int r = 0; r < m_table->rowCount(); ++r)
            if (m_table->item(r, ColAfter)->text().toLongLong() < 0)
                lacking << tr("%1: на складе %2 шт, а %4 нужно %3 шт").arg(
                    m_table->item(r, ColSku)->text(), m_table->item(r, ColCurrent)->text(),
                    QString::number(qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColQty))->value()),
                    kind() == Kind::Transfer ? tr("переместить") : tr("списать"));
        if (!lacking.isEmpty()) {
            m_problems = lacking;
            recompute();
            QMessageBox::warning(this, tr("Не хватает остатка"),
                                 tr("%1 Исправьте количество в красных строках или уберите их:\n\n%2")
                                     .arg(kind() == Kind::Transfer ? tr("Ничего не перемещено.") : tr("Ничего не списано."),
                                          lacking.mid(0, 10).join(QLatin1Char('\n'))));
            return;
        }
    }
    const QString where = kind() == Kind::Transfer ? tr("со склада «%1» на склад «%2»").arg(m_warehouse->currentText(), m_warehouseTo->currentText())
                                                   : tr("склад «%1»").arg(m_warehouse->currentText());
    const QString question = tr("%1: позиций %2, всего %3 шт, %4. Провести одним документом?")
                                 .arg(kindText(kind())).arg(m_table->rowCount()).arg(totalQty()).arg(where);
    if (QMessageBox::question(this, windowTitle(), question) != QMessageBox::Yes)
        return;
    if (!apply()) {
        QMessageBox::warning(this, tr("Не проведено"),
                             tr("Ничего не изменилось.\n\n%1").arg(m_problems.mid(0, 10).join(QLatin1Char('\n'))));
        return;
    }
    const int docId = Database::lastDocumentId();
    QMessageBox::information(this, windowTitle(), tr("Готово: %1, позиций %2, всего %3 шт.%4")
                                                    .arg(kindText(kind())).arg(m_table->rowCount()).arg(totalQty())
                                                    .arg(docId > 0 ? tr("\n\nСоздан документ №%1: открыть и отменить целиком его можно в разделе «Документы».").arg(docId) : QString()));
    accept();
}

void BatchDialog::reject()
{
    if (!m_applied && m_table->rowCount() > 0) {
        if (QMessageBox::question(this, windowTitle(), tr("Список не проведён и будет потерян. Закрыть?")) != QMessageBox::Yes)
            return;
    }
    QDialog::reject();
}
