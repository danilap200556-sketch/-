#include "cardstab.h"
#include "applog.h"
#include "categorypicker.h"
#include "marketapi.h"
#include "marketcategories.h"
#include "paramform.h"
#include "productpickdialog.h"
#include "theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSqlQuery>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

enum Col { ColOffer = 0, ColName, ColCategory, ColState, ColCount };

QString stateText(const MarketCard &c)
{
    if (c.status == QLatin1String("sent"))
        return QObject::tr("отправлена");
    if (c.status == QLatin1String("error"))
        return QObject::tr("ошибка");
    return QObject::tr("черновик");
}

QDoubleSpinBox *spin(QWidget *parent, double max, int decimals, const QString &suffix)
{
    auto *s = new QDoubleSpinBox(parent);
    s->setRange(0, max);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setGroupSeparatorShown(true);
    return s;
}

} // namespace

CardsTab::CardsTab(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    // ---------------- слева: список карточек
    auto *left = new QWidget(splitter);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 8, 0);
    auto *accRow = new QHBoxLayout();
    accRow->addWidget(new QLabel(tr("Кабинет:"), left));
    m_accountCombo = new QComboBox(left);
    accRow->addWidget(m_accountCombo, 1);
    ll->addLayout(accRow);
    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(tr("Найти: артикул, название, бренд или штрихкод"));
    m_search->setClearButtonEnabled(true);
    ll->addWidget(m_search);

    m_table = new QTableWidget(0, ColCount, left);
    m_table->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("Категория"), tr("Состояние")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(ColOffer, 120);
    m_table->setColumnWidth(ColName, 200);
    m_table->setColumnWidth(ColCategory, 110);
    ll->addWidget(m_table, 1);

    auto *b1 = new QHBoxLayout();
    auto *addBtn = new QPushButton(tr("Добавить из товаров…"), left);
    auto *blankBtn = new QPushButton(tr("По артикулу…"), left);
    auto *delBtn = new QPushButton(tr("Удалить"), left);
    b1->addWidget(addBtn);
    b1->addWidget(blankBtn);
    b1->addWidget(delBtn);
    ll->addLayout(b1);
    auto *b2 = new QHBoxLayout();
    auto *refillBtn = new QPushButton(tr("Обновить из базы…"), left);
    auto *pullBtn = new QPushButton(tr("Загрузить с Маркета"), left);
    auto *checkBtn = new QPushButton(tr("Состояние на Маркете"), left);
    b2->addWidget(refillBtn);
    b2->addWidget(pullBtn);
    b2->addWidget(checkBtn);
    ll->addLayout(b2);
    auto *toProductBtn = new QPushButton(tr("Записать в товары…"), left);
    toProductBtn->setToolTip(tr("Сохранить бренд, страну, габариты, ТН ВЭД, теги, категорию и характеристики из выбранных карточек в товары базы"));
    ll->addWidget(toProductBtn);
    auto *sendBtn = new QPushButton(tr("Отправить на Маркет"), left);
    sendBtn->setProperty("primary", true);
    ll->addWidget(sendBtn);
    m_summary = new QLabel(left);
    m_summary->setObjectName("muted");
    ll->addWidget(m_summary);
    m_status = new QLabel(left);
    m_status->setWordWrap(true);
    ll->addWidget(m_status);
    splitter->addWidget(left);

    // ---------------- справа: форма карточки
    auto *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_formPanel = new QWidget(scroll);
    scroll->setWidget(m_formPanel);
    auto *fl = new QVBoxLayout(m_formPanel);
    fl->setContentsMargins(8, 4, 8, 8);

    m_cardState = new QLabel(m_formPanel);
    m_cardState->setWordWrap(true);
    fl->addWidget(m_cardState);

    auto *basic = new QGroupBox(tr("Основное"), m_formPanel);
    auto *bf = new QFormLayout(basic);
    m_offerId = new QLineEdit(basic);
    m_offerId->setToolTip(tr("Ваш артикул товара на Маркете (SKU у продавца, offerId). У каждого товара он свой и "
                             "после первой отправки не меняется: другой артикул - это уже другой товар."));
    bf->addRow(tr("Артикул на Маркете *"), m_offerId);
    m_name = new QLineEdit(basic);
    m_name->setToolTip(tr("Тип товара + бренд + модель + особенности (цвет, размер). До 256 символов."));
    bf->addRow(tr("Название *"), m_name);
    m_vendor = new QLineEdit(basic);
    bf->addRow(tr("Бренд *"), m_vendor);
    m_vendorCode = new QLineEdit(basic);
    m_vendorCode->setToolTip(tr("Артикул производителя (по умолчанию - наш артикул)."));
    bf->addRow(tr("Код производителя"), m_vendorCode);
    m_description = new QPlainTextEdit(basic);
    m_description->setFixedHeight(110);
    bf->addRow(tr("Описание *"), m_description);
    m_barcodes = new QLineEdit(basic);
    m_barcodes->setPlaceholderText(tr("через «;»"));
    bf->addRow(tr("Штрихкоды"), m_barcodes);
    m_price = spin(basic, 10'000'000, 2, tr(" ₽"));
    bf->addRow(tr("Цена"), m_price);
    m_countries = new QLineEdit(basic);
    m_countries->setPlaceholderText(tr("например, Китай; Вьетнам"));
    bf->addRow(tr("Страна производства"), m_countries);
    fl->addWidget(basic);

    auto *cat = new QGroupBox(tr("Категория и характеристики"), m_formPanel);
    auto *cl = new QVBoxLayout(cat);
    auto *catRow = new QHBoxLayout();
    m_categoryLabel = new QLabel(cat);
    m_categoryLabel->setWordWrap(true);
    catRow->addWidget(m_categoryLabel, 1);
    auto *catBtn = new QPushButton(tr("Выбрать категорию…"), cat);
    catRow->addWidget(catBtn);
    cl->addLayout(catRow);
    m_paramsBox = new QGroupBox(tr("Характеристики категории (обязательные отмечены *)"), cat);
    auto *pl = new QVBoxLayout(m_paramsBox);
    m_paramsHint = new QLabel(m_paramsBox);
    m_paramsHint->setWordWrap(true);
    m_paramsHint->setObjectName("muted");
    pl->addWidget(m_paramsHint);
    m_paramForm = new ParamForm(m_paramsBox);
    pl->addWidget(m_paramForm);
    connect(m_paramForm, &ParamForm::changed, this, [this]() { markDirty(); });
    cl->addWidget(m_paramsBox);
    fl->addWidget(cat);

    auto *media = new QGroupBox(tr("Фото и видео"), m_formPanel);
    auto *mf = new QFormLayout(media);
    m_pictures = new QPlainTextEdit(media);
    m_pictures->setFixedHeight(90);
    m_pictures->setPlaceholderText(tr("по одной ссылке в строке; первая - главное фото"));
    mf->addRow(tr("Фото (ссылки) *"), m_pictures);
    auto *photosBtn = new QPushButton(tr("Ссылки на фото из базы"), media);
    photosBtn->setToolTip(tr("Берёт фото товара из базы и даёт Маркету ссылки на них через ваш сайт "
                             "(адрес сайта - в меню «Сайт»)."));
    mf->addRow(QString(), photosBtn);
    m_videos = new QPlainTextEdit(media);
    m_videos->setFixedHeight(50);
    m_videos->setPlaceholderText(tr("ссылки на видео, по одной в строке"));
    mf->addRow(tr("Видео (ссылки)"), m_videos);
    fl->addWidget(media);

    auto *dims = new QGroupBox(tr("Упаковка"), m_formPanel);
    auto *df = new QFormLayout(dims);
    m_length = spin(dims, 10'000, 1, tr(" см"));
    m_width = spin(dims, 10'000, 1, tr(" см"));
    m_height = spin(dims, 10'000, 1, tr(" см"));
    m_weight = spin(dims, 100'000, 3, tr(" кг"));
    df->addRow(tr("Длина"), m_length);
    df->addRow(tr("Ширина"), m_width);
    df->addRow(tr("Высота"), m_height);
    df->addRow(tr("Вес брутто"), m_weight);
    auto *dimHint = new QLabel(tr("Габариты и вес передаются, только когда заполнены все четыре."), dims);
    dimHint->setObjectName("muted");
    df->addRow(dimHint);
    fl->addWidget(dims);

    auto *more = new QGroupBox(tr("Прочее"), m_formPanel);
    auto *rf = new QFormLayout(more);
    m_commodity = new QLineEdit(more);
    m_commodity->setPlaceholderText(tr("код ТН ВЭД, 10 цифр"));
    rf->addRow(tr("ТН ВЭД"), m_commodity);
    m_tags = new QLineEdit(more);
    m_tags->setPlaceholderText(tr("через «;»"));
    rf->addRow(tr("Теги"), m_tags);
    m_adult = new QCheckBox(tr("товар для взрослых (18+)"), more);
    rf->addRow(QString(), m_adult);
    fl->addWidget(more);

    auto *actions = new QHBoxLayout();
    auto *saveBtn = new QPushButton(tr("Сохранить"), m_formPanel);
    saveBtn->setProperty("primary", true);
    m_pullProductBtn = new QPushButton(tr("Подставить из базы…"), m_formPanel);
    m_pullMarketBtn = new QPushButton(tr("Загрузить с Маркета"), m_formPanel);
    actions->addWidget(saveBtn);
    actions->addWidget(m_pullProductBtn);
    actions->addWidget(m_pullMarketBtn);
    actions->addStretch(1);
    fl->addLayout(actions);
    fl->addStretch(1);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({520, 700});

    // изменения полей
    for (QLineEdit *e : {m_offerId, m_name, m_vendor, m_vendorCode, m_barcodes, m_countries, m_commodity, m_tags})
        connect(e, &QLineEdit::textChanged, this, [this]() { markDirty(); });
    for (QPlainTextEdit *e : {m_description, m_pictures, m_videos})
        connect(e, &QPlainTextEdit::textChanged, this, [this]() { markDirty(); });
    for (QDoubleSpinBox *s : {m_price, m_length, m_width, m_height, m_weight})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this]() { markDirty(); });
    connect(m_adult, &QCheckBox::toggled, this, [this]() { markDirty(); });

    connect(m_accountCombo, &QComboBox::currentIndexChanged, this, &CardsTab::accountChanged);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &CardsTab::currentRowChanged);
    connect(m_search, &QLineEdit::textChanged, this, &CardsTab::applyFilter);
    connect(addBtn, &QPushButton::clicked, this, &CardsTab::addFromProducts);
    connect(blankBtn, &QPushButton::clicked, this, &CardsTab::addBlank);
    connect(delBtn, &QPushButton::clicked, this, &CardsTab::removeSelected);
    connect(refillBtn, &QPushButton::clicked, this, &CardsTab::refillFromDatabase);
    connect(m_pullProductBtn, &QPushButton::clicked, this, &CardsTab::refillFromDatabase);
    connect(toProductBtn, &QPushButton::clicked, this, &CardsTab::writeToProducts);
    connect(pullBtn, &QPushButton::clicked, this, &CardsTab::pullFromMarket);
    connect(m_pullMarketBtn, &QPushButton::clicked, this, &CardsTab::pullFromMarket);
    connect(checkBtn, &QPushButton::clicked, this, &CardsTab::checkOnMarket);
    connect(sendBtn, &QPushButton::clicked, this, &CardsTab::sendToMarket);
    connect(catBtn, &QPushButton::clicked, this, &CardsTab::chooseCategory);
    connect(photosBtn, &QPushButton::clicked, this, &CardsTab::photoLinksFromDatabase);
    connect(saveBtn, &QPushButton::clicked, this, &CardsTab::saveCurrent);

    setFormEnabled(false);
}

