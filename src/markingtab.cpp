#include "markingtab.h"

#include "cabinetxml.h"
#include "theme.h"
#include "cabinetxmldialog.h"
#include "chestnyznak.h"
#include "cryptosign.h"
#include "csvimport.h"
#include "flowlayout.h"
#include "markingorderdialog.h"
#include "markingprintdialog.h"
#include "xlsx.h"

#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

enum Column { ColStatus, ColSku, ColName, ColGtin, ColSerial, ColCode, ColSource, ColPrints, ColCreated, ColCount };

// Список товаров для выбора: «артикул - название».
void fillProducts(QComboBox *combo, bool withBlank, const QString &blankText)
{
    combo->clear();
    if (withBlank)
        combo->addItem(blankText, 0);
    QSqlQuery q(QStringLiteral("SELECT id, sku, name FROM products ORDER BY sku"));
    while (q.next())
        combo->addItem(QStringLiteral("%1 - %2").arg(q.value(1).toString(), q.value(2).toString()), q.value(0).toInt());
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    auto *completer = new QCompleter(combo->model(), combo);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    combo->setCompleter(completer);
}

int comboProductId(const QComboBox *combo)
{
    const int idx = combo->findText(combo->currentText(), Qt::MatchExactly);
    return idx >= 0 ? combo->itemData(idx).toInt() : combo->currentData().toInt();
}

// Строки из файла: txt/csv - по строке на код, xlsx - любая ячейка, похожая на код.
QStringList readCodesFromFile(const QString &path, QString *error)
{
    QStringList out;
    const QString suffix = QFileInfo(path).suffix().toLower();
    auto looksLikeCode = [](const QString &cell) {
        const QString c = cell.trimmed();
        return c.size() >= 19 && (c.startsWith(QLatin1String("01")) || c.startsWith(QLatin1String("(01)")) || c.startsWith(QLatin1String("]d2")) || c.contains(MarkingCode::kGs));
    };
    if (suffix == QLatin1String("xlsx")) {
        const Xlsx::Table t = Xlsx::readFirstSheet(path);
        if (!t.ok) {
            if (error)
                *error = t.error;
            return out;
        }
        for (const QStringList &row : t.rows)
            for (const QString &cell : row)
                if (looksLikeCode(cell)) {
                    out << cell.trimmed();
                    break;
                }
        return out;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return out;
    }
    const QString text = QString::fromUtf8(f.readAll());
    for (QString line : MarkingCode::splitText(text)) {
        // В CSV код часто в кавычках (внутри кавычки удвоены): снимаем одну пару.
        if (line.size() >= 2 && line.startsWith(QLatin1Char('"')) && line.endsWith(QLatin1Char('"'))) {
            line = line.mid(1, line.size() - 2);
            line.replace(QLatin1String("\"\""), QLatin1String("\""));
        }
        if (looksLikeCode(line))
            out << line;
    }
    return out;
}

// ---------------------------------------------------------------- добавление кодов вставкой

class AddCodesDialog : public QDialog
{
public:
    explicit AddCodesDialog(QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Добавить коды маркировки"));
        resize(760, 520);
        auto *l = new QVBoxLayout(this);
        auto *hint = new QLabel(tr("Вставьте коды маркировки (из файла, письма, личного кабинета «Честного знака»): по одному в строке. Подойдёт любой вид: "
                                   "с разделителем GS, в скобках (01)…(21)…, слитно, со сканера. Если разделители GS потерялись при копировании, "
                                   "программа восстановит их для кодов обуви и одежды (серийный номер 13 знаков, ключ 4, подпись 44 или 88)."), this);
        hint->setWordWrap(true);
        l->addWidget(hint);
        m_text = new QPlainTextEdit(this);
        m_text->setPlaceholderText(tr("0104600702084030215abcdefghij12\\x1d91EE06\\x1d92...."));
        l->addWidget(m_text, 1);
        auto *row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Товар:"), this));
        m_product = new QComboBox(this);
        fillProducts(m_product, true, tr("Определить по GTIN (у товара должен быть такой штрихкод)"));
        row->addWidget(m_product, 1);
        l->addLayout(row);
        m_info = new QLabel(this);
        l->addWidget(m_info);
        auto *buttons = new QDialogButtonBox(this);
        buttons->addButton(tr("Добавить"), QDialogButtonBox::AcceptRole);
        buttons->addButton(tr("Отмена"), QDialogButtonBox::RejectRole);
        l->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_text, &QPlainTextEdit::textChanged, this, [this]() { updateInfo(); });
        updateInfo();
    }
    QStringList codes() const { return MarkingCode::splitText(m_text->toPlainText()); }
    int productId() const { return comboProductId(m_product); }

