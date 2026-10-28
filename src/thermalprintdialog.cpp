#include "thermalprintdialog.h"

#include "barcode.h"
#include "database.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPrinterInfo>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum Column { ColSku, ColName, ColCode, ColCopies, ColCount };
constexpr int kMaxCopies = 5000;
}

ThermalPrintDialog::ThermalPrintDialog(const QList<Item> &items, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Печать штрихкодов на термоэтикетках"));
    resize(1000, 700);
    auto *layout = new QVBoxLayout(this);

    // --- Настройки печати
    auto *settingsBox = new QGroupBox(tr("Этикетка и принтер"), this);
    auto *sl = new QHBoxLayout(settingsBox);
    const ThermalLabel::Settings saved = ThermalLabel::loadSettings();
    m_width = new QDoubleSpinBox(settingsBox);
    m_width->setRange(20, 120);
    m_width->setSuffix(tr(" мм"));
    m_width->setValue(saved.widthMm);
    m_height = new QDoubleSpinBox(settingsBox);
    m_height->setRange(15, 120);
    m_height->setSuffix(tr(" мм"));
    m_height->setValue(saved.heightMm);
    m_rotate = new QCheckBox(tr("Повернуть на 90°"), settingsBox);
    m_rotate->setToolTip(tr("Если принтер печатает этикетку «боком», включите: ширина и длина бумаги поменяются местами."));
    m_rotate->setChecked(saved.rotate);
    m_printer = new QComboBox(settingsBox);
    m_printer->addItem(tr("Принтер по умолчанию"), QString());
    for (const QString &name : QPrinterInfo::availablePrinterNames())
        m_printer->addItem(name, name);
    const int pi = m_printer->findData(saved.printer);
    m_printer->setCurrentIndex(pi >= 0 ? pi : 0);
    m_middle = new QComboBox(settingsBox);
    m_middle->addItem(tr("Название"), 0);
    m_middle->addItem(tr("Описание товара"), 1);
    m_middle->addItem(tr("Название и описание"), 2);
    m_middle->addItem(tr("Ничего"), 3);
    sl->addWidget(new QLabel(tr("Ширина:"), settingsBox));
    sl->addWidget(m_width);
    sl->addWidget(new QLabel(tr("Высота:"), settingsBox));
    sl->addWidget(m_height);
    sl->addWidget(m_rotate);
    sl->addSpacing(12);
    sl->addWidget(new QLabel(tr("Принтер:"), settingsBox));
    sl->addWidget(m_printer, 1);
    sl->addSpacing(12);
    sl->addWidget(new QLabel(tr("В середине:"), settingsBox));
    sl->addWidget(m_middle);
    layout->addWidget(settingsBox);

    // --- Товары и число копий
    auto *copiesRow = new QHBoxLayout();
    m_allCopies = new QSpinBox(this);
    m_allCopies->setRange(0, kMaxCopies);
    m_allCopies->setValue(1);
    auto *allBtn = new QPushButton(tr("Поставить всем"), this);
    m_stockWarehouse = new QComboBox(this);
    QSqlQuery wq("SELECT id, name FROM warehouses ORDER BY name");
    while (wq.next())
        m_stockWarehouse->addItem(tr("по остатку: %1").arg(wq.value(1).toString()), wq.value(0).toInt());
    auto *stockBtn = new QPushButton(tr("Копий = остатку на складе"), this);
    stockBtn->setEnabled(m_stockWarehouse->count() > 0);
    m_stockWarehouse->setVisible(m_stockWarehouse->count() > 0);
    copiesRow->addWidget(new QLabel(tr("Копий:"), this));
    copiesRow->addWidget(m_allCopies);
    copiesRow->addWidget(allBtn);
    copiesRow->addSpacing(16);
    copiesRow->addWidget(m_stockWarehouse);
    copiesRow->addWidget(stockBtn);
    copiesRow->addStretch();
    layout->addLayout(copiesRow);

    auto *middle = new QHBoxLayout();
    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("Штрихкод EAN-13"), tr("Копий")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColCode, QHeaderView::ResizeToContents);
    middle->addWidget(m_table, 3);

    auto *previewBox = new QGroupBox(tr("Так будет выглядеть этикетка"), this);
    auto *pl = new QVBoxLayout(previewBox);
    m_preview = new QLabel(previewBox);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumSize(480, 340);
    m_preview->setStyleSheet(QStringLiteral("background: #e9edf3; border: 1px solid #c5cde0;"));
    pl->addWidget(m_preview, 1);
    middle->addWidget(previewBox, 2);
    layout->addLayout(middle, 1);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto *bottom = new QHBoxLayout();
    m_ownBtn = new QPushButton(tr("Присвоить свой EAN-13 товарам без штрихкода"), this);
    m_printBtn = new QPushButton(tr("Печать"), this);
    auto *pdfBtn = new QPushButton(tr("Сохранить в PDF..."), this);
    auto *closeBtn = new QPushButton(tr("Закрыть"), this);
    bottom->addWidget(m_ownBtn);
    bottom->addStretch();
    bottom->addWidget(m_printBtn);
    bottom->addWidget(pdfBtn);
    bottom->addWidget(closeBtn);
    layout->addLayout(bottom);
    for (QPushButton *b : findChildren<QPushButton *>())
        b->setAutoDefault(false);

    // --- Данные товаров одним запросом
    QHash<int, int> copiesById;
    QStringList marks;
    for (const Item &it : items) {
        copiesById[it.productId] += it.copies; // один товар в списке дважды - копии складываются
        marks << QString::number(it.productId);
    }
    QHash<int, Row> byId;
    if (!marks.isEmpty()) {
        QSqlQuery q(QStringLiteral("SELECT id, sku, name, COALESCE(description, '') FROM products WHERE id IN (%1)").arg(marks.join(QLatin1Char(','))));
        while (q.next())
            byId[q.value(0).toInt()] = Row{q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString()};
    }
    for (const Item &it : items)
        if (byId.contains(it.productId) && !std::any_of(m_rows.cbegin(), m_rows.cend(), [&](const Row &r) { return r.productId == it.productId; }))
            m_rows << byId.value(it.productId);
    m_loading = true;
    m_table->setRowCount(int(m_rows.size()));
    for (int r = 0; r < m_rows.size(); ++r) {
        m_table->setItem(r, ColSku, new QTableWidgetItem(m_rows[r].sku));
        m_table->setItem(r, ColName, new QTableWidgetItem(m_rows[r].name));
        auto *code = new QComboBox(m_table);
        m_table->setCellWidget(r, ColCode, code);
        auto *spin = new QSpinBox(m_table);
        spin->setRange(0, kMaxCopies);
        spin->setValue(qBound(0, copiesById.value(m_rows[r].productId, 1), kMaxCopies));
        m_table->setCellWidget(r, ColCopies, spin);
        connect(spin, &QSpinBox::valueChanged, this, [this]() { updateSummary(); });
        connect(code, &QComboBox::currentIndexChanged, this, [this]() { updatePreview(); updateSummary(); });
    }
    m_loading = false;
    reloadCodes();

    connect(allBtn, &QPushButton::clicked, this, &ThermalPrintDialog::applyCopiesToAll);
    connect(stockBtn, &QPushButton::clicked, this, &ThermalPrintDialog::copiesFromStock);
    connect(m_ownBtn, &QPushButton::clicked, this, &ThermalPrintDialog::createOwnCodes);
    connect(m_printBtn, &QPushButton::clicked, this, &ThermalPrintDialog::print);
    connect(pdfBtn, &QPushButton::clicked, this, &ThermalPrintDialog::savePdf);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &ThermalPrintDialog::updatePreview);
    for (auto *w : {static_cast<QWidget *>(m_width), static_cast<QWidget *>(m_height)})
        connect(static_cast<QDoubleSpinBox *>(w), &QDoubleSpinBox::valueChanged, this, &ThermalPrintDialog::settingsChanged);
    connect(m_rotate, &QCheckBox::toggled, this, &ThermalPrintDialog::settingsChanged);
    connect(m_middle, &QComboBox::currentIndexChanged, this, &ThermalPrintDialog::settingsChanged);
    if (m_table->rowCount() > 0)
        m_table->selectRow(0);
    updatePreview();
    updateSummary();
}