// ---------------------------------------------------------------------------
// состояние и вспомогательное

void CardsTab::setFormEnabled(bool on)
{
    m_formPanel->setEnabled(on);
    if (!on) {
        m_loading = true;
        for (QLineEdit *e : {m_offerId, m_name, m_vendor, m_vendorCode, m_barcodes, m_countries, m_commodity, m_tags})
            e->clear();
        for (QPlainTextEdit *e : {m_description, m_pictures, m_videos})
            e->clear();
        for (QDoubleSpinBox *s : {m_price, m_length, m_width, m_height, m_weight})
            s->setValue(0);
        m_adult->setChecked(false);
        m_categoryLabel->setText(QString());
        m_cardState->setText(tr("Выберите карточку слева или добавьте новую: «Добавить из товаров…» создаст карточки "
                                "для товаров из базы и сразу подставит их данные."));
        m_paramForm->clear();
        m_paramsHint->setText(QString());
        m_loading = false;
    }
}

bool CardsTab::currentAccount(MarketAccount *acc) const
{
    const int i = m_accountCombo->currentIndex();
    if (i < 0 || i >= m_accounts.size())
        return false;
    *acc = m_accounts[i];
    return true;
}

int CardsTab::currentCardIndex() const
{
    const auto rows = m_table->selectionModel()->selectedRows();
    return rows.size() == 1 ? m_table->item(rows.first().row(), ColOffer)->data(Qt::UserRole).toInt() : -1;
}