private:
    void updateInfo()
    {
        int ok = 0, bad = 0;
        for (const QString &c : codes())
            (MarkingCode::parse(c).ok ? ok : bad)++;
        m_info->setText(tr("Разобрано кодов: %1, не распознано: %2").arg(ok).arg(bad));
    }
    QPlainTextEdit *m_text;
    QComboBox *m_product;
    QLabel *m_info;
};

// ---------------------------------------------------------------- настройки СУЗ

class SettingsDialog : public QDialog
{
public:
    explicit SettingsDialog(QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("СУЗ «Честного знака»: настройки"));
        resize(720, 560);
        auto *l = new QVBoxLayout(this);
        auto *hint = new QLabel(tr("Данные берутся у «Честного знака» (станция управления заказами, СУЗ API v3). <b>Идентификатор СУЗ (omsId)</b> и "
                                   "<b>«Идентификатор соединения»</b> устройства - в разделе «Управление заказами» → «Устройства». <b>clientToken</b> живёт 10 часов "
                                   "и получается электронной подписью: выберите сертификат (токен с подписью вставлен в этот компьютер, нужен КриптоПро) и нажмите "
                                   "«Получить токен» - дальше программа сама обновляет его, когда он заканчивается. Токен вписан в общую базу, его видят все компьютеры; "
                                   "сертификат выбирается на каждом компьютере отдельно. Заказ кодов тоже подписывается этим сертификатом."), this);
        hint->setWordWrap(true);
        hint->setTextFormat(Qt::RichText);
        l->addWidget(hint);
        auto *urlHint = new QLabel(tr("Адрес СУЗ - это адрес API: <b>https://suzgrid.crpt.ru/api/v3</b>. Адрес страницы «Управление заказами» из личного кабинета "
                                      "(…/management/orders) - это сайт, а не API: программа заменяет его на адрес API сама."), this);
        urlHint->setWordWrap(true);
        urlHint->setTextFormat(Qt::RichText);
        l->addWidget(urlHint);
        m_loaded = ChestnyZnak::loadSettings();
        const ChestnyZnak::Settings &s = m_loaded;
        auto *form = new QFormLayout();
        m_url = new QLineEdit(s.baseUrl, this);
        m_oms = new QLineEdit(s.omsId, this);
        m_conn = new QLineEdit(s.omsConnection, this);
        m_conn->setPlaceholderText(tr("«Идентификатор соединения» устройства (Управление заказами → Устройства)"));
        m_token = new QLineEdit(s.clientToken, this);
        m_token->setEchoMode(QLineEdit::Password);
        m_cert = new QComboBox(this);
        m_cert->setMinimumWidth(380);
        auto *findCerts = new QPushButton(tr("Найти сертификаты"), this);
        auto *certRow = new QHBoxLayout();
        certRow->addWidget(m_cert, 1);
        certRow->addWidget(findCerts);
        m_signer = new QLineEdit(s.signerUrl, this);
        m_signer->setPlaceholderText(tr("необязательно: внешняя программа-подписант, например http://127.0.0.1:8080/sign"));
        m_template = new QSpinBox(this);
        m_template->setRange(0, 999999);
        m_template->setSpecialValueText(tr("по умолчанию для группы (обувь 1, лёгкая промышленность 10)"));
        m_template->setValue(s.templateId);
        m_group = new QComboBox(this);
        for (const QString &g : CabinetXml::groups())
            m_group->addItem(CabinetXml::groupTitle(g), g);
        m_release = new QComboBox(this);
        m_contact = new QLineEdit(s.contactPerson, this);
        m_contact->setMaxLength(128);
        m_contact->setPlaceholderText(tr("необязательно: контактное лицо в заказе"));
        form->addRow(tr("Адрес СУЗ:"), m_url);
        form->addRow(tr("Идентификатор СУЗ (omsId):"), m_oms);
        form->addRow(tr("Идентификатор соединения:"), m_conn);
        form->addRow(tr("Сертификат УКЭП (этот компьютер):"), certRow);
        form->addRow(tr("clientToken:"), m_token);
        form->addRow(tr("Подписант (необязательно):"), m_signer);
        form->addRow(tr("Товарная группа:"), m_group);
        form->addRow(tr("Способ выпуска в оборот:"), m_release);
        form->addRow(tr("Контактное лицо:"), m_contact);
        form->addRow(tr("Шаблон КМ (templateId):"), m_template);
        l->addLayout(form);
        m_state = new QLabel(this);
        m_state->setWordWrap(true);
        l->addWidget(m_state);
        auto *buttons = new QDialogButtonBox(this);
        auto *getToken = buttons->addButton(tr("Получить токен"), QDialogButtonBox::ActionRole);
        auto *test = buttons->addButton(tr("Проверить связь"), QDialogButtonBox::ActionRole);
        buttons->addButton(tr("Сохранить"), QDialogButtonBox::AcceptRole);
        buttons->addButton(tr("Отмена"), QDialogButtonBox::RejectRole);
        l->addWidget(buttons);
        fillCertificates();
        showTokenState();
        fillReleaseMethods(s.releaseMethodType);
        const int gi = m_group->findData(s.productGroup);
        m_group->setCurrentIndex(gi >= 0 ? gi : 0);
        fillReleaseMethods(s.releaseMethodType);
        connect(m_group, &QComboBox::currentIndexChanged, this, [this]() { fillReleaseMethods(m_release->currentData().toString()); });
        connect(findCerts, &QPushButton::clicked, this, [this]() { fillCertificates(true); });
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            const QString before = m_url->text().trimmed();
            ChestnyZnak::saveSettings(current());
            if (before != m_url->text().trimmed())
                QMessageBox::information(this, windowTitle(), tr("Адрес СУЗ исправлен на адрес API: %1").arg(m_url->text()));
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(test, &QPushButton::clicked, this, [this]() {
            const ChestnyZnak::Reply r = ChestnyZnak::ping(current());
            m_state->setText(r.ok ? tr("Связь есть: СУЗ ответил.") : tr("Не получилось: %1").arg(r.error));
        });
        connect(getToken, &QPushButton::clicked, this, [this]() { obtainToken(); });
        for (QPushButton *b : findChildren<QPushButton *>())
            b->setAutoDefault(false);
    }
    ChestnyZnak::Settings current() const
    {
        ChestnyZnak::Settings s = m_loaded;
        s.baseUrl = ChestnyZnak::normalizeBaseUrl(m_url->text());
        if (s.baseUrl != m_url->text().trimmed())
            m_url->setText(s.baseUrl); // показываем исправленный адрес API
        s.omsId = m_oms->text().trimmed();
        s.omsConnection = m_conn->text().trimmed();
        s.clientToken = m_token->text().trimmed();
        // Токен, вписанный вручную, живёт неизвестно сколько: срок отсчитываем только у полученного программой.
        s.tokenIssuedAt = s.clientToken == m_issuedToken ? m_issuedAt : QDateTime();
        s.certThumbprint = m_cert->currentData().toString();
        s.signerUrl = m_signer->text().trimmed();
        s.templateId = m_template->value();
        s.productGroup = m_group->currentData().toString();
        s.releaseMethodType = m_release->currentData().toString();
        s.contactPerson = m_contact->text().trimmed();
        return s;
    }

