#include "stocktaketab.h"

#include "barcode.h"
#include "batchparser.h"
#include "database.h"
#include "photostore.h"
#include "productfinder.h"
#include "productsuggest.h"
#include "xlsx.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "theme.h"

namespace {

QString pieces(qint64 n)
{
    return QStringLiteral("%1 шт").arg(n);
}

QString signedNumber(qint64 n)
{
    return n > 0 ? QStringLiteral("+%1").arg(n) : QString::number(n);
}

qint64 nowMs()
{
    static QElapsedTimer t = [] { QElapsedTimer x; x.start(); return x; }();
    return t.elapsed();
}

} // namespace

StocktakeTab::StocktakeTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *top = new QHBoxLayout();
    top->addWidget(new QLabel(tr("Склад:"), this));
    m_warehouse = new QComboBox(this);
    m_warehouse->setMinimumWidth(220);
    top->addWidget(m_warehouse);
    m_startBtn = new QPushButton(tr("Начать подсчёт"), this);
    m_finishBtn = new QPushButton(tr("Завершить инвентаризацию..."), this);
    m_discardBtn = new QPushButton(tr("Сбросить подсчёт"), this);
    m_exportBtn = new QPushButton(tr("Выгрузить в Excel..."), this);
    top->addWidget(m_startBtn);
    top->addWidget(m_finishBtn);
    top->addWidget(m_discardBtn);
    top->addWidget(m_exportBtn);
    top->addStretch();
    layout->addLayout(top);

    m_banner = new QLabel(this);
    m_banner->setWordWrap(true);
    m_banner->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::colors().muted.name()));
    layout->addWidget(m_banner);

    m_scan = new QLineEdit(this);
    m_scan->setPlaceholderText(tr("Отсканируйте штрихкод - каждый скан добавляет одну штуку. Можно ввести артикул или «артикул количество»"));
    m_scan->setClearButtonEnabled(true);
    QFont big = m_scan->font();
    big.setPointSizeF(big.pointSizeF() * 1.4);
    m_scan->setFont(big);
    m_scan->setMinimumHeight(44);
    layout->addWidget(m_scan);
    m_suggest = new ProductSuggest(m_scan, this);
    m_suggest->setQueryFunction([](const QString &text) {
        const QList<BatchParser::Line> lines = BatchParser::parse(text);
        return lines.size() == 1 && lines.first().error.isEmpty() ? lines.first().key : text;
    });
    connect(m_suggest, &ProductSuggest::chosen, this, [this](int id, const QString &, const QString &typed) {
        const QList<BatchParser::Line> lines = BatchParser::parse(typed);
        const qint64 qty = lines.size() == 1 && lines.first().error.isEmpty() ? lines.first().qty : 1;
        if (countProduct(id, int(qMin<qint64>(qty, Stocktake::kMaxCount))))
            m_scan->clear();
    });

    m_status = new QLabel(this);
    m_status->setMinimumHeight(24);
    layout->addWidget(m_status);

    auto *filter = new QHBoxLayout();
    filter->addWidget(new QLabel(tr("Показать:"), this));
    m_view = new QComboBox(this);
    m_view->addItem(tr("Посчитанное"), Counted);
    m_view->addItem(tr("Только расхождения"), Differences);
    m_view->addItem(tr("Не посчитанное (после завершения станет 0)"), Missing);
    filter->addWidget(m_view);
    m_plusBtn = new QPushButton(tr("+1"), this);
    m_minusBtn = new QPushButton(tr("−1"), this);
    m_removeBtn = new QPushButton(tr("Убрать из подсчёта"), this);
    m_plusBtn->setToolTip(tr("Добавить одну штуку выбранному товару"));
    m_minusBtn->setToolTip(tr("Убрать одну штуку (лишний скан)"));
    filter->addWidget(m_plusBtn);
    filter->addWidget(m_minusBtn);
    filter->addWidget(m_removeBtn);
    filter->addStretch();
    layout->addLayout(filter);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("В учёте"), tr("Посчитано"), tr("Разница")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    layout->addWidget(m_table, 1);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_zeroMissing = new QCheckBox(tr("Не посчитанное считать нулевым: при завершении у товаров склада, которых нет в подсчёте, остаток станет 0"), this);
    m_zeroMissing->setChecked(true);
    layout->addWidget(m_zeroMissing);

    m_summaryTimer = new QTimer(this);
    m_summaryTimer->setSingleShot(true);
    m_summaryTimer->setInterval(250);
    connect(m_summaryTimer, &QTimer::timeout, this, &StocktakeTab::updateSummary);
    // Подсчёт общий: пока страница открыта и с ней не работают, подтягиваем то, что досчитали с других компьютеров.
    m_autoTimer = new QTimer(this);
    m_autoTimer->setInterval(15000);
    connect(m_autoTimer, &QTimer::timeout, this, [this]() {
        if (isVisible() && m_info.valid() && nowMs() - m_lastScan > 4000 && !m_table->viewport()->findChild<QLineEdit *>())
            loadCount();
    });
    m_autoTimer->start();

    connect(m_warehouse, &QComboBox::currentIndexChanged, this, &StocktakeTab::warehouseChanged);
    connect(m_startBtn, &QPushButton::clicked, this, &StocktakeTab::startCount);
    connect(m_finishBtn, &QPushButton::clicked, this, &StocktakeTab::finishCount);
    connect(m_discardBtn, &QPushButton::clicked, this, &StocktakeTab::discardCount);
    connect(m_exportBtn, &QPushButton::clicked, this, &StocktakeTab::exportReport);
    connect(m_scan, &QLineEdit::returnPressed, this, &StocktakeTab::scanEntered);
    connect(m_view, &QComboBox::currentIndexChanged, this, &StocktakeTab::viewChanged);
    connect(m_table, &QTableWidget::itemChanged, this, &StocktakeTab::itemEdited);
    connect(m_plusBtn, &QPushButton::clicked, this, [this]() { adjustSelected(+1); });
    connect(m_minusBtn, &QPushButton::clicked, this, [this]() { adjustSelected(-1); });
    connect(m_removeBtn, &QPushButton::clicked, this, &StocktakeTab::removeSelected);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &StocktakeTab::updateControls);

    updateControls();
}