QList<int> CardsTab::selectedCardIndexes() const
{
    QList<int> list;
    for (const QModelIndex &i : m_table->selectionModel()->selectedRows())
        list << m_table->item(i.row(), ColOffer)->data(Qt::UserRole).toInt();
    return list;
}

QString CardsTab::siteUrl()
{
    QSettings st;
    QString url = st.value("siteUrl").toString().trimmed();
    if (!url.isEmpty())
        return url;
    bool ok = false;
    url = QInputDialog::getText(this, tr("Адрес сайта"),
                                tr("Маркет скачивает фото по ссылке, поэтому фото отдаются с вашего сайта. Введите адрес сайта "
                                   "(например, https://sklad.example.ru). Если сайта нет, оставьте пустым: фото можно будет "
                                   "вписать ссылками вручную."),
                                QLineEdit::Normal, QString(), &ok).trimmed();
    if (ok && !url.isEmpty()) {
        const QUrl parsed(url);
        if (parsed.isValid() && (parsed.scheme() == "https" || parsed.scheme() == "http") && !parsed.host().isEmpty())
            st.setValue("siteUrl", url);
        else {
            QMessageBox::warning(this, tr("Неверный адрес"), tr("Адрес должен начинаться с https:// (или http://)."));
            return QString();
        }
    }
    return url;
}

void CardsTab::setStatus(const QString &text, bool error)
{
    m_status->setStyleSheet(error ? QStringLiteral("color: %1;").arg(Theme::colors().danger.name()) : QString());
    m_status->setText(text);
}

QHash<qint64, QList<MarketCategoryParam>> CardsTab::knownParams() const
{
    QHash<qint64, QList<MarketCategoryParam>> map;
    for (const MarketCard &c : m_cards) {
        if (c.categoryId > 0 && !map.contains(c.categoryId)) {
            QList<MarketCategoryParam> p;
            if (MarketCategories::cachedParams(c.categoryId, &p))
                map.insert(c.categoryId, p);
        }
    }
    return map;
}

// ---------------------------------------------------------------------------
// список

void CardsTab::refresh()
{
    if (!commitCurrent())
        return; // открытая карточка не сохранилась (пользователь увидел почему) - список не трогаем
    const int prev = m_accountCombo->currentData().toInt();
    m_accounts = MarketAccount::loadAll();
    {
        const QSignalBlocker b(m_accountCombo);
        m_accountCombo->clear();
        int select = 0;
        for (int i = 0; i < m_accounts.size(); ++i) {
            m_accountCombo->addItem(m_accounts[i].name, m_accounts[i].id);
            if (m_accounts[i].id == prev)
                select = i;
        }
        if (m_accountCombo->count() > 0)
            m_accountCombo->setCurrentIndex(select);
    }
    const int keep = m_shown >= 0 && m_shown < m_cards.size() ? m_cards[m_shown].id : 0;
    reloadCards(keep);
    if (m_accounts.isEmpty())
        setStatus(tr("Кабинетов Маркета пока нет: добавьте кабинет на вкладке «Яндекс Маркет»."));
}

void CardsTab::accountChanged()
{
    commitCurrent();
    reloadCards(0);
}

void CardsTab::reloadCards(int selectId)
{
    MarketAccount acc;
    m_loading = true;
    m_shown = -1;
    m_cards.clear();
    if (currentAccount(&acc)) {
        QString err;
        m_cards = MarketCards::loadAll(acc.id, &err);
        if (!err.isEmpty())
            setStatus(tr("Не удалось прочитать карточки: %1").arg(err), true);
    }
    fillTable();
    m_loading = false;
    int row = -1;
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (selectId > 0 && m_cards[m_table->item(r, ColOffer)->data(Qt::UserRole).toInt()].id == selectId)
            row = r;
    if (row < 0 && m_table->rowCount() > 0 && selectId == 0)
        row = 0;
    if (row >= 0)
        m_table->selectRow(row);
    else
        currentRowChanged();
}

