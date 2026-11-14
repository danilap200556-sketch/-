#include "markingtab.h"

#include "appsettings.h"
#include "cabinetxml.h"
#include "theme.h"
#include "cabinetxmldialog.h"
#include "chestnyznak.h"
#include "cryptosign.h"
#include "csvimport.h"
#include "flowlayout.h"
#include "markingorderdialog.h"
#include "markingowner.h"
#include "markingownersdialog.h"
#include "markingprintdialog.h"
#include "xlsx.h"

#include <QApplication>
#include <algorithm>
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
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QSqlQuery>
#include <QAbstractTableModel>
#include <QItemSelection>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace {

enum Column { ColStatus, ColSku, ColName, ColGtin, ColSerial, ColCode, ColOwner, ColWarehouse, ColSource, ColPrints, ColCreated, ColCount };

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

// Склады для выбора (название по алфавиту). ИП и склад - независимые признаки кода: склад не привязан к ИП.
QList<QPair<int, QString>> warehouseList()
{
    QList<QPair<int, QString>> out;
    QSqlQuery q(QStringLiteral("SELECT id, name FROM warehouses ORDER BY lower(name)"));
    while (q.next())
        out.append({q.value(0).toInt(), q.value(1).toString()});
    return out;
}

QString warehouseName(int id)
{
    for (const QPair<int, QString> &w : warehouseList())
        if (w.first == id)
            return w.second;
    return {};
}

// «ИП и склад»: куда записать коды (keepOption - первым пунктом «не менять»: для «Назначить ИП / склад»). Любой из двух можно не задавать:
// без ИП и без склада.
class TargetDialog : public QDialog
{
public:
    TargetDialog(QWidget *parent, const QString &title, const QString &text, bool keepOption, int defaultOwner, int defaultWarehouse)
        : QDialog(parent)
    {
        setWindowTitle(title);
        auto *l = new QVBoxLayout(this);
        auto *label = new QLabel(text, this);
        label->setWordWrap(true);
        l->addWidget(label);
        auto *form = new QFormLayout();
        m_owner = new QComboBox(this);
        if (keepOption)
            m_owner->addItem(tr("не менять"), -1);
        m_owner->addItem(keepOption ? tr("без ИП (снять)") : tr("без ИП"), 0);
        const QList<MarkingOwner::Owner> owners = MarkingOwner::list();
        for (const MarkingOwner::Owner &o : owners)
            m_owner->addItem(o.name, o.id);
        m_warehouse = new QComboBox(this);
        if (keepOption)
            m_warehouse->addItem(tr("не менять"), -1);
        m_warehouse->addItem(keepOption ? tr("без склада (снять)") : tr("без склада"), 0);
        const QList<QPair<int, QString>> warehouses = warehouseList();
        for (const QPair<int, QString> &w : warehouses)
            m_warehouse->addItem(w.second, w.first);
        if (const int oi = m_owner->findData(defaultOwner); oi >= 0)
            m_owner->setCurrentIndex(oi);
        if (const int wi = m_warehouse->findData(defaultWarehouse); wi >= 0)
            m_warehouse->setCurrentIndex(wi);
        if (!owners.isEmpty())
            form->addRow(tr("ИП:"), m_owner);
        if (!warehouses.isEmpty())
            form->addRow(tr("Склад:"), m_warehouse);
        l->addLayout(form);
        auto *buttons = new QDialogButtonBox(this);
        buttons->addButton(keepOption ? tr("Назначить") : tr("Записать"), QDialogButtonBox::AcceptRole);
        buttons->addButton(tr("Отмена"), QDialogButtonBox::RejectRole);
        l->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }
    int ownerId() const { return m_owner->currentData().toInt(); }
    int warehouseId() const { return m_warehouse->currentData().toInt(); }

private:
    QComboBox *m_owner;
    QComboBox *m_warehouse;
};

// ---------------------------------------------------------------- добавление кодов вставкой

