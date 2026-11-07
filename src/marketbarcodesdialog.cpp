#include "marketbarcodesdialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

using namespace MarketBarcodes;

namespace {

enum Col { ColCheck = 0, ColOffer, ColName, ColOurs, ColMarket, ColResult, ColNote };

QString joined(const QStringList &l)
{
    return l.isEmpty() ? QStringLiteral("-") : l.join(QStringLiteral(", "));
}

} // namespace

MarketBarcodesDialog::MarketBarcodesDialog(const MarketAccount &account, const QStringList &onlyOfferIds, QWidget *parent)
    : QDialog(parent), m_account(account), m_only(onlyOfferIds)
{
    setWindowTitle(tr("Штрихкоды (GTIN) на Маркет - %1").arg(account.name));
    resize(1150, 720);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Берёт штрихкоды наших товаров из базы, сравнивает с карточками на Маркете и передаёт на Маркет ТОЛЬКО штрихкоды (остальное в карточке не "
                                "меняется). Товар на Маркете и в базе связывается по артикулу на Маркете (а если его нет - по артикулу). Маркет принимает EAN-13, EAN-8 "
                                "и UPC-A. Какие штрихкоды НЕ передавать, решаете вы: впишите начала кодов ниже (по умолчанию 20 - свои коды приложения), а в таблице "
                                "снимите галочки у тех карточек, которые менять не нужно. Изменения на Маркете появляются не сразу - до нескольких минут. Ничего не "
                                "отправляется, пока вы не нажмёте «Отправить отмеченные»."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *scope = new QLabel(m_only.isEmpty() ? tr("Товары: все товары базы, которые есть в каталоге кабинета «%1».").arg(account.name)
                                              : tr("Товары: выбранные карточки (%1) кабинета «%2».").arg(m_only.size()).arg(account.name),
                             this);
    layout->addWidget(scope);

    m_replace = new QRadioButton(tr("Заменить: на карточке останутся только наши штрихкоды (как в базе)"), this);
    m_replace->setChecked(true);
    m_add = new QRadioButton(tr("Добавить наши штрихкоды к тем, что уже есть на Маркете"), this);
    layout->addWidget(m_replace);
    layout->addWidget(m_add);

    auto *prefixRow = new QHBoxLayout();
    prefixRow->addWidget(new QLabel(tr("Не передавать штрихкоды, которые начинаются на:"), this));
    m_prefixes = new QLineEdit(this);
    m_prefixes->setMaximumWidth(220);
    m_prefixes->setPlaceholderText(tr("например 20 или 20, 21"));
    m_prefixes->setToolTip(tr("Цифры через запятую или пробел. Код, который начинается на любое из них, остаётся в базе и на Маркет не уходит.\n"
                              "20 - свои штрихкоды приложения (диапазон 200...299 для внутреннего использования). Пусто - передавать все подходящие."));
    m_prefixes->setText(QSettings().value(QStringLiteral("marketBarcodes/excludePrefixes"), MarketBarcodes::defaultExcludePrefixes().join(QStringLiteral(", "))).toString());
    prefixRow->addWidget(m_prefixes);
    prefixRow->addStretch();
    layout->addLayout(prefixRow);

    auto *row = new QHBoxLayout();
    m_compareBtn = new QPushButton(tr("Сравнить с Маркетом"), this);
    auto *allBtn = new QPushButton(tr("Отметить все"), this);
    auto *noneBtn = new QPushButton(tr("Снять все"), this);
    m_showAll = new QCheckBox(tr("Показывать и те, что менять не нужно"), this);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Найти: артикул, название, штрихкод"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(280);
    allBtn->setToolTip(tr("Отметить все карточки, которые сейчас видны в таблице"));
    noneBtn->setToolTip(tr("Снять отметку со всех видимых карточек"));
    row->addWidget(m_compareBtn);
    row->addSpacing(12);
    row->addWidget(allBtn);
    row->addWidget(noneBtn);
    row->addSpacing(12);
    row->addWidget(m_filter);
    row->addSpacing(12);
    row->addWidget(m_showAll);
    row->addStretch();
    layout->addLayout(row);

    m_summary = new QLabel(tr("Нажмите «Сравнить с Маркетом»."), this);
    layout->addWidget(m_summary);

    m_table = new QTableWidget(0, 7, this);
    m_table->setHorizontalHeaderLabels({QString(), tr("Артикул на Маркете"), tr("Название"), tr("Наши штрихкоды"), tr("На Маркете сейчас"), tr("Что будет на Маркете"), tr("Замечание")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 1);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(130);
    layout->addWidget(m_log);

    auto *buttons = new QHBoxLayout();
    m_sendBtn = new QPushButton(tr("Отправить отмеченные"), this);
    m_sendBtn->setProperty("primary", true);
    m_sendBtn->setMinimumWidth(230);
    m_closeBtn = new QPushButton(tr("Закрыть"), this);
    buttons->addStretch();
    buttons->addWidget(m_sendBtn);
    buttons->addWidget(m_closeBtn);
    layout->addLayout(buttons);
    for (QPushButton *b : findChildren<QPushButton *>())
        b->setAutoDefault(false);

    connect(m_compareBtn, &QPushButton::clicked, this, &MarketBarcodesDialog::compare);
    connect(m_sendBtn, &QPushButton::clicked, this, &MarketBarcodesDialog::send);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(allBtn, &QPushButton::clicked, this, [this]() { checkAll(true); });
    connect(noneBtn, &QPushButton::clicked, this, [this]() { checkAll(false); });
    connect(m_showAll, &QCheckBox::toggled, this, &MarketBarcodesDialog::fillTable);
    connect(m_filter, &QLineEdit::textChanged, this, &MarketBarcodesDialog::applyFilter);
    connect(m_table, &QTableWidget::itemChanged, this, &MarketBarcodesDialog::updateButtons);
    // Другой режим или другой список «не передавать» - сравнение пересчитывается по уже загруженному, Маркет заново не спрашиваем
    // (список правится на лету, поэтому пересчёт - после короткой паузы в наборе).
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(450);
    connect(m_rebuildTimer, &QTimer::timeout, this, &MarketBarcodesDialog::rebuild);
    connect(m_prefixes, &QLineEdit::textChanged, this, [this]() { m_rebuildTimer->start(); });
    connect(m_replace, &QAbstractButton::toggled, this, [this]() { m_rebuildTimer->start(); });
    connect(m_add, &QAbstractButton::toggled, this, [this]() { m_rebuildTimer->start(); });
    updateButtons();
}

Options MarketBarcodesDialog::options() const
{
    Options o;
    o.mode = m_add->isChecked() ? Mode::Add : Mode::Replace;
    o.excludePrefixes = MarketBarcodes::parsePrefixes(m_prefixes->text());
    return o;
}

void MarketBarcodesDialog::log(const QString &line)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss"), line));
    QApplication::processEvents();
}

void MarketBarcodesDialog::compare()
{
    m_busy = true;
    updateButtons();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    log(tr("Загружаю каталог и штрихкоды с Маркета..."));
    MarketApi api(m_account.apiKey, [this](const QString &s) { log(s); });
    QString err;
    const bool ok = MarketBarcodes::load(m_account, api, m_only, &m_source, &err);
    QApplication::restoreOverrideCursor();
    m_busy = false;
    if (!ok) {
        log(tr("Не удалось сравнить: %1").arg(err));
        m_summary->setText(tr("Не удалось сравнить: %1").arg(err));
        m_loaded = false;
        m_source = MarketBarcodes::Source();
        m_compared = MarketBarcodes::Compared();
        m_table->setRowCount(0);
        updateButtons();
        return;
    }
    m_loaded = true;
    rebuild();
    log(m_summary->text());
}

void MarketBarcodesDialog::rebuild()
{
    m_rebuildTimer->stop();
    if (!m_loaded)
        return;
    const Options opt = options();
    QSettings().setValue(QStringLiteral("marketBarcodes/excludePrefixes"), opt.excludePrefixes.join(QStringLiteral(", ")));
    m_compared = MarketBarcodes::build(m_source, opt);
    int change = 0, same = 0, none = 0, archived = 0, byPrefix = 0;
    for (const Row &r : std::as_const(m_compared.rows)) {
        change += r.status == Status::Change;
        same += r.status == Status::Same;
        none += r.status == Status::NoGtin;
        archived += r.status == Status::Archived;
        byPrefix += r.skippedByPrefix;
    }
    QString text = tr("Артикулов в базе: %1; есть в каталоге кабинета: %2 (нет на Маркете: %3). Будет отправлено: %4, уже совпадает: %5, нет подходящего штрихкода: %6, в архиве: %7.")
                       .arg(m_compared.ourOffers).arg(m_compared.rows.size()).arg(m_compared.notOnMarket).arg(change).arg(same).arg(none).arg(archived);
    if (!opt.excludePrefixes.isEmpty())
        text += tr(" По списку «не передавать начинающиеся на %1» отсеяно штрихкодов: %2.").arg(opt.excludePrefixes.join(QStringLiteral(", "))).arg(byPrefix);
    m_summary->setText(text);
    fillTable();
}

void MarketBarcodesDialog::fillTable()
{
    m_table->blockSignals(true);
    m_table->setRowCount(0);
    for (int i = 0; i < m_compared.rows.size(); ++i) {
        const Row &r = m_compared.rows[i];
        if ((r.status == Status::Same || r.status == Status::Archived) && !m_showAll->isChecked())
            continue;
        if (r.status == Status::NoGtin && !m_showAll->isChecked() && r.skipped.isEmpty())
            continue; // нечего передавать и нечего объяснять
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *check = new QTableWidgetItem;
        check->setData(Qt::UserRole, i);
        if (r.status == Status::Change) {
            check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
            check->setCheckState(Qt::Checked);
        } else {
            check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        m_table->setItem(row, ColCheck, check);
        m_table->setItem(row, ColOffer, new QTableWidgetItem(r.offerId));
        m_table->setItem(row, ColName, new QTableWidgetItem(r.name));
        m_table->setItem(row, ColOurs, new QTableWidgetItem(joined(r.ours)));
        m_table->setItem(row, ColMarket, new QTableWidgetItem(joined(r.market)));
        auto *result = new QTableWidgetItem(r.status == Status::Change ? joined(r.result) : statusText(r.status));
        QColor color = QColor("#757575");
        if (r.status == Status::Change)
            color = QColor("#2e7d32");
        else if (r.status == Status::NoGtin)
            color = QColor("#c62828");
        result->setForeground(color);
        m_table->setItem(row, ColResult, result);
        auto *note = new QTableWidgetItem(r.note);
        note->setToolTip(r.note);
        m_table->setItem(row, ColNote, note);
    }
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(ColName, qMin(m_table->columnWidth(ColName), 300));
    m_table->blockSignals(false);
    applyFilter();
}

void MarketBarcodesDialog::applyFilter()
{
    const QString needle = m_filter->text().trimmed();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        bool show = needle.isEmpty();
        for (int col = ColOffer; col <= ColMarket && !show; ++col) {
            const auto *it = m_table->item(row, col);
            show = it && it->text().contains(needle, Qt::CaseInsensitive);
        }
        m_table->setRowHidden(row, !show);
    }
    updateButtons();
}

int MarketBarcodesDialog::checkedCount() const
{
    int n = 0;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto *c = m_table->item(row, ColCheck);
        if (!m_table->isRowHidden(row) && c && (c->flags() & Qt::ItemIsUserCheckable) && c->checkState() == Qt::Checked)
            ++n;
    }
    return n;
}