private:
    // Способы выпуска в оборот, допустимые для выбранной товарной группы
    void fillReleaseMethods(const QString &keep)
    {
        const QString group = m_group->currentData().toString();
        m_release->clear();
        for (const QString &r : CabinetXml::releaseMethods(group))
            m_release->addItem(QStringLiteral("%1 - %2").arg(r, CabinetXml::releaseMethodTitle(r)), r);
        const int i = m_release->findData(keep);
        m_release->setCurrentIndex(i >= 0 ? i : 0);
    }
    void fillCertificates(bool report = false)
    {
        m_cert->clear();
        m_cert->addItem(tr("- не использовать (подпись внешним подписантом или без подписи) -"), QString());
        if (!CryptoSign::available()) {
            m_cert->setEnabled(false);
            m_cert->setToolTip(tr("Подпись через КриптоПро доступна только в Windows."));
            return;
        }
        QString err;
        const QList<CryptoSign::Certificate> certs = CryptoSign::certificates(&err);
        for (const CryptoSign::Certificate &c : certs)
            m_cert->addItem(tr("%1 - до %2%3").arg(c.subject, c.validTo.toLocalTime().toString(QStringLiteral("dd.MM.yyyy")), c.expired() ? tr(" (СРОК ИСТЁК)") : QString()), c.thumbprint);
        const int i = m_cert->findData(m_loaded.certThumbprint);
        if (i >= 0)
            m_cert->setCurrentIndex(i);
        else if (!certs.isEmpty() && m_loaded.certThumbprint.isEmpty() && !certs.first().expired())
            m_cert->setCurrentIndex(1); // единственный (или самый свежий) действующий сертификат
        if (certs.isEmpty() && (report || !err.isEmpty()))
            m_state->setText(tr("Сертификаты не найдены: %1").arg(err));
        else if (report)
            m_state->setText(tr("Найдено сертификатов: %1.").arg(certs.size()));
    }
    void showTokenState()
    {
        const ChestnyZnak::Settings &s = m_loaded;
        if (s.clientToken.isEmpty())
            m_state->setText(tr("Токена пока нет: выберите сертификат, укажите идентификатор соединения и нажмите «Получить токен»."));
        else if (s.tokenIssuedAt.isValid())
            m_state->setText(tr("Токен получен %1, действует до %2.").arg(s.tokenIssuedAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")),
                                                                      s.tokenIssuedAt.addSecs(10 * 3600).toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))));
        m_issuedToken = s.clientToken;
        m_issuedAt = s.tokenIssuedAt;
    }
    void obtainToken()
    {
        const ChestnyZnak::Settings s = current();
        const ChestnyZnak::Signer sign = ChestnyZnak::makeSigner(s, false);
        m_state->setText(tr("Получаю токен: подпишите запрос (если токен с подписью защищён ПИН-кодом, КриптоПро спросит его)…"));
        QApplication::processEvents();
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const ChestnyZnak::TokenReply r = ChestnyZnak::obtainToken(s, sign);
        QApplication::restoreOverrideCursor();
        if (!r.ok) {
            m_state->setText(tr("Токен не получен: %1").arg(r.error));
            return;
        }
        // Прежний токен с этого момента недействителен - сохраняем новый сразу, а не по кнопке «Сохранить».
        m_token->setText(r.token);
        m_issuedToken = r.token;
        m_issuedAt = QDateTime::currentDateTimeUtc();
        ChestnyZnak::saveSettings(current());
        m_loaded = ChestnyZnak::loadSettings();
        m_state->setText(tr("Токен получен и сохранён, действует 10 часов (до %1). Дальше программа обновит его сама.")
                             .arg(m_issuedAt.addSecs(10 * 3600).toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))));
    }

    ChestnyZnak::Settings m_loaded;
    QString m_issuedToken;
    QDateTime m_issuedAt;
    QLineEdit *m_url, *m_oms, *m_conn, *m_token, *m_signer;
    QComboBox *m_cert, *m_group, *m_release;
    QLineEdit *m_contact;
    QSpinBox *m_template;
    QLabel *m_state;
};

} // namespace

