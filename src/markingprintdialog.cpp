#include "markingprintdialog.h"

#include "productmarket.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPrinterInfo>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

// Цвет из названия товара («Nike Air Force 1 White», «Кроссовки чёрные»): слова-цвета по порядку, через «/». Пусто - не нашлось.
// Русские цвета - только с окончанием прилагательного («серый», но не «серия»).
QString guessColor(const QString &name)
{
    struct Pair { const char *pattern; const char *color; };
    static const Pair kColors[] = {
        {"white|бел%1", "белый"}, {"black|чёрн%1|черн%1", "чёрный"}, {"red|красн%1", "красный"},
        {"blue|navy|син%1|голуб%1", "синий"}, {"gr[ae]y|сер%1", "серый"}, {"green|зелён%1|зелен%1", "зелёный"},
        {"beige|cream|бежев%1", "бежевый"}, {"brown|коричнев%1", "коричневый"}, {"pink|розов%1", "розовый"},
        {"yellow|жёлт%1|желт%1", "жёлтый"}, {"orange|оранжев%1", "оранжевый"}, {"purple|violet|фиолетов%1", "фиолетовый"},
    };
    const QString end = QStringLiteral("(?:ый|ий|ая|яя|ое|ее|ые|ие|ого|его|ых|их|ой|ей|ым|им)");
    const QString text = name.toLower();
    QList<QPair<int, QString>> found;
    for (const Pair &c : kColors) {
        const QRegularExpression re(QStringLiteral("(?<![\\p{L}])(?:%1)(?![\\p{L}])").arg(QString::fromUtf8(c.pattern).arg(end)),
                                    QRegularExpression::UseUnicodePropertiesOption);
        const auto m = re.match(text);
        if (m.hasMatch())
            found.append({int(m.capturedStart()), QString::fromUtf8(c.color)});
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    QStringList out;
    for (const auto &f : std::as_const(found))
        if (!out.contains(f.second))
            out << f.second;
    return out.join(QLatin1Char('/'));
}

} // namespace

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

    QList<int> productIds;
    for (const MarkingCode::Row &r : std::as_const(m_rows))
        if (r.productId > 0 && !productIds.contains(r.productId))
            productIds << r.productId;
    if (!productIds.isEmpty())
        m_info = ProductMarket::loadMany(productIds);

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

    auto *templateRow = new QHBoxLayout();
    templateRow->addWidget(new QLabel(tr("Шаблон:"), this));
    m_template = new QComboBox(this);
    m_template->addItem(tr("Стандартный: код слева, артикул, название и размер справа"), QStringLiteral("standard"));
    m_template->addItem(tr("Как в кабинете «Честного знака»: бренд, артикул, продукция, цвет, размер, изготовитель, страна"), QStringLiteral("cabinet"));
    m_template->setToolTip(tr("Второй шаблон рассчитан на горизонтальную этикетку (например 58×40 мм); на вертикальной рисуется стандартный."));
    const int ti = m_template->findData(QSettings().value(QStringLiteral("thermal/markingTemplate"), QStringLiteral("standard")).toString());
    m_template->setCurrentIndex(ti >= 0 ? ti : 0);
    templateRow->addWidget(m_template, 1);
    layout->addLayout(templateRow);

    m_fieldsBox = new QGroupBox(tr("Поля шаблона: пусто - берётся из карточки товара, заполнено - подставляется на все этикетки"), this);
    auto *fl = new QHBoxLayout(m_fieldsBox);
    auto addField = [&](QLineEdit *&edit, const QString &caption, const QString &hint) {
        edit = new QLineEdit(m_fieldsBox);
        edit->setPlaceholderText(hint);
        edit->setClearButtonEnabled(true);
        auto *col = new QVBoxLayout();
        col->addWidget(new QLabel(caption, m_fieldsBox));
        col->addWidget(edit);
        fl->addLayout(col, 1);
    };
    addField(m_brand, tr("Бренд"), tr("из товара"));
    addField(m_product, tr("Продукция"), tr("из товара"));
    addField(m_color, tr("Цвет"), tr("из названия"));
    addField(m_manufacturer, tr("Изготовитель"), tr("из товара"));
    addField(m_country, tr("Страна"), tr("из товара"));
    layout->addWidget(m_fieldsBox);
    m_fieldsBox->setVisible(m_template->currentData().toString() == QLatin1String("cabinet"));

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
    connect(m_template, &QComboBox::currentIndexChanged, this, [this]() {
        m_fieldsBox->setVisible(m_template->currentData().toString() == QLatin1String("cabinet"));
        QSettings().setValue(QStringLiteral("thermal/markingTemplate"), m_template->currentData().toString());
        updatePreview();
    });
    for (QLineEdit *e : {m_brand, m_product, m_color, m_manufacturer, m_country})
        connect(e, &QLineEdit::textChanged, this, &MarkingPrintDialog::updatePreview);
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
    ThermalLabel::MarkingLabel label{r.sku, r.name, r.code};
    if (m_template->currentData().toString() != QLatin1String("cabinet"))
        return label;
    label.layout = ThermalLabel::MarkingLabel::Layout::Cabinet;
    const ProductMarketInfo info = m_info.value(r.productId);
    auto pick = [](const QLineEdit *edit, const QString &fromProduct) { return edit->text().trimmed().isEmpty() ? fromProduct : edit->text().trimmed(); };
    label.brand = pick(m_brand, info.vendor);
    label.product = pick(m_product, info.categoryName);
    label.color = pick(m_color, guessColor(r.name));
    label.manufacturer = pick(m_manufacturer, info.vendor);
    label.country = pick(m_country, info.countries.isEmpty() ? QString() : info.countries.first()); // на этикетке одна страна: первая из карточки
    return label;
}

void MarkingPrintDialog::setCabinetTemplate(bool cabinet)
{
    const int i = m_template->findData(cabinet ? QStringLiteral("cabinet") : QStringLiteral("standard"));
    if (i >= 0)
        m_template->setCurrentIndex(i);
}

void MarkingPrintDialog::setOverrides(const QString &brand, const QString &product, const QString &color, const QString &manufacturer, const QString &country)
{
    m_brand->setText(brand);
    m_product->setText(product);
    m_color->setText(color);
    m_manufacturer->setText(manufacturer);
    m_country->setText(country);
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