class AddCodesDialog : public QDialog
{
public:
    explicit AddCodesDialog(QWidget *parent, int defaultOwner = 0, int defaultWarehouse = 0)
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
        // ИП, на которого записываются коды (если ИП заведены).
        const QList<MarkingOwner::Owner> owners = MarkingOwner::list();
        m_owner = new QComboBox(this);
        m_owner->addItem(tr("без ИП"), 0);
        for (const MarkingOwner::Owner &o : owners)
            m_owner->addItem(o.name, o.id);
        if (const int oi = m_owner->findData(defaultOwner); oi >= 0)
            m_owner->setCurrentIndex(oi);
        // Склад, под остаток которого заказаны коды (необязательно; к ИП не привязан).
        m_warehouse = new QComboBox(this);
        m_warehouse->addItem(tr("без склада"), 0);
        for (const QPair<int, QString> &w : warehouseList())
            m_warehouse->addItem(w.second, w.first);
        if (const int wi = m_warehouse->findData(defaultWarehouse); wi >= 0)
            m_warehouse->setCurrentIndex(wi);
        auto *ownerRow = new QHBoxLayout();
        if (!owners.isEmpty()) {
            ownerRow->addWidget(new QLabel(tr("ИП:"), this));
            ownerRow->addWidget(m_owner, 1);
        }
        if (m_warehouse->count() > 1) {
            ownerRow->addWidget(new QLabel(tr("Склад:"), this));
            ownerRow->addWidget(m_warehouse, 1);
        }
        if (ownerRow->count() > 0)
            l->addLayout(ownerRow);
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
    int ownerId() const { return m_owner->currentData().toInt(); }
    int warehouseId() const { return m_warehouse->currentData().toInt(); }

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
    QComboBox *m_owner;
    QComboBox *m_warehouse;
    QLabel *m_info;
};

// ---------------------------------------------------------------- настройки СУЗ

class SettingsDialog : public QDialog
{
public:
    explicit SettingsDialog(QWidget *parent, int ownerId = 0)
        : QDialog(parent)
        , m_curOwner(ownerId > 0 ? ownerId : 0)
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
        m_loaded = ChestnyZnak::loadSettings(m_curOwner);
        const ChestnyZnak::Settings &s = m_loaded;
        auto *form = new QFormLayout();
        // ИП: у каждого свои идентификатор СУЗ, соединение, токен и сертификат; остальное (адрес СУЗ, группа, способ выпуска...) общее.
        m_owner = new QComboBox(this);
        m_owner->addItem(tr("Общие настройки (без ИП)"), 0);
        for (const MarkingOwner::Owner &o : MarkingOwner::list())
            m_owner->addItem(o.inn.isEmpty() ? o.name : tr("%1 (ИНН %2)").arg(o.name, o.inn), o.id);
        if (const int oi = m_owner->findData(m_curOwner); oi >= 0)
            m_owner->setCurrentIndex(oi);
        else
            m_curOwner = 0;
        form->addRow(tr("ИП:"), m_owner);
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
        connect(m_owner, &QComboBox::currentIndexChanged, this, [this]() { switchOwner(m_owner->currentData().toInt()); });
        connect(findCerts, &QPushButton::clicked, this, [this]() { fillCertificates(true); });
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            const QString before = m_url->text().trimmed();
            saveAll();
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
        s.ownerId = m_curOwner;
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
    // Переключение ИП: правки прежнего ИП остаются черновиком до «Сохранить», поля нового ИП читаются из базы (или из его черновика).
    void switchOwner(int ownerId)
    {
        m_drafts[m_curOwner] = current();
        m_curOwner = ownerId;
        m_loaded = m_drafts.contains(ownerId) ? m_drafts.value(ownerId) : ChestnyZnak::loadSettings(ownerId);
        m_oms->setText(m_loaded.omsId);
        m_conn->setText(m_loaded.omsConnection);
        m_token->setText(m_loaded.clientToken);
        const int ci = m_cert->findData(m_loaded.certThumbprint);
        m_cert->setCurrentIndex(ci >= 0 ? ci : 0);
        m_state->clear();
        showTokenState();
    }
    // Сохраняет всех ИП, у которых что-то менялось в этом окне, и текущего; общие поля (адрес СУЗ, группа...) берутся из формы для всех.
    void saveAll()
    {
        m_drafts[m_curOwner] = current();
        const ChestnyZnak::Settings form = current();
        for (auto it = m_drafts.begin(); it != m_drafts.end(); ++it) {
            ChestnyZnak::Settings d = it.value();
            d.baseUrl = form.baseUrl;
            d.signerUrl = form.signerUrl;
            d.templateId = form.templateId;
            d.productGroup = form.productGroup;
            d.releaseMethodType = form.releaseMethodType;
            d.contactPerson = form.contactPerson;
            d.ownerId = it.key();
            ChestnyZnak::saveSettings(d);
        }
    }
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
        m_drafts.remove(m_curOwner);
        m_loaded = ChestnyZnak::loadSettings(m_curOwner);
        m_state->setText(tr("Токен получен и сохранён, действует 10 часов (до %1). Дальше программа обновит его сама.")
                             .arg(m_issuedAt.addSecs(10 * 3600).toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))));
    }

    ChestnyZnak::Settings m_loaded;
    int m_curOwner = 0;
    QMap<int, ChestnyZnak::Settings> m_drafts;
    QComboBox *m_owner = nullptr;
    QString m_issuedToken;
    QDateTime m_issuedAt;
    QLineEdit *m_url, *m_oms, *m_conn, *m_token, *m_signer;
    QComboBox *m_cert, *m_group, *m_release;
    QLineEdit *m_contact;
    QSpinBox *m_template;
    QLabel *m_state;
};

} // namespace