int StocktakeTab::warehouseId() const
{
    return m_warehouse->currentData().toInt();
}

void StocktakeTab::refresh()
{
    const int previous = warehouseId();
    {
        QSignalBlocker block(m_warehouse);
        m_warehouse->clear();
        QSqlQuery q("SELECT w.id, w.name, EXISTS (SELECT 1 FROM stocktakes s WHERE s.warehouse_id = w.id AND s.status = 'open') "
                    "FROM warehouses w ORDER BY w.name");
        while (q.next())
            m_warehouse->addItem(q.value(1).toString() + (q.value(2).toBool() ? tr("  (идёт подсчёт)") : QString()), q.value(0).toInt());
        const int index = m_warehouse->findData(previous);
        m_warehouse->setCurrentIndex(index >= 0 ? index : (m_warehouse->count() > 0 ? 0 : -1));
    }
    loadCount();
}

void StocktakeTab::warehouseChanged()
{
    m_unknown.clear();
    m_status->clear();
    loadCount();
}

void StocktakeTab::loadCount()
{
    m_info = warehouseId() > 0 ? Stocktake::current(warehouseId()) : Stocktake::Info();
    reloadTable();
    updateSummary();
    updateControls();
}

void StocktakeTab::updateControls()
{
    const bool open = m_info.valid();
    m_startBtn->setVisible(!open);
    m_startBtn->setEnabled(warehouseId() > 0);
    m_finishBtn->setVisible(open);
    m_discardBtn->setVisible(open);
    m_exportBtn->setEnabled(open);
    m_scan->setEnabled(open);
    m_zeroMissing->setEnabled(open);
    m_view->setEnabled(open);
    const bool rowSelected = open && !m_table->selectedItems().isEmpty() && m_view->currentData().toInt() != Missing;
    m_plusBtn->setEnabled(rowSelected);
    m_minusBtn->setEnabled(rowSelected);
    m_removeBtn->setEnabled(rowSelected);
    if (!open) {
        m_banner->setText(m_warehouse->count() == 0
                              ? tr("Сначала добавьте склад на странице «Склады».")
                              : tr("Для этого склада подсчёт не начат. Нажмите «Начать подсчёт» и сканируйте штрихкоды товаров, "
                                   "которые лежат на складе. Когда закончите - «Завершить инвентаризацию»: остатки станут равны посчитанному, "
                                   "а у того, чего в подсчёте нет, будет 0."));
    } else {
        m_banner->setText(tr("Идёт подсчёт №%1 (начат %2%3). Каждый скан - ещё одна штука; чего нет в подсчёте, у того после завершения "
                             "остаток на этом складе станет 0. Пока идёт подсчёт, остатки склада лучше не менять.")
                              .arg(m_info.id)
                              .arg(m_info.startedAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")))
                              .arg(m_info.startedBy.isEmpty() ? QString() : tr(", %1").arg(m_info.startedBy)));
    }
}

void StocktakeTab::startCount()
{
    QString err;
    if (Stocktake::start(warehouseId(), &err) <= 0) {
        QMessageBox::warning(this, tr("Инвентаризация"), err);
        return;
    }
    refresh();
    m_scan->setFocus();
}

void StocktakeTab::reloadTable()
{
    m_filling = true;
    m_table->setRowCount(0);
    m_rows.clear();
    if (m_info.valid()) {
        const int view = m_view->currentData().toInt();
        if (view == Missing) {
            m_rows = Stocktake::missingRows(m_info.id);
        } else {
            m_rows = Stocktake::countedRows(m_info.id);
            if (view == Differences) {
                QList<Stocktake::Row> diff;
                for (const Stocktake::Row &r : std::as_const(m_rows))
                    if (r.diff() != 0)
                        diff << r;
                m_rows = diff;
            }
        }
        m_table->setRowCount(int(m_rows.size()));
        for (int i = 0; i < m_rows.size(); ++i)
            fillRow(i, m_rows[i]);
    }
    m_filling = false;
}

void StocktakeTab::fillRow(int row, const Stocktake::Row &r)
{
    auto cell = [&](int col, const QString &text, bool number, bool editable) {
        auto *item = new QTableWidgetItem(text);
        Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        if (editable)
            flags |= Qt::ItemIsEditable;
        item->setFlags(flags);
        if (number)
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(row, col, item);
        return item;
    };
    auto *sku = cell(ColSku, r.sku, false, false);
    sku->setData(Qt::UserRole, r.productId);
    cell(ColName, r.name, false, false);
    cell(ColBook, QString::number(r.book), true, false);
    cell(ColCounted, QString::number(r.counted), true, true);
    auto *diff = cell(ColDiff, r.diff() == 0 ? QStringLiteral("0") : signedNumber(r.diff()), true, false);
    if (r.diff() > 0)
        diff->setForeground(Theme::colors().ok);
    else if (r.diff() < 0)
        diff->setForeground(Theme::colors().danger);
}

int StocktakeTab::rowOfProduct(int productId) const
{
    for (int i = 0; i < m_table->rowCount(); ++i)
        if (m_table->item(i, ColSku) && m_table->item(i, ColSku)->data(Qt::UserRole).toInt() == productId)
            return i;
    return -1;
}

void StocktakeTab::showRow(const Stocktake::Row &r)
{
    // В «Посчитанном» последний скан - первая строка; в других видах строку обновляем на месте (или убираем/добавляем).
    const int view = m_view->currentData().toInt();
    m_filling = true;
    const int old = rowOfProduct(r.productId);
    if (old >= 0) {
        m_table->removeRow(old);
        m_rows.removeAt(old);
    }
    const bool belongs = view == Counted || (view == Differences && r.diff() != 0);
    if (belongs) {
        m_table->insertRow(0);
        m_rows.prepend(r);
        fillRow(0, r);
        m_table->selectRow(0);
        m_table->scrollToTop();
    }
    m_filling = false;
}

bool StocktakeTab::countProduct(int productId, int qty)
{
    if (!m_info.valid())
        return false;
    Stocktake::Row row;
    QString err;
    if (!Stocktake::add(m_info.id, productId, qty, &row, &err)) {
        say(err, true);
        return false;
    }
    m_lastScan = nowMs();
    showRow(row);
    say(tr("%1 — %2: посчитано %3 (в учёте %4)").arg(row.sku, row.name).arg(row.counted).arg(row.book), false);
    m_summaryTimer->start();
    return true;
}

void StocktakeTab::say(const QString &text, bool error)
{
    m_status->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg((error ? Theme::colors().danger : Theme::colors().ok).name()));
    m_status->setText(text);
    if (error)
        QApplication::beep();
}

