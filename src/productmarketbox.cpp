#include "productmarketbox.h"
#include "categorypicker.h"
#include "marketapi.h"
#include "marketcategories.h"
#include "paramform.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QDoubleSpinBox *dimension(QWidget *parent, int decimals, const QString &suffix, double max)
{
    auto *s = new QDoubleSpinBox(parent);
    s->setRange(0, max);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setSpecialValueText(QObject::tr("не задано"));
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    return s;
}

} // namespace

ProductMarketBox::ProductMarketBox(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto *head = new QHBoxLayout();
    m_toggle = new QToolButton(this);
    m_toggle->setCheckable(true);
    m_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_toggle->setAutoRaise(true);
    m_toggle->setCursor(Qt::PointingHandCursor);
    head->addWidget(m_toggle);
    m_summary = new QLabel(this);
    m_summary->setObjectName("muted");
    m_summary->setWordWrap(true);
    head->addWidget(m_summary, 1);
    root->addLayout(head);

    m_body = new QWidget(this);
    auto *bl = new QVBoxLayout(m_body);
    bl->setContentsMargins(0, 0, 0, 0);
    auto *form = new QFormLayout();
    bl->addLayout(form);

    auto *vendorRow = new QHBoxLayout();
    m_vendor = new QLineEdit(m_body);
    m_vendor->setPlaceholderText(tr("бренд, напр. Nike"));
    m_vendorGuess = new QPushButton(tr("По названию"), m_body);
    m_vendorGuess->setAutoDefault(false);
    m_vendorGuess->setToolTip(tr("Определить бренд по названию товара (Nike, Adidas и т. п.)"));
    vendorRow->addWidget(m_vendor, 1);
    vendorRow->addWidget(m_vendorGuess);
    form->addRow(tr("Бренд"), vendorRow);

    m_countries = new QLineEdit(m_body);
    m_countries->setPlaceholderText(tr("страны производства через «;», напр. Китай; Вьетнам"));
    form->addRow(tr("Страна"), m_countries);

    auto *packRow = new QHBoxLayout();
    m_length = dimension(m_body, 1, tr(" см"), 1000);
    m_width = dimension(m_body, 1, tr(" см"), 1000);
    m_height = dimension(m_body, 1, tr(" см"), 1000);
    m_weight = dimension(m_body, 3, tr(" кг"), 1000);
    m_length->setToolTip(tr("Длина упаковки"));
    m_width->setToolTip(tr("Ширина упаковки"));
    m_height->setToolTip(tr("Высота упаковки"));
    m_weight->setToolTip(tr("Вес с упаковкой"));
    packRow->addWidget(new QLabel(tr("Д"), m_body));
    packRow->addWidget(m_length);
    packRow->addWidget(new QLabel(tr("Ш"), m_body));
    packRow->addWidget(m_width);
    packRow->addWidget(new QLabel(tr("В"), m_body));
    packRow->addWidget(m_height);
    packRow->addWidget(new QLabel(tr("Вес"), m_body));
    packRow->addWidget(m_weight);
    packRow->addStretch();
    form->addRow(tr("Упаковка"), packRow);

    m_commodity = new QLineEdit(m_body);
    m_commodity->setPlaceholderText(tr("код ТН ВЭД, если нужен (10 цифр)"));
    form->addRow(tr("ТН ВЭД"), m_commodity);
    m_tags = new QLineEdit(m_body);
    m_tags->setPlaceholderText(tr("теги через «;»"));
    form->addRow(tr("Теги"), m_tags);
    m_adult = new QCheckBox(tr("Товар для взрослых (18+)"), m_body);
    form->addRow(QString(), m_adult);

    auto *catRow = new QHBoxLayout();
    m_categoryLabel = new QLabel(m_body);
    m_categoryLabel->setWordWrap(true);
    m_categoryBtn = new QPushButton(tr("Выбрать…"), m_body);
    m_categoryBtn->setAutoDefault(false);
    m_categoryClear = new QPushButton(tr("Убрать"), m_body);
    m_categoryClear->setAutoDefault(false);
    catRow->addWidget(m_categoryLabel, 1);
    catRow->addWidget(m_categoryBtn);
    catRow->addWidget(m_categoryClear);
    form->addRow(tr("Категория Маркета"), catRow);

    m_paramsHint = new QLabel(m_body);
    m_paramsHint->setObjectName("muted");
    m_paramsHint->setWordWrap(true);
    bl->addWidget(m_paramsHint);
    m_paramsLoad = new QPushButton(tr("Загрузить характеристики с Маркета"), m_body);
    m_paramsLoad->setAutoDefault(false);
    m_paramsLoad->hide();
    bl->addWidget(m_paramsLoad, 0, Qt::AlignLeft);
    m_paramForm = new ParamForm(this);
    m_paramsScroll = new QScrollArea(m_body);
    m_paramsScroll->setWidgetResizable(true);
    m_paramsScroll->setFrameShape(QFrame::NoFrame);
    m_paramsScroll->setWidget(m_paramForm);
    m_paramsScroll->setMinimumHeight(110);
    m_paramsScroll->setMaximumHeight(240);
    bl->addWidget(m_paramsScroll);
    root->addWidget(m_body);

    connect(m_toggle, &QToolButton::toggled, this, &ProductMarketBox::setExpanded);
    connect(m_vendorGuess, &QPushButton::clicked, this, &ProductMarketBox::guessVendor);
    connect(m_categoryBtn, &QPushButton::clicked, this, &ProductMarketBox::chooseCategory);
    connect(m_categoryClear, &QPushButton::clicked, this, &ProductMarketBox::clearCategory);
    connect(m_paramsLoad, &QPushButton::clicked, this, [this]() { rebuildParams(true); });
    setExpanded(false);
}