// ---------------------------------------------------------------- таблица кодов

// Модель вместо QTableWidget: ячейки не создаются заранее (их тысячи), текст и цвета считаются только для видимых строк, а перезагрузка списка -
// это один сброс модели. С QTableWidget на 2000 строках каждая перезагрузка (после каждого скана!) создавала ~18 000 объектов и пересчитывала
// ширину столбцов «по содержимому» на каждую ячейку.
class MarkingModel : public QAbstractTableModel
{
public:
    explicit MarkingModel(QObject *parent) : QAbstractTableModel(parent)
    {
        m_headers = {tr("Статус"), tr("Артикул"), tr("Название"), tr("GTIN"), tr("Серийный номер"), tr("Код"), tr("ИП"), tr("Склад"), tr("Источник"), tr("Печатей"), tr("Создан")};
        m_new = tr("новый");
        m_printed = tr("напечатан");
        m_kiNew = tr("КИ, без криптохвоста");
        m_kiPrinted = tr("нанесён (КИ)");
        m_kiTip = tr("Код идентификации: в нём нет криптохвоста (групп 91 и 92), поэтому Data Matrix для нанесения на товар из него не получится. "
                     "Такие коды нужны для документов Системы маркировки; печать для них отключена.");
        m_notFound = tr("- не найден -");
        m_boldFont.setBold(true);
    }

    void setRows(QList<MarkingCode::Row> rows)
    {
        beginResetModel();
        m_rows = std::move(rows);
        m_noTail.resize(m_rows.size());
        for (int i = 0; i < m_rows.size(); ++i)
            m_noTail[i] = !MarkingCode::hasCryptoTail(m_rows[i].code);
        endResetModel();
    }
    const MarkingCode::Row &row(int r) const { return m_rows.at(r); }
    // Новый код, который можно печатать (у кода идентификации криптохвоста нет - его не печатают).
    bool isNewPrintable(int r) const { return m_rows.at(r).status != QLatin1String("printed") && !m_noTail.at(r); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override { return parent.isValid() ? 0 : int(m_rows.size()); }
    int columnCount(const QModelIndex &parent = QModelIndex()) const override { return parent.isValid() ? 0 : ColCount; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < m_headers.size())
            return m_headers.at(section);
        return {};
    }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size())
            return {};
        const MarkingCode::Row &r = m_rows.at(index.row());
        const bool ki = m_noTail.at(index.row());
        const bool printed = r.status == QLatin1String("printed");
        if (role == Qt::UserRole)
            return r.id;
        switch (index.column()) {
        case ColStatus:
            if (role == Qt::DisplayRole)
                return printed ? (ki ? m_kiPrinted : m_printed) : (ki ? m_kiNew : m_new);
            // Статус виден сразу по цвету: новый - зелёный, напечатан - спокойный серый, код идентификации - янтарный.
            if (role == Qt::ForegroundRole)
                return ki ? Theme::colors().warn : (printed ? Theme::colors().muted : Theme::colors().ok);
            if (role == Qt::FontRole && !ki && !printed)
                return m_boldFont;
            if (role == Qt::ToolTipRole && ki)
                return m_kiTip;
            break;
        case ColSku:
            if (role == Qt::DisplayRole)
                return r.productId > 0 ? r.sku : m_notFound;
            break;
        case ColName:
            if (role == Qt::DisplayRole)
                return r.name;
            break;
        case ColGtin:
            if (role == Qt::DisplayRole)
                return r.gtin;
            break;
        case ColSerial:
            if (role == Qt::DisplayRole)
                return r.serial;
            break;
        case ColCode:
            if (role == Qt::DisplayRole)
                return MarkingCode::shortText(r.code);
            if (role == Qt::ToolTipRole)
                return QString(r.code).replace(MarkingCode::kGs, QStringLiteral("␝"));
            break;
        case ColOwner:
            if (role == Qt::DisplayRole)
                return r.ownerName;
            break;
        case ColWarehouse:
            if (role == Qt::DisplayRole)
                return r.warehouseName;
            break;
        case ColSource:
            if (role == Qt::DisplayRole)
                return r.source == QLatin1String("suz") ? tr("СУЗ %1").arg(r.orderId)
                                                        : (r.source == QLatin1String("scan") ? tr("сканер") : (r.source == QLatin1String("registry") ? tr("реестр ЧЗ") : tr("импорт")));
            break;
        case ColPrints:
            if (role == Qt::DisplayRole)
                return r.printCount;
            break;
        case ColCreated:
            if (role == Qt::DisplayRole)
                return r.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"));
            break;
        }
        return {};
    }