int ThermalPrintDialog::rowCount() const { return m_table->rowCount(); }

void ThermalPrintDialog::setCopies(int row, int copies)
{
    if (auto *spin = qobject_cast<QSpinBox *>(m_table->cellWidget(row, ColCopies)))
        spin->setValue(copies);
}

int ThermalPrintDialog::totalLabels() const
{
    int total = 0;
    for (int r = 0; r < m_table->rowCount(); ++r)
        total += qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColCopies))->value();
    return total;
}

ThermalLabel::Settings ThermalPrintDialog::currentSettings() const
{
    ThermalLabel::Settings s;
    s.widthMm = m_width->value();
    s.heightMm = m_height->value();
    s.rotate = m_rotate->isChecked();
    s.printer = m_printer->currentData().toString();
    return s;
}

void ThermalPrintDialog::reloadCodes()
{
    // Штрихкоды всех товаров одним запросом; в списке - только те, что можно нарисовать (EAN-13 и UPC-A).
    QHash<int, QStringList> codes;
    if (!m_rows.isEmpty()) {
        QStringList marks;
        for (const Row &r : std::as_const(m_rows))
            marks << QString::number(r.productId);
        QSqlQuery q(QStringLiteral("SELECT product_id, barcode FROM product_barcodes WHERE product_id IN (%1) ORDER BY created_at, barcode").arg(marks.join(QLatin1Char(','))));
        while (q.next())
            if (Barcode::canRenderEan13(q.value(1).toString()))
                codes[q.value(0).toInt()] << q.value(1).toString();
    }
    m_loading = true;
    for (int r = 0; r < m_rows.size(); ++r) {
        auto *combo = qobject_cast<QComboBox *>(m_table->cellWidget(r, ColCode));
        const QString keep = combo->currentData().toString();
        combo->clear();
        for (const QString &c : codes.value(m_rows[r].productId))
            combo->addItem(c, c);
        if (combo->count() == 0)
            combo->addItem(tr("— нет —"), QString());
        const int idx = combo->findData(keep);
        if (idx >= 0)
            combo->setCurrentIndex(idx);
    }
    m_loading = false;
    updatePreview();
    updateSummary();
}