// Штрихкод, артикул или артикул на Маркете целиком. Незнакомый штрихкод предлагаем привязать к товару.
bool StocktakeTab::resolveCode(const QString &key, int *productId)
{
    QString why;
    const int id = ProductFinder::findExact(key, &why);
    if (id >= 0) {
        *productId = id;
        return true;
    }
    const QString code = Barcode::normalize(key);
    if (Barcode::isValid(code)) {
        // Похоже на штрихкод, которого нет в базе: привязать к товару и сразу посчитать, либо отложить.
        QStringList items;
        QList<int> ids;
        QSqlQuery q("SELECT id, sku, name FROM products ORDER BY sku");
        while (q.next()) {
            ids << q.value(0).toInt();
            items << QStringLiteral("%1 — %2").arg(q.value(1).toString(), q.value(2).toString());
        }
        if (!items.isEmpty()) {
            bool ok = false;
            const QString choice = QInputDialog::getItem(
                this, tr("Новый штрихкод"),
                tr("Штрихкод %1 не привязан ни к одному товару.\nК какому товару его привязать? (Отмена - пропустить и продолжить)").arg(code),
                items, 0, false, &ok);
            if (ok) {
                const int chosen = ids.value(items.indexOf(choice), -1);
                QString err;
                if (chosen >= 0 && Barcode::attach(chosen, code, &err)) {
                    *productId = chosen;
                    return true;
                }
                say(err, true);
                return false;
            }
        }
        m_unknown[code] += 1;
        QStringList list;
        for (auto it = m_unknown.cbegin(); it != m_unknown.cend(); ++it)
            list << QStringLiteral("%1 ×%2").arg(it.key()).arg(it.value());
        say(tr("Штрихкод %1 не опознан и не посчитан. Не опознано за этот сеанс: %2").arg(code, list.join(QStringLiteral(", "))), true);
        return false;
    }
    say(why.isEmpty() ? tr("«%1»: товар не найден").arg(key) : why, true);
    return false;
}

