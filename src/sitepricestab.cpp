#include "sitepricestab.h"

#include "database.h"
#include "theme.h"
#include "xlsx.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using SitePrices::Entry;
using SitePrices::ModelRow;
using SitePrices::SaleState;

namespace {

QString money(double v)
{
    const bool whole = std::fabs(v - std::round(v)) < 0.005;
    QString s = QString::number(std::fabs(v), 'f', whole ? 0 : 2);
    const int dot = s.indexOf(QLatin1Char('.'));
    int i = (dot < 0 ? s.size() : dot) - 3;
    while (i > 0) {
        s.insert(i, QChar(0x00A0));
        i -= 3;
    }
    return v < 0 ? QLatin1Char('-') + s : s;
}

// «12 990», «12990,5 ₽», «12 990 р.» -> число
double parseMoney(const QString &text, bool *ok)
{
    QString s = text;
    s.remove(QRegularExpression(QStringLiteral("[\\s\\x{00A0}\\x{202F}₽рР.]+$")));
    s.remove(QRegularExpression(QStringLiteral("[\\s\\x{00A0}\\x{202F}]")));
    s.replace(QLatin1Char(','), QLatin1Char('.'));
    bool good = false;
    const double v = s.toDouble(&good);
    if (ok)
        *ok = good && std::isfinite(v);
    return v;
}

QString dateText(const QDate &d)
{
    return d.isValid() ? d.toString(QStringLiteral("dd.MM.yyyy")) : QString();
}

// «15.11.2026», «2026-11-15» или число Excel (дата, сохранённая в таблице как число). Пустая строка - допустимо, дата пустая.
QDate parseDate(const QString &text, bool *ok)
{
    const QString s = text.trimmed();
    if (ok)
        *ok = true;
    if (s.isEmpty())
        return QDate();
    for (const char *fmt : {"dd.MM.yyyy", "d.M.yyyy", "yyyy-MM-dd", "dd.MM.yy"}) {
        const QDate d = QDate::fromString(s, QString::fromLatin1(fmt));
        if (d.isValid())
            return d;
    }
    bool num = false;
    const double serial = s.toDouble(&num);
    if (num && serial > 30000 && serial < 80000)
        return QDate(1899, 12, 30).addDays(static_cast<qint64>(serial));
    if (ok)
        *ok = false;
    return QDate();
}

// Числа в таблице сортируются как числа, а не как текст.
class NumItem : public QTableWidgetItem
{
public:
    using QTableWidgetItem::QTableWidgetItem;
    bool operator<(const QTableWidgetItem &other) const override
    {
        return data(Qt::UserRole + 1).toDouble() < other.data(Qt::UserRole + 1).toDouble();
    }
};

QTableWidgetItem *numItem(double value, const QString &text)
{
    auto *it = new NumItem(text);
    it->setData(Qt::UserRole + 1, value);
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return it;
}

QComboBox *stepCombo(QWidget *parent)
{
    auto *c = new QComboBox(parent);
    c->addItem(QObject::tr("до 1 ₽"), 1);
    c->addItem(QObject::tr("до 10 ₽"), 10);
    c->addItem(QObject::tr("до 100 ₽"), 100);
    return c;
}

// ---- Диалог «Цена сайта» для нескольких моделей ----
class PriceDialog : public QDialog
{
public:
    PriceDialog(int count, const ModelRow &example, QWidget *parent)
        : QDialog(parent), m_example(example)
    {
        setWindowTitle(tr("Цена сайта"));
        auto *form = new QFormLayout(this);
        form->addRow(new QLabel(tr("Выбрано моделей: %1").arg(count), this));
        m_mode = new QComboBox(this);
        m_mode->addItem(tr("Как на складе"));
        m_mode->addItem(tr("Цена склада плюс наценка, %"));
        m_mode->addItem(tr("Одна цена для всех, ₽"));
        form->addRow(tr("Как считать:"), m_mode);
        m_percent = new QDoubleSpinBox(this);
        m_percent->setRange(-90, 1000);
        m_percent->setDecimals(1);
        m_percent->setValue(20);
        m_percent->setSuffix(QStringLiteral(" %"));
        form->addRow(tr("Наценка:"), m_percent);
        m_fixed = new QDoubleSpinBox(this);
        m_fixed->setRange(1, 10000000);
        m_fixed->setDecimals(0);
        m_fixed->setValue(example.warehousePrice > 0 ? example.warehousePrice : 1000);
        m_fixed->setSuffix(QStringLiteral(" ₽"));
        form->addRow(tr("Цена:"), m_fixed);
        m_step = stepCombo(this);
        m_step->setCurrentIndex(1);
        form->addRow(tr("Округлять:"), m_step);
        m_preview = new QLabel(this);
        m_preview->setWordWrap(true);
        m_preview->setObjectName(QStringLiteral("muted"));
        form->addRow(m_preview);
        auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        box->button(QDialogButtonBox::Ok)->setText(tr("Применить"));
        box->button(QDialogButtonBox::Cancel)->setText(tr("Отмена"));
        form->addRow(box);
        connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() { update(); });
        connect(m_percent, &QDoubleSpinBox::valueChanged, this, [this]() { update(); });
        connect(m_fixed, &QDoubleSpinBox::valueChanged, this, [this]() { update(); });
        connect(m_step, &QComboBox::currentIndexChanged, this, [this]() { update(); });
        update();
    }