private:
    QList<MarkingCode::Row> m_rows;
    QList<bool> m_noTail;
    QStringList m_headers;
    QString m_new, m_printed, m_kiNew, m_kiPrinted, m_kiTip, m_notFound;
    QFont m_boldFont;
};

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
    m_owner = new QComboBox(this);
    m_owner->setToolTip(tr("ИП, чьи коды показаны. Сканер записывает коды на выбранного ИП."));
    m_warehouse = new QComboBox(this);
    m_warehouse->setToolTip(tr("Склад, под остаток которого заказаны коды (с ИП он не связан). Сканер записывает коды на выбранный склад."));
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Поиск: артикул, название, GTIN, серийный номер"));
    m_search->setClearButtonEnabled(true);
    m_scan = new QLineEdit(this);
    m_scan->setPlaceholderText(tr("Сканер: отсканируйте код маркировки, Enter - добавить"));
    m_scan->setMinimumWidth(260);
    filters->addWidget(m_status);
    filters->addWidget(m_owner);
    filters->addWidget(m_warehouse);
    filters->addWidget(m_search, 1);
    filters->addWidget(m_scan, 1);
    layout->addLayout(filters);

    auto *buttons = new FlowLayout(nullptr, 6);
    auto *addBtn = new QPushButton(tr("Добавить коды..."), this);
    auto *fileBtn = new QPushButton(tr("Из файла..."), this);
    auto *orderBtn = new QPushButton(tr("Заказать в СУЗ..."), this);
    auto *printBtn = new QPushButton(tr("Печать на термоэтикетке..."), this);
    auto *introBtn = new QPushButton(tr("Ввод в оборот (Excel/XML)..."), this);
    introBtn->setToolTip(tr("Сохранить выделенные коды (или все в списке) файлом «Ввод в оборот» для загрузки в личный кабинет «Честного знака»"));
    auto *selNewBtn = new QPushButton(tr("Выделить новые"), this);
    auto *linkBtn = new QPushButton(tr("Привязать к товару..."), this);
    auto *delBtn = new QPushButton(tr("Удалить"), this);
    auto *setBtn = new QPushButton(tr("Настройки СУЗ..."), this);
    auto *assignBtn = new QPushButton(tr("Назначить ИП / склад..."), this);
    assignBtn->setToolTip(tr("Записать выделенные коды на ИП и/или склад (или снять): так разделяют коды, добавленные раньше. ИП и склад задаются независимо."));
    auto *ownersBtn = new QPushButton(tr("ИП..."), this);
    ownersBtn->setToolTip(tr("Справочник ИП: название, ИНН, настройки СУЗ каждого. Склады к ИП не привязаны: ИП кабинета Маркета - в настройках кабинета."));
    selNewBtn->setToolTip(tr("Выделить в списке все коды, которые ещё не печатались"));
    linkBtn->setToolTip(tr("Выделенным кодам без товара назначить товар: его штрихкод станет GTIN этих кодов"));
    delBtn->setToolTip(tr("Удалить выделенные коды, которые ещё не печатались (напечатанные могут быть уже наклеены - их не трогаем)"));
    for (QPushButton *b : {addBtn, fileBtn, orderBtn, printBtn, introBtn, selNewBtn, linkBtn, assignBtn, delBtn, ownersBtn, setBtn})
        buttons->addWidget(b);
    layout->addLayout(buttons);

    m_counts = new QLabel(this);
    layout->addWidget(m_counts);
    m_notice = new QLabel(this);
    m_notice->setObjectName(QStringLiteral("noticeBanner"));
    m_notice->setWordWrap(true);
    m_notice->setVisible(false);
    layout->addWidget(m_notice);

    m_model = new MarkingModel(this);
    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    QHeaderView *h = m_table->horizontalHeader();
    h->setResizeContentsPrecision(40);   // ширина «по содержимому» считается по первым строкам, а не по всем двум тысячам
    h->setSectionResizeMode(ColName, QHeaderView::Stretch);
    h->setSectionResizeMode(ColCode, QHeaderView::Stretch);
    for (int c : {ColStatus, ColSku, ColGtin, ColSerial, ColOwner, ColWarehouse, ColSource, ColPrints, ColCreated})
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
    connect(assignBtn, &QPushButton::clicked, this, &MarkingTab::assignTarget);
    connect(ownersBtn, &QPushButton::clicked, this, &MarkingTab::manageOwners);
    reloadOwners();
    connect(m_status, &QComboBox::currentIndexChanged, this, [this]() { reload(); });
    connect(m_owner, &QComboBox::currentIndexChanged, this, [this]() { reload(); });
    connect(m_warehouse, &QComboBox::currentIndexChanged, this, [this]() { reload(); });
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_searchTimer, &QTimer::timeout, this, [this]() { reload(); });
    connect(m_scan, &QLineEdit::returnPressed, this, &MarkingTab::scanned);
}