void ProductMarketBox::setExpanded(bool on)
{
    m_body->setVisible(on);
    m_summary->setVisible(!on);
    m_toggle->setText(on ? tr("▾ Параметры для Яндекс Маркета") : tr("▸ Параметры для Яндекс Маркета"));
    if (m_toggle->isChecked() != on) {
        const QSignalBlocker b(m_toggle);
        m_toggle->setChecked(on);
    }
    if (!on)
        updateSummary();
}

void ProductMarketBox::updateSummary()
{
    const QString s = ProductMarket::summary(info());
    m_summary->setText(s.isEmpty() ? tr("не заполнены (бренд, габариты, категория, характеристики - для карточек Маркета)") : s);
}

void ProductMarketBox::setInfo(const ProductMarketInfo &m)
{
    m_original = m;
    m_vendor->setText(m.vendor);
    m_countries->setText(m.countries.join(QStringLiteral("; ")));
    m_length->setValue(m.lengthCm);
    m_width->setValue(m.widthCm);
    m_height->setValue(m.heightCm);
    m_weight->setValue(m.weightKg);
    m_commodity->setText(m.commodityCode);
    m_tags->setText(m.tags.join(QStringLiteral("; ")));
    m_adult->setChecked(m.adult);
    m_categoryId = m.categoryId;
    m_categoryName = m.categoryName;
    m_categoryLabel->setText(m_categoryId > 0 ? (m_categoryName.isEmpty() ? QString::number(m_categoryId) : m_categoryName)
                                              : tr("не выбрана"));
    rebuildParams(false);
    setExpanded(!m.isEmpty());
    updateSummary();
}

ProductMarketInfo ProductMarketBox::info() const
{
    ProductMarketInfo m;
    m.vendor = m_vendor->text().trimmed();
    m.countries = MarketCards::splitList(m_countries->text());
    m.lengthCm = m_length->value();
    m.widthCm = m_width->value();
    m.heightCm = m_height->value();
    m.weightKg = m_weight->value();
    m.commodityCode = m_commodity->text().trimmed();
    m.tags = MarketCards::splitList(m_tags->text());
    m.adult = m_adult->isChecked();
    m.categoryId = m_categoryId;
    m.categoryName = m_categoryName;
    // Поля характеристик есть - берём значения из них; нет (категории нет, Маркет недоступен) - оставляем как было.
    if (m_categoryId <= 0)
        m.params.clear();
    else if (m_paramsShown)
        m.params = m_paramForm->values();
    else if (m_categoryId == m_original.categoryId)
        m.params = m_original.params;
    return m;
}

void ProductMarketBox::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    for (QLineEdit *e : {m_vendor, m_countries, m_commodity, m_tags})
        e->setReadOnly(readOnly);
    for (QDoubleSpinBox *s : {m_length, m_width, m_height, m_weight})
        s->setReadOnly(readOnly);
    m_adult->setEnabled(!readOnly);
    m_vendorGuess->setVisible(!readOnly);
    m_categoryBtn->setVisible(!readOnly);
    m_categoryClear->setVisible(!readOnly);
    m_paramForm->setEnabled(!readOnly);
}