void StocktakeTab::scanEntered()
{
    const QString text = m_scan->text().trimmed();
    if (text.isEmpty() || !m_info.valid())
        return;
    const QList<BatchParser::Line> lines = BatchParser::parse(text);
    if (lines.size() != 1 || !lines.first().error.isEmpty()) {
        say(lines.isEmpty() ? tr("Пусто") : lines.first().error, true);
        m_scan->selectAll();
        return;
    }
    const BatchParser::Line line = lines.first();
    int productId = -1;
    if (!resolveCode(line.key, &productId)) {
        // Не нашли целиком: часть артикула или названия - если подходит один товар, берём его, иначе покажем список.
        const QList<ProductFinder::Suggestion> found = ProductFinder::suggest(line.key, 2);
        if (found.size() == 1) {
            productId = found.first().id;
        } else {
            if (found.size() > 1) {
                say(tr("«%1»: подходит несколько товаров - выберите нужный в списке под строкой").arg(line.key), true);
                m_suggest->searchNow();
            }
            m_scan->selectAll();
            return;
        }
    }
    if (countProduct(productId, int(qMin<qint64>(line.qty, Stocktake::kMaxCount))))
        m_scan->clear();
    else
        m_scan->selectAll();
}

void StocktakeTab::itemEdited(QTableWidgetItem *item)
{
    if (m_filling || !item || item->column() != ColCounted || !m_info.valid())
        return;
    const int row = item->row();
    const int productId = m_table->item(row, ColSku)->data(Qt::UserRole).toInt();
    bool ok = false;
    const int value = item->text().trimmed().toInt(&ok);
    if (!ok || value < 0 || value > Stocktake::kMaxCount) {
        QMessageBox::warning(this, tr("Посчитано"), tr("Введите целое число от 0 до %1.").arg(Stocktake::kMaxCount));
        reloadTable();
        return;
    }
    QString err;
    if (!Stocktake::setCounted(m_info.id, productId, value, nullptr, &err)) {
        QMessageBox::warning(this, tr("Инвентаризация"), err);
        loadCount();
        return;
    }
    m_lastScan = nowMs();
    reloadTable();
    updateSummary();
}

