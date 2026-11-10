#include "cabinetxmldialog.h"

#include "appsettings.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace {

const char kDirKey[] = "cz/xmlDir";
const char kFormatKey[] = "cz/introFormat";
const char kBracketsKey[] = "cz/introBrackets";

QString defaultDirectory()
{
    const QString saved = QSettings().value(QString::fromLatin1(kDirKey)).toString();
    if (!saved.isEmpty())
        return saved;
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

QLabel *wrapped(const QString &text, QWidget *parent, bool gray = false)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    if (gray)
        l->setStyleSheet(QStringLiteral("color: gray;"));
    return l;
}

QString stamp() { return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")); }

} // namespace

// ---------------------------------------------------------------------------------------------------------------- заказ кодов

CabinetOrderXmlDialog::CabinetOrderXmlDialog(const QList<CabinetXml::OrderItem> &items, QWidget *parent)
    : QDialog(parent)
    , m_items(items)
{
    setWindowTitle(tr("XML заказа кодов для личного кабинета"));
    resize(680, 620);
    auto *l = new QVBoxLayout(this);
    l->addWidget(wrapped(tr("Файл заказа по схеме «Честного знака» (urn:oms.order). Загрузите его в личном кабинете: <b>Заказы → Активные → Импортировать</b> → "
                            "выберите товарную группу → файл → «Загрузить», затем отправьте заказ на исполнение (там вы подписываете его своим сертификатом). "
                            "Подпись в программе не нужна. Длинный список разбивается на несколько файлов: в файле не больше 10 товарных позиций."),
                         this));

    auto *form = new QFormLayout();
    m_group = new QComboBox(this);
    for (const QString &g : CabinetXml::groups())
        m_group->addItem(CabinetXml::groupTitle(g), g);
    m_contact = new QLineEdit(this);
    m_contact->setMaxLength(128);
    m_contact->setPlaceholderText(tr("необязательно: контактное лицо (до 128 знаков)"));
    m_release = new QComboBox(this);
    m_create = new QComboBox(this);
    for (const QString &c : CabinetXml::createMethods())
        m_create->addItem(QStringLiteral("%1 - %2").arg(c, CabinetXml::createMethodTitle(c)), c);
    m_create->setToolTip(tr("Способ изготовления. SELF_MADE - самостоятельно (как в вашем образце заказа); CEM, CM, CL, CA указываются только при работе через сервис-провайдера."));
    m_template = new QComboBox(this);
    m_cis = new QComboBox(this);
    for (const char *c : {"UNIT", "BUNDLE", "SET"})
        m_cis->addItem(QString::fromLatin1(c), QString::fromLatin1(c));
    m_cisLabel = new QLabel(tr("Тип КМ:"), this);
    m_producer = new QLineEdit(this);
    m_producer->setPlaceholderText(tr("необязательно: ИНН владельца карточки товара"));
    m_productionOrder = new QLineEdit(this);
    m_productionOrder->setPlaceholderText(tr("необязательно: идентификатор производственного заказа"));
    m_operatorSerials = new QRadioButton(tr("серийные номера присваивает система (OPERATOR)"), this);
    m_ownSerials = new QRadioButton(tr("свои серийные номера: программа сама придумает случайные по 12 знаков (SELF_MADE)"), this);
    m_operatorSerials->setChecked(true);
    form->addRow(tr("Товарная группа:"), m_group);
    form->addRow(tr("Контактное лицо:"), m_contact);
    form->addRow(tr("Способ выпуска в оборот:"), m_release);
    form->addRow(tr("Способ изготовления:"), m_create);
    form->addRow(tr("Шаблон КМ:"), m_template);
    form->addRow(m_cisLabel, m_cis);
    form->addRow(tr("Серийные номера:"), m_operatorSerials);
    form->addRow(QString(), m_ownSerials);
    form->addRow(tr("ИНН владельца карточки:"), m_producer);
    form->addRow(tr("Произв. заказ:"), m_productionOrder);
    l->addLayout(form);

    auto *dirRow = new QHBoxLayout();
    m_dir = new QLineEdit(defaultDirectory(), this);
    auto *browse = new QPushButton(tr("Папка…"), this);
    browse->setAutoDefault(false);
    dirRow->addWidget(new QLabel(tr("Сохранить в:"), this));
    dirRow->addWidget(m_dir, 1);
    dirRow->addWidget(browse);
    l->addLayout(dirRow);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    l->addWidget(m_summary);
    m_state = new QLabel(this);
    m_state->setWordWrap(true);
    l->addWidget(m_state);
    l->addStretch(1);

    auto *buttons = new QDialogButtonBox(this);
    auto *save = buttons->addButton(tr("Сохранить файлы"), QDialogButtonBox::ActionRole);
    save->setProperty("primary", true);
    m_openBtn = buttons->addButton(tr("Открыть папку"), QDialogButtonBox::ActionRole);
    m_openBtn->setEnabled(false);
    buttons->addButton(tr("Закрыть"), QDialogButtonBox::RejectRole);
    l->addWidget(buttons);
    for (QPushButton *b : findChildren<QPushButton *>())
        b->setAutoDefault(false);

    // Прежние значения
    CabinetXml::OrderOptions saved;
    saved.group = AppSettings::get(QStringLiteral("cz_xml_group"), saved.group);
    saved.contactPerson = AppSettings::get(QStringLiteral("cz_xml_contact"));
    saved.releaseMethod = AppSettings::get(QStringLiteral("cz_xml_release"), saved.releaseMethod);
    saved.createMethod = AppSettings::get(QStringLiteral("cz_xml_create"), saved.createMethod);
    saved.ownSerials = AppSettings::get(QStringLiteral("cz_xml_own_serials")) == QLatin1String("1");
    saved.templateId = AppSettings::get(QStringLiteral("cz_xml_template")).toInt();
    saved.producerInn = AppSettings::get(QStringLiteral("cz_xml_producer"));
    saved.productionOrderId = AppSettings::get(QStringLiteral("cz_xml_prod_order"));
    saved.cisType = AppSettings::get(QStringLiteral("cz_xml_cis"), saved.cisType);
    setOptions(saved);

    connect(m_group, &QComboBox::currentIndexChanged, this, [this]() { groupChanged(); });
    for (QComboBox *c : {m_release, m_create, m_template, m_cis})
        connect(c, &QComboBox::currentIndexChanged, this, [this]() { refreshSummary(); });
    for (QLineEdit *e : {m_contact, m_producer, m_productionOrder})
        connect(e, &QLineEdit::textChanged, this, [this]() { refreshSummary(); });
    connect(m_ownSerials, &QRadioButton::toggled, this, [this]() { refreshSummary(); });
    connect(browse, &QPushButton::clicked, this, [this]() { chooseDirectory(); });
    connect(save, &QPushButton::clicked, this, [this]() { exportFiles(); });
    connect(m_openBtn, &QPushButton::clicked, this, [this]() { openDirectory(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    refreshSummary();
}

CabinetXml::OrderOptions CabinetOrderXmlDialog::options() const
{
    CabinetXml::OrderOptions o;
    o.group = m_group->currentData().toString();
    o.contactPerson = m_contact->text().trimmed();
    o.releaseMethod = m_release->currentData().toString();
    o.createMethod = m_create->currentData().toString();
    o.ownSerials = m_ownSerials->isChecked();
    o.templateId = m_template->currentData().toInt();
    o.producerInn = m_producer->text().trimmed();
    o.productionOrderId = m_productionOrder->text().trimmed();
    o.cisType = m_cis->currentData().toString();
    return o;
}

void CabinetOrderXmlDialog::setOptions(const CabinetXml::OrderOptions &o)
{
    const QSignalBlocker block(m_group);
    const int gi = m_group->findData(o.group);
    m_group->setCurrentIndex(gi >= 0 ? gi : 0);
    // Списки зависят от группы
    const QString group = m_group->currentData().toString();
    m_release->clear();
    for (const QString &r : CabinetXml::releaseMethods(group))
        m_release->addItem(QStringLiteral("%1 - %2").arg(r, CabinetXml::releaseMethodTitle(r)), r);
    m_template->clear();
    for (int t : CabinetXml::templateIds(group))
        m_template->addItem(QString::number(t), t);
    const int ri = m_release->findData(o.releaseMethod);
    m_release->setCurrentIndex(ri >= 0 ? ri : 0);
    const int ci = m_create->findData(o.createMethod);
    m_create->setCurrentIndex(ci >= 0 ? ci : 0);
    const int ti = m_template->findData(o.templateId > 0 ? o.templateId : CabinetXml::defaultTemplateId(group));
    m_template->setCurrentIndex(ti >= 0 ? ti : 0);
    const int si = m_cis->findData(o.cisType);
    m_cis->setCurrentIndex(si >= 0 ? si : 0);
    m_contact->setText(o.contactPerson);
    m_producer->setText(o.producerInn);
    m_productionOrder->setText(o.productionOrderId);
    (o.ownSerials ? m_ownSerials : m_operatorSerials)->setChecked(true);
    const bool lp = group == QLatin1String("lp");
    m_cis->setVisible(lp);
    m_cisLabel->setVisible(lp);
}

void CabinetOrderXmlDialog::groupChanged()
{
    CabinetXml::OrderOptions o = options();
    o.templateId = 0; // у новой группы свой шаблон по умолчанию
    setOptions(o);
    refreshSummary();
}

void CabinetOrderXmlDialog::setDirectory(const QString &directory)
{
    m_dir->setText(directory);
}

void CabinetOrderXmlDialog::refreshSummary()
{
    QMap<QString, qint64> byGtin;
    for (const auto &it : m_items)
        byGtin[it.gtin.trimmed()] += it.quantity;
    qint64 total = 0;
    for (qint64 q : byGtin)
        total += q;
    const int positions = byGtin.size();
    const int filesCount = (positions + CabinetXml::kProductsPerOrderFile - 1) / CabinetXml::kProductsPerOrderFile;
    QString text = tr("Позиций (разных GTIN): <b>%1</b>, кодов всего: <b>%2</b> → файлов: <b>%3</b>.").arg(positions).arg(total).arg(qMax(filesCount, 0));
    const QString problem = CabinetXml::checkOrder(m_items, options());
    if (!problem.isEmpty())
        text += QStringLiteral("<br><span style=\"color:#c0392b\">") + tr("Нельзя сохранить: %1.").arg(problem.toHtmlEscaped()) + QStringLiteral("</span>");
    else if (options().ownSerials)
        text += QStringLiteral("<br>") + tr("Свои серийные номера придумываются случайно (цифры и латинские буквы), в кабинете коды всё равно придут с ключом проверки и подписью.");
    m_summary->setText(text);
}

QString CabinetOrderXmlDialog::stateText() const
{
    return m_state->text();
}

void CabinetOrderXmlDialog::saveSettings() const
{
    const CabinetXml::OrderOptions o = options();
    AppSettings::set(QStringLiteral("cz_xml_group"), o.group);
    AppSettings::set(QStringLiteral("cz_xml_contact"), o.contactPerson);
    AppSettings::set(QStringLiteral("cz_xml_release"), o.releaseMethod);
    AppSettings::set(QStringLiteral("cz_xml_create"), o.createMethod);
    AppSettings::set(QStringLiteral("cz_xml_own_serials"), o.ownSerials ? QStringLiteral("1") : QStringLiteral("0"));
    AppSettings::set(QStringLiteral("cz_xml_template"), QString::number(o.templateId));
    AppSettings::set(QStringLiteral("cz_xml_producer"), o.producerInn.isEmpty() ? QString(QLatin1String("")) : o.producerInn);
    AppSettings::set(QStringLiteral("cz_xml_prod_order"), o.productionOrderId.isEmpty() ? QString(QLatin1String("")) : o.productionOrderId);
    AppSettings::set(QStringLiteral("cz_xml_cis"), o.cisType);
    QSettings().setValue(QString::fromLatin1(kDirKey), m_dir->text().trimmed());
}

void CabinetOrderXmlDialog::chooseDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Куда сохранить файлы"), m_dir->text());
    if (!dir.isEmpty())
        m_dir->setText(dir);
}

void CabinetOrderXmlDialog::openDirectory()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_dir->text().trimmed()));
}

