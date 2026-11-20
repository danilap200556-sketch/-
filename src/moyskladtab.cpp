#include "moyskladtab.h"

#include "moyskladautosync.h"
#include "theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSqlError>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum StoreCol { SColName, SColWarehouse, SColCount };
enum ChangeCol { CColSku, CColName, CColWarehouse, CColWas, CColWill, CColCount };
constexpr int kMaxShownChanges = 5000;

QString normName(const QString &s) { return s.trimmed().toLower(); }

QString num(qint64 n)
{
    return QLocale().toString(n);
}

} // namespace

MoySkladTab::MoySkladTab(MoySkladAutoSync *autoSync, QWidget *parent)
    : QWidget(parent)
    , m_auto(autoSync)
{
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Остатки из МойСклад приходят в программу. МойСклад здесь главный: остатки сопоставленных складов в программе приводятся к его "
                               "остаткам, а каждое расхождение записывается в журнал одним документом («Синхронизация с МойСклад»: его можно открыть в разделе "
                               "«Документы» и отменить целиком). В МойСклад программа ничего не записывает - только читает."),
                            this);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("muted"));
    layout->addWidget(hint);

    m_pages = new QTabWidget(this);
    layout->addWidget(m_pages, 1);

    // --- Подключение
    {
        auto *page = new QWidget(m_pages);
        auto *l = new QVBoxLayout(page);
        auto *form = new QFormLayout();
        m_token = new QLineEdit(page);
        m_token->setEchoMode(QLineEdit::Password);
        m_token->setPlaceholderText(tr("токен МойСклад (или логин:пароль)"));
        auto *show = new QCheckBox(tr("показать"), page);
        auto *tokenRow = new QHBoxLayout();
        tokenRow->addWidget(m_token, 1);
        tokenRow->addWidget(show);
        form->addRow(tr("Токен:"), tokenRow);
        l->addLayout(form);
        auto *tokenHelp = new QLabel(tr("Токен создаётся в МойСклад: «Настройки» → «Пользователи» → ваш пользователь → «Токены» (нужен доступ к складам и отчёту «Остатки»). "
                                        "Можно ввести и «логин:пароль» от МойСклад."),
                                     page);
        tokenHelp->setWordWrap(true);
        tokenHelp->setObjectName(QStringLiteral("muted"));
        l->addWidget(tokenHelp);

        auto *checkRow = new QHBoxLayout();
        auto *checkBtn = new QPushButton(tr("Проверить подключение"), page);
        checkBtn->setProperty("primary", true);
        m_connStatus = new QLabel(page);
        m_connStatus->setWordWrap(true);
        checkRow->addWidget(checkBtn);
        checkRow->addWidget(m_connStatus, 1);
        l->addLayout(checkRow);

        l->addWidget(new QLabel(tr("Какой склад МойСклад - какой склад программы (остатки «не загружать» остаются как есть):"), page));
        m_stores = new QTableWidget(0, SColCount, page);
        m_stores->setHorizontalHeaderLabels({tr("Склад в МойСклад"), tr("Склад в программе")});
        m_stores->setSelectionMode(QAbstractItemView::NoSelection);
        m_stores->verticalHeader()->setVisible(false);
        m_stores->horizontalHeader()->setSectionResizeMode(SColName, QHeaderView::Stretch);
        m_stores->horizontalHeader()->setSectionResizeMode(SColWarehouse, QHeaderView::Stretch);
        l->addWidget(m_stores, 1);
        auto *mapRow = new QHBoxLayout();
        auto *matchBtn = new QPushButton(tr("Подобрать по названию"), page);
        matchBtn->setToolTip(tr("Склады с одинаковым названием в МойСклад и в программе сопоставляются сами (уже выбранные не трогаются)"));
        auto *createBtn = new QPushButton(tr("Создать недостающие склады в программе"), page);
        createBtn->setToolTip(tr("Для складов МойСклад, которым нет пары, завести склады с такими же названиями"));
        mapRow->addWidget(matchBtn);
        mapRow->addWidget(createBtn);
        mapRow->addStretch();
        l->addLayout(mapRow);

        auto *optForm = new QFormLayout();
        m_stockType = new QComboBox(page);
        m_stockType->addItem(tr("Остаток (физический)"), false);
        m_stockType->addItem(tr("Доступный остаток (за вычетом резерва)"), true);
        optForm->addRow(tr("Переносить:"), m_stockType);
        l->addLayout(optForm);
        m_zeroUnknown = new QCheckBox(tr("обнулять в программе товары, которых нет в отчёте МойСклад"), page);
        m_zeroUnknown->setToolTip(tr("По умолчанию товары программы, неизвестные МойСклад (или с другим артикулом), остаются как есть"));
        l->addWidget(m_zeroUnknown);
        auto *saveBtn = new QPushButton(tr("Сохранить"), page);
        saveBtn->setProperty("primary", true);
        auto *saveRow = new QHBoxLayout();
        saveRow->addWidget(saveBtn);
        saveRow->addStretch();
        l->addLayout(saveRow);
        m_pages->addTab(page, tr("Подключение"));

        connect(show, &QCheckBox::toggled, this, [this](bool on) { m_token->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
        connect(checkBtn, &QPushButton::clicked, this, [this]() { checkConnection(); });
        connect(matchBtn, &QPushButton::clicked, this, &MoySkladTab::matchByName);
        connect(createBtn, &QPushButton::clicked, this, &MoySkladTab::createMissingWarehouses);
        connect(saveBtn, &QPushButton::clicked, this, &MoySkladTab::saveConnection);
    }

    // --- Остатки
    {
        auto *page = new QWidget(m_pages);
        auto *l = new QVBoxLayout(page);
        auto *note = new QLabel(tr("Артикул товара в МойСклад (если его нет - код) сравнивается с артикулом товара в программе (или артикулом на Маркете). "
                                   "«Показать расхождения» ничего не меняет; «Загрузить остатки сейчас» записывает их."),
                                page);
        note->setWordWrap(true);
        note->setObjectName(QStringLiteral("muted"));
        l->addWidget(note);
        auto *row = new QHBoxLayout();
        m_previewBtn = new QPushButton(tr("Показать расхождения"), page);
        m_loadBtn = new QPushButton(tr("Загрузить остатки сейчас"), page);
        m_loadBtn->setProperty("primary", true);
        row->addWidget(m_previewBtn);
        row->addWidget(m_loadBtn);
        row->addStretch();
        l->addLayout(row);
        m_summary = new QLabel(page);
        m_summary->setWordWrap(true);
        l->addWidget(m_summary);
        m_changes = new QTableWidget(0, CColCount, page);
        m_changes->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("Склад"), tr("Сейчас в программе"), tr("Будет (МойСклад)")});
        m_changes->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_changes->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_changes->verticalHeader()->setVisible(false);
        QHeaderView *h = m_changes->horizontalHeader();
        h->setResizeContentsPrecision(40);
        h->setSectionResizeMode(CColName, QHeaderView::Stretch);
        for (int c : {CColSku, CColWarehouse, CColWas, CColWill})
            h->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        l->addWidget(m_changes, 1);
        m_pages->addTab(page, tr("Остатки"));

        connect(m_previewBtn, &QPushButton::clicked, this, [this]() { preview(); });
        connect(m_loadBtn, &QPushButton::clicked, this, [this]() { loadNow(true); });
    }

    // --- Автозагрузка
    {
        auto *page = new QWidget(m_pages);
        auto *l = new QVBoxLayout(page);
        m_autoEnabled = new QCheckBox(tr("Автоматически загружать остатки из МойСклад"), page);
        QFont bold = m_autoEnabled->font();
        bold.setBold(true);
        m_autoEnabled->setFont(bold);
        l->addWidget(m_autoEnabled);
        auto *every = new QHBoxLayout();
        every->addWidget(new QLabel(tr("Раз в"), page));
        m_autoEvery = new QSpinBox(page);
        m_autoEvery->setRange(1, 999);
        every->addWidget(m_autoEvery);
        m_autoUnit = new QComboBox(page);
        m_autoUnit->addItem(tr("минут"), 1);
        m_autoUnit->addItem(tr("часов"), 60);
        every->addWidget(m_autoUnit);
        every->addWidget(new QLabel(tr("(не чаще, чем раз в %1 минут)").arg(MoySkladAutoSync::kMinIntervalMinutes), page));
        every->addStretch();
        l->addLayout(every);
        auto *autoNote = new QLabel(tr("Загрузка идёт сама, как кнопка «Загрузить остатки сейчас», только без вопросов. Если результат похож на ошибку "
                                       "сопоставления (например, обнуляется больше половины товаров), он не применяется - об этом сообщит значок в трее. "
                                       "Работает, пока запущена программа: если закрыть окно, она свернётся в значок у часов (трей) и продолжит работать, выйти "
                                       "совсем - значок → «Выход». Расписание хранится на этом компьютере; токен и сопоставление складов - общие."),
                                    page);
        autoNote->setWordWrap(true);
        autoNote->setObjectName(QStringLiteral("muted"));
        l->addWidget(autoNote);
        m_autoStatus = new QLabel(page);
        m_autoStatus->setWordWrap(true);
        l->addWidget(m_autoStatus);
        auto *buttons = new QHBoxLayout();
        auto *saveBtn = new QPushButton(tr("Сохранить расписание"), page);
        saveBtn->setProperty("primary", true);
        m_autoRunBtn = new QPushButton(tr("Загрузить сейчас"), page);
        buttons->addWidget(saveBtn);
        buttons->addWidget(m_autoRunBtn);
        buttons->addStretch();
        l->addLayout(buttons);
        l->addStretch();
        m_pages->addTab(page, tr("Автозагрузка"));

        const auto s = m_auto->settings();
        m_autoEnabled->setChecked(s.enabled);
        const bool hours = s.intervalMinutes % 60 == 0 && s.intervalMinutes >= 60;
        m_autoUnit->setCurrentIndex(hours ? 1 : 0);
        m_autoEvery->setValue(hours ? s.intervalMinutes / 60 : s.intervalMinutes);
        connect(saveBtn, &QPushButton::clicked, this, &MoySkladTab::saveSchedule);
        connect(m_autoRunBtn, &QPushButton::clicked, this, [this]() {
            if (m_auto->isRunning())
                return;
            m_auto->runNow();
            QMessageBox::information(this, tr("МойСклад"), m_auto->lastResult().isEmpty() ? tr("Готово.") : m_auto->lastResult());
        });
    }

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    m_log->setMaximumHeight(110);
    m_log->setPlaceholderText(tr("Журнал обмена с МойСклад"));
    layout->addWidget(m_log);

    connect(m_auto, &MoySkladAutoSync::message, this, &MoySkladTab::log);
    connect(m_auto, &MoySkladAutoSync::statusChanged, this, &MoySkladTab::updateAutoStatus);
    connect(m_auto, &MoySkladAutoSync::stockChanged, this, [this](int documentId) {
        emit stockChanged();
        if (documentId > 0)
            emit documentCreated(documentId);
    });
    updateAutoStatus();

    // Текущие настройки.
    const MoySklad::Settings s = MoySklad::loadSettings();
    m_token->setText(s.token);
    m_stockType->setCurrentIndex(s.freeStock ? 1 : 0);
    m_zeroUnknown->setChecked(s.zeroUnknown);
    m_storeList = MoySklad::cachedStores();
    rebuildStores(s.storeMap);
}