    // Цена сайта для модели по выбранным правилам (0 - посчитать нельзя).
    double priceFor(const ModelRow &m) const
    {
        const int mode = m_mode->currentIndex();
        if (mode == 2)
            return m_fixed->value();
        if (m.warehousePrice <= 0)
            return 0;
        if (mode == 0)
            return m.warehousePrice;
        const double raw = m.warehousePrice * (1.0 + m_percent->value() / 100.0);
        const int step = m_step->currentData().toInt();
        const double s = step > 1 ? step : 1;
        return std::max(1.0, std::round(raw / s) * s);
    }

private:
    void update()
    {
        const int mode = m_mode->currentIndex();
        m_percent->setEnabled(mode == 1);
        m_fixed->setEnabled(mode == 2);
        m_step->setEnabled(mode == 1);
        const double v = priceFor(m_example);
        m_preview->setText(v > 0 ? tr("Например, %1: цена склада %2 ₽ → цена сайта %3 ₽.").arg(m_example.key, money(m_example.warehousePrice), money(v))
                                 : tr("У модели %1 нет цены склада: для неё цена не изменится.").arg(m_example.key));
    }

    ModelRow m_example;
    QComboBox *m_mode;
    QDoubleSpinBox *m_percent;
    QDoubleSpinBox *m_fixed;
    QComboBox *m_step;
    QLabel *m_preview;
};

// ---- Диалог «Скидка» ----
class SaleDialog : public QDialog
{
public:
    SaleDialog(int count, const ModelRow &example, QWidget *parent)
        : QDialog(parent), m_example(example)
    {
        setWindowTitle(tr("Скидка на сайте"));
        auto *form = new QFormLayout(this);
        form->addRow(new QLabel(tr("Выбрано моделей: %1").arg(count), this));
        m_percent = new QSpinBox(this);
        m_percent->setRange(1, 90);
        m_percent->setValue(20);
        m_percent->setSuffix(QStringLiteral(" %"));
        form->addRow(tr("Скидка:"), m_percent);
        m_step = stepCombo(this);
        m_step->setCurrentIndex(1);
        form->addRow(tr("Округлять цену:"), m_step);
        m_period = new QCheckBox(tr("Ограничить сроком"), this);
        form->addRow(m_period);
        m_from = new QDateEdit(QDate::currentDate(), this);
        m_to = new QDateEdit(QDate::currentDate().addDays(7), this);
        for (auto *d : {m_from, m_to}) {
            d->setCalendarPopup(true);
            d->setDisplayFormat(QStringLiteral("dd.MM.yyyy"));
        }
        form->addRow(tr("С (включительно):"), m_from);
        form->addRow(tr("По (включительно):"), m_to);
        m_preview = new QLabel(this);
        m_preview->setWordWrap(true);
        m_preview->setObjectName(QStringLiteral("muted"));
        form->addRow(m_preview);
        auto *note = new QLabel(tr("У моделей без цены сайта обычной ценой станет цена склада. Прежняя скидка этих моделей заменится."), this);
        note->setWordWrap(true);
        note->setObjectName(QStringLiteral("muted"));
        form->addRow(note);
        auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        box->button(QDialogButtonBox::Ok)->setText(tr("Применить"));
        box->button(QDialogButtonBox::Cancel)->setText(tr("Отмена"));
        form->addRow(box);
        connect(box, &QDialogButtonBox::accepted, this, [this]() {
            if (m_period->isChecked() && m_to->date() < m_from->date()) {
                QMessageBox::warning(this, tr("Скидка"), tr("Скидка не может заканчиваться раньше, чем начинается."));
                return;
            }
            accept();
        });
        connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_period, &QCheckBox::toggled, this, [this]() { update(); });
        connect(m_percent, &QSpinBox::valueChanged, this, [this]() { update(); });
        connect(m_step, &QComboBox::currentIndexChanged, this, [this]() { update(); });
        update();
    }

    int percent() const { return m_percent->value(); }
    int step() const { return m_step->currentData().toInt(); }
    QDate from() const { return m_period->isChecked() ? m_from->date() : QDate(); }
    QDate to() const { return m_period->isChecked() ? m_to->date() : QDate(); }

private:
    void update()
    {
        m_from->setEnabled(m_period->isChecked());
        m_to->setEnabled(m_period->isChecked());
        const double base = m_example.hasSite ? m_example.site.price : m_example.warehousePrice;
        m_preview->setText(base > 0 ? tr("Например, %1: %2 ₽ → %3 ₽.").arg(m_example.key, money(base), money(SitePrices::priceByPercent(base, m_percent->value(), step())))
                                    : tr("У модели %1 нет цены: для неё скидка не применится.").arg(m_example.key));
    }

    ModelRow m_example;
    QSpinBox *m_percent;
    QComboBox *m_step;
    QCheckBox *m_period;
    QDateEdit *m_from;
    QDateEdit *m_to;
    QLabel *m_preview;
};

bool sameEntry(const Entry &a, const Entry &b)
{
    auto eq = [](double x, double y) { return std::fabs(x - y) < 0.005; };
    if (!eq(a.price, b.price) || a.salePrice.has_value() != b.salePrice.has_value())
        return false;
    if (a.salePrice && !eq(*a.salePrice, *b.salePrice))
        return false;
    return !a.salePrice || (a.saleFrom == b.saleFrom && a.saleTo == b.saleTo);
}

} // namespace