// ---------------------------------------------------------------- страница

MarkingTab::MarkingTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    m_hint = new QLabel(tr("Коды маркировки «Честного знака»: добавьте их (вставкой, из файла, сканером или заказом в СУЗ), они привяжутся к товарам по GTIN "
                           "(= штрихкод EAN-13 товара) и печатаются на термоэтикетках 58×40: Data Matrix, артикул, название, (01) и (21)."), this);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet("color: gray;");
    layout->addWidget(m_hint);

    auto *filters = new QHBoxLayout();
    m_status = new QComboBox(this);
    m_status->addItem(tr("Все коды"), QString());
    m_status->addItem(tr("Новые (не печатались)"), QStringLiteral("new"));
    m_status->addItem(tr("Напечатанные"), QStringLiteral("printed"));
    m_status->addItem(tr("Без товара"), QStringLiteral("unlinked"));
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Поиск: артикул, название, GTIN, серийный номер"));
    m_search->setClearButtonEnabled(true);
    m_scan = new QLineEdit(this);
    m_scan->setPlaceholderText(tr("Сканер: отсканируйте код маркировки, Enter - добавить"));
    m_scan->setMinimumWidth(260);
    filters->addWidget(m_status);
    filters->addWidget(m_search, 1);
    filters->addWidget(m_scan, 1);
    layout->addLayout(filters);

    auto *buttons = new FlowLayout(nullptr, 6);
    auto *addBtn = new QPushButton(tr("Добавить коды..."), this);
    auto *fileBtn = new QPushButton(tr("Из файла..."), this);
    auto *orderBtn = new QPushButton(tr("Заказать в СУЗ..."), this);
    auto *printBtn = new QPushButton(tr("Печать на термоэтикетке..."), this);
    auto *introBtn = new QPushButton(tr("Ввод в оборот (XML)..."), this);
    introBtn->setToolTip(tr("Сохранить выделенные коды (или все в списке) файлом «Ввод в оборот» для загрузки в личный кабинет «Честного знака»"));
    auto *selNewBtn = new QPushButton(tr("Выделить новые"), this);
    auto *linkBtn = new QPushButton(tr("Привязать к товару..."), this);
    auto *delBtn = new QPushButton(tr("Удалить"), this);
    auto *setBtn = new QPushButton(tr("Настройки СУЗ..."), this);
    selNewBtn->setToolTip(tr("Выделить в списке все коды, которые ещё не печатались"));
    linkBtn->setToolTip(tr("Выделенным кодам без товара назначить товар: его штрихкод станет GTIN этих кодов"));
    delBtn->setToolTip(tr("Удалить выделенные коды, которые ещё не печатались (напечатанные могут быть уже наклеены - их не трогаем)"));
    for (QPushButton *b : {addBtn, fileBtn, orderBtn, printBtn, introBtn, selNewBtn, linkBtn, delBtn, setBtn})
        buttons->addWidget(b);
    layout->addLayout(buttons);

    m_counts = new QLabel(this);
    layout->addWidget(m_counts);
    m_notice = new QLabel(this);
    m_notice->setObjectName(QStringLiteral("noticeBanner"));
    m_notice->setWordWrap(true);
    m_notice->setVisible(false);
    layout->addWidget(m_notice);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Статус"), tr("Артикул"), tr("Название"), tr("GTIN"), tr("Серийный номер"), tr("Код"), tr("Источник"), tr("Печатей"), tr("Создан")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    QHeaderView *h = m_table->horizontalHeader();
    h->setSectionResizeMode(ColName, QHeaderView::Stretch);
    h->setSectionResizeMode(ColCode, QHeaderView::Stretch);
    for (int c : {ColStatus, ColSku, ColGtin, ColSerial, ColSource, ColPrints, ColCreated})
        h->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    layout->addWidget(m_table, 1);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(250);

    connect(addBtn, &QPushButton::clicked, this, &MarkingTab::addCodes);
    connect(fileBtn, &QPushButton::clicked, this, &MarkingTab::addFromFile);
    connect(orderBtn, &QPushButton::clicked, this, &MarkingTab::orderCodes);
    connect(printBtn, &QPushButton::clicked, this, &MarkingTab::print);
    connect(introBtn, &QPushButton::clicked, this, &MarkingTab::introXml);
    connect(selNewBtn, &QPushButton::clicked, this, &MarkingTab::selectNew);
    connect(linkBtn, &QPushButton::clicked, this, &MarkingTab::linkSelected);
    connect(delBtn, &QPushButton::clicked, this, &MarkingTab::removeSelected);
    connect(setBtn, &QPushButton::clicked, this, &MarkingTab::settings);
    connect(m_status, &QComboBox::currentIndexChanged, this, [this]() { reload(); });
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_searchTimer, &QTimer::timeout, this, [this]() { reload(); });
    connect(m_scan, &QLineEdit::returnPressed, this, &MarkingTab::scanned);
}