void MoySkladTab::refresh()
{
    // Список складов программы мог измениться: пересобираем выбор, сохраняя то, что уже выбрано на экране.
    rebuildStores(mapFromTable());
    updateAutoStatus();
}

QList<QPair<int, QString>> MoySkladTab::warehouses() const
{
    QList<QPair<int, QString>> out;
    QSqlQuery q(QStringLiteral("SELECT id, name FROM warehouses ORDER BY lower(name)"));
    while (q.next())
        out.append({q.value(0).toInt(), q.value(1).toString()});
    return out;
}

QHash<QString, int> MoySkladTab::mapFromTable() const
{
    QHash<QString, int> map;
    for (int r = 0; r < m_stores->rowCount(); ++r) {
        auto *combo = qobject_cast<QComboBox *>(m_stores->cellWidget(r, SColWarehouse));
        const QString id = m_stores->item(r, SColName)->data(Qt::UserRole).toString();
        if (combo && !id.isEmpty() && combo->currentData().toInt() > 0)
            map.insert(id, combo->currentData().toInt());
    }
    return map;
}

void MoySkladTab::rebuildStores(const QHash<QString, int> &map)
{
    const QList<QPair<int, QString>> whs = warehouses();
    // Склады, которые известны (из последней проверки), плюс сопоставленные, которых там уже нет.
    QList<MoySklad::Store> list;
    for (const MoySklad::Store &st : std::as_const(m_storeList))
        if (!st.archived || map.contains(st.id))
            list.append(st);
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        bool known = false;
        for (const MoySklad::Store &st : std::as_const(list))
            known = known || st.id == it.key();
        if (!known)
            list.append({it.key(), tr("(склад не найден: %1)").arg(it.key().left(8)), false});
    }
    QSignalBlocker block(m_stores);
    m_stores->setRowCount(0);
    m_stores->setRowCount(int(list.size()));
    for (int r = 0; r < list.size(); ++r) {
        const MoySklad::Store &st = list.at(r);
        auto *item = new QTableWidgetItem(st.archived ? tr("%1 (архивный)").arg(st.name) : st.name);
        item->setData(Qt::UserRole, st.id);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        m_stores->setItem(r, SColName, item);
        auto *combo = new QComboBox(m_stores);
        combo->addItem(tr("- не загружать -"), 0);
        for (const auto &w : whs)
            combo->addItem(w.second, w.first);
        const int wi = combo->findData(map.value(st.id, 0));
        combo->setCurrentIndex(wi < 0 ? 0 : wi);
        m_stores->setCellWidget(r, SColWarehouse, combo);
    }
}