void CardsTab::fillTable()
{
    const QSignalBlocker b(m_table);
    m_table->setRowCount(0);
    for (int i = 0; i < m_cards.size(); ++i) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        for (int c = 0; c < ColCount; ++c)
            m_table->setItem(row, c, new QTableWidgetItem());
        m_table->item(row, ColOffer)->setData(Qt::UserRole, i);
        updateRow(i);
    }
    applyFilter();
    int sent = 0, err = 0, draft = 0;
    for (const MarketCard &c : std::as_const(m_cards)) {
        if (c.status == QLatin1String("sent")) ++sent;
        else if (c.status == QLatin1String("error")) ++err;
        else ++draft;
    }
    m_summary->setText(tr("Карточек: %1 · отправлено %2 · черновиков %3 · с ошибкой %4")
                           .arg(m_cards.size()).arg(sent).arg(draft).arg(err));
}

void CardsTab::updateRow(int index)
{
    for (int r = 0; r < m_table->rowCount(); ++r) {
        if (m_table->item(r, ColOffer)->data(Qt::UserRole).toInt() != index)
            continue;
        const MarketCard &c = m_cards[index];
        m_table->item(r, ColOffer)->setText(c.offerId);
        m_table->item(r, ColName)->setText(c.name);
        m_table->item(r, ColCategory)->setText(c.categoryName.section(QStringLiteral(" › "), -1));
        auto *st = m_table->item(r, ColState);
        st->setText(stateText(c));
        const auto &colors = Theme::colors();
        st->setForeground(c.status == QLatin1String("sent") ? colors.ok : c.status == QLatin1String("error") ? colors.danger : colors.muted);
        QStringList tip;
        if (!c.marketStatus.isEmpty())
            tip << tr("На Маркете: %1").arg(c.marketStatus);
        if (!c.lastResult.isEmpty())
            tip << c.lastResult;
        for (int col = 0; col < ColCount; ++col)
            m_table->item(r, col)->setToolTip(tip.join(QLatin1Char('\n')));
        return;
    }
}

void CardsTab::applyFilter()
{
    const QString text = m_search->text().trimmed();
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const MarketCard &c = m_cards[m_table->item(r, ColOffer)->data(Qt::UserRole).toInt()];
        const bool show = text.isEmpty() || c.offerId.contains(text, Qt::CaseInsensitive) || c.name.contains(text, Qt::CaseInsensitive) ||
                          c.vendor.contains(text, Qt::CaseInsensitive) || c.barcodes.join(' ').contains(text);
        m_table->setRowHidden(r, !show);
    }
}

void CardsTab::currentRowChanged()
{
    if (m_loading)
        return;
    if (!commitCurrent()) {
        // не сохранилась - остаёмся на той же карточке
        for (int r = 0; r < m_table->rowCount(); ++r) {
            if (m_table->item(r, ColOffer)->data(Qt::UserRole).toInt() == m_shown) {
                const QSignalBlocker b(m_table);
                m_table->selectRow(r);
            }
        }
        return;
    }
    const int index = currentCardIndex();
    if (index < 0) {
        m_shown = -1;
        setFormEnabled(false);
        if (selectedCardIndexes().size() > 1)
            m_cardState->setText(tr("Выбрано карточек: %1. Кнопки слева применяются ко всем выбранным.").arg(selectedCardIndexes().size()));
        return;
    }
    showCard(index);
}

// ---------------------------------------------------------------------------
// форма

void CardsTab::showCard(int index)
{
    if (index < 0 || index >= m_cards.size())
        return;
    const MarketCard &c = m_cards[index];
    m_shown = index;
    m_loading = true;
    m_formPanel->setEnabled(true);
    m_offerId->setText(c.offerId);
    m_name->setText(c.name);
    m_vendor->setText(c.vendor);
    m_vendorCode->setText(c.vendorCode);
    m_description->setPlainText(c.description);
    m_barcodes->setText(c.barcodes.join(QStringLiteral("; ")));
    m_price->setValue(c.price);
    m_countries->setText(c.countries.join(QStringLiteral("; ")));
    m_pictures->setPlainText(c.pictures.join(QLatin1Char('\n')));
    m_videos->setPlainText(c.videos.join(QLatin1Char('\n')));
    m_length->setValue(c.lengthCm);
    m_width->setValue(c.widthCm);
    m_height->setValue(c.heightCm);
    m_weight->setValue(c.weightKg);
    m_commodity->setText(c.commodityCode);
    m_tags->setText(c.tags.join(QStringLiteral("; ")));
    m_adult->setChecked(c.adult);
    m_categoryLabel->setText(c.categoryId > 0 ? (c.categoryName.isEmpty() ? tr("Категория №%1").arg(c.categoryId) : c.categoryName)
                                              : tr("Категория не выбрана"));
    QStringList state;
    state << tr("Состояние: <b>%1</b>").arg(stateText(c).toHtmlEscaped());
    if (!c.marketStatus.isEmpty())
        state << tr("на Маркете: %1").arg(c.marketStatus.toHtmlEscaped());
    if (c.marketSku > 0)
        state << tr("SKU на Маркете: %1").arg(c.marketSku);
    QString text = state.join(QStringLiteral(" · "));
    if (!c.lastResult.isEmpty())
        text += QStringLiteral("<br>") + c.lastResult.toHtmlEscaped();
    m_cardState->setTextFormat(Qt::RichText);
    m_cardState->setText(text);
    rebuildParams(c);
    m_loading = false;
    m_dirty = false;
}