void MarkingTab::refresh()
{
    reloadOwners();
    reload();
}

int MarkingTab::ownerFilter() const
{
    return m_owner->currentData().toInt();
}

int MarkingTab::warehouseFilter() const
{
    return m_warehouse->currentData().toInt();
}

// Выпадающие списки ИП и складов сверху (выбор сохраняется, если такой ИП/склад ещё есть).
void MarkingTab::reloadOwners()
{
    const QList<MarkingOwner::Owner> owners = MarkingOwner::list();
    {
        const int keep = m_owner->count() > 0 ? m_owner->currentData().toInt() : -1;
        QSignalBlocker block(m_owner);
        m_owner->clear();
        m_owner->addItem(tr("Все ИП"), -1);
        m_owner->addItem(tr("Без ИП"), 0);
        for (const MarkingOwner::Owner &o : owners)
            m_owner->addItem(o.name, o.id);
        const int i = m_owner->findData(keep);
        m_owner->setCurrentIndex(i >= 0 ? i : 0);
        m_owner->setVisible(!owners.isEmpty());   // пока ИП не заведены, список не нужен
    }
    const QList<QPair<int, QString>> warehouses = warehouseList();
    {
        const int keep = m_warehouse->count() > 0 ? m_warehouse->currentData().toInt() : -1;
        QSignalBlocker block(m_warehouse);
        m_warehouse->clear();
        m_warehouse->addItem(tr("Все склады"), -1);
        m_warehouse->addItem(tr("Без склада"), 0);
        for (const QPair<int, QString> &w : warehouses)
            m_warehouse->addItem(w.second, w.first);
        const int i = m_warehouse->findData(keep);
        m_warehouse->setCurrentIndex(i >= 0 ? i : 0);
        m_warehouse->setVisible(!warehouses.isEmpty());
    }
    m_table->setColumnHidden(ColOwner, owners.isEmpty());
    m_table->setColumnHidden(ColWarehouse, warehouses.isEmpty());
}

// Куда записать новые коды. Без заведённых ИП и складов - «без ИП, без склада», как раньше. Иначе спрашиваем: ИП обязателен, если они есть (для
// каждого свой кабинет и свои заказы), склад необязателен. По умолчанию - что выбрано в верхних списках, а если там «все» - как в прошлый раз.
bool MarkingTab::targetForNewCodes(const QString &what, int *ownerId, int *warehouseId)
{
    *ownerId = 0;
    *warehouseId = 0;
    if (MarkingOwner::list().isEmpty() && warehouseList().isEmpty())
        return true;
    int defOwner = ownerFilter() > 0 ? ownerFilter() : AppSettings::get(QStringLiteral("cz_last_owner")).toInt();
    int defWarehouse = warehouseFilter() > 0 ? warehouseFilter() : AppSettings::get(QStringLiteral("cz_last_warehouse")).toInt();
    TargetDialog dlg(this, tr("Куда записать коды"), tr("%1: на какого ИП и под какой склад записать коды? ИП и склад независимы; любой из них можно не задавать.").arg(what),
                     false, defOwner, defWarehouse);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    *ownerId = dlg.ownerId();
    *warehouseId = dlg.warehouseId();
    AppSettings::set(QStringLiteral("cz_last_owner"), QString::number(*ownerId));
    AppSettings::set(QStringLiteral("cz_last_warehouse"), QString::number(*warehouseId));
    return true;
}

