#include "markingprintdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPrinterInfo>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

MarkingPrintDialog::MarkingPrintDialog(const QList<int> &codeIds, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Печать кодов маркировки на термоэтикетках"));
    resize(1000, 680);
    for (int id : codeIds) {
        const MarkingCode::Row r = MarkingCode::get(id);
        if (r.id > 0)
            m_rows << r;
    }

    auto *layout = new QVBoxLayout(this);
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
    sl->addWidget(new QLabel(tr("Ширина:"), settingsBox));
    sl->addWidget(m_width);
    sl->addWidget(new QLabel(tr("Высота:"), settingsBox));
    sl->addWidget(m_height);
    sl->addWidget(m_rotate);
    sl->addSpacing(12);
    sl->addWidget(new QLabel(tr("Принтер:"), settingsBox));
    sl->addWidget(m_printer, 1);
    layout->addWidget(settingsBox);

    auto *middle = new QHBoxLayout();
    m_table = new QTableWidget(m_rows.size(), 5, this);
    m_table->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("GTIN"), tr("Серийный номер"), tr("Статус")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int r = 0; r < m_rows.size(); ++r) {
        const MarkingCode::Row &row = m_rows[r];
        m_table->setItem(r, 0, new QTableWidgetItem(row.sku.isEmpty() ? tr("(товар не найден)") : row.sku));
        m_table->setItem(r, 1, new QTableWidgetItem(row.name));
        m_table->setItem(r, 2, new QTableWidgetItem(row.gtin));
        m_table->setItem(r, 3, new QTableWidgetItem(row.serial));
        m_table->setItem(r, 4, new QTableWidgetItem(row.status == QLatin1String("printed") ? tr("печатался %1 р.").arg(row.printCount) : tr("новый")));
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setMinimumSectionSize(110); // название не схлопывается в «...»
    m_table->setMinimumWidth(560);
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
    auto *printBtn = new QPushButton(tr("Печать"), this);
    auto *pdfBtn = new QPushButton(tr("Сохранить в PDF..."), this);
    pdfBtn->setToolTip(tr("Проверить, как выглядят этикетки: PDF не отмечает коды напечатанными."));
    auto *closeBtn = new QPushButton(tr("Закрыть"), this);
    bottom->addStretch();
    bottom->addWidget(printBtn);
    bottom->addWidget(pdfBtn);
    bottom->addWidget(closeBtn);
    layout->addLayout(bottom);
    for (QPushButton *b : findChildren<QPushButton *>())
        b->setAutoDefault(false);

    connect(printBtn, &QPushButton::clicked, this, &MarkingPrintDialog::print);
    connect(pdfBtn, &QPushButton::clicked, this, &MarkingPrintDialog::savePdf);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MarkingPrintDialog::updatePreview);
    connect(m_width, &QDoubleSpinBox::valueChanged, this, &MarkingPrintDialog::updatePreview);
    connect(m_height, &QDoubleSpinBox::valueChanged, this, &MarkingPrintDialog::updatePreview);
    connect(m_rotate, &QCheckBox::toggled, this, &MarkingPrintDialog::updatePreview);
    if (m_table->rowCount() > 0)
        m_table->selectRow(0);
    updatePreview();
    const int again = alreadyPrinted();
    m_summary->setText(tr("Этикеток к печати: %1 (по одной на код).%2").arg(m_rows.size())
                           .arg(again > 0 ? tr(" Из них уже печатались: %1 - код маркировки наклеивается на товар один раз, повторно печатайте только если этикетка испорчена.").arg(again) : QString()));
}

ThermalLabel::Settings MarkingPrintDialog::currentSettings() const
{
    ThermalLabel::Settings s;
    s.widthMm = m_width->value();
    s.heightMm = m_height->value();
    s.rotate = m_rotate->isChecked();
    s.printer = m_printer->currentData().toString();
    return s;
}

ThermalLabel::MarkingLabel MarkingPrintDialog::labelFor(int row) const
{
    const MarkingCode::Row &r = m_rows[row];
    return ThermalLabel::MarkingLabel{r.sku, r.name, r.code};
}

int MarkingPrintDialog::alreadyPrinted() const
{
    int n = 0;
    for (const MarkingCode::Row &r : m_rows)
        n += r.status == QLatin1String("printed");
    return n;
}

void MarkingPrintDialog::updatePreview()
{
    const int row = m_table->currentRow() >= 0 ? m_table->currentRow() : 0;
    if (m_rows.isEmpty() || row >= m_rows.size()) {
        m_preview->clear();
        return;
    }
    ThermalLabel::Settings s = currentSettings();
    s.rotate = false; // в просмотре этикетка всегда «лицом»
    const QImage img = ThermalLabel::renderMarking(labelFor(row), s, 203, false);
    m_preview->setPixmap(QPixmap::fromImage(img).scaled(m_preview->size() - QSize(16, 16), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

bool MarkingPrintDialog::doPrint(const QString &pdfPath, bool markPrinted, QString *error)
{
    if (m_rows.isEmpty()) {
        if (error)
            *error = tr("Нет кодов для печати");
        return false;
    }
    const ThermalLabel::Settings s = currentSettings();
    ThermalLabel::saveSettings(s);
    const bool ok = ThermalLabel::printPages(s, m_rows.size(), [this, &s](int index, int dpi) {
        return ThermalLabel::renderMarking(labelFor(index), s, dpi, true);
    }, error, pdfPath);
    if (!ok)
        return false;
    if (markPrinted) {
        QList<int> ids;
        for (const MarkingCode::Row &r : std::as_const(m_rows))
            ids << r.id;
        QString err;
        if (!MarkingCode::markPrinted(ids, &err)) {
            if (error)
                *error = tr("Напечатано, но отметить коды не удалось: %1").arg(err);
            return false;
        }
        m_printed = true;
    }
    return true;
}

bool MarkingPrintDialog::printToPdf(const QString &path, bool markPrinted, QString *error)
{
    return doPrint(path, markPrinted, error);
}

void MarkingPrintDialog::print()
{
    if (m_rows.isEmpty())
        return;
    const int again = alreadyPrinted();
    if (again > 0
        && QMessageBox::question(this, tr("Коды уже печатались"),
                                 tr("%1 из %2 кодов уже печатались. Код маркировки наклеивается на товар один раз - печатайте повторно только если этикетка "
                                    "испорчена. Напечатать?").arg(again).arg(m_rows.size())) != QMessageBox::Yes)
        return;
    QString err;
    if (!doPrint(QString(), true, &err)) {
        QMessageBox::warning(this, tr("Печать"), err);
        return;
    }
    QMessageBox::information(this, tr("Печать"), tr("Отправлено на печать: %1 этикеток. Коды отмечены напечатанными.").arg(m_rows.size()));
    accept();
}

void MarkingPrintDialog::savePdf()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Сохранить этикетки в PDF"), QStringLiteral("marking-labels.pdf"), tr("PDF (*.pdf)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!doPrint(path, false, &err))
        QMessageBox::warning(this, tr("PDF"), err);
    else
        QMessageBox::information(this, tr("PDF"), tr("Сохранено: %1").arg(path));
}