SitePricesTab::SitePricesTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *info = new QLabel(tr("Цены и скидки, которые видят покупатели на сайте. Они не зависят от цены склада (её используют учёт и Яндекс Маркет). "
                               "Цена задаётся на модель - артикул без размера - и действует на все её размеры. "
                               "Если у модели цены сайта нет, сайт берёт цену склада. Ячейки цены, скидки и сроков правятся двойным щелчком."),
                            this);
    info->setWordWrap(true);
    info->setObjectName(QStringLiteral("muted"));
    layout->addWidget(info);

    auto *filterRow = new QHBoxLayout();
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Артикул модели или название"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(280);
    filterRow->addWidget(m_search);
    m_filter = new QComboBox(this);
    m_filter->addItem(tr("Все модели"));
    m_filter->addItem(tr("Без цены сайта (берётся цена склада)"));
    m_filter->addItem(tr("Скидка действует сейчас"));
    m_filter->addItem(tr("Скидка начнётся позже"));
    m_filter->addItem(tr("Скидка закончилась"));
    m_filter->addItem(tr("Цена сайта отличается от цены склада"));
    m_filter->addItem(tr("Есть остаток"));
    filterRow->addWidget(m_filter);
    filterRow->addStretch();
    m_summary = new QLabel(this);
    filterRow->addWidget(m_summary);
    layout->addLayout(filterRow);

    auto *buttons = new QHBoxLayout();
    m_fromWarehouseBtn = new QPushButton(tr("Цена как на складе"), this);
    m_priceBtn = new QPushButton(tr("Задать цену…"), this);
    m_saleBtn = new QPushButton(tr("Скидка…"), this);
    m_saleBtn->setProperty("primary", true);
    m_clearSaleBtn = new QPushButton(tr("Убрать скидку"), this);
    m_removeBtn = new QPushButton(tr("Убрать цену сайта"), this);
    auto *exportBtn = new QPushButton(tr("Выгрузить в Excel…"), this);
    auto *importBtn = new QPushButton(tr("Загрузить из Excel…"), this);
    for (auto *b : {m_fromWarehouseBtn, m_priceBtn, m_saleBtn, m_clearSaleBtn, m_removeBtn})
        buttons->addWidget(b);
    buttons->addStretch();
    buttons->addWidget(exportBtn);
    buttons->addWidget(importBtn);
    layout->addLayout(buttons);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Артикул модели"), tr("Название"), tr("Размеры"), tr("Остаток"), tr("Цена склада"), tr("Цена сайта"),
                                        tr("Скидка, %"), tr("Со скидкой"), tr("Скидка с"), tr("Скидка по"), tr("Сейчас на сайте")});
    const QStringList tips = {tr("Артикул без размера: DD1391-100_42 -> DD1391-100"), tr("Название первого размера модели"),
                              tr("Сколько размеров (товаров) в модели"), tr("Остаток по всем размерам и складам"),
                              tr("Цена склада, ₽ (наименьшая среди размеров). Здесь не меняется"),
                              tr("Обычная цена на сайте, ₽. Пусто - сайт берёт цену склада"),
                              tr("Размер скидки в процентах. Можно вписать процент - цена со скидкой посчитается сама"),
                              tr("Цена со скидкой, ₽. Должна быть меньше обычной цены сайта"),
                              tr("Первый день скидки. Пусто - действует сразу"), tr("Последний день скидки включительно. Пусто - без срока"),
                              tr("Что увидит покупатель сегодня")};
    for (int c = 0; c < ColCount; ++c)
        m_table->horizontalHeaderItem(c)->setToolTip(tips[c]);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(34);
    m_table->setAlternatingRowColors(true);
    m_table->sortByColumn(ColKey, Qt::AscendingOrder); // по умолчанию Qt сортирует по убыванию
    m_table->setSortingEnabled(true);
    auto *header = m_table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setStretchLastSection(true);
    const int widths[ColCount] = {150, 250, 84, 84, 112, 106, 96, 106, 100, 100, 240};
    for (int c = 0; c < ColCount; ++c)
        m_table->setColumnWidth(c, widths[c]);
    layout->addWidget(m_table, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(250);
    connect(m_searchTimer, &QTimer::timeout, this, &SitePricesTab::applyFilter);
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_filter, &QComboBox::currentIndexChanged, this, &SitePricesTab::applyFilter);
    connect(m_table, &QTableWidget::itemChanged, this, &SitePricesTab::onItemChanged);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &SitePricesTab::onSelectionChanged);
    connect(m_fromWarehouseBtn, &QPushButton::clicked, this, &SitePricesTab::priceFromWarehouse);
    connect(m_priceBtn, &QPushButton::clicked, this, &SitePricesTab::setPriceDialog);
    connect(m_saleBtn, &QPushButton::clicked, this, &SitePricesTab::setSaleDialog);
    connect(m_clearSaleBtn, &QPushButton::clicked, this, &SitePricesTab::clearSale);
    connect(m_removeBtn, &QPushButton::clicked, this, &SitePricesTab::removeSitePrice);
    connect(exportBtn, &QPushButton::clicked, this, &SitePricesTab::exportExcel);
    connect(importBtn, &QPushButton::clicked, this, &SitePricesTab::importExcel);
    onSelectionChanged();
}