void MarketBarcodesDialog::updateButtons()
{
    const int n = checkedCount();
    m_sendBtn->setEnabled(!m_busy && m_loaded && n > 0);
    m_sendBtn->setText(n > 0 ? tr("Отправить отмеченные: %1").arg(n) : tr("Отправить отмеченные"));
    m_compareBtn->setEnabled(!m_busy);
}

void MarketBarcodesDialog::checkAll(bool on)
{
    m_table->blockSignals(true);
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *c = m_table->item(row, ColCheck);
        if (!m_table->isRowHidden(row) && c && (c->flags() & Qt::ItemIsUserCheckable))
            c->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    m_table->blockSignals(false);
    updateButtons();
}

void MarketBarcodesDialog::send()
{
    QList<Row> chosen;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto *c = m_table->item(row, ColCheck);
        if (!m_table->isRowHidden(row) && c && (c->flags() & Qt::ItemIsUserCheckable) && c->checkState() == Qt::Checked)
            chosen << m_compared.rows[c->data(Qt::UserRole).toInt()];
    }
    if (chosen.isEmpty())
        return;
    const QString how = m_add->isChecked() ? tr("к штрихкодам на Маркете будут ДОБАВЛЕНЫ наши") : tr("на карточках останутся ТОЛЬКО наши штрихкоды (остальные уйдут)");
    if (QMessageBox::question(this, tr("Отправить на Маркет"),
                              tr("Отправить штрихкоды для %1 карточек кабинета «%2»?\n\nПри этом %3. Остальные данные карточек не меняются.").arg(chosen.size()).arg(m_account.name, how))
        != QMessageBox::Yes)
        return;
    m_busy = true;
    updateButtons();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    MarketApi api(m_account.apiKey, [this](const QString &s) { log(s); });
    QString err;
    const Report rep = MarketBarcodes::push(api, m_account, chosen, &err);
    QApplication::restoreOverrideCursor();
    m_busy = false;
    if (rep.accepted > 0)
        m_changed = true;
    for (const QString &line : rep.lines)
        log(line);
    log(tr("Принято Маркетом: %1, отклонено: %2. Изменения на Маркете появятся в течение нескольких минут.").arg(rep.accepted).arg(rep.rejected));
    if (!err.isEmpty())
        log(tr("Ошибка: %1").arg(err));
    // Принятые отмечаем в таблице как отправленные (с Маркета заново не читаем: он применяет изменения не мгновенно).
    for (Row &r : m_compared.rows) {
        if (rep.acceptedOffers.contains(r.offerId)) {
            r.status = Status::Same;
            r.market = r.result;
            r.note = tr("отправлено на Маркет: изменения появятся в течение нескольких минут");
        } else if (rep.rejectedOffers.contains(r.offerId)) {
            r.note = tr("Маркет отклонил - причина в журнале");
        }
    }
    fillTable();
    if (rep.rejected > 0)
        QMessageBox::warning(this, tr("Часть карточек не принята"), tr("Маркет отклонил карточек: %1. Причины - в журнале окна.").arg(rep.rejected));
}