QString ThermalPrintDialog::codeFor(int row) const
{
    auto *combo = qobject_cast<QComboBox *>(m_table->cellWidget(row, ColCode));
    return combo ? combo->currentData().toString() : QString();
}

QString ThermalPrintDialog::middleText(const Row &r) const
{
    switch (m_middle->currentData().toInt()) {
    case 1: return r.description.isEmpty() ? r.name : r.description;
    case 2: return r.description.isEmpty() ? r.name : r.name + QStringLiteral(". ") + r.description;
    case 3: return QString();
    default: return r.name;
    }
}

ThermalLabel::Ean13Label ThermalPrintDialog::labelFor(int row) const
{
    return {m_rows[row].sku, middleText(m_rows[row]), codeFor(row)};
}

void ThermalPrintDialog::settingsChanged()
{
    updatePreview();
}

void ThermalPrintDialog::updatePreview()
{
    if (m_loading)
        return;
    int row = m_table->currentRow();
    if (row < 0 && m_table->rowCount() > 0)
        row = 0;
    if (row < 0) {
        m_preview->clear();
        return;
    }
    const QImage img = ThermalLabel::renderEan13(labelFor(row), currentSettings(), 203, false);
    // Показываем в натуральную величину при 203 dpi, а если не помещается - уменьшаем.
    QPixmap pm = QPixmap::fromImage(img);
    const QSize box = m_preview->size() - QSize(16, 16);
    if (pm.width() > box.width() || pm.height() > box.height())
        pm = pm.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_preview->setPixmap(pm);
}

void ThermalPrintDialog::updateSummary()
{
    if (m_loading)
        return;
    int missing = 0, missingCopies = 0;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const int copies = qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColCopies))->value();
        if (codeFor(r).isEmpty() && copies > 0) {
            ++missing;
            missingCopies += copies;
        }
    }
    QString text = tr("Товаров: %1, этикеток: %2.").arg(m_table->rowCount()).arg(totalLabels());
    if (missing > 0)
        text += tr(" У %1 тов. нет штрихкода EAN-13 (этикеток: %2) - присвойте им свой кнопкой слева внизу.").arg(missing).arg(missingCopies);
    m_summary->setText(text);
    m_summary->setStyleSheet(missing > 0 ? QStringLiteral("color: #d64545;") : QString());
    m_ownBtn->setVisible(missing > 0);
    m_printBtn->setEnabled(totalLabels() > 0);
}