void ProductMarketBox::guessVendor()
{
    const QString name = m_nameProvider ? m_nameProvider() : QString();
    const QString vendor = MarketCards::guessVendor(name);
    if (vendor.isEmpty()) {
        QMessageBox::information(this, tr("Бренд"), tr("По названию бренд определить не удалось - впишите его вручную."));
        return;
    }
    m_vendor->setText(vendor);
}

void ProductMarketBox::chooseCategory()
{
    const QList<MarketAccount> accounts = MarketAccount::loadAll();
    if (accounts.isEmpty()) {
        QMessageBox::information(this, tr("Категория Маркета"),
                                 tr("Чтобы выбрать категорию, подключите кабинет Яндекс Маркета (раздел «Яндекс Маркет»)."));
        return;
    }
    const MarketAccount &acc = accounts.first(); // дерево категорий общее для всех кабинетов
    QJsonObject tree;
    bool refresh = false;
    for (;;) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        MarketApi api(acc.apiKey);
        QString err;
        const bool ok = MarketCategories::loadTree(api, refresh, &tree, &err);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            QMessageBox::warning(this, tr("Категории Маркета"), tr("Не удалось получить список категорий:\n%1").arg(err));
            return;
        }
        CategoryPickerDialog dlg(tree, m_categoryId, this);
        if (dlg.exec() == QDialog::Accepted) {
            const bool changed = dlg.categoryId() != m_categoryId;
            if (changed) {
                m_categoryId = dlg.categoryId();
                m_categoryName = dlg.categoryPath();
                m_original.categoryId = m_categoryId;
                m_original.params.clear(); // характеристики прежней категории к новой не подходят
            }
            m_categoryLabel->setText(m_categoryName);
            if (changed || !m_paramsShown)
                rebuildParams(true);
            return;
        }
        if (!dlg.refreshRequested())
            return;
        refresh = true;
    }
}

void ProductMarketBox::clearCategory()
{
    m_categoryId = 0;
    m_categoryName.clear();
    m_categoryLabel->setText(tr("не выбрана"));
    m_original.params.clear();
    rebuildParams(false);
}

void ProductMarketBox::rebuildParams(bool allowNetwork)
{
    m_paramForm->clear();
    m_paramsShown = false;
    m_paramsScroll->hide();
    m_paramsLoad->hide();
    if (m_categoryId <= 0) {
        m_paramsHint->setText(tr("Выберите категорию - здесь появятся её характеристики (размер, цвет, материал и т. п.)."));
        m_paramsHint->show();
        return;
    }
    QList<MarketCategoryParam> params;
    QString err;
    bool ok = MarketCategories::cachedParams(m_categoryId, &params);
    if (!ok && !allowNetwork) {
        m_paramsHint->setText(m_readOnly ? tr("Характеристики категории не загружены. Сохранённые значения: %1.").arg(m_original.params.size())
                                         : tr("Характеристики категории не загружены (сохранённых значений: %1). Они не потеряются.")
                                               .arg(m_original.params.size()));
        m_paramsHint->show();
        m_paramsLoad->setVisible(!m_readOnly);
        return;
    }
    if (!ok && !m_readOnly) {
        const QList<MarketAccount> accounts = MarketAccount::loadAll();
        if (!accounts.isEmpty()) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            MarketApi api(accounts.first().apiKey);
            ok = MarketCategories::loadParams(api, accounts.first().businessId, m_categoryId, false, &params, &err);
            QApplication::restoreOverrideCursor();
        } else {
            err = tr("кабинет Маркета не подключён");
        }
    }
    if (!ok) {
        m_paramsHint->setText(tr("Характеристики категории сейчас недоступны (%1). Сохранённые значения не потеряются.")
                                  .arg(err.isEmpty() ? tr("нет в кэше") : err));
        m_paramsHint->show();
        m_paramsLoad->setVisible(!m_readOnly);
        return;
    }
    if (params.isEmpty()) {
        m_paramsHint->setText(tr("У этой категории нет отдельных характеристик."));
        m_paramsHint->show();
        return;
    }
    m_paramsHint->setText(tr("Характеристики категории (обязательные отмечены *). Из них собираются карточки Маркета."));
    m_paramsHint->show();
    m_paramForm->setParams(params, m_original.params);
    m_paramForm->setEnabled(!m_readOnly);
    m_paramsShown = true;
    m_paramsScroll->show();
}