void CardsTab::readForm(MarketCard *card) const
{
    card->offerId = m_offerId->text().trimmed();
    card->name = m_name->text().trimmed();
    card->vendor = m_vendor->text().trimmed();
    card->vendorCode = m_vendorCode->text().trimmed();
    card->description = m_description->toPlainText().trimmed();
    card->barcodes = MarketCards::splitList(m_barcodes->text());
    card->price = m_price->value();
    card->countries = MarketCards::splitList(m_countries->text());
    card->pictures = MarketCards::splitLines(m_pictures->toPlainText());
    card->videos = MarketCards::splitLines(m_videos->toPlainText());
    card->lengthCm = m_length->value();
    card->widthCm = m_width->value();
    card->heightCm = m_height->value();
    card->weightKg = m_weight->value();
    card->commodityCode = m_commodity->text().trimmed();
    card->tags = MarketCards::splitList(m_tags->text());
    card->adult = m_adult->isChecked();
    card->params = m_paramForm->values();
}

bool CardsTab::commitCurrent()
{
    if (m_shown < 0 || m_shown >= m_cards.size() || !m_dirty)
        return true;
    MarketCard edited = m_cards[m_shown];
    readForm(&edited);
    if (edited.offerId != m_cards[m_shown].offerId) {
        // другой артикул - это для Маркета уже другой товар
        edited.status = QStringLiteral("draft");
        edited.marketSku = 0;
        edited.marketStatus.clear();
    }
    // правка отправленной карточки - снова черновик, пока не отправлена
    if (edited.status != QLatin1String("draft"))
        edited.status = QStringLiteral("draft");
    QString err;
    if (!MarketCards::save(&edited, &err)) {
        QMessageBox::warning(this, tr("Карточка не сохранена"), err);
        return false;
    }
    m_cards[m_shown] = edited;
    m_dirty = false;
    updateRow(m_shown);
    return true;
}

void CardsTab::saveCurrent()
{
    if (m_shown < 0)
        return;
    if (commitCurrent())
        setStatus(tr("Карточка «%1» сохранена.").arg(m_cards[m_shown].offerId));
}

// ---------------------------------------------------------------------------
// категория и её характеристики

void CardsTab::chooseCategory()
{
    if (m_shown < 0)
        return;
    MarketAccount acc;
    if (!currentAccount(&acc))
        return;
    bool refresh = false;
    for (;;) {
        if (m_tree.isEmpty() || refresh) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            MarketApi api(acc.apiKey);
            QString err;
            const bool ok = MarketCategories::loadTree(api, refresh, &m_tree, &err);
            QApplication::restoreOverrideCursor();
            if (!ok) {
                QMessageBox::warning(this, tr("Категории Маркета"), tr("Не удалось получить список категорий:\n%1").arg(err));
                return;
            }
        }
        CategoryPickerDialog dlg(m_tree, m_cards[m_shown].categoryId, this);
        if (dlg.exec() == QDialog::Accepted) {
            MarketCard &c = m_cards[m_shown];
            readForm(&c); // введённое не теряем
            c.categoryId = dlg.categoryId();
            c.categoryName = dlg.categoryPath();
            m_categoryLabel->setText(c.categoryName);
            c.params.clear(); // характеристики прежней категории к новой не подходят
            rebuildParams(c);
            m_dirty = true;
            commitCurrent();
            return;
        }
        if (!dlg.refreshRequested())
            return;
        refresh = true;
    }
}

void CardsTab::rebuildParams(const MarketCard &card, bool forceReload)
{
    m_paramForm->clear();
    m_params.clear();
    if (card.categoryId <= 0) {
        m_paramsHint->setText(tr("Выберите категорию - здесь появятся её характеристики (размер, цвет, материал и т. п.)."));
        m_paramsHint->show();
        return;
    }
    MarketAccount acc;
    QString err;
    bool ok = !forceReload && MarketCategories::cachedParams(card.categoryId, &m_params);
    if (!ok && currentAccount(&acc)) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        MarketApi api(acc.apiKey);
        ok = MarketCategories::loadParams(api, acc.businessId, card.categoryId, forceReload, &m_params, &err);
        QApplication::restoreOverrideCursor();
    }
    if (!ok) {
        m_paramsHint->setText(tr("Не удалось получить характеристики категории: %1").arg(err));
        m_paramsHint->show();
        return;
    }
    if (m_params.isEmpty()) {
        m_paramsHint->setText(tr("У этой категории нет отдельных характеристик."));
        m_paramsHint->show();
        return;
    }
    m_paramsHint->hide();
    m_paramForm->setParams(m_params, card.params);
}

void CardsTab::photoLinksFromDatabase()
{
    if (m_shown < 0)
        return;
    MarketCard &c = m_cards[m_shown];
    if (c.productId <= 0) {
        QMessageBox::information(this, tr("Фото"), tr("Карточка не связана с товаром в базе - ссылки на фото вписываются вручную."));
        return;
    }
    const QString url = siteUrl();
    QString why;
    const QStringList links = MarketCards::publicPhotoLinks(c.productId, url, &why);
    if (!why.isEmpty()) {
        QMessageBox::warning(this, tr("Фото"), tr("Не удалось получить ссылки: %1").arg(why));
        return;
    }
    if (links.isEmpty()) {
        QMessageBox::information(this, tr("Фото"), tr("У товара нет фото в базе. Добавьте их в карточке товара на вкладке «Товары»."));
        return;
    }
    m_pictures->setPlainText(links.join(QLatin1Char('\n')));
    setStatus(tr("Ссылок на фото: %1. Сайт должен быть обновлён до версии с адресами /pub/photo/…, иначе Маркет их не скачает.").arg(links.size()));
}

// ---------------------------------------------------------------------------
// действия со списком