void SitePricesTab::refresh()
{
    QString error;
    const QList<ModelRow> rows = SitePrices::loadModels(&error);
    if (rows.isEmpty() && !error.isEmpty()) {
        setStatus(tr("Не удалось прочитать цены сайта: %1").arg(error), true);
        return;
    }
    m_rows = rows;
    std::sort(m_rows.begin(), m_rows.end(), [](const ModelRow &a, const ModelRow &b) { return a.key.compare(b.key, Qt::CaseInsensitive) < 0; });
    m_index.clear();
    for (int i = 0; i < m_rows.size(); ++i)
        m_index.insert(m_rows[i].key, i);
    applyFilter();
}

bool SitePricesTab::visibleByFilter(const ModelRow &m) const
{
    const QString needle = m_search->text().trimmed();
    if (!needle.isEmpty()) {
        bool all = true;
        for (const QString &w : needle.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
            all = all && (m.key.contains(w, Qt::CaseInsensitive) || m.name.contains(w, Qt::CaseInsensitive));
        if (!all)
            return false;
    }
    const QDate today = QDate::currentDate();
    switch (m_filter->currentIndex()) {
    case 1: return !m.hasSite;
    case 2: return m.hasSite && SitePrices::saleState(m.site, today) == SaleState::Active;
    case 3: return m.hasSite && SitePrices::saleState(m.site, today) == SaleState::Scheduled;
    case 4: return m.hasSite && SitePrices::saleState(m.site, today) == SaleState::Ended;
    case 5: return m.hasSite && std::fabs(m.site.price - m.warehousePrice) >= 0.005;
    case 6: return m.stock > 0;
    default: return true;
    }
}

void SitePricesTab::applyFilter()
{
    m_loading = true;
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    int row = 0;
    for (const ModelRow &m : m_rows) {
        if (!visibleByFilter(m))
            continue;
        m_table->insertRow(row);
        fillTableRow(row, m);
        ++row;
    }
    m_table->setSortingEnabled(true);
    m_loading = false;
    setSummary();
    onSelectionChanged();
}

void SitePricesTab::fillTableRow(int row, const ModelRow &m)
{
    const Theme::Colors &c = Theme::colors();
    const QDate today = QDate::currentDate();
    auto readOnly = [](QTableWidgetItem *it) {
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        return it;
    };
    auto *key = readOnly(new QTableWidgetItem(m.key));
    key->setData(Qt::UserRole, m.key);
    m_table->setItem(row, ColKey, key);
    m_table->setItem(row, ColName, readOnly(new QTableWidgetItem(m.name)));
    m_table->setItem(row, ColSizes, readOnly(numItem(m.sizes, QString::number(m.sizes))));
    m_table->setItem(row, ColStock, readOnly(numItem(static_cast<double>(m.stock), QString::number(m.stock))));
    m_table->setItem(row, ColWarehouse, readOnly(numItem(m.warehousePrice, m.warehousePrice > 0 ? money(m.warehousePrice) : QStringLiteral("-"))));

    const bool has = m.hasSite;
    auto *price = numItem(has ? m.site.price : 0, has ? money(m.site.price) : QString());
    if (!has)
        price->setToolTip(tr("Цена сайта не задана: сайт берёт цену склада"));
    m_table->setItem(row, ColPrice, price);

    const bool sale = has && m.site.salePrice.has_value();
    const int pct = sale ? SitePrices::discountPercent(m.site.price, *m.site.salePrice) : 0;
    m_table->setItem(row, ColPercent, numItem(pct, sale ? QString::number(pct) : QString()));
    m_table->setItem(row, ColSale, numItem(sale ? *m.site.salePrice : 0, sale ? money(*m.site.salePrice) : QString()));
    auto *from = new QTableWidgetItem(sale ? dateText(m.site.saleFrom) : QString());
    auto *to = new QTableWidgetItem(sale ? dateText(m.site.saleTo) : QString());
    from->setToolTip(tr("Пусто - скидка действует сразу. Формат: дд.мм.гггг"));
    to->setToolTip(tr("Пусто - скидка без срока. Последний день включительно. Формат: дд.мм.гггг"));
    m_table->setItem(row, ColFrom, from);
    m_table->setItem(row, ColTo, to);

    QString now;
    QColor color = c.text;
    if (!has) {
        now = m.warehousePrice > 0 ? tr("%1 ₽ (цена склада)").arg(money(m.warehousePrice)) : tr("нет цены - модель не показывается");
        color = c.muted;
    } else {
        const SaleState st = SitePrices::saleState(m.site, today);
        switch (st) {
        case SaleState::None:
            now = tr("%1 ₽").arg(money(m.site.price));
            break;
        case SaleState::Active:
            now = tr("%1 ₽ (было %2), -%3%%4").arg(money(*m.site.salePrice), money(m.site.price)).arg(pct)
                      .arg(m.site.saleTo.isValid() ? tr(" до %1").arg(m.site.saleTo.toString(QStringLiteral("dd.MM"))) : QString());
            color = c.ok;
            break;
        case SaleState::Scheduled:
            now = tr("%1 ₽; с %2 будет %3 ₽").arg(money(m.site.price), m.site.saleFrom.toString(QStringLiteral("dd.MM")), money(*m.site.salePrice));
            color = c.accent;
            break;
        case SaleState::Ended:
            now = tr("%1 ₽ (скидка закончилась)").arg(money(m.site.price));
            color = c.muted;
            break;
        }
    }
    auto *nowItem = readOnly(new QTableWidgetItem(now));
    nowItem->setForeground(QBrush(color));
    if (m.orphan)
        nowItem->setToolTip(tr("Товаров с таким артикулом в базе больше нет. Цену можно убрать."));
    m_table->setItem(row, ColNow, nowItem);
    if (m.orphan)
        for (int col = 0; col < ColCount; ++col)
            if (auto *it = m_table->item(row, col))
                it->setForeground(QBrush(c.muted));
}

void SitePricesTab::updateTableRowFor(const QString &key)
{
    const int ri = rowIndexOf(key);
    if (ri < 0)
        return;
    m_loading = true;
    const bool sorting = m_table->isSortingEnabled();
    m_table->setSortingEnabled(false);
    for (int r = 0; r < m_table->rowCount(); ++r) {
        if (m_table->item(r, ColKey) && m_table->item(r, ColKey)->data(Qt::UserRole).toString() == key) {
            fillTableRow(r, m_rows[ri]);
            break;
        }
    }
    m_table->setSortingEnabled(sorting);
    m_loading = false;
}

int SitePricesTab::rowIndexOf(const QString &key) const
{
    return m_index.value(key, -1);
}

QStringList SitePricesTab::selectedKeys() const
{
    QStringList keys;
    QSet<int> seen;
    for (const QModelIndex &idx : m_table->selectionModel()->selectedRows()) {
        if (seen.contains(idx.row()))
            continue;
        seen.insert(idx.row());
        keys << m_table->item(idx.row(), ColKey)->data(Qt::UserRole).toString();
    }
    return keys;
}

QList<int> SitePricesTab::selectedRowIndexes() const
{
    QList<int> out;
    for (const QString &k : selectedKeys()) {
        const int i = rowIndexOf(k);
        if (i >= 0)
            out << i;
    }
    return out;
}

void SitePricesTab::onSelectionChanged()
{
    const bool any = !m_table->selectionModel()->selectedRows().isEmpty();
    for (auto *b : {m_fromWarehouseBtn, m_priceBtn, m_saleBtn, m_clearSaleBtn, m_removeBtn})
        b->setEnabled(any);
}

void SitePricesTab::setSummary()
{
    const QDate today = QDate::currentDate();
    int withSite = 0, onSale = 0, without = 0;
    for (const ModelRow &m : m_rows) {
        if (m.orphan)
            continue;
        if (m.hasSite) {
            ++withSite;
            onSale += SitePrices::saleState(m.site, today) == SaleState::Active;
        } else {
            ++without;
        }
    }
    m_summary->setText(tr("Показано %1 из %2 · цена сайта задана: %3 · скидка сейчас: %4 · без цены сайта: %5")
                           .arg(m_table->rowCount()).arg(m_rows.size()).arg(withSite).arg(onSale).arg(without));
}

void SitePricesTab::setStatus(const QString &text, bool problem)
{
    const Theme::Colors &c = Theme::colors();
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg((problem ? c.danger : c.muted).name()));
    m_status->setText(text);
}