void MarkingTab::refresh()
{
    reload();
}

QList<int> MarkingTab::selectedIds() const
{
    QList<int> ids;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &i : rows) {
        const QTableWidgetItem *item = m_table->item(i.row(), ColStatus);
        if (item)
            ids << item->data(Qt::UserRole).toInt();
    }
    return ids;
}

void MarkingTab::reload()
{
    MarkingCode::Filter f;
    const QString st = m_status->currentData().toString();
    if (st == QLatin1String("unlinked"))
        f.onlyUnlinked = true;
    else
        f.status = st;
    f.text = m_search->text();
    const QList<MarkingCode::Row> rows = MarkingCode::list(f);
    m_table->setSortingEnabled(false);
    m_table->setRowCount(int(rows.size()));
    for (int r = 0; r < rows.size(); ++r) {
        const MarkingCode::Row &row = rows[r];
        // Код без криптохвоста - код идентификации (КИ): им подают сведения в Систему маркировки, напечатать из него нельзя.
        const bool ki = !MarkingCode::hasCryptoTail(row.code);
        auto *status = new QTableWidgetItem(row.status == QLatin1String("printed") ? (ki ? tr("нанесён (КИ)") : tr("напечатан")) : (ki ? tr("КИ, без криптохвоста") : tr("новый")));
        if (ki)
            status->setToolTip(tr("Код идентификации: в нём нет криптохвоста (групп 91 и 92), поэтому Data Matrix для нанесения на товар из него не получится. "
                                  "Такие коды нужны для документов Системы маркировки; печать для них отключена."));
        // Статус виден сразу по цвету: новый - зелёный, напечатан - спокойный серый, код идентификации - янтарный.
        status->setForeground(ki ? Theme::colors().warn : (row.status == QLatin1String("printed") ? Theme::colors().muted : Theme::colors().ok));
        if (!ki && row.status != QLatin1String("printed")) {
            QFont f = status->font();
            f.setBold(true);
            status->setFont(f);
        }
        status->setData(Qt::UserRole, row.id);
        m_table->setItem(r, ColStatus, status);
        m_table->setItem(r, ColSku, new QTableWidgetItem(row.productId > 0 ? row.sku : tr("- не найден -")));
        m_table->setItem(r, ColName, new QTableWidgetItem(row.name));
        m_table->setItem(r, ColGtin, new QTableWidgetItem(row.gtin));
        m_table->setItem(r, ColSerial, new QTableWidgetItem(row.serial));
        auto *code = new QTableWidgetItem(MarkingCode::shortText(row.code));
        code->setToolTip(QString(row.code).replace(MarkingCode::kGs, QStringLiteral("␝")));
        m_table->setItem(r, ColCode, code);
        m_table->setItem(r, ColSource, new QTableWidgetItem(row.source == QLatin1String("suz") ? tr("СУЗ %1").arg(row.orderId) : (row.source == QLatin1String("scan") ? tr("сканер") : (row.source == QLatin1String("registry") ? tr("реестр ЧЗ") : tr("импорт")))));
        m_table->setItem(r, ColPrints, new QTableWidgetItem(QString::number(row.printCount)));
        m_table->setItem(r, ColCreated, new QTableWidgetItem(row.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))));
    }
    const MarkingCode::Counts c = MarkingCode::counts();
    m_counts->setText(tr("Кодов: %1 · новых: %2 · напечатано: %3 · без товара: %4%5").arg(c.total).arg(c.fresh).arg(c.printed).arg(c.unlinked)
                          .arg(rows.size() >= f.limit ? tr(" · показаны первые %1").arg(f.limit) : QString()));
}

