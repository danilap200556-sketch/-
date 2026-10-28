#include "markingtab.h"

#include "chestnyznak.h"
#include "csvimport.h"
#include "flowlayout.h"
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
        resize(640, 360);
        auto *l = new QVBoxLayout(this);
        auto *hint = new QLabel(tr("Данные берутся в личном кабинете «Честного знака» (станция управления заказами, СУЗ API v3): адрес СУЗ, идентификатор СУЗ (omsId) "
                                   "и clientToken. Они хранятся в общей базе - вводить на каждом компьютере не нужно.\n"
                                   "Заказ кодов подписывается электронной подписью. Если на этом компьютере запущена программа-подписант (получает тело запроса "
                                   "POST-ом и возвращает подпись в base64), укажите её адрес; без неё заказ создают в личном кабинете, а здесь по номеру заказа "
                                   "скачивают коды."), this);
        hint->setWordWrap(true);
        l->addWidget(hint);
        const ChestnyZnak::Settings s = ChestnyZnak::loadSettings();
        auto *form = new QFormLayout();
        m_url = new QLineEdit(s.baseUrl, this);
        m_oms = new QLineEdit(s.omsId, this);
        m_token = new QLineEdit(s.clientToken, this);
        m_token->setEchoMode(QLineEdit::Password);
        m_signer = new QLineEdit(s.signerUrl, this);
        m_signer->setPlaceholderText(tr("необязательно, например http://127.0.0.1:8080/sign"));
        m_template = new QSpinBox(this);
        m_template->setRange(0, 999999);
        m_template->setSpecialValueText(tr("не указывать"));
        m_template->setValue(s.templateId);
        form->addRow(tr("Адрес СУЗ:"), m_url);
        form->addRow(tr("Идентификатор СУЗ (omsId):"), m_oms);
        form->addRow(tr("clientToken:"), m_token);
        form->addRow(tr("Подписант:"), m_signer);
        form->addRow(tr("Шаблон кода (templateId):"), m_template);
        l->addLayout(form);
        m_state = new QLabel(this);
        m_state->setWordWrap(true);
        l->addWidget(m_state);
        auto *buttons = new QDialogButtonBox(this);
        auto *test = buttons->addButton(tr("Проверить связь"), QDialogButtonBox::ActionRole);
        buttons->addButton(tr("Сохранить"), QDialogButtonBox::AcceptRole);
        buttons->addButton(tr("Отмена"), QDialogButtonBox::RejectRole);
        l->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() { ChestnyZnak::saveSettings(current()); accept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(test, &QPushButton::clicked, this, [this]() {
            const ChestnyZnak::Reply r = ChestnyZnak::ping(current());
            m_state->setText(r.ok ? tr("Связь есть: СУЗ ответил.") : tr("Не получилось: %1").arg(r.error));
        });
        for (QPushButton *b : findChildren<QPushButton *>())
            b->setAutoDefault(false);
    }
    ChestnyZnak::Settings current() const
    {
        ChestnyZnak::Settings s;
        s.baseUrl = m_url->text().trimmed();
        s.omsId = m_oms->text().trimmed();
        s.clientToken = m_token->text().trimmed();
        s.signerUrl = m_signer->text().trimmed();
        s.templateId = m_template->value();
        return s;
    }

private:
    QLineEdit *m_url, *m_oms, *m_token, *m_signer;
    QSpinBox *m_template;
    QLabel *m_state;
};

// ---------------------------------------------------------------- заказ кодов в СУЗ