bool SitePricesTab::saveEntries(const QList<Entry> &entries, const QString &okText)
{
    QString error;
    if (!SitePrices::save(entries, Database::currentUser(), &error)) {
        QMessageBox::warning(this, tr("Цены сайта"), tr("Не удалось сохранить: %1").arg(error));
        return false;
    }
    const QDateTime now = QDateTime::currentDateTime();
    for (const Entry &e : entries) {
        const int i = rowIndexOf(e.modelKey);
        if (i < 0)
            continue;
        m_rows[i].hasSite = true;
        m_rows[i].site = e;
        m_rows[i].updatedBy = Database::currentUser();
        m_rows[i].updatedAt = now;
        m_rows[i].orphan = false;
    }
    m_loading = true;
    const bool sorting = m_table->isSortingEnabled();
    m_table->setSortingEnabled(false);
    for (const Entry &e : entries)
        for (int r = 0; r < m_table->rowCount(); ++r)
            if (m_table->item(r, ColKey)->data(Qt::UserRole).toString() == e.modelKey) {
                fillTableRow(r, m_rows[rowIndexOf(e.modelKey)]);
                break;
            }
    m_table->setSortingEnabled(sorting);
    m_loading = false;
    setSummary();
    setStatus(okText);
    return true;
}

void SitePricesTab::onItemChanged(QTableWidgetItem *item)
{
    if (m_loading || !item)
        return;
    const int col = item->column();
    if (col < ColPrice || col > ColTo)
        return;
    const QString key = m_table->item(item->row(), ColKey)->data(Qt::UserRole).toString();
    const int ri = rowIndexOf(key);
    if (ri < 0)
        return;
    const ModelRow m = m_rows[ri];
    const QString text = item->text().trimmed();
    auto restore = [&](const QString &why) {
        setStatus(why, true);
        updateTableRowFor(key);
    };

    Entry e = m.hasSite ? m.site : Entry{key, m.warehousePrice, std::nullopt, QDate(), QDate()};
    QString note = tr("Сохранено: %1").arg(key);

    if (col == ColPrice) {
        if (text.isEmpty()) {
            if (!m.hasSite) {
                updateTableRowFor(key);
                return;
            }
            if (QMessageBox::question(this, tr("Цена сайта"), tr("Убрать цену сайта у модели %1? Сайт вернётся к цене склада.").arg(key)) != QMessageBox::Yes) {
                updateTableRowFor(key);
                return;
            }
            QString err;
            if (!SitePrices::remove({key}, &err)) {
                restore(tr("Не удалось убрать: %1").arg(err));
                return;
            }
            m_rows[ri].hasSite = false;
            m_rows[ri].site = Entry();
            updateTableRowFor(key);
            setSummary();
            setStatus(tr("Цена сайта убрана: %1 теперь по цене склада").arg(key));
            return;
        }
        bool ok = false;
        const double v = parseMoney(text, &ok);
        if (!ok || v <= 0) {
            restore(tr("Цена сайта - число больше нуля"));
            return;
        }
        e.price = v;
        if (e.salePrice && *e.salePrice >= v) {
            e.salePrice.reset();
            e.saleFrom = e.saleTo = QDate();
            note += tr(". Скидка снята: цена со скидкой не может быть выше обычной");
        }
    } else {
        if (!m.hasSite) {
            if (m.warehousePrice <= 0) {
                restore(tr("Сначала задайте цену сайта: у модели нет цены склада"));
                return;
            }
            note += tr(". Обычной ценой на сайте стала цена склада");
        }
        if (col == ColPercent) {
            bool ok = false;
            const double p = text.isEmpty() ? 0 : parseMoney(text, &ok);
            if (text.isEmpty() || (ok && p == 0)) {
                e.salePrice.reset();
                e.saleFrom = e.saleTo = QDate();
            } else if (!ok || p < 1 || p > 90) {
                restore(tr("Скидка - от 1 до 90 процентов"));
                return;
            } else {
                e.salePrice = SitePrices::priceByPercent(e.price, p, 1);
                if (*e.salePrice >= e.price) {
                    restore(tr("При такой цене скидка не получается: цена со скидкой не меньше обычной"));
                    return;
                }
            }
        } else if (col == ColSale) {
            if (text.isEmpty()) {
                e.salePrice.reset();
                e.saleFrom = e.saleTo = QDate();
            } else {
                bool ok = false;
                const double v = parseMoney(text, &ok);
                if (!ok || v <= 0 || v >= e.price) {
                    restore(tr("Цена со скидкой должна быть больше нуля и меньше обычной цены (%1 ₽)").arg(money(e.price)));
                    return;
                }
                e.salePrice = v;
            }
        } else { // сроки скидки
            if (!e.salePrice) {
                restore(tr("Сначала задайте скидку, потом её срок"));
                return;
            }
            bool ok = false;
            const QDate d = parseDate(text, &ok);
            if (!ok) {
                restore(tr("Дата - в виде дд.мм.гггг, например 15.11.2026"));
                return;
            }
            (col == ColFrom ? e.saleFrom : e.saleTo) = d;
            if (e.saleFrom.isValid() && e.saleTo.isValid() && e.saleTo < e.saleFrom) {
                restore(tr("Скидка не может заканчиваться раньше, чем начинается"));
                return;
            }
        }
    }
    if (!saveEntries({e}, note))
        updateTableRowFor(key);
}