void MarkingTab::showAddResult(const MarkingCode::AddResult &r)
{
    QString text = tr("Добавлено кодов: %1\nУже были в базе: %2\nНе распознано: %3").arg(r.added).arg(r.duplicates).arg(r.invalid);
    if (r.added > 0)
        text += tr("\nС найденным товаром: %1, без товара: %2").arg(r.linked).arg(r.unlinked);
    if (!r.unknownGtins.isEmpty())
        text += tr("\n\nТовар не найден для GTIN: %1\nПривяжите товар кнопкой «Привязать к товару…» (его штрихкод станет GTIN) - или добавьте GTIN как штрихкод товара.")
                    .arg(r.unknownGtins.mid(0, 10).join(QStringLiteral(", ")) + (r.unknownGtins.size() > 10 ? QStringLiteral(" …") : QString()));
    if (!r.problems.isEmpty())
        text += tr("\n\nНе распознано:\n%1").arg(r.problems.mid(0, 8).join(QLatin1Char('\n')));
    QMessageBox::information(this, tr("Коды маркировки"), text);
}

void MarkingTab::addCodes()
{
    AddCodesDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const MarkingCode::AddResult r = MarkingCode::add(dlg.codes(), QStringLiteral("import"), QString(), dlg.productId());
    reload();
    emit codesChanged();
    showAddResult(r);
}