QList<int> MarkingTab::selectedIds() const
{
    QList<int> ids;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    ids.reserve(rows.size());
    for (const QModelIndex &i : rows)
        ids << m_model->row(i.row()).id;
    std::sort(ids.begin(), ids.end());   // в порядке номеров, а не выделения
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
    f.ownerId = ownerFilter();
    f.warehouseId = warehouseFilter();
    f.limit = 5000;   // таблица на модели держит тысячи строк без задержек (на QTableWidget было 2000)
    MarkingCode::Counts c;
    QList<MarkingCode::Row> rows = MarkingCode::listWithCounts(f, &c);   // список и счётчики - один запрос
    const int shown = int(rows.size());
    m_model->setRows(std::move(rows));
    m_counts->setText(tr("Кодов: %1 · новых: %2 · напечатано: %3 · без товара: %4%5").arg(c.total).arg(c.fresh).arg(c.printed).arg(c.unlinked)
                          .arg(shown >= f.limit ? tr(" · показаны первые %1").arg(f.limit) : QString()));
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
    AddCodesDialog dlg(this, ownerFilter() > 0 ? ownerFilter() : 0, warehouseFilter() > 0 ? warehouseFilter() : 0);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const MarkingCode::AddResult r = MarkingCode::add(dlg.codes(), QStringLiteral("import"), QString(), dlg.productId(), QStringLiteral("new"), dlg.ownerId(), dlg.warehouseId());
    reload();
    emit codesChanged();
    showAddResult(r);
}

void MarkingTab::addFromFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Файл с кодами маркировки"), QString(), tr("Коды (*.txt *.csv *.xlsx);;Все файлы (*)"));
    if (path.isEmpty())
        return;
    int owner = 0, warehouse = 0;
    if (!targetForNewCodes(tr("Файл «%1»").arg(QFileInfo(path).fileName()), &owner, &warehouse))
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
        const MarkingCode::AddResult r = MarkingCode::addRegistry(registry, owner, warehouse);
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
    const MarkingCode::AddResult r = MarkingCode::add(codes, QStringLiteral("import"), QString(), 0, QStringLiteral("new"), owner, warehouse);
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
    // С заведёнными ИП код записывается на того, кто выбран в списке сверху; без выбора код ушёл бы не туда. Склад необязателен: «все склады» - без склада.
    if (ownerFilter() <= 0 && !MarkingOwner::list().isEmpty()) {
        m_hint->setText(tr("Выберите ИП в списке сверху: отсканированный код нужно записать на конкретного ИП (код не добавлен, отсканируйте его снова)."));
        return;
    }
    const MarkingCode::AddResult r = MarkingCode::add({text}, QStringLiteral("scan"), QString(), 0, QStringLiteral("new"), qMax(0, ownerFilter()), qMax(0, warehouseFilter()));
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
    MarkingOrderDialog dlg(this, ownerFilter() > 0 ? ownerFilter() : 0, warehouseFilter() > 0 ? warehouseFilter() : 0);
    dlg.exec();
    reload();
    emit codesChanged();
}

void MarkingTab::settings()
{
    SettingsDialog dlg(this, ownerFilter() > 0 ? ownerFilter() : 0);
    dlg.exec();
}