void SitePricesTab::priceFromWarehouse()
{
    QList<Entry> entries;
    int skipped = 0;
    for (int i : selectedRowIndexes()) {
        const ModelRow &m = m_rows[i];
        if (m.warehousePrice <= 0) {
            ++skipped;
            continue;
        }
        Entry e = m.hasSite ? m.site : Entry{m.key, m.warehousePrice, std::nullopt, QDate(), QDate()};
        e.price = m.warehousePrice;
        if (e.salePrice && *e.salePrice >= e.price) {
            e.salePrice.reset();
            e.saleFrom = e.saleTo = QDate();
        }
        entries << e;
    }
    if (!entries.isEmpty() && saveEntries(entries, tr("Цена как на складе: %1 мод.").arg(entries.size())) && skipped)
        setStatus(tr("Цена как на складе: %1 мод.; у %2 нет цены склада - пропущены").arg(entries.size()).arg(skipped));
}

void SitePricesTab::setPriceDialog()
{
    const QList<int> idx = selectedRowIndexes();
    if (idx.isEmpty())
        return;
    PriceDialog dlg(idx.size(), m_rows[idx.first()], this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QList<Entry> entries;
    int skipped = 0;
    for (int i : idx) {
        const ModelRow &m = m_rows[i];
        const double v = dlg.priceFor(m);
        if (v <= 0) {
            ++skipped;
            continue;
        }
        Entry e = m.hasSite ? m.site : Entry{m.key, v, std::nullopt, QDate(), QDate()};
        e.price = v;
        if (e.salePrice && *e.salePrice >= e.price) {
            e.salePrice.reset();
            e.saleFrom = e.saleTo = QDate();
        }
        entries << e;
    }
    if (entries.isEmpty()) {
        setStatus(tr("Ни у одной выбранной модели нет цены склада: считать не из чего"), true);
        return;
    }
    if (saveEntries(entries, tr("Цена сайта задана: %1 мод.").arg(entries.size())) && skipped)
        setStatus(tr("Цена сайта задана: %1 мод.; у %2 нет цены склада - пропущены").arg(entries.size()).arg(skipped));
}

void SitePricesTab::setSaleDialog()
{
    const QList<int> idx = selectedRowIndexes();
    if (idx.isEmpty())
        return;
    SaleDialog dlg(idx.size(), m_rows[idx.first()], this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QList<Entry> entries;
    int skipped = 0;
    for (int i : idx) {
        const ModelRow &m = m_rows[i];
        const double base = m.hasSite ? m.site.price : m.warehousePrice;
        if (base <= 0) {
            ++skipped;
            continue;
        }
        const double sale = SitePrices::priceByPercent(base, dlg.percent(), dlg.step());
        if (sale >= base) {
            ++skipped;
            continue;
        }
        Entry e{m.key, base, sale, dlg.from(), dlg.to()};
        entries << e;
    }
    if (entries.isEmpty()) {
        setStatus(tr("Скидку применить не к чему: у выбранных моделей нет цены"), true);
        return;
    }
    if (saveEntries(entries, tr("Скидка %1%: %2 мод.").arg(dlg.percent()).arg(entries.size())) && skipped)
        setStatus(tr("Скидка %1%: %2 мод.; %3 пропущены (нет цены)").arg(dlg.percent()).arg(entries.size()).arg(skipped));
}

void SitePricesTab::clearSale()
{
    QList<Entry> entries;
    for (int i : selectedRowIndexes()) {
        const ModelRow &m = m_rows[i];
        if (!m.hasSite || !m.site.salePrice)
            continue;
        Entry e = m.site;
        e.salePrice.reset();
        e.saleFrom = e.saleTo = QDate();
        entries << e;
    }
    if (entries.isEmpty()) {
        setStatus(tr("У выбранных моделей нет скидки"));
        return;
    }
    saveEntries(entries, tr("Скидка убрана: %1 мод.").arg(entries.size()));
}

void SitePricesTab::removeSitePrice()
{
    QStringList keys;
    for (int i : selectedRowIndexes())
        if (m_rows[i].hasSite)
            keys << m_rows[i].key;
    if (keys.isEmpty()) {
        setStatus(tr("У выбранных моделей цена сайта не задана"));
        return;
    }
    if (QMessageBox::question(this, tr("Убрать цену сайта"),
                              tr("Убрать цену и скидку сайта у моделей: %1?\nСайт вернётся к цене склада.").arg(keys.size())) != QMessageBox::Yes)
        return;
    QString error;
    if (!SitePrices::remove(keys, &error)) {
        QMessageBox::warning(this, tr("Цены сайта"), tr("Не удалось убрать: %1").arg(error));
        return;
    }
    refresh();
    setStatus(tr("Цена сайта убрана у моделей: %1").arg(keys.size()));
}

void SitePricesTab::exportExcel()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Выгрузить цены сайта"),
                                                      QStringLiteral("цены_сайта_%1.xlsx").arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))),
                                                      tr("Excel (*.xlsx)"));
    if (path.isEmpty())
        return;
    Xlsx::Sheet sheet;
    sheet.name = QStringLiteral("Цены сайта");
    sheet.rows.append({QStringLiteral("Артикул модели"), QStringLiteral("Название"), QStringLiteral("Размеров"), QStringLiteral("Остаток"),
                       QStringLiteral("Цена склада"), QStringLiteral("Цена сайта"), QStringLiteral("Цена со скидкой"), QStringLiteral("Скидка, %"),
                       QStringLiteral("Скидка с"), QStringLiteral("Скидка по")});
    auto num = [](double v) { return QString::number(v, 'f', std::fabs(v - std::round(v)) < 0.005 ? 0 : 2); };
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const int i = rowIndexOf(m_table->item(r, ColKey)->data(Qt::UserRole).toString());
        if (i < 0)
            continue;
        const ModelRow &m = m_rows[i];
        const bool sale = m.hasSite && m.site.salePrice;
        sheet.rows.append({m.key, m.name, QString::number(m.sizes), QString::number(m.stock), m.warehousePrice > 0 ? num(m.warehousePrice) : QString(),
                           m.hasSite ? num(m.site.price) : QString(), sale ? num(*m.site.salePrice) : QString(),
                           sale ? QString::number(SitePrices::discountPercent(m.site.price, *m.site.salePrice)) : QString(),
                           sale ? dateText(m.site.saleFrom) : QString(), sale ? dateText(m.site.saleTo) : QString()});
    }
    sheet.numericColumns = {2, 3, 4, 5, 6, 7};
    sheet.columnWidths = {22, 50, 10, 10, 14, 14, 16, 12, 13, 13};
    QString error;
    if (!Xlsx::write(path, {sheet}, &error)) {
        QMessageBox::warning(this, tr("Цены сайта"), tr("Не удалось записать файл: %1").arg(error));
        return;
    }
    setStatus(tr("Выгружено моделей: %1 - %2").arg(sheet.rows.size() - 1).arg(path));
}