void StocktakeTab::adjustSelected(int delta)
{
    const int row = m_table->currentRow();
    if (row < 0 || !m_info.valid())
        return;
    const int productId = m_table->item(row, ColSku)->data(Qt::UserRole).toInt();
    Stocktake::Row r;
    QString err;
    if (!Stocktake::add(m_info.id, productId, delta, &r, &err)) {
        say(err, true);
        return;
    }
    m_lastScan = nowMs();
    reloadTable();
    const int again = rowOfProduct(productId);
    if (again >= 0)
        m_table->selectRow(again);
    m_summaryTimer->start();
}

void StocktakeTab::removeSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || !m_info.valid())
        return;
    const int productId = m_table->item(row, ColSku)->data(Qt::UserRole).toInt();
    QString err;
    if (!Stocktake::setCounted(m_info.id, productId, -1, nullptr, &err)) {
        say(err, true);
        return;
    }
    reloadTable();
    updateSummary();
}

void StocktakeTab::viewChanged()
{
    reloadTable();
    updateControls();
}

void StocktakeTab::updateSummary()
{
    if (!m_info.valid()) {
        m_summary->clear();
        return;
    }
    const Stocktake::Summary s = Stocktake::summary(m_info.id);
    QString text = tr("Посчитано: %1 поз., %2.").arg(s.positions).arg(pieces(s.units));
    if (s.positions > 0)
        text += tr(" Расхождений: %1 (излишек %2, недостача %3).").arg(s.diffPositions).arg(pieces(s.surplus), pieces(s.shortage));
    text += tr(" Не посчитано, но числится на складе: %1 поз., %2.").arg(s.missingPositions).arg(pieces(s.missingUnits));
    m_summary->setText(text);
}