MoySklad::Settings MoySkladTab::settingsFromUi() const
{
    MoySklad::Settings s;
    s.token = m_token->text().trimmed();
    s.freeStock = m_stockType->currentData().toBool();
    s.zeroUnknown = m_zeroUnknown->isChecked();
    s.storeMap = mapFromTable();
    return s;
}

void MoySkladTab::setConnectionStatus(const QString &text, bool ok)
{
    m_connStatus->setText(text);
    m_connStatus->setStyleSheet(ok ? QString() : QStringLiteral("color: #d64545;"));
}

void MoySkladTab::log(const QString &text)
{
    m_log->appendPlainText(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), text));
}

void MoySkladTab::saveConnection()
{
    if (MoySklad::saveSettings(settingsFromUi()))
        log(tr("Настройки сохранены."));
    else
        QMessageBox::warning(this, tr("МойСклад"), tr("Не удалось сохранить настройки в базе."));
}

bool MoySkladTab::checkConnection()
{
    const QString token = m_token->text().trimmed();
    if (token.isEmpty()) {
        setConnectionStatus(tr("Введите токен."), false);
        return false;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    MoySklad::Api api(token, [this](const QString &s) { log(s); });
    QList<MoySklad::Store> stores;
    QString err;
    const bool ok = api.stores(&stores, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        setConnectionStatus(tr("Не удалось подключиться: %1").arg(err), false);
        return false;
    }
    m_storeList = stores;
    MoySklad::cacheStores(stores);
    int active = 0;
    for (const MoySklad::Store &s : stores)
        active += s.archived ? 0 : 1;
    setConnectionStatus(tr("Подключение работает. Складов в МойСклад: %1.").arg(active), true);
    rebuildStores(mapFromTable());
    matchByName();
    return true;
}

void MoySkladTab::matchByName()
{
    const QList<QPair<int, QString>> whs = warehouses();
    int matched = 0;
    for (int r = 0; r < m_stores->rowCount(); ++r) {
        auto *combo = qobject_cast<QComboBox *>(m_stores->cellWidget(r, SColWarehouse));
        if (!combo || combo->currentData().toInt() > 0)
            continue;
        QString name = m_stores->item(r, SColName)->text();
        name.remove(tr(" (архивный)"));
        for (const auto &w : whs) {
            if (normName(w.second) == normName(name)) {
                combo->setCurrentIndex(combo->findData(w.first));
                ++matched;
                break;
            }
        }
    }
    if (matched > 0)
        log(tr("Сопоставлено по названию складов: %1. Нажмите «Сохранить».").arg(matched));
}

void MoySkladTab::createMissingWarehouses()
{
    const QList<QPair<int, QString>> whs = warehouses();
    QStringList names;
    for (int r = 0; r < m_stores->rowCount(); ++r) {
        auto *combo = qobject_cast<QComboBox *>(m_stores->cellWidget(r, SColWarehouse));
        if (!combo || combo->currentData().toInt() > 0)
            continue;
        const QString id = m_stores->item(r, SColName)->data(Qt::UserRole).toString();
        QString name = m_stores->item(r, SColName)->text();
        if (name.endsWith(tr(" (архивный)")) || name.startsWith(QLatin1Char('(')))
            continue;   // архивные и не найденные не заводим
        bool exists = false;
        for (const auto &w : whs)
            exists = exists || normName(w.second) == normName(name);
        if (!exists && !id.isEmpty())
            names << name.trimmed();
    }
    names.removeDuplicates();
    if (names.isEmpty()) {
        QMessageBox::information(this, tr("МойСклад"), tr("Все склады МойСклад уже сопоставлены или у них есть склад с таким же названием."));
        return;
    }
    if (QMessageBox::question(this, tr("МойСклад"), tr("Создать в программе склады: %1?").arg(names.join(QStringLiteral(", ")))) != QMessageBox::Yes)
        return;
    for (const QString &name : std::as_const(names)) {
        QSqlQuery q;
        q.prepare(QStringLiteral("INSERT INTO warehouses (name) VALUES (?)"));
        q.addBindValue(name);
        if (!q.exec()) {
            QMessageBox::warning(this, tr("МойСклад"), tr("Склад «%1» не создан: %2").arg(name, q.lastError().text()));
            break;
        }
    }
    emit warehousesChanged();
    rebuildStores(mapFromTable());
    matchByName();
}

void MoySkladTab::showPlan(const MoySklad::Plan &plan)
{
    m_plan = plan;
    QSignalBlocker block(m_changes);
    m_changes->setRowCount(0);
    const int shown = int(qMin<qsizetype>(plan.changes.size(), kMaxShownChanges));
    m_changes->setRowCount(shown);
    for (int r = 0; r < shown; ++r) {
        const MoySklad::Change &c = plan.changes.at(r);
        auto put = [&](int col, const QString &text, bool right = false) {
            auto *it = new QTableWidgetItem(text);
            if (right)
                it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_changes->setItem(r, col, it);
        };
        put(CColSku, c.sku);
        put(CColName, c.name);
        put(CColWarehouse, c.warehouse);
        put(CColWas, QString::number(c.was), true);
        put(CColWill, QString::number(c.will), true);
    }
    QString text = tr("МойСклад: товаров <b>%1</b>, найдено в программе <b>%2</b>. Расхождений: <b>%3</b>").arg(plan.positions).arg(plan.matched).arg(plan.changes.size());
    if (!plan.changes.isEmpty())
        text += tr(" (было %1 шт., станет %2 шт. по этим позициям)").arg(num(plan.unitsBefore()), num(plan.unitsAfter()));
    text += QLatin1Char('.');
    if (plan.unknown > 0)
        text += tr(" Есть остаток в МойСклад, но товара нет в программе: %1 (например: %2).").arg(plan.unknown).arg(plan.unknownSamples.join(QStringLiteral(", ")));
    for (const QString &n : plan.notes)
        text += QLatin1Char(' ') + n + QLatin1Char('.');
    if (plan.changes.size() > shown)
        text += tr(" В таблице показаны первые %1.").arg(shown);
    if (plan.suspicious)
        text += tr(" <span style=\"color:#d64545\"><b>Внимание: %1.</b> Проверьте сопоставление складов и артикулов.</span>").arg(plan.suspicion);
    m_summary->setText(text);
}

bool MoySkladTab::preview()
{
    if (m_busy)
        return false;
    const MoySklad::Settings s = settingsFromUi();
    if (!s.isConfigured()) {
        m_summary->setText(tr("Введите токен на вкладке «Подключение»."));
        return false;
    }
    MoySklad::saveSettings(s);
    m_busy = true;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    MoySklad::Api api(s.token, [this](const QString &t) { log(t); });
    MoySklad::Plan plan;
    QString err;
    const bool ok = MoySklad::buildPlan(s, api, &plan, &err);
    QApplication::restoreOverrideCursor();
    m_busy = false;
    if (!ok) {
        m_changes->setRowCount(0);
        m_plan = MoySklad::Plan();
        m_summary->setText(tr("<span style=\"color:#d64545\">Не удалось: %1</span>").arg(err.toHtmlEscaped()));
        log(err);
        return false;
    }
    showPlan(plan);
    log(MoySklad::summary(plan));
    return true;
}

int MoySkladTab::loadNow(bool ask)
{
    if (!preview())
        return -1;
    const MoySklad::Plan plan = m_plan;
    if (plan.changes.isEmpty()) {
        m_summary->setText(m_summary->text() + tr(" Остатки уже совпадают, загружать нечего."));
        return 0;
    }
    if (ask) {
        QString text = tr("Привести остатки в программе к МойСклад?\n\nПозиций будет изменено: %1 (на складах: %2). Было %3 шт., станет %4 шт. по этим позициям.\n"
                          "Всё запишется одним документом, его можно отменить в разделе «Документы».")
                           .arg(plan.changes.size()).arg(plan.mappedWarehouses).arg(num(plan.unitsBefore()), num(plan.unitsAfter()));
        if (plan.suspicious)
            text += tr("\n\nВНИМАНИЕ: %1. Это похоже на ошибку сопоставления складов или артикулов. Лучше сначала проверить таблицу расхождений.").arg(plan.suspicion);
        if (QMessageBox::question(this, tr("МойСклад"), text) != QMessageBox::Yes)
            return -1;
    }
    m_busy = true;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    MoySklad::ApplyResult res;
    QString err;
    const bool ok = MoySklad::apply(plan, tr("Синхронизация с МойСклад"), &res, &err);
    QApplication::restoreOverrideCursor();
    m_busy = false;
    if (!ok) {
        log(tr("Не записано: %1").arg(err));
        m_summary->setText(tr("<span style=\"color:#d64545\">Не записано: %1</span>").arg(err.toHtmlEscaped()));
        if (ask)
            QMessageBox::warning(this, tr("МойСклад"), tr("Остатки не записаны: %1").arg(err));
        return -1;
    }
    log(tr("Загружено: изменено позиций - %1%2.").arg(res.changed).arg(res.documentId > 0 ? tr(", документ №%1").arg(res.documentId) : QString()));
    m_changes->setRowCount(0);
    m_summary->setText(tr("Загружено: изменено позиций - <b>%1</b>%2.").arg(res.changed).arg(res.documentId > 0 ? tr(", создан документ №%1").arg(res.documentId) : QString()));
    emit stockChanged();
    if (res.documentId > 0)
        emit documentCreated(res.documentId);
    return res.changed;
}

void MoySkladTab::saveSchedule()
{
    MoySkladAutoSync::Settings s;
    s.enabled = m_autoEnabled->isChecked();
    s.intervalMinutes = m_autoEvery->value() * m_autoUnit->currentData().toInt();
    if (s.intervalMinutes < MoySkladAutoSync::kMinIntervalMinutes) {
        s.intervalMinutes = MoySkladAutoSync::kMinIntervalMinutes;
        m_autoUnit->setCurrentIndex(0);
        m_autoEvery->setValue(s.intervalMinutes);
        QMessageBox::information(this, tr("МойСклад"), tr("Чаще, чем раз в %1 минут, остатки не загружаются (у МойСклад есть лимиты на запросы). Поставлено %1.")
                                                           .arg(MoySkladAutoSync::kMinIntervalMinutes));
    }
    if (s.enabled) {
        const MoySklad::Settings cfg = settingsFromUi();
        if (!cfg.isConfigured() || cfg.mappedStores() == 0) {
            QMessageBox::warning(this, tr("МойСклад"), tr("Сначала введите токен и сопоставьте хотя бы один склад на вкладке «Подключение» и нажмите «Сохранить»."));
            m_autoEnabled->setChecked(false);
            s.enabled = false;
        } else {
            MoySklad::saveSettings(cfg);   // автозагрузка берёт то, что сохранено
        }
    }
    m_auto->setSettings(s);
}

void MoySkladTab::updateAutoStatus()
{
    m_autoStatus->setText(m_auto->statusText());
    m_autoRunBtn->setEnabled(!m_auto->isRunning());
}