void MarkingTab::addFromFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Файл с кодами маркировки"), QString(), tr("Коды (*.txt *.csv *.xlsx);;Все файлы (*)"));
    if (path.isEmpty())
        return;
    QString err;
    // Выгрузка «Реестр кодов» из личного кабинета (CSV с колонками «Код», «GTIN», «Статус кода»...) - коды идентификации с их статусами.
    QList<MarkingCode::RegistryRow> registry;
    if (MarkingCode::readRegistryCsv(path, &registry, &err)) {
        if (registry.isEmpty()) {
            QMessageBox::information(this, tr("Файл"), tr("Это выгрузка «Реестр кодов», но в ней нет ни одного кода."));
            return;
        }
        int noTail = 0, applied = 0;
        for (const MarkingCode::RegistryRow &r : std::as_const(registry)) {
            noTail += !MarkingCode::hasCryptoTail(MarkingCode::parse(r.code).code);
            applied += r.status != QLatin1String("EMITTED");
        }
        const MarkingCode::AddResult r = MarkingCode::addRegistry(registry);
        reload();
        emit codesChanged();
        showAddResult(r);
        if (noTail > 0)
            QMessageBox::information(this, tr("Реестр кодов"),
                                     tr("Это выгрузка «Реестр кодов» из личного кабинета: строк %1, из них нанесённых на товар (не «Эмитирован») - %2. В %3 кодах нет криптохвоста "
                                        "(это коды идентификации): они добавлены для учёта и документов, а напечатать из них этикетку нельзя - для печати нужны коды, "
                                        "скачанные из заказа («Получение кодов» в кабинете или «Получить коды» в СУЗ).").arg(registry.size()).arg(applied).arg(noTail));
        return;
    }
    if (!err.isEmpty()) {
        QMessageBox::warning(this, tr("Файл"), tr("Не удалось прочитать файл: %1").arg(err));
        return;
    }
    const QStringList codes = readCodesFromFile(path, &err);
    if (!err.isEmpty()) {
        QMessageBox::warning(this, tr("Файл"), tr("Не удалось прочитать файл: %1").arg(err));
        return;
    }
    if (codes.isEmpty()) {
        QMessageBox::information(this, tr("Файл"), tr("В файле не найдено кодов маркировки (строки вида 01…21…)."));
        return;
    }
    const MarkingCode::AddResult r = MarkingCode::add(codes, QStringLiteral("import"));
    reload();
    emit codesChanged();
    showAddResult(r);
}

void MarkingTab::scanned()
{
    const QString text = m_scan->text();
    m_scan->clear();
    if (text.trimmed().isEmpty())
        return;
    const MarkingCode::AddResult r = MarkingCode::add({text}, QStringLiteral("scan"));
    reload();
    emit codesChanged();
    if (r.added == 1) {
        m_hint->setText(tr("Добавлен код: %1").arg(MarkingCode::shortText(MarkingCode::parse(text).code)) + (r.unlinked > 0 ? tr(" (товар по GTIN не найден)") : QString()));
    } else if (r.duplicates == 1) {
        m_hint->setText(tr("Такой код уже есть."));
    } else {
        m_hint->setText(tr("Код не распознан: %1").arg(r.problems.value(0)));
    }
}

void MarkingTab::orderCodes()
{
    MarkingOrderDialog dlg(this);
    dlg.exec();
    reload();
    emit codesChanged();
}

void MarkingTab::settings()
{
    SettingsDialog dlg(this);
    dlg.exec();
}