int CabinetOrderXmlDialog::exportFiles()
{
    m_files.clear();
    m_error.clear();
    const QList<QByteArray> xml = CabinetXml::orderFiles(m_items, options(), &m_error);
    if (xml.isEmpty()) {
        m_state->setText(QStringLiteral("<span style=\"color:#c0392b\">") + tr("Файлы не сохранены: %1.").arg(m_error.toHtmlEscaped()) + QStringLiteral("</span>"));
        return 0;
    }
    if (!CabinetXml::writeFiles(m_dir->text(), QStringLiteral("zakaz-%1-%2").arg(options().group, stamp()), xml, &m_files, &m_error)) {
        m_state->setText(QStringLiteral("<span style=\"color:#c0392b\">") + tr("Файлы не сохранены: %1.").arg(m_error.toHtmlEscaped()) + QStringLiteral("</span>"));
        m_files.clear();
        return 0;
    }
    saveSettings();
    m_state->setText(tr("Сохранено файлов: <b>%1</b> в папку %2. Загрузите их в кабинете: Заказы → Активные → Импортировать.").arg(m_files.size()).arg(m_dir->text().toHtmlEscaped()));
    m_openBtn->setEnabled(true);
    return m_files.size();
}

// ---------------------------------------------------------------------------------------------------------------- ввод в оборот