// Выделенные коды записываются на выбранные ИП и/или склад (или они снимаются). Каждое из двух можно не менять.
void MarkingTab::assignTarget()
{
    const QList<int> ids = selectedIds();
    if (ids.isEmpty()) {
        QMessageBox::information(this, tr("Назначить ИП / склад"), tr("Выделите коды в таблице (Ctrl или Shift + клик)."));
        return;
    }
    if (MarkingOwner::list().isEmpty() && warehouseList().isEmpty()) {
        QMessageBox::information(this, tr("Назначить ИП / склад"), tr("ИП ещё не заведены (кнопка «ИП…»), а складов нет."));
        return;
    }
    TargetDialog dlg(this, tr("Назначить ИП / склад"), tr("Записать выделенные коды (%1) на ИП и склад. «Не менять» оставляет прежнее значение.").arg(ids.size()), true, -1, -1);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const int owner = dlg.ownerId(), warehouse = dlg.warehouseId();
    int changedOwner = 0, changedWarehouse = 0;
    if (owner >= 0)
        changedOwner = MarkingCode::assignOwner(ids, owner);
    if (warehouse >= 0)
        changedWarehouse = MarkingCode::assignWarehouse(ids, warehouse);
    reload();
    emit codesChanged();
    QStringList parts;
    if (owner >= 0)
        parts << tr("ИП «%1»: изменено кодов - %2").arg(owner > 0 ? MarkingOwner::nameOf(owner) : tr("без ИП")).arg(changedOwner);
    if (warehouse >= 0)
        parts << tr("склад «%1»: изменено кодов - %2").arg(warehouse > 0 ? warehouseName(warehouse) : tr("без склада")).arg(changedWarehouse);
    showNotice(parts.isEmpty() ? tr("Ничего не изменено.") : parts.join(QStringLiteral("; ")) + QLatin1Char('.'));
}

void MarkingTab::manageOwners()
{
    MarkingOwnersDialog dlg(this);
    connect(&dlg, &MarkingOwnersDialog::suzSettingsRequested, this, [this](int ownerId) {
        SettingsDialog settingsDialog(this, ownerId);
        settingsDialog.exec();
    });
    dlg.exec();
    reloadOwners();
    reload();
    emit codesChanged();
}

void MarkingTab::selectNew()
{
    // Одним выделением на всё: диапазоны подряд идущих новых кодов (а не по сигналу на каждую строку).
    QItemSelection selection;
    const int n = m_model->rowCount();
    for (int r = 0; r < n;) {
        if (!m_model->isNewPrintable(r)) {
            ++r;
            continue;
        }
        int end = r;
        while (end + 1 < n && m_model->isNewPrintable(end + 1))
            ++end;
        selection.select(m_model->index(r, 0), m_model->index(end, ColCount - 1));
        r = end + 1;
    }
    m_table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
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
    const QModelIndexList selected = m_table->selectionModel()->selectedRows();
    QList<int> ids;
    QSet<int> owners;
    QString source = tr("Выделенные коды");
    if (!selected.isEmpty()) {
        for (const QModelIndex &i : selected) {
            ids << m_model->row(i.row()).id;
            owners.insert(m_model->row(i.row()).ownerId);
        }
        std::sort(ids.begin(), ids.end());
    } else {
        const int n = m_model->rowCount();
        ids.reserve(n);
        for (int r = 0; r < n; ++r) {
            ids << m_model->row(r).id;
            owners.insert(m_model->row(r).ownerId);
        }
        source = tr("Все коды в списке");
    }
    if (ids.isEmpty()) {
        QMessageBox::information(this, tr("Ввод в оборот"), tr("В списке нет кодов."));
        return;
    }
    // У каждого ИП свой кабинет и свой ИНН: файл для кабинета делается по одному ИП.
    if (owners.size() > 1) {
        QMessageBox::warning(this, tr("Ввод в оборот"),
                             tr("В выбранных кодах - разные ИП (%1). Файл «Ввод в оборот» загружается в кабинет одного ИП: выберите ИП в списке сверху "
                                "или выделите коды одного ИП.").arg(owners.size()));
        return;
    }
    const int owner = owners.isEmpty() ? 0 : *owners.constBegin();
    const MarkingOwner::Owner o = MarkingOwner::get(owner);
    if (o.id > 0)
        source = tr("%1 (%2)").arg(source, o.name);
    CabinetIntroXmlDialog dlg(MarkingCode::codesByIds(ids), source, this);
    if (o.id > 0 && !o.inn.isEmpty())
        dlg.setInn(o.inn);   // ИНН этого ИП вместо запомненного прежде
    dlg.exec();
}

void MarkingTab::linkSelected()
{
    QStringList gtins;
    const QModelIndexList selected = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &i : selected) {
        const MarkingCode::Row &r = m_model->row(i.row());   // уже загружены: в базу ходить не нужно
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