void MarkingTab::selectNew()
{
    m_table->clearSelection();
    const QItemSelectionModel::SelectionFlags flags = QItemSelectionModel::Select | QItemSelectionModel::Rows;
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (m_table->item(r, ColStatus)->text() == tr("новый"))
            m_table->selectionModel()->select(m_table->model()->index(r, 0), flags);
}

void MarkingTab::print()
{
    QList<int> ids = selectedIds();
    if (ids.isEmpty()) {
        QMessageBox::information(this, tr("Печать"), tr("Выделите коды в таблице (Ctrl или Shift + клик) или нажмите «Выделить новые»."));
        return;
    }
    MarkingPrintDialog dlg(ids, this);
    dlg.exec();
    const int printed = dlg.printedCount();
    reload();
    emit codesChanged();
    // Без модального окна: после работы драйвера принтера окно сообщения могло оказаться позади программы и блокировать её.
    if (printed > 0)
        showNotice(tr("Напечатано этикеток: %1. Коды отмечены напечатанными.").arg(printed));
}

void MarkingTab::showNotice(const QString &text)
{
    m_notice->setText(text);
    m_notice->setVisible(!text.isEmpty());
    QTimer::singleShot(20000, m_notice, [this, text]() {
        if (m_notice->text() == text)
            m_notice->setVisible(false);
    });
}

// Коды идентификации (КИ) выделенных строк (или всех в списке, если ничего не выделено) - файлом «Ввод в оборот» для кабинета.
void MarkingTab::introXml()
{
    QList<int> ids = selectedIds();
    QString source = tr("Выделенные коды");
    if (ids.isEmpty()) {
        for (int r = 0; r < m_table->rowCount(); ++r) {
            const QTableWidgetItem *item = m_table->item(r, ColStatus);
            if (item)
                ids << item->data(Qt::UserRole).toInt();
        }
        source = tr("Все коды в списке");
    }
    if (ids.isEmpty()) {
        QMessageBox::information(this, tr("Ввод в оборот"), tr("В списке нет кодов."));
        return;
    }
    CabinetIntroXmlDialog dlg(MarkingCode::codesByIds(ids), source, this);
    dlg.exec();
}

void MarkingTab::linkSelected()
{
    const QList<int> ids = selectedIds();
    QStringList gtins;
    for (int id : ids) {
        const MarkingCode::Row r = MarkingCode::get(id);
        if (r.productId == 0 && !gtins.contains(r.gtin))
            gtins << r.gtin;
    }
    if (gtins.isEmpty()) {
        QMessageBox::information(this, tr("Привязка"), tr("Среди выделенных нет кодов без товара."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Привязать GTIN к товару"));
    auto *l = new QVBoxLayout(&dlg);
    l->addWidget(new QLabel(tr("GTIN: %1\nВыберите товар: его штрихкод станет GTIN этих кодов (если свободен).").arg(gtins.join(QStringLiteral(", "))), &dlg));
    auto *product = new QComboBox(&dlg);
    fillProducts(product, false, QString());
    l->addWidget(product);
    auto *buttons = new QDialogButtonBox(&dlg);
    buttons->addButton(tr("Привязать"), QDialogButtonBox::AcceptRole);
    buttons->addButton(tr("Отмена"), QDialogButtonBox::RejectRole);
    l->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const int productId = comboProductId(product);
    if (productId <= 0)
        return;
    QStringList errors;
    for (const QString &gtin : std::as_const(gtins)) {
        QString err;
        if (!MarkingCode::linkGtin(gtin, productId, &err))
            errors << err;
    }
    const int n = MarkingCode::relink();
    reload();
    emit codesChanged();
    QMessageBox::information(this, tr("Привязка"), tr("Кодам назначен товар: %1.%2").arg(n).arg(errors.isEmpty() ? QString() : QStringLiteral("\n") + errors.join(QLatin1Char('\n'))));
}

void MarkingTab::removeSelected()
{
    const QList<int> ids = selectedIds();
    if (ids.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Удалить коды"), tr("Удалить выделенные коды (%1)? Напечатанные не удаляются.").arg(ids.size())) != QMessageBox::Yes)
        return;
    const int n = MarkingCode::remove(ids, true);
    reload();
    emit codesChanged();
    if (n < ids.size())
        QMessageBox::information(this, tr("Удалить коды"), tr("Удалено: %1. Остальные уже печатались.").arg(n));
}