void ThermalPrintDialog::applyCopiesToAll()
{
    for (int r = 0; r < m_table->rowCount(); ++r)
        setCopies(r, m_allCopies->value());
}

void ThermalPrintDialog::copiesFromStock()
{
    const int wh = m_stockWarehouse->currentData().toInt();
    if (wh <= 0 || m_rows.isEmpty())
        return;
    QStringList marks;
    for (const Row &r : std::as_const(m_rows))
        marks << QString::number(r.productId);
    QHash<int, int> qty;
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT product_id, quantity FROM stock WHERE warehouse_id = ? AND product_id IN (%1)").arg(marks.join(QLatin1Char(','))));
    q.addBindValue(wh);
    if (q.exec())
        while (q.next())
            qty[q.value(0).toInt()] = q.value(1).toInt();
    for (int r = 0; r < m_rows.size(); ++r)
        setCopies(r, qBound(0, qty.value(m_rows[r].productId, 0), kMaxCopies));
}

void ThermalPrintDialog::createOwnCodes()
{
    QList<int> ids;
    for (int r = 0; r < m_rows.size(); ++r)
        if (codeFor(r).isEmpty())
            ids << m_rows[r].productId;
    QString err;
    const int n = Barcode::ensureOwn(ids, &err);
    if (n < 0) {
        QMessageBox::warning(this, tr("Штрихкоды"), err);
        return;
    }
    reloadCodes();
}

bool ThermalPrintDialog::askAboutMissingCodes()
{
    bool any = false;
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (codeFor(r).isEmpty() && qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColCopies))->value() > 0)
            any = true;
    if (!any)
        return true;
    QMessageBox box(QMessageBox::Question, tr("Нет штрихкода"),
                    tr("У части товаров нет штрихкода EAN-13. Присвоить им свой штрихкод и напечатать, или пропустить такие товары?"),
                    QMessageBox::NoButton, this);
    QPushButton *assign = box.addButton(tr("Присвоить и напечатать"), QMessageBox::AcceptRole);
    QPushButton *skip = box.addButton(tr("Пропустить их"), QMessageBox::DestructiveRole);
    box.addButton(tr("Отмена"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == assign) {
        createOwnCodes();
        return true;
    }
    if (box.clickedButton() == skip) {
        for (int r = 0; r < m_table->rowCount(); ++r)
            if (codeFor(r).isEmpty())
                setCopies(r, 0);
        return totalLabels() > 0;
    }
    return false;
}

bool ThermalPrintDialog::doPrint(const QString &pdfPath, QString *error)
{
    // Страницы - по копиям каждого товара; одинаковые картинки не рисуем заново.
    QList<int> order;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        if (codeFor(r).isEmpty())
            continue;
        const int copies = qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColCopies))->value();
        for (int i = 0; i < copies; ++i)
            order << r;
    }
    const ThermalLabel::Settings s = currentSettings();
    ThermalLabel::saveSettings(s);
    QHash<int, QImage> cache;
    int cacheDpi = 0;
    return ThermalLabel::printPages(s, int(order.size()), [&](int index, int dpi) {
        if (dpi != cacheDpi) {
            cache.clear();
            cacheDpi = dpi;
        }
        const int row = order[index];
        if (!cache.contains(row))
            cache.insert(row, ThermalLabel::renderEan13(labelFor(row), s, dpi, true));
        return cache.value(row);
    }, error, pdfPath);
}

void ThermalPrintDialog::print()
{
    if (!askAboutMissingCodes())
        return;
    QString err;
    setCursor(Qt::WaitCursor);
    const bool ok = doPrint(QString(), &err);
    unsetCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Печать"), err);
        return;
    }
    QMessageBox::information(this, tr("Печать"), tr("Отправлено на печать: этикеток %1.").arg(totalLabels()));
}

bool ThermalPrintDialog::printToPdf(const QString &path, QString *error)
{
    return doPrint(path, error);
}

void ThermalPrintDialog::savePdf()
{
    if (!askAboutMissingCodes())
        return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Сохранить этикетки в PDF"), QStringLiteral("etiketki.pdf"), tr("PDF-документ (*.pdf)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!doPrint(path, &err))
        QMessageBox::warning(this, tr("PDF"), err);
    else
        QMessageBox::information(this, tr("PDF"), tr("Сохранено: %1 (по странице на каждую этикетку).").arg(path));
}