void StocktakeTab::finishCount()
{
    if (!m_info.valid())
        return;
    const Stocktake::Summary s = Stocktake::summary(m_info.id);
    const bool zero = m_zeroMissing->isChecked();
    if (s.positions == 0 && zero && s.missingPositions > 0) {
        // Пустой подсчёт с обнулением стёр бы весь склад - переспрашиваем отдельно.
        const auto answer = QMessageBox::warning(
            this, tr("Ничего не посчитано"),
            tr("В подсчёте нет ни одного товара, а на складе числится %1 поз. (%2). Если продолжить, весь склад «%3» будет обнулён.\n\n"
               "Продолжить?").arg(s.missingPositions).arg(pieces(s.missingUnits), m_info.warehouse),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    QString text = tr("Завершить инвентаризацию склада «%1»?\n\nПосчитано: %2 поз., %3.\nРасхождения: %4 поз. (излишек %5, недостача %6).")
                       .arg(m_info.warehouse).arg(s.positions).arg(pieces(s.units)).arg(s.diffPositions).arg(pieces(s.surplus), pieces(s.shortage));
    if (zero)
        text += tr("\nНе посчитано, но числится на складе: %1 поз., %2 - их остаток станет 0.").arg(s.missingPositions).arg(pieces(s.missingUnits));
    else
        text += tr("\nНе посчитанные товары (%1 поз.) останутся как есть.").arg(s.missingPositions);
    text += tr("\n\nОстатки изменятся одним документом «Инвентаризация»; его можно отменить в разделе «Документы».");
    QMessageBox box(QMessageBox::Question, tr("Завершить инвентаризацию"), text, QMessageBox::NoButton, this);
    QPushButton *go = box.addButton(tr("Провести"), QMessageBox::AcceptRole);
    box.addButton(tr("Отмена"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != go)
        return;

    Stocktake::FinishResult result;
    QString err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = Stocktake::finish(m_info.id, zero, &result, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Инвентаризация не проведена"), err);
        loadCount();
        return;
    }
    refresh();
    m_status->clear();
    if (result.changed > 0) {
        QMessageBox::information(this, tr("Инвентаризация проведена"),
                                 tr("Остаток изменён у товаров: %1. Документ №%2 - его можно открыть и отменить в разделе «Документы».")
                                     .arg(result.changed).arg(result.documentId));
        emit stockChanged();
        emit documentCreated(result.documentId);
    } else {
        QMessageBox::information(this, tr("Инвентаризация проведена"), tr("Расхождений нет - остатки не менялись."));
    }
}

void StocktakeTab::discardCount()
{
    if (!m_info.valid())
        return;
    const auto answer = QMessageBox::question(
        this, tr("Сбросить подсчёт"),
        tr("Сбросить подсчёт №%1 (посчитано %2 поз.)? Остатки не изменятся, посчитанное будет потеряно.").arg(m_info.id).arg(m_info.positions),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    QString err;
    if (!Stocktake::discard(m_info.id, &err))
        QMessageBox::warning(this, tr("Инвентаризация"), err);
    m_unknown.clear();
    refresh();
}

void StocktakeTab::exportReport()
{
    if (!m_info.valid())
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Выгрузить подсчёт в Excel"),
        tr("Инвентаризация_%1_%2.xlsx").arg(m_info.warehouse, QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))), tr("Excel (*.xlsx)"));
    if (path.isEmpty())
        return;
    Xlsx::Sheet sheet;
    sheet.name = tr("Подсчёт");
    sheet.rows.append({tr("Артикул"), tr("Название"), tr("В учёте"), tr("Посчитано"), tr("Разница"), tr("Примечание")});
    sheet.numericColumns = {2, 3, 4};
    sheet.columnWidths = {22, 45, 10, 11, 10, 28};
    for (const Stocktake::Row &r : Stocktake::countedRows(m_info.id))
        sheet.rows.append({r.sku, r.name, QString::number(r.book), QString::number(r.counted), QString::number(r.diff()), QString()});
    for (const Stocktake::Row &r : Stocktake::missingRows(m_info.id))
        sheet.rows.append({r.sku, r.name, QString::number(r.book), QString::number(0), QString::number(-r.book), tr("не посчитано")});
    QString err;
    if (!Xlsx::write(path.endsWith(QLatin1String(".xlsx"), Qt::CaseInsensitive) ? path : path + QLatin1String(".xlsx"), {sheet}, &err))
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сохранить файл:\n%1").arg(err));
}