CabinetIntroXmlDialog::CabinetIntroXmlDialog(const QStringList &codes, const QString &source, QWidget *parent)
    : QDialog(parent)
    , m_codes(codes)
{
    setWindowTitle(tr("«Ввод в оборот» для личного кабинета"));
    resize(660, 460);
    auto *l = new QVBoxLayout(this);
    l->addWidget(wrapped(tr("Список кодов идентификации (КИ - «01…21…», без криптохвоста) для личного кабинета. Загрузите файл в кабинете и подпишите там. "
                            "Вводите в оборот коды, которые уже нанесены на товар."),
                         this));
    auto *form = new QFormLayout();
    m_format = new QComboBox(this);
    for (CabinetXml::IntroFormat f : {CabinetXml::IntroFormat::Xls, CabinetXml::IntroFormat::Xlsx, CabinetXml::IntroFormat::Xml})
        m_format->addItem(CabinetXml::introFormatTitle(f), int(f));
    const int savedFormat = QSettings().value(QString::fromLatin1(kFormatKey), int(CabinetXml::IntroFormat::Xls)).toInt();
    const int formatIndex = m_format->findData(savedFormat);
    m_format->setCurrentIndex(formatIndex >= 0 ? formatIndex : 0);
    m_brackets = new QCheckBox(tr("Записывать коды со скобками: (01)…(21)…"), this);
    m_brackets->setChecked(QSettings().value(QString::fromLatin1(kBracketsKey), false).toBool());
    m_inn = new QLineEdit(AppSettings::get(QStringLiteral("cz_inn")), this);
    m_inn->setPlaceholderText(tr("ИНН участника оборота: 10 или 12 цифр"));
    m_date = new QDateEdit(QDate::currentDate(), this);
    m_date->setCalendarPopup(true);
    m_date->setDisplayFormat(QStringLiteral("dd.MM.yyyy"));
    form->addRow(tr("Формат файла:"), m_format);
    form->addRow(QString(), m_brackets);
    form->addRow(tr("ИНН участника:"), m_inn);
    form->addRow(tr("Дата ввода в оборот:"), m_date);
    l->addLayout(form);
    m_hint = wrapped(QString(), this, true);
    l->addWidget(m_hint);

    auto *dirRow = new QHBoxLayout();
    m_dir = new QLineEdit(defaultDirectory(), this);
    auto *browse = new QPushButton(tr("Папка…"), this);
    dirRow->addWidget(new QLabel(tr("Сохранить в:"), this));
    dirRow->addWidget(m_dir, 1);
    dirRow->addWidget(browse);
    l->addLayout(dirRow);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    l->addWidget(m_summary);
    m_state = new QLabel(this);
    m_state->setWordWrap(true);
    l->addWidget(m_state);
    l->addStretch(1);

    auto *buttons = new QDialogButtonBox(this);
    auto *save = buttons->addButton(tr("Сохранить файлы"), QDialogButtonBox::ActionRole);
    save->setProperty("primary", true);
    m_openBtn = buttons->addButton(tr("Открыть папку"), QDialogButtonBox::ActionRole);
    m_openBtn->setEnabled(false);
    buttons->addButton(tr("Закрыть"), QDialogButtonBox::RejectRole);
    l->addWidget(buttons);
    for (QPushButton *b : findChildren<QPushButton *>())
        b->setAutoDefault(false);

    // Источник и число кодов
    int bad = 0;
    const int unique = int(CabinetXml::introCodes(m_codes, &bad).size());
    m_summary->setText(tr("%1: кодов идентификации <b>%2</b> (повторы убраны)%3. Файлов: <b>%4</b> (не больше %5 кодов в файле).")
                           .arg(source.toHtmlEscaped())
                           .arg(unique)
                           .arg(bad > 0 ? tr(", не похожих на код GS1 пропущено: %1").arg(bad) : QString())
                           .arg((unique + CabinetXml::kCodesPerIntroFile - 1) / CabinetXml::kCodesPerIntroFile)
                           .arg(CabinetXml::kCodesPerIntroFile));

    connect(m_format, &QComboBox::currentIndexChanged, this, [this]() { formatChanged(); });
    connect(browse, &QPushButton::clicked, this, [this]() { chooseDirectory(); });
    connect(save, &QPushButton::clicked, this, [this]() { exportFiles(); });
    connect(m_openBtn, &QPushButton::clicked, this, [this]() { openDirectory(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    formatChanged();
}

CabinetXml::IntroFormat CabinetIntroXmlDialog::format() const
{
    return CabinetXml::IntroFormat(m_format->currentData().toInt());
}

void CabinetIntroXmlDialog::setFormat(CabinetXml::IntroFormat format)
{
    const int index = m_format->findData(int(format));
    if (index >= 0)
        m_format->setCurrentIndex(index);
}

void CabinetIntroXmlDialog::setBrackets(bool brackets) { m_brackets->setChecked(brackets); }

// Для Excel ИНН и дату указывают уже в кабинете при загрузке списка; они нужны только внутри XML.
void CabinetIntroXmlDialog::formatChanged()
{
    const bool xml = format() == CabinetXml::IntroFormat::Xml;
    m_inn->setEnabled(xml);
    m_date->setEnabled(xml);
    m_brackets->setEnabled(!xml);
    if (xml) {
        m_hint->setText(tr("Файл документа vvod_individuals (action_id 5.3) с ИНН и датой. Формат повторяет присланный образец; XSD-схемы этого документа у нас нет, "
                           "поэтому сначала загрузите в кабинет файл с одним-двумя кодами: если кабинет ответит ошибкой, пришлите её текст - формат подправим."));
    } else {
        m_hint->setText(tr("Один лист, столбец A, по одному коду в строке, без заголовка - как в шаблоне кабинета. "
                           "ИНН и дату ввода в оборот укажите в кабинете при загрузке файла. Если кабинет не принимает .xls, сохраните в .xlsx."));
    }
}

void CabinetIntroXmlDialog::setInn(const QString &inn) { m_inn->setText(inn); }
void CabinetIntroXmlDialog::setDate(const QDate &date) { m_date->setDate(date); }
void CabinetIntroXmlDialog::setDirectory(const QString &directory) { m_dir->setText(directory); }
QString CabinetIntroXmlDialog::stateText() const { return m_state->text(); }

void CabinetIntroXmlDialog::chooseDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Куда сохранить файлы"), m_dir->text());
    if (!dir.isEmpty())
        m_dir->setText(dir);
}

void CabinetIntroXmlDialog::openDirectory()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_dir->text().trimmed()));
}