void SitePricesTab::importExcel()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Загрузить цены сайта из Excel"), QString(), tr("Excel (*.xlsx)"));
    if (path.isEmpty())
        return;
    const Xlsx::Table table = Xlsx::readFirstSheet(path);
    if (!table.ok || table.rows.isEmpty()) {
        QMessageBox::warning(this, tr("Цены сайта"), tr("Не удалось прочитать файл: %1").arg(table.error.isEmpty() ? tr("он пустой") : table.error));
        return;
    }
    // Колонки находим по заголовкам (регистр и лишние пробелы не важны), порядок любой.
    auto findCol = [&](const QStringList &names) {
        const QStringList &head = table.rows.first();
        for (int c = 0; c < head.size(); ++c)
            if (names.contains(head[c].simplified().toLower()))
                return c;
        return -1;
    };
    const int cKey = findCol({QStringLiteral("артикул модели"), QStringLiteral("модель"), QStringLiteral("артикул"), QStringLiteral("sku")});
    const int cPrice = findCol({QStringLiteral("цена сайта"), QStringLiteral("цена")});
    const int cSale = findCol({QStringLiteral("цена со скидкой"), QStringLiteral("цена скидки")});
    const int cPct = findCol({QStringLiteral("скидка, %"), QStringLiteral("скидка %"), QStringLiteral("скидка")});
    const int cFrom = findCol({QStringLiteral("скидка с"), QStringLiteral("акция с")});
    const int cTo = findCol({QStringLiteral("скидка по"), QStringLiteral("акция по")});
    if (cKey < 0 || cPrice < 0) {
        QMessageBox::warning(this, tr("Цены сайта"),
                             tr("В первой строке файла нужны заголовки «Артикул модели» и «Цена сайта». "
                                "Необязательные: «Цена со скидкой» (или «Скидка, %»), «Скидка с», «Скидка по». "
                                "Проще всего начать с выгрузки из этой вкладки."));
        return;
    }
    auto cell = [&](const QStringList &row, int c) { return c >= 0 && c < row.size() ? row[c].trimmed() : QString(); };

    QList<Entry> changes;
    QStringList problems;
    int unchanged = 0;
    QSet<QString> seen;
    for (int r = 1; r < table.rows.size(); ++r) {
        const QStringList &row = table.rows[r];
        const QString raw = cell(row, cKey);
        if (raw.isEmpty())
            continue;
        const QString key = SitePrices::modelKey(raw);
        auto problem = [&](const QString &why) { problems << tr("строка %1 (%2): %3").arg(r + 1).arg(raw, why); };
        const int ri = rowIndexOf(key);
        if (ri < 0) {
            problem(tr("такой модели нет в товарах"));
            continue;
        }
        if (seen.contains(key)) {
            problem(tr("модель уже была выше в файле"));
            continue;
        }
        seen.insert(key);
        const ModelRow &m = m_rows[ri];
        Entry e;
        e.modelKey = key;
        const QString priceText = cell(row, cPrice);
        if (priceText.isEmpty()) {
            problem(tr("не указана цена сайта"));
            continue;
        }
        bool ok = false;
        e.price = parseMoney(priceText, &ok);
        if (!ok || e.price <= 0) {
            problem(tr("цена сайта «%1» - не число больше нуля").arg(priceText));
            continue;
        }
        const QString saleText = cell(row, cSale);
        const QString pctText = cell(row, cPct);
        if (!saleText.isEmpty()) {
            const double v = parseMoney(saleText, &ok);
            if (!ok || v <= 0 || v >= e.price) {
                problem(tr("цена со скидкой «%1» должна быть больше нуля и меньше обычной").arg(saleText));
                continue;
            }
            e.salePrice = v;
        } else if (!pctText.isEmpty()) {
            const double p = parseMoney(pctText, &ok);
            if (!ok || p < 0 || p > 90) {
                problem(tr("скидка «%1» - от 1 до 90 процентов").arg(pctText));
                continue;
            }
            if (p >= 1)
                e.salePrice = SitePrices::priceByPercent(e.price, p, 1);
        }
        if (e.salePrice) {
            bool okFrom = false, okTo = false;
            e.saleFrom = parseDate(cell(row, cFrom), &okFrom);
            e.saleTo = parseDate(cell(row, cTo), &okTo);
            if (!okFrom || !okTo) {
                problem(tr("дата скидки - в виде дд.мм.гггг"));
                continue;
            }
            if (e.saleFrom.isValid() && e.saleTo.isValid() && e.saleTo < e.saleFrom) {
                problem(tr("скидка заканчивается раньше, чем начинается"));
                continue;
            }
        }
        if (m.hasSite && sameEntry(m.site, e)) {
            ++unchanged;
            continue;
        }
        changes << e;
    }

    QString text = tr("Будет записано моделей: %1.\nБез изменений: %2.\nПропущено с ошибками: %3.").arg(changes.size()).arg(unchanged).arg(problems.size());
    if (!problems.isEmpty())
        text += QStringLiteral("\n\n") + problems.mid(0, 12).join(QLatin1Char('\n')) + (problems.size() > 12 ? tr("\n… и ещё %1").arg(problems.size() - 12) : QString());
    if (changes.isEmpty()) {
        QMessageBox::information(this, tr("Загрузка цен сайта"), tr("Записывать нечего.\n\n") + text);
        return;
    }
    if (QMessageBox::question(this, tr("Загрузка цен сайта"), text + tr("\n\nЗаписать?")) != QMessageBox::Yes)
        return;
    saveEntries(changes, tr("Загружено из Excel: %1 мод., пропущено с ошибками: %2").arg(changes.size()).arg(problems.size()));
}