void CardsTab::addFromProducts()
{
    MarketAccount acc;
    if (!currentAccount(&acc)) {
        QMessageBox::information(this, tr("Нет кабинета"), tr("Сначала добавьте кабинет Маркета на вкладке «Яндекс Маркет»."));
        return;
    }
    if (!commitCurrent())
        return;
    QSet<QString> existing;
    for (const MarketCard &c : std::as_const(m_cards))
        existing.insert(c.offerId);
    ProductPickDialog dlg(existing, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QList<int> ids = dlg.selectedProductIds();
    if (ids.isEmpty())
        return;
    const QString url = siteUrl();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList skipped, notes;
    QString err;
    const QList<MarketCard> made = MarketCards::createFromProducts(acc.id, ids, url, &skipped, &notes, &err);
    QApplication::restoreOverrideCursor();
    if (!err.isEmpty()) {
        QMessageBox::warning(this, tr("Карточки"), tr("Не удалось создать карточки:\n%1").arg(err));
        return;
    }
    reloadCards(made.isEmpty() ? 0 : made.first().id);
    QString text = tr("Создано карточек: %1.").arg(made.size());
    if (!skipped.isEmpty())
        text += tr(" Уже были: %1.").arg(skipped.size());
    setStatus(text);
    if (!notes.isEmpty() || !skipped.isEmpty()) {
        QMessageBox box(QMessageBox::Information, tr("Карточки созданы"), text, QMessageBox::Ok, this);
        box.setInformativeText(tr("Данные подставлены из базы. Осталось выбрать категорию и заполнить то, чего нет в базе "
                                  "(бренд, характеристики, габариты)."));
        QStringList detail = notes;
        if (!skipped.isEmpty())
            detail << tr("Уже есть карточки для: %1").arg(skipped.join(QStringLiteral(", ")));
        box.setDetailedText(detail.join(QLatin1Char('\n')));
        box.exec();
    }
}

void CardsTab::addBlank()
{
    MarketAccount acc;
    if (!currentAccount(&acc))
        return;
    if (!commitCurrent())
        return;
    bool ok = false;
    const QString offer = QInputDialog::getText(this, tr("Новая карточка"),
                                                tr("Артикул на Маркете (offerId). Если такой товар есть в базе (по артикулу или артикулу на Маркете), "
                                                   "данные подставятся из него:"),
                                                QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || offer.isEmpty())
        return;
    MarketCard c;
    c.accountId = acc.id;
    c.offerId = offer;
    QSqlQuery q;
    q.prepare("SELECT id FROM products WHERE sku = ? OR market_sku = ? ORDER BY (market_sku = ?) DESC LIMIT 1");
    q.addBindValue(offer);
    q.addBindValue(offer);
    q.addBindValue(offer);
    QString note;
    if (q.exec() && q.next())
        MarketCards::fillFromProduct(&c, q.value(0).toInt(), MarketCards::FAll, siteUrl(), &note);
    QString err;
    if (!MarketCards::save(&c, &err)) {
        QMessageBox::warning(this, tr("Карточка не создана"), err);
        return;
    }
    reloadCards(c.id);
    setStatus(c.productId > 0 ? tr("Карточка создана, данные взяты из товара в базе.") : tr("Карточка создана: заполните поля справа."));
}

void CardsTab::removeSelected()
{
    const QList<int> sel = selectedCardIndexes();
    if (sel.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Удалить карточки"),
                              tr("Удалить карточки из программы: %1 шт.? На Маркете товары останутся как есть.").arg(sel.size())) != QMessageBox::Yes)
        return;
    QList<int> ids;
    for (int i : sel)
        ids << m_cards[i].id;
    QString err;
    m_dirty = false;
    if (!MarketCards::remove(ids, &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    reloadCards(0);
    setStatus(tr("Удалено карточек: %1.").arg(ids.size()));
}

void CardsTab::refillFromDatabase()
{
    if (!commitCurrent())
        return;
    QList<int> sel = selectedCardIndexes();
    if (sel.isEmpty()) {
        QMessageBox::information(this, tr("Выберите карточки"), tr("Выделите карточки в списке слева (Ctrl или Shift + клик, Ctrl+A - все)."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Подставить из базы"));
    auto *l = new QVBoxLayout(&dlg);
    l->addWidget(new QLabel(tr("Что взять из товара в базе (карточек: %1):").arg(sel.size()), &dlg));
    struct Item { QString text; int flag; bool on; };
    const QList<Item> items = {
        {tr("Название"), MarketCards::FName, false}, {tr("Описание"), MarketCards::FDescription, false},
        {tr("Штрихкоды"), MarketCards::FBarcodes, true}, {tr("Цену"), MarketCards::FPrice, true},
        {tr("Ссылки на фото (через сайт)"), MarketCards::FPhotos, true},
        {tr("Бренд (из товара; если там пусто - по названию, только если в карточке пусто) и код производителя"), MarketCards::FVendor, true},
        {tr("Габариты и вес упаковки (если заданы у товара)"), MarketCards::FPack, true},
        {tr("Категорию и характеристики (если заданы у товара)"), MarketCards::FCategory, true},
        {tr("Страну, ТН ВЭД, теги и 18+ (то, что заполнено у товара)"), MarketCards::FDetails, true}};
    QList<QCheckBox *> boxes;
    for (const Item &it : items) {
        auto *cb = new QCheckBox(it.text, &dlg);
        cb->setChecked(it.on);
        l->addWidget(cb);
        boxes << cb;
    }
    l->addWidget(new QLabel(tr("Выбранное в карточках будет заменено данными из базы."), &dlg));
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    l->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    int fields = 0;
    for (int i = 0; i < items.size(); ++i)
        if (boxes[i]->isChecked())
            fields |= items[i].flag;
    if (!fields)
        return;
    const QString url = (fields & MarketCards::FPhotos) ? siteUrl() : QString();
    QList<MarketCard *> ptrs;
    for (int i : sel)
        ptrs << &m_cards[i];
    QStringList notes;
    QString err;
    const int keep = m_shown >= 0 ? m_cards[m_shown].id : 0;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = MarketCards::refillFromProducts(ptrs, fields, url, &notes, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Карточки"), tr("Не удалось сохранить:\n%1").arg(err));
        return;
    }
    reloadCards(keep ? keep : m_cards[sel.first()].id);
    setStatus(tr("Данные из базы подставлены в карточки: %1.").arg(sel.size()));
    if (!notes.isEmpty()) {
        QMessageBox box(QMessageBox::Information, tr("Подставлено из базы"), tr("Готово, но есть замечания (%1).").arg(notes.size()), QMessageBox::Ok, this);
        box.setDetailedText(notes.join(QLatin1Char('\n')));
        box.exec();
    }
}

void CardsTab::writeToProducts()
{
    if (!commitCurrent())
        return;
    const QList<int> sel = selectedCardIndexes();
    if (sel.isEmpty()) {
        QMessageBox::information(this, tr("Выберите карточки"), tr("Выделите карточки в списке слева (Ctrl или Shift + клик, Ctrl+A - все)."));
        return;
    }
    if (QMessageBox::question(this, tr("Записать в товары"),
                              tr("Записать в товары базы данные из карточек (%1 шт.): бренд, страну, габариты и вес, ТН ВЭД, теги, 18+, "
                                 "категорию и характеристики?\n\nЗаполненное в карточке заменит то же у товара, пустое у товара ничего не "
                                 "сотрёт. Название, описание, цена и фото товара не меняются. Потом эти данные можно подставлять в новые "
                                 "карточки и выгружать в Excel.").arg(sel.size())) != QMessageBox::Yes)
        return;
    QList<const MarketCard *> cards;
    for (int i : sel)
        cards << &m_cards[i];
    int written = 0;
    QStringList notes;
    QString err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = MarketCards::writeToProducts(cards, &written, &notes, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Карточки"), tr("Не удалось записать в товары:\n%1").arg(err));
        return;
    }
    setStatus(tr("Записано в товары: %1.").arg(written));
    if (!notes.isEmpty()) {
        QMessageBox box(QMessageBox::Information, tr("Записано в товары"), tr("Готово (%1), но есть замечания (%2).").arg(written).arg(notes.size()), QMessageBox::Ok, this);
        box.setDetailedText(notes.join(QLatin1Char('\n')));
        box.exec();
    }
}

void CardsTab::pullFromMarket()
{
    MarketAccount acc;
    if (!currentAccount(&acc) || !commitCurrent())
        return;
    QList<int> sel = selectedCardIndexes();
    if (sel.isEmpty())
        for (int i = 0; i < m_cards.size(); ++i)
            sel << i;
    if (sel.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Загрузить с Маркета"),
                              tr("Взять с Маркета данные карточек (%1 шт.): название, описание, бренд, фото, габариты, характеристики и т. д.?\n"
                                 "Поля, которые Маркет вернёт, заменят ваши; остальные останутся как есть.").arg(sel.size())) != QMessageBox::Yes)
        return;
    QStringList offerIds;
    for (int i : sel)
        offerIds << m_cards[i].offerId;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    MarketApi api(acc.apiKey);
    QHash<QString, MarketOfferInfo> info;
    QString err;
    const bool ok = api.offerInfo(acc, offerIds, &info, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        setStatus(tr("Не удалось получить данные с Маркета: %1").arg(err), true);
        QMessageBox::warning(this, tr("Маркет"), tr("Не удалось получить данные с Маркета:\n%1").arg(err));
        return;
    }
    QJsonObject tree;
    MarketCategories::loadTree(api, false, &tree, nullptr); // из кэша, если есть - для названия категории
    QList<MarketCard *> changed;
    int missing = 0;
    for (int i : sel) {
        MarketCard &c = m_cards[i];
        const auto it = info.constFind(c.offerId);
        if (it == info.cend()) {
            ++missing;
            continue;
        }
        MarketCards::applyMarketInfo(&c, it.value());
        if (c.categoryId > 0 && !tree.isEmpty()) {
            const QString path = MarketCategories::pathOf(tree, c.categoryId);
            if (!path.isEmpty())
                c.categoryName = path;
        }
        changed << &c;
    }
    QString saveErr;
    if (!MarketCards::saveMany(changed, true, nullptr, &saveErr)) {
        QMessageBox::warning(this, tr("Карточки"), tr("Не удалось сохранить:\n%1").arg(saveErr));
        return;
    }
    const int keep = m_shown >= 0 ? m_cards[m_shown].id : (changed.isEmpty() ? 0 : changed.first()->id);
    reloadCards(keep);
    setStatus(tr("С Маркета загружено карточек: %1%2.").arg(changed.size()).arg(missing ? tr(", на Маркете нет: %1").arg(missing) : QString()));
}

void CardsTab::checkOnMarket()
{
    MarketAccount acc;
    if (!currentAccount(&acc) || !commitCurrent())
        return;
    QList<int> sel = selectedCardIndexes();
    if (sel.isEmpty())
        for (int i = 0; i < m_cards.size(); ++i)
            sel << i;
    if (sel.isEmpty())
        return;
    QStringList offerIds;
    for (int i : sel)
        offerIds << m_cards[i].offerId;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    MarketApi api(acc.apiKey);
    QHash<QString, MarketOfferInfo> info;
    QString err;
    const bool ok = api.offerInfo(acc, offerIds, &info, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        setStatus(tr("Не удалось получить данные с Маркета: %1").arg(err), true);
        QMessageBox::warning(this, tr("Маркет"), tr("Не удалось получить данные с Маркета:\n%1").arg(err));
        return;
    }
    QList<MarketCard *> changed;
    int found = 0, withErrors = 0;
    for (int i : sel) {
        MarketCard &c = m_cards[i];
        const auto it = info.constFind(c.offerId);
        if (it == info.cend()) {
            c.marketStatus = tr("на Маркете не найден");
            c.marketSku = 0;
        } else {
            ++found;
            c.marketStatus = MarketCards::describeCardStatus(it->cardStatus);
            c.marketSku = it->marketSku;
            if (!it->contentErrors.isEmpty()) {
                ++withErrors;
                c.lastResult = tr("Маркет: ") + it->contentErrors.join(QStringLiteral("; "));
            } else if (c.status == QLatin1String("error")) {
                c.lastResult = tr("Маркет ошибок не показывает");
            }
        }
        changed << &c;
    }
    QString saveErr;
    MarketCards::saveMany(changed, true, nullptr, &saveErr);
    const int keep = m_shown >= 0 ? m_cards[m_shown].id : 0;
    reloadCards(keep);
    setStatus(tr("Проверено: %1, найдено на Маркете: %2, с замечаниями Маркета: %3.").arg(sel.size()).arg(found).arg(withErrors));
}

void CardsTab::sendToMarket()
{
    MarketAccount acc;
    if (!currentAccount(&acc) || !commitCurrent())
        return;
    QList<int> sel = selectedCardIndexes();
    if (sel.isEmpty()) {
        for (int i = 0; i < m_cards.size(); ++i)
            if (m_cards[i].status != QLatin1String("sent"))
                sel << i;
        if (sel.isEmpty()) {
            QMessageBox::information(this, tr("Нечего отправлять"), tr("Все карточки уже отправлены. Выделите карточки, чтобы отправить их ещё раз."));
            return;
        }
        if (QMessageBox::question(this, tr("Отправить на Маркет"),
                                  tr("Ничего не выделено. Отправить все карточки, которые ещё не отправлены (%1 шт.)?").arg(sel.size())) != QMessageBox::Yes)
            return;
    }

    // Характеристики нужных категорий (из кэша или с Маркета) - чтобы проверить обязательные поля до отправки.
    QHash<qint64, QList<MarketCategoryParam>> params = knownParams();
    QString loadErr;
    {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        MarketApi api(acc.apiKey);
        for (int i : sel) {
            const qint64 cat = m_cards[i].categoryId;
            if (cat > 0 && !params.contains(cat)) {
                QList<MarketCategoryParam> p;
                QString e;
                if (MarketCategories::loadParams(api, acc.businessId, cat, false, &p, &e))
                    params.insert(cat, p);
                else if (loadErr.isEmpty())
                    loadErr = e;
            }
        }
        QApplication::restoreOverrideCursor();
    }
    QList<MarketCard *> ready;
    QStringList problems;
    for (int i : sel) {
        MarketCard &c = m_cards[i];
        const auto it = params.constFind(c.categoryId);
        const QStringList miss = MarketCards::missingForSend(c, it == params.cend() ? nullptr : &it.value());
        if (miss.isEmpty())
            ready << &c;
        else
            problems << tr("%1: нет — %2").arg(c.offerId, miss.join(QStringLiteral(", ")));
    }
    if (ready.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, tr("Отправить на Маркет"), tr("Ни одна карточка не готова: не хватает обязательных данных."), QMessageBox::Ok, this);
        box.setDetailedText(problems.join(QLatin1Char('\n')));
        box.exec();
        return;
    }
    QString question = tr("Отправить на Маркет карточек: %1 (кабинет «%2»)?\n\nТовары добавятся в ассортимент или обновятся; Маркет обрабатывает их до нескольких минут.")
                           .arg(ready.size()).arg(acc.name);
    if (!problems.isEmpty())
        question += tr("\n\nНе будут отправлены (не хватает данных): %1. Подробности - кнопка «Показать подробности».").arg(problems.size());
    QMessageBox ask(QMessageBox::Question, tr("Отправить на Маркет"), question, QMessageBox::Yes | QMessageBox::No, this);
    if (!problems.isEmpty())
        ask.setDetailedText(problems.join(QLatin1Char('\n')));
    if (ask.exec() != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    MarketApi api(acc.apiKey);
    QString err;
    const MarketCards::SendReport report = MarketCards::send(api, acc, ready, params, &err);
    QApplication::restoreOverrideCursor();
    const int keep = m_shown >= 0 ? m_cards[m_shown].id : 0;
    reloadCards(keep);
    QString text = tr("Отправлено: принято %1, отклонено %2%3.").arg(report.accepted).arg(report.rejected)
                       .arg(problems.isEmpty() ? QString() : tr(", не готово к отправке %1").arg(problems.size()));
    setStatus(text, report.rejected > 0);
    AppLog::write(QStringLiteral("market-cards"), QStringLiteral("«%1»: %2 %3").arg(acc.name, text, report.lines.join(QStringLiteral(" | "))).left(2000));
    QMessageBox box(report.rejected ? QMessageBox::Warning : QMessageBox::Information, tr("Отправка на Маркет"), text, QMessageBox::Ok, this);
    QStringList detail = report.lines;
    if (!problems.isEmpty())
        detail << QString() << tr("Не отправлены - не хватает данных:") << problems;
    box.setDetailedText(detail.join(QLatin1Char('\n')));
    if (report.rejected == 0)
        box.setInformativeText(tr("Маркет обрабатывает карточки до нескольких минут. Состояние можно посмотреть кнопкой «Состояние на Маркете»."));
    box.exec();
}