int CabinetIntroXmlDialog::exportFiles()
{
    m_files.clear();
    m_error.clear();
    const CabinetXml::IntroFormat fmt = format();
    const QString prefix = QStringLiteral("vvod-v-oborot-%1").arg(stamp());
    auto failed = [this]() {
        m_state->setText(QStringLiteral("<span style=\"color:#c0392b\">") + tr("Файлы не сохранены: %1.").arg(m_error.toHtmlEscaped()) + QStringLiteral("</span>"));
        m_files.clear();
        return 0;
    };
    if (fmt == CabinetXml::IntroFormat::Xml) {
        CabinetXml::IntroOptions o;
        o.inn = m_inn->text().trimmed();
        o.date = m_date->date();
        int skipped = 0;
        const QList<QByteArray> xml = CabinetXml::introFiles(m_codes, o, &m_error, &skipped);
        if (xml.isEmpty() || !CabinetXml::writeFiles(m_dir->text(), prefix, xml, &m_files, &m_error))
            return failed();
        AppSettings::set(QStringLiteral("cz_inn"), o.inn);
    } else if (!CabinetXml::writeIntroTables(m_dir->text(), prefix, m_codes, fmt, m_brackets->isChecked(), &m_files, &m_error)) {
        return failed();
    }
    QSettings settings;
    settings.setValue(QString::fromLatin1(kDirKey), m_dir->text().trimmed());
    settings.setValue(QString::fromLatin1(kFormatKey), int(fmt));
    settings.setValue(QString::fromLatin1(kBracketsKey), m_brackets->isChecked());
    m_state->setText(tr("Сохранено файлов: <b>%1</b> в папку %2. Загрузите их в личном кабинете и подпишите там.").arg(m_files.size()).arg(m_dir->text().toHtmlEscaped()));
    m_openBtn->setEnabled(true);
    return m_files.size();
}