class OrderDialog : public QDialog
{
public:
    explicit OrderDialog(QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Заказать коды маркировки в СУЗ"));
        resize(760, 620);
        auto *l = new QVBoxLayout(this);
        auto *hint = new QLabel(tr("Выберите товар (GTIN берётся из его штрихкода EAN-13) и количество, нажмите «Создать заказ». Коды готовятся не мгновенно: "
                                   "через некоторое время нажмите «Получить коды» - они попадут в таблицу. Заказ, созданный в личном кабинете, - введите его номер "
                                   "и GTIN и сразу нажмите «Получить коды»."), this);
        hint->setWordWrap(true);
        l->addWidget(hint);
        auto *form = new QFormLayout();
        m_product = new QComboBox(this);
        fillProducts(m_product, true, tr("- выбрать товар -"));
        m_gtin = new QLineEdit(this);
        m_gtin->setPlaceholderText(tr("GTIN, 14 цифр"));
        m_qty = new QSpinBox(this);
        m_qty->setRange(1, 150000);
        m_qty->setValue(10);
        m_order = new QLineEdit(this);
        m_order->setPlaceholderText(tr("номер заказа (появится после «Создать заказ»)"));
        form->addRow(tr("Товар:"), m_product);
        form->addRow(tr("GTIN:"), m_gtin);
        form->addRow(tr("Количество кодов:"), m_qty);
        form->addRow(tr("Номер заказа:"), m_order);
        l->addLayout(form);
        auto *btns = new QHBoxLayout();
        auto *ping = new QPushButton(tr("Проверить связь"), this);
        auto *create = new QPushButton(tr("Создать заказ"), this);
        auto *status = new QPushButton(tr("Состояние заказа"), this);
        auto *fetch = new QPushButton(tr("Получить коды"), this);
        auto *close = new QPushButton(tr("Закрыть"), this);
        for (QPushButton *b : {ping, create, status, fetch, close}) {
            b->setAutoDefault(false);
            btns->addWidget(b);
        }
        l->addLayout(btns);
        m_log = new QPlainTextEdit(this);
        m_log->setReadOnly(true);
        l->addWidget(m_log, 1);

        connect(m_product, &QComboBox::currentIndexChanged, this, [this]() { fillGtin(); });
        connect(m_product->lineEdit(), &QLineEdit::editingFinished, this, [this]() { fillGtin(); });
        connect(ping, &QPushButton::clicked, this, [this]() { run([this](const ChestnyZnak::Settings &s) { return ChestnyZnak::ping(s); }, tr("Связь")); });
        connect(create, &QPushButton::clicked, this, [this]() { create_(); });
        connect(status, &QPushButton::clicked, this, [this]() {
            const QString order = m_order->text().trimmed(), gtin = m_gtin->text().trimmed();
            if (order.isEmpty()) {
                log(tr("Введите номер заказа."));
                return;
            }
            run([order, gtin](const ChestnyZnak::Settings &s) { return ChestnyZnak::orderStatus(s, order, gtin); }, tr("Состояние заказа"));
        });
        connect(fetch, &QPushButton::clicked, this, [this]() { fetch_(); });
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
    }
    int added() const { return m_added; }

private:
    void log(const QString &line) { m_log->appendPlainText(line); }
    void fillGtin()
    {
        const int id = comboProductId(m_product);
        if (id <= 0)
            return;
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT barcode FROM product_barcodes WHERE product_id = ? AND barcode ~ '^[0-9]{12,13}$' ORDER BY length(barcode) DESC, created_at LIMIT 1"));
        q.addBindValue(id);
        if (q.exec() && q.next())
            m_gtin->setText(MarkingCode::gtinFromBarcode(q.value(0).toString()));
        else
            m_gtin->clear();
    }
    bool ready(ChestnyZnak::Settings *s)
    {
        *s = ChestnyZnak::loadSettings();
        if (!s->isConfigured()) {
            log(tr("СУЗ не настроен: кнопка «Настройки СУЗ…» на странице «Честный знак»."));
            return false;
        }
        return true;
    }
    template <class F> void run(F call, const QString &what)
    {
        ChestnyZnak::Settings s;
        if (!ready(&s))
            return;
        const ChestnyZnak::Reply r = call(s);
        for (const QString &t : r.trace)
            log(QStringLiteral("  ") + t);
        if (!r.ok)
            log(tr("%1: ошибка - %2").arg(what, r.error));
        else
            log(tr("%1: %2").arg(what, r.body.isEmpty() ? tr("ответ получен") : QString::fromUtf8(r.body.left(600)).simplified()));
    }
    void create_()
    {
        ChestnyZnak::Settings s;
        if (!ready(&s))
            return;
        const QString gtin = m_gtin->text().trimmed();
        if (!MarkingCode::gtinChecksumOk(gtin) || gtin.size() != 14) {
            log(tr("GTIN должен состоять из 14 цифр с верной контрольной цифрой."));
            return;
        }
        const ChestnyZnak::Reply r = ChestnyZnak::createOrder(s, {{gtin, m_qty->value(), 0}});
        for (const QString &t : r.trace)
            log(QStringLiteral("  ") + t);
        if (!r.ok) {
            log(tr("Заказ не создан: %1").arg(r.error));
            return;
        }
        const QString id = r.json.value(QStringLiteral("orderId")).toVariant().toString();
        if (id.isEmpty()) {
            log(tr("СУЗ ответил, но номера заказа в ответе нет: %1").arg(QString::fromUtf8(r.body.left(300))));
            return;
        }
        m_order->setText(id);
        log(tr("Заказ создан: %1. Подождите, пока СУЗ подготовит коды, и нажмите «Получить коды».").arg(id));
    }
    void fetch_()
    {
        ChestnyZnak::Settings s;
        if (!ready(&s))
            return;
        const QString order = m_order->text().trimmed(), gtin = m_gtin->text().trimmed();
        if (order.isEmpty() || gtin.isEmpty()) {
            log(tr("Нужны номер заказа и GTIN."));
            return;
        }
        QStringList codes, trace;
        QString err;
        const bool ok = ChestnyZnak::fetchCodes(s, order, gtin, m_qty->value(), &codes, &err, &trace, [this](int got, int total) {
            log(tr("  получено %1 из %2").arg(got).arg(total));
            QApplication::processEvents();
            return true;
        });
        for (const QString &t : std::as_const(trace))
            log(QStringLiteral("  ") + t);
        if (!ok) {
            log(tr("Коды не получены: %1").arg(err));
            return;
        }
        // GTIN взят из штрихкода товара, поэтому коды привяжутся к нему сами (по GTIN).
        const MarkingCode::AddResult res = MarkingCode::add(codes, QStringLiteral("suz"), order);
        m_added += res.added;
        log(tr("Получено кодов: %1; добавлено в таблицу: %2, уже были: %3, не распознано: %4.").arg(codes.size()).arg(res.added).arg(res.duplicates).arg(res.invalid));
        for (const QString &p : res.problems)
            log(QStringLiteral("  ") + p);
        if (!err.isEmpty())
            log(tr("Предупреждение: %1").arg(err));
        if (res.unlinked > 0)
            log(tr("Товар по GTIN не найден у %1 кодов - привяжите их кнопкой «Привязать к товару…».").arg(res.unlinked));
    }

    QComboBox *m_product;
    QLineEdit *m_gtin;
    QSpinBox *m_qty;
    QLineEdit *m_order;
    QPlainTextEdit *m_log;
    int m_added = 0;
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
    auto *selNewBtn = new QPushButton(tr("Выделить новые"), this);
    auto *linkBtn = new QPushButton(tr("Привязать к товару..."), this);
    auto *delBtn = new QPushButton(tr("Удалить"), this);
    auto *setBtn = new QPushButton(tr("Настройки СУЗ..."), this);
    selNewBtn->setToolTip(tr("Выделить в списке все коды, которые ещё не печатались"));
    linkBtn->setToolTip(tr("Выделенным кодам без товара назначить товар: его штрихкод станет GTIN этих кодов"));
    delBtn->setToolTip(tr("Удалить выделенные коды, которые ещё не печатались (напечатанные могут быть уже наклеены - их не трогаем)"));
    for (QPushButton *b : {addBtn, fileBtn, orderBtn, printBtn, selNewBtn, linkBtn, delBtn, setBtn})
        buttons->addWidget(b);
    layout->addLayout(buttons);

    m_counts = new QLabel(this);
    layout->addWidget(m_counts);

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
        auto *status = new QTableWidgetItem(row.status == QLatin1String("printed") ? tr("напечатан") : tr("новый"));
        status->setData(Qt::UserRole, row.id);
        m_table->setItem(r, ColStatus, status);
        m_table->setItem(r, ColSku, new QTableWidgetItem(row.productId > 0 ? row.sku : tr("- не найден -")));
        m_table->setItem(r, ColName, new QTableWidgetItem(row.name));
        m_table->setItem(r, ColGtin, new QTableWidgetItem(row.gtin));
        m_table->setItem(r, ColSerial, new QTableWidgetItem(row.serial));
        auto *code = new QTableWidgetItem(MarkingCode::shortText(row.code));
        code->setToolTip(QString(row.code).replace(MarkingCode::kGs, QStringLiteral("␝")));
        m_table->setItem(r, ColCode, code);
        m_table->setItem(r, ColSource, new QTableWidgetItem(row.source == QLatin1String("suz") ? tr("СУЗ %1").arg(row.orderId) : (row.source == QLatin1String("scan") ? tr("сканер") : tr("импорт"))));
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
    OrderDialog dlg(this);
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
    reload();
    emit codesChanged();
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
