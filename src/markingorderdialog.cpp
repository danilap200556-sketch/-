#include "markingorderdialog.h"

#include "appsettings.h"
#include "batchparser.h"
#include "cabinetxmldialog.h"
#include "chestnyznak.h"
#include "markingcode.h"
#include "productfinder.h"
#include "productsuggest.h"

#include <algorithm>
#include <functional>

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSqlError>
#include <QSpinBox>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Col { ColSku = 0, ColName, ColGtin, ColQty, ColOrder, ColState, ColCount };

const char kPendingKey[] = "marking_pending_orders";

QString idList(const QList<int> &ids)
{
    QStringList s;
    for (int id : ids)
        s << QString::number(id);
    return s.join(QLatin1Char(','));
}

} // namespace

MarkingOrderDialog::MarkingOrderDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Заказать коды маркировки в СУЗ"));
    resize(980, 700);
    auto *l = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Соберите список товаров и сколько кодов нужно на каждый: введите артикул, штрихкод или часть названия (Enter), вставьте список "
                               "«артикул количество» из Excel или нажмите «Товары с остатком». GTIN берётся из штрихкода товара. «Создать заказ» отправит весь "
                               "список одним заказом в СУЗ; коды готовятся не мгновенно - через время нажмите «Получить коды»: они попадут в таблицу «Честный знак». "
                               "Номера заказов запоминаются, окно можно закрыть и вернуться. Заказ из личного кабинета: впишите его номер ниже, добавьте позиции и "
                               "нажмите «Получить коды»."),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    l->addWidget(hint);

    auto *addRow = new QHBoxLayout();
    m_scan = new QLineEdit(this);
    m_scan->setPlaceholderText(tr("Артикул [количество], штрихкод или часть названия; Enter - добавить в список"));
    m_scan->setClearButtonEnabled(true);
    addRow->addWidget(m_scan, 1);
    auto *pasteBtn = new QPushButton(tr("Вставить список из буфера"), this);
    auto *stockBtn = new QPushButton(tr("Товары с остатком"), this);
    stockBtn->setToolTip(tr("Добавить товары, у которых остаток больше числа свободных (не напечатанных) кодов: заказать недостающие"));
    auto *removeBtn = new QPushButton(tr("Убрать выбранные"), this);
    auto *clearBtn = new QPushButton(tr("Очистить"), this);
    for (QPushButton *b : {pasteBtn, stockBtn, removeBtn, clearBtn})
        addRow->addWidget(b);
    l->addLayout(addRow);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Артикул"), tr("Название"), tr("GTIN (14 цифр)"), tr("Кодов"), tr("Заказ в СУЗ"), tr("Состояние")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_table->verticalHeader()->setVisible(false);
    QHeaderView *h = m_table->horizontalHeader();
    h->setResizeContentsPrecision(40); // заказ на сотни артикулов: ширина по первым строкам
    h->setSectionResizeMode(ColName, QHeaderView::Stretch);
    for (int c : {ColSku, ColGtin, ColQty, ColOrder})
        h->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    h->setSectionResizeMode(ColState, QHeaderView::Stretch);
    m_table->verticalHeader()->setDefaultSectionSize(34);
    l->addWidget(m_table, 1);

    m_summary = new QLabel(this);
    l->addWidget(m_summary);

    auto *orderRow = new QHBoxLayout();
    orderRow->addWidget(new QLabel(tr("Номер заказа из личного кабинета:"), this));
    m_order = new QLineEdit(this);
    m_order->setPlaceholderText(tr("только для заказа, созданного не здесь; для позиций ниже без своего заказа"));
    orderRow->addWidget(m_order, 1);
    l->addLayout(orderRow);

    auto *btns = new QHBoxLayout();
    auto *ping = new QPushButton(tr("Проверить связь"), this);
    m_createBtn = new QPushButton(tr("Создать заказ"), this);
    m_createBtn->setProperty("primary", true);
    auto *status = new QPushButton(tr("Состояние заказов"), this);
    m_fetchBtn = new QPushButton(tr("Получить коды"), this);
    m_stopBtn = new QPushButton(tr("Остановить"), this);
    m_stopBtn->setEnabled(false);
    auto *xmlBtn = new QPushButton(tr("XML для кабинета…"), this);
    xmlBtn->setToolTip(tr("Сохранить список как файл заказа (urn:oms.order) для загрузки в личном кабинете: Заказы → Активные → Импортировать. Подпись в программе не нужна."));
    auto *close = new QPushButton(tr("Закрыть"), this);
    for (QPushButton *b : {ping, m_createBtn, status, m_fetchBtn, xmlBtn, m_stopBtn, close}) {
        b->setAutoDefault(false);
        btns->addWidget(b);
    }
    for (QPushButton *b : {pasteBtn, stockBtn, removeBtn, clearBtn})
        b->setAutoDefault(false);
    l->addLayout(btns);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(170);
    l->addWidget(m_log);

    m_suggest = new ProductSuggest(m_scan, this);
    connect(m_suggest, &ProductSuggest::chosen, this, [this](int id, const QString &, const QString &) {
        addProduct(id, 1);
        m_scan->clear();
        m_scan->setFocus();
    });
    connect(m_scan, &QLineEdit::returnPressed, this, &MarkingOrderDialog::scanEntered);
    connect(pasteBtn, &QPushButton::clicked, this, &MarkingOrderDialog::pasteFromClipboard);
    connect(xmlBtn, &QPushButton::clicked, this, [this]() { exportXml(); });
    connect(stockBtn, &QPushButton::clicked, this, [this]() {
        const int n = addByStock();
        appendLog(n > 0 ? tr("Добавлено позиций по остаткам: %1.").arg(n) : tr("Свободных кодов уже хватает на весь остаток (или остатка нет)."));
    });
    connect(removeBtn, &QPushButton::clicked, this, &MarkingOrderDialog::removeSelected);
    connect(clearBtn, &QPushButton::clicked, this, &MarkingOrderDialog::clearAll);
    connect(ping, &QPushButton::clicked, this, [this]() {
        const ChestnyZnak::Settings s = freshSettings();
        if (!s.isConfigured()) {
            appendLog(tr("СУЗ не настроен: кнопка «Настройки СУЗ…» на странице «Честный знак»."));
            return;
        }
        const ChestnyZnak::Reply r = ChestnyZnak::ping(s);
        for (const QString &t : r.trace)
            appendLog(QStringLiteral("  ") + t);
        appendLog(r.ok ? tr("Связь: ответ получен") : tr("Связь: ошибка - %1").arg(r.error));
    });
    connect(m_createBtn, &QPushButton::clicked, this, &MarkingOrderDialog::createClicked);
    connect(status, &QPushButton::clicked, this, [this]() { checkStatus(); });
    connect(m_fetchBtn, &QPushButton::clicked, this, [this]() { fetchCodes(); });
    connect(m_stopBtn, &QPushButton::clicked, this, [this]() { m_stop = true; });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (m_updating || it->column() != ColGtin)
            return;
        // GTIN поправили вручную: проверяем сразу.
        const QString g = validGtin(it->text());
        m_updating = true;
        if (g.isEmpty())
            setState(it->row(), tr("неверный GTIN: нужно 14 цифр с верной контрольной цифрой"), true);
        else {
            it->setText(g);
            setState(it->row(), QString(), false);
        }
        m_updating = false;
        refreshSummary();
    });
    auto *del = new QShortcut(QKeySequence::Delete, m_table);
    del->setContext(Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, &MarkingOrderDialog::removeSelected);

    restore();
    refreshSummary();
    m_scan->setFocus();
}

// Настройки СУЗ с актуальным токеном: если токен получен программой и заканчивается, а на этом компьютере есть чем подписать - обновляется сам.
ChestnyZnak::Settings MarkingOrderDialog::freshSettings()
{
    ChestnyZnak::Settings s = ChestnyZnak::loadSettings();
    QString note;
    if (!ChestnyZnak::ensureFreshToken(&s, &note) || !note.isEmpty())
        appendLog(note);
    return s;
}

// Список позиций - файлом заказа для личного кабинета (без отправки в СУЗ и без подписи).
void MarkingOrderDialog::exportXml()
{
    QList<CabinetXml::OrderItem> items;
    int skipped = 0;
    for (const Line &l : lines()) {
        if (l.problem || l.gtin.trimmed().isEmpty() || l.qty <= 0) {
            ++skipped;
            continue;
        }
        items.append({l.gtin.trimmed(), l.qty});
    }
    if (items.isEmpty()) {
        appendLog(tr("XML: в списке нет позиций с GTIN и количеством - сначала добавьте товары."));
        return;
    }
    if (skipped > 0)
        appendLog(tr("XML: позиций без GTIN или с ошибкой пропущено: %1.").arg(skipped));
    CabinetOrderXmlDialog dlg(items, this);
    dlg.exec();
    if (!dlg.files().isEmpty())
        appendLog(tr("XML: сохранено файлов заказа: %1 (%2).").arg(dlg.files().size()).arg(QFileInfo(dlg.files().first()).absolutePath()));
}

int MarkingOrderDialog::lineCount() const { return m_table->rowCount(); }
QString MarkingOrderDialog::log() const { return m_log->toPlainText(); }

QList<MarkingOrderDialog::Line> MarkingOrderDialog::lines() const
{
    QList<Line> out;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        Line l;
        l.productId = m_table->item(r, ColSku)->data(Qt::UserRole).toInt();
        l.sku = m_table->item(r, ColSku)->text();
        l.name = m_table->item(r, ColName)->text();
        l.gtin = m_table->item(r, ColGtin)->text().trimmed();
        l.qty = qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColQty))->value();
        l.orderId = m_table->item(r, ColOrder)->text().trimmed();
        l.received = m_table->item(r, ColState)->data(Qt::UserRole).toInt();
        l.state = m_table->item(r, ColState)->text();
        l.problem = m_table->item(r, ColState)->data(Qt::UserRole + 1).toBool();
        out << l;
    }
    return out;
}

void MarkingOrderDialog::appendLog(const QString &line)
{
    m_log->appendPlainText(line);
    QApplication::processEvents();
}

void MarkingOrderDialog::setOrderNumber(const QString &orderId) { m_order->setText(orderId); }

// 14 цифр GTIN с верной контрольной цифрой; из штрихкода EAN-13 / UPC-A / EAN-8 GTIN-14 получается дополнением нулями. Пусто - не годится.
QString MarkingOrderDialog::validGtin(const QString &text) const
{
    const QString d = text.trimmed();
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    if (!digits.match(d).hasMatch())
        return QString();
    QString g = d;
    if (d.size() == 12 || d.size() == 13)
        g = MarkingCode::gtinFromBarcode(d);
    else if (d.size() == 8)
        g = d.rightJustified(14, QLatin1Char('0'));
    if (g.size() != 14 || !MarkingCode::gtinChecksumOk(g))
        return QString();
    return g;
}

void MarkingOrderDialog::setState(int row, const QString &text, bool problem)
{
    auto *it = m_table->item(row, ColState);
    it->setText(text);
    it->setData(Qt::UserRole + 1, problem);
    it->setForeground(problem ? QBrush(QColor(0xd6, 0x45, 0x45)) : QBrush());
    it->setToolTip(text);
}

int MarkingOrderDialog::rowOf(int productId) const
{
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (productId > 0 && m_table->item(r, ColSku)->data(Qt::UserRole).toInt() == productId)
            return r;
    return -1;
}

int MarkingOrderDialog::addProducts(const QList<QPair<int, int>> &items)
{
    // Артикулы, названия и штрихкоды всех новых строк - двумя запросами, а не по запросу на строку.
    QList<int> fresh;
    for (const auto &it : items)
        if (rowOf(it.first) < 0 && !fresh.contains(it.first))
            fresh << it.first;
    QHash<int, QPair<QString, QString>> info;
    QHash<int, QString> barcode;
    if (!fresh.isEmpty()) {
        QSqlQuery q;
        if (q.exec(QStringLiteral("SELECT id, sku, name FROM products WHERE id IN (%1)").arg(idList(fresh))))
            while (q.next())
                info.insert(q.value(0).toInt(), {q.value(1).toString(), q.value(2).toString()});
        // Лучший штрихкод: настоящий (не начинающийся на 2 - свои коды приложения не GTIN), потом длиннее и раньше добавленный.
        QSqlQuery b;
        if (b.exec(QStringLiteral("SELECT DISTINCT ON (product_id) product_id, barcode FROM product_barcodes "
                                  "WHERE product_id IN (%1) AND barcode ~ '^[0-9]{12,13}$' "
                                  "ORDER BY product_id, (barcode LIKE '2%%'), length(barcode) DESC, created_at").arg(idList(fresh))))
            while (b.next())
                barcode.insert(b.value(0).toInt(), b.value(1).toString());
    }
    int missing = 0;
    m_updating = true;
    for (const auto &it : items) {
        int row = rowOf(it.first);
        if (row >= 0) {
            auto *spin = qobject_cast<QSpinBox *>(m_table->cellWidget(row, ColQty));
            spin->setValue(qMin(kMaxQty, spin->value() + it.second));
            continue;
        }
        if (!info.contains(it.first)) {
            appendLog(tr("Товар №%1 не найден (возможно, удалён).").arg(it.first));
            continue;
        }
        row = m_table->rowCount();
        m_table->insertRow(row);
        auto *sku = new QTableWidgetItem(info.value(it.first).first);
        sku->setData(Qt::UserRole, it.first);
        sku->setFlags(sku->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, ColSku, sku);
        auto *name = new QTableWidgetItem(info.value(it.first).second);
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, ColName, name);
        QString gtin, problem;
        const QString code = barcode.value(it.first);
        if (code.isEmpty()) {
            problem = tr("нет штрихкода EAN-13: впишите GTIN вручную");
        } else if (code.startsWith(QLatin1Char('2'))) {
            problem = tr("штрихкод внутренний (2…), это не GTIN: впишите настоящий GTIN");
        } else {
            gtin = MarkingCode::gtinFromBarcode(code);
            if (!MarkingCode::gtinChecksumOk(gtin)) {
                problem = tr("у штрихкода %1 неверная контрольная цифра: впишите GTIN вручную").arg(code);
                gtin.clear();
            }
        }
        m_table->setItem(row, ColGtin, new QTableWidgetItem(gtin));
        auto *spin = new QSpinBox(m_table);
        spin->setRange(1, kMaxQty);
        spin->setValue(qBound(1, it.second, kMaxQty));
        spin->setGroupSeparatorShown(true);
        connect(spin, &QSpinBox::valueChanged, this, [this]() { refreshSummary(); });
        m_table->setCellWidget(row, ColQty, spin);
        auto *order = new QTableWidgetItem();
        order->setFlags(order->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, ColOrder, order);
        auto *state = new QTableWidgetItem();
        state->setFlags(state->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, ColState, state);
        setState(row, problem, !problem.isEmpty());
        if (!problem.isEmpty())
            ++missing;
    }
    m_updating = false;
    refreshSummary();
    return missing;
}

bool MarkingOrderDialog::addProduct(int productId, int qty)
{
    const int before = lineCount();
    const int missing = addProducts({{productId, qty}});
    return lineCount() > before ? missing == 0 : rowOf(productId) >= 0;
}

int MarkingOrderDialog::addText(const QString &text)
{
    const QList<BatchParser::Line> parsed = BatchParser::parse(text);
    if (parsed.isEmpty()) {
        appendLog(tr("Список пуст: впишите артикул и количество."));
        return 1;
    }
    if (parsed.size() > BatchParser::kMaxLines) {
        appendLog(tr("Слишком много строк (больше %1) - разбейте список на части.").arg(BatchParser::kMaxLines));
        return 1;
    }
    QStringList keys;
    for (const BatchParser::Line &l : parsed)
        if (l.error.isEmpty())
            keys << l.key;
    QHash<QString, int> ids;
    QHash<QString, QString> errors;
    ProductFinder::findExactMany(keys, &ids, &errors);
    int bad = 0;
    QList<QPair<int, int>> items;
    for (const BatchParser::Line &l : parsed) {
        if (!l.error.isEmpty()) {
            appendLog(tr("строка %1: %2: %3").arg(l.n).arg(l.key.isEmpty() ? tr("(пусто)") : l.key, l.error));
            ++bad;
        } else if (!ids.contains(l.key)) {
            appendLog(tr("строка %1: %2").arg(l.n).arg(errors.value(l.key)));
            ++bad;
        } else {
            items.append({ids.value(l.key), int(qMin<qint64>(l.qty, kMaxQty))});
        }
    }
    if (!items.isEmpty())
        addProducts(items);
    return bad;
}

int MarkingOrderDialog::addByStock()
{
    QSqlQuery q;
    // Остаток по всем складам минус свободные (ещё не напечатанные) коды, привязанные к товару: заказать надо разницу.
    if (!q.exec(QStringLiteral("SELECT p.id, SUM(s.quantity)::int - COALESCE((SELECT COUNT(*) FROM marking_codes c WHERE c.product_id = p.id AND c.status = 'new'), 0)::int "
                               "FROM products p JOIN stock s ON s.product_id = p.id GROUP BY p.id "
                               "HAVING SUM(s.quantity) > COALESCE((SELECT COUNT(*) FROM marking_codes c WHERE c.product_id = p.id AND c.status = 'new'), 0) "
                               "ORDER BY p.sku"))) {
        appendLog(tr("Не удалось прочитать остатки: %1").arg(q.lastError().text()));
        return 0;
    }
    QList<QPair<int, int>> items;
    while (q.next())
        items.append({q.value(0).toInt(), int(qMin<qint64>(q.value(1).toLongLong(), kMaxQty))});
    const int before = lineCount();
    if (!items.isEmpty())
        addProducts(items);
    return lineCount() - before;
}

void MarkingOrderDialog::scanEntered()
{
    const QString text = m_scan->text().trimmed();
    if (text.isEmpty())
        return;
    if (addText(text) == 0) {
        m_scan->clear();
        return;
    }
    // Точного совпадения нет: если подходит ровно один товар по части артикула или названия, берём его.
    const QList<BatchParser::Line> parsed = BatchParser::parse(text);
    if (parsed.size() == 1 && parsed.first().error.isEmpty()) {
        const QList<ProductFinder::Suggestion> found = ProductFinder::suggest(parsed.first().key, 2);
        if (found.size() == 1) {
            addProduct(found.first().id, int(qMin<qint64>(parsed.first().qty, kMaxQty)));
            m_scan->clear();
            return;
        }
        if (found.size() > 1) {
            appendLog(tr("«%1»: подходит несколько товаров - выберите нужный в списке под строкой или введите артикул точнее.").arg(parsed.first().key));
            m_suggest->searchNow();
        }
    }
    m_scan->selectAll();
}

void MarkingOrderDialog::pasteFromClipboard()
{
    const QString text = QApplication::clipboard()->text();
    if (text.trimmed().isEmpty()) {
        appendLog(tr("В буфере обмена нет текста: скопируйте колонки «артикул» и «количество» из Excel."));
        return;
    }
    addText(text);
}

void MarkingOrderDialog::removeSelected()
{
    QList<int> rows;
    for (const QModelIndex &i : m_table->selectionModel()->selectedRows())
        rows << i.row();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int r : std::as_const(rows))
        m_table->removeRow(r);
    save();
    refreshSummary();
}

void MarkingOrderDialog::clearAll()
{
    if (m_table->rowCount() > 0) {
        bool pending = false;
        for (const Line &l : lines())
            pending = pending || (!l.orderId.isEmpty() && l.received < l.qty);
        if (pending && QMessageBox::question(this, windowTitle(),
                                             tr("В списке есть заказанные позиции, коды по которым ещё не получены. Очистить список? Номера заказов будут забыты.")) != QMessageBox::Yes)
            return;
    }
    m_table->setRowCount(0);
    save();
    refreshSummary();
}

void MarkingOrderDialog::refreshSummary()
{
    int codes = 0, problems = 0, ordered = 0, done = 0;
    const QList<Line> ls = lines();
    for (const Line &l : ls) {
        codes += l.qty;
        problems += l.problem;
        ordered += !l.orderId.isEmpty();
        done += !l.orderId.isEmpty() && l.received >= l.qty;
    }
    QString s = tr("Позиций: %1, кодов: %2").arg(ls.size()).arg(codes);
    if (ordered)
        s += tr("; заказано позиций: %1, получено полностью: %2").arg(ordered).arg(done);
    if (problems)
        s += tr("; нужно исправить GTIN: %1").arg(problems);
    m_summary->setText(s);
}

// Незавершённые позиции (заказ создан, коды получены не все) - в общую базу: окно можно закрыть, а коды забрать позже.
void MarkingOrderDialog::save() const
{
    QJsonArray arr;
    for (const Line &l : lines())
        if (!l.orderId.isEmpty() && l.received < l.qty)
            arr.append(QJsonObject{{QStringLiteral("o"), l.orderId}, {QStringLiteral("g"), l.gtin}, {QStringLiteral("q"), l.qty}, {QStringLiteral("r"), l.received},
                                   {QStringLiteral("p"), l.productId}, {QStringLiteral("s"), l.sku}});
    // Пустая строка, а не NULL: значение настройки NOT NULL.
    AppSettings::set(QString::fromLatin1(kPendingKey), arr.isEmpty() ? QString(QLatin1String("")) : QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void MarkingOrderDialog::restore()
{
    const QJsonDocument doc = QJsonDocument::fromJson(AppSettings::get(QString::fromLatin1(kPendingKey)).toUtf8());
    if (!doc.isArray() || doc.array().isEmpty())
        return;
    QList<QPair<int, int>> items;
    QHash<int, QJsonObject> saved;
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        const int pid = o.value(QStringLiteral("p")).toInt();
        if (pid > 0 && !saved.contains(pid)) {
            saved.insert(pid, o);
            items.append({pid, o.value(QStringLiteral("q")).toInt()});
        }
    }
    addProducts(items);
    m_updating = true;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QJsonObject o = saved.value(m_table->item(r, ColSku)->data(Qt::UserRole).toInt());
        if (o.isEmpty())
            continue;
        m_table->item(r, ColGtin)->setText(o.value(QStringLiteral("g")).toString());
        m_table->item(r, ColOrder)->setText(o.value(QStringLiteral("o")).toString());
        auto *spin = qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColQty));
        spin->setValue(qMax(1, o.value(QStringLiteral("q")).toInt()));
        m_table->item(r, ColState)->setData(Qt::UserRole, o.value(QStringLiteral("r")).toInt());
        setState(r, tr("заказ создан раньше: получено %1 из %2").arg(o.value(QStringLiteral("r")).toInt()).arg(o.value(QStringLiteral("q")).toInt()), false);
    }
    m_updating = false;
    appendLog(tr("Восстановлены заказанные раньше позиции, по которым коды ещё не получены: %1.").arg(saved.size()));
}

void MarkingOrderDialog::createClicked()
{
    int n = 0, codes = 0, bad = 0;
    for (const Line &l : lines()) {
        if (!l.orderId.isEmpty() || l.received > 0)
            continue;
        if (validGtin(l.gtin).isEmpty())
            ++bad;
        else {
            ++n;
            codes += l.qty;
        }
    }
    if (n == 0) {
        appendLog(bad ? tr("Нечего заказывать: у позиций неверный GTIN (см. колонку «Состояние»).") : tr("Нечего заказывать: добавьте товары в список (или все позиции уже заказаны)."));
        return;
    }
    const int orders = (n + kLinesPerOrder - 1) / kLinesPerOrder;
    if (QMessageBox::question(this, windowTitle(),
                              tr("Создать заказ в СУЗ: позиций %1, всего кодов %2%3%4?")
                                  .arg(n).arg(codes)
                                  .arg(orders > 1 ? tr(" (заказов: %1, по %2 позиций)").arg(orders).arg(kLinesPerOrder) : QString(),
                                       bad ? tr("\n\nПозиции с неверным GTIN (%1) в заказ не попадут.").arg(bad) : QString())) != QMessageBox::Yes)
        return;
    createOrders();
}

int MarkingOrderDialog::createOrders()
{
    const ChestnyZnak::Settings s = freshSettings();
    if (!s.isConfigured()) {
        appendLog(tr("СУЗ не настроен: кнопка «Настройки СУЗ…» на странице «Честный знак»."));
        return 0;
    }
    QList<int> rows;
    QHash<QString, int> byGtin; // один GTIN - одна строка заказа
    const QList<Line> all = lines();
    for (int r = 0; r < all.size(); ++r) {
        const Line &l = all.at(r);
        if (!l.orderId.isEmpty() || l.received > 0)
            continue;
        const QString g = validGtin(l.gtin);
        if (g.isEmpty()) {
            // Пустой GTIN уже объяснён в строке (нет штрихкода, внутренний код...); вписанный вручную и неверный - объясняем.
            if (!l.gtin.isEmpty() || l.state.isEmpty())
                setState(r, tr("неверный GTIN: нужно 14 цифр с верной контрольной цифрой"), true);
            continue;
        }
        if (byGtin.contains(g)) {
            setState(r, tr("этот GTIN уже есть в списке (строка %1)").arg(byGtin.value(g) + 1), true);
            continue;
        }
        byGtin.insert(g, r);
        rows << r;
    }
    refreshSummary();
    if (rows.isEmpty()) {
        appendLog(tr("Нечего заказывать."));
        return 0;
    }
    m_busy = true;
    int created = 0;
    for (int start = 0; start < rows.size(); start += kLinesPerOrder) {
        const QList<int> chunk = rows.mid(start, kLinesPerOrder);
        QList<ChestnyZnak::OrderLine> order;
        int codes = 0;
        for (int r : chunk) {
            const int qty = qobject_cast<QSpinBox *>(m_table->cellWidget(r, ColQty))->value();
            order.append({validGtin(m_table->item(r, ColGtin)->text()), qty, 0});
            codes += qty;
        }
        const ChestnyZnak::Reply reply = ChestnyZnak::createOrder(s, order);
        for (const QString &t : reply.trace)
            appendLog(QStringLiteral("  ") + t);
        if (!reply.ok) {
            appendLog(tr("Заказ не создан: %1").arg(reply.error));
            for (int r : chunk)
                setState(r, tr("заказ не создан: %1").arg(reply.error), true);
            break;
        }
        const QString id = reply.json.value(QStringLiteral("orderId")).toVariant().toString();
        if (id.isEmpty()) {
            appendLog(tr("СУЗ ответил, но номера заказа в ответе нет: %1").arg(QString::fromUtf8(reply.body.left(300))));
            for (int r : chunk)
                setState(r, tr("СУЗ не вернул номер заказа"), true);
            break;
        }
        ++created;
        for (int r : chunk) {
            m_table->item(r, ColOrder)->setText(id);
            setState(r, tr("заказано, ждёт кодов"), false);
        }
        appendLog(tr("Заказ создан: %1 (позиций %2, кодов %3).").arg(id).arg(chunk.size()).arg(codes));
    }
    m_busy = false;
    save();
    refreshSummary();
    if (created > 0)
        appendLog(tr("Подождите, пока СУЗ подготовит коды, и нажмите «Получить коды». Номера заказов сохранены: окно можно закрыть."));
    return created;
}

int MarkingOrderDialog::fetchCodes()
{
    const ChestnyZnak::Settings s = freshSettings();
    if (!s.isConfigured()) {
        appendLog(tr("СУЗ не настроен: кнопка «Настройки СУЗ…» на странице «Честный знак»."));
        return 0;
    }
    const QString common = m_order->text().trimmed();
    m_stop = false;
    m_busy = true;
    m_stopBtn->setEnabled(true);
    m_fetchBtn->setEnabled(false);
    int addedNow = 0, fetched = 0, waiting = 0, failed = 0;
    const QList<Line> snapshot = lines();
    for (int r = 0; r < snapshot.size() && !m_stop; ++r) {
        const Line &l = snapshot.at(r);
        const QString order = l.orderId.isEmpty() ? common : l.orderId;
        if (order.isEmpty() || l.received >= l.qty)
            continue;
        const QString gtin = validGtin(l.gtin);
        if (gtin.isEmpty()) {
            if (!l.gtin.isEmpty() || l.state.isEmpty())
                setState(r, tr("неверный GTIN: нужно 14 цифр с верной контрольной цифрой"), true);
            ++failed;
            continue;
        }
        // СУЗ выдаёт каждый код один раз: то, что уже скачано в таблицу (в том числе прошлым запуском), заново не просим.
        int received = qMax(l.received, qMin(l.qty, MarkingCode::countForOrder(order, gtin)));
        if (received >= l.qty) {
            m_table->item(r, ColState)->setData(Qt::UserRole, received);
            setState(r, tr("получено %1 из %2 (коды уже в таблице)").arg(received).arg(l.qty), false);
            if (l.orderId.isEmpty())
                m_table->item(r, ColOrder)->setText(order);
            continue;
        }
        setState(r, tr("получаю коды…"), false);
        QStringList codes, trace;
        QString err;
        ChestnyZnak::fetchCodes(s, order, gtin, l.qty - received, &codes, &err, &trace, [this, r, received, qty = l.qty](int got, int total) {
            Q_UNUSED(total)
            setState(r, tr("получено %1 из %2…").arg(received + got).arg(qty), false);
            QApplication::processEvents();
            return !m_stop;
        });
        for (const QString &t : std::as_const(trace))
            appendLog(QStringLiteral("  ") + t);
        if (!codes.isEmpty()) {
            // Коды привязываются к товару по GTIN (он взят из штрихкода товара); повторно скачанные считаются как «уже были».
            const MarkingCode::AddResult res = MarkingCode::add(codes, QStringLiteral("suz"), order);
            received = qMin(l.qty, received + res.added + res.duplicates);
            addedNow += res.added;
            m_added += res.added;
            fetched += res.added + res.duplicates;
            if (res.unlinked > 0)
                appendLog(tr("%1: товар по GTIN %2 не найден у %3 кодов - привяжите их кнопкой «Привязать к товару…».").arg(l.sku, gtin).arg(res.unlinked));
        }
        m_table->item(r, ColState)->setData(Qt::UserRole, received);
        if (received >= l.qty) {
            setState(r, tr("получено %1 из %2").arg(received).arg(l.qty), false);
            if (l.orderId.isEmpty())
                m_table->item(r, ColOrder)->setText(order);
        } else {
            ++waiting;
            setState(r, err.isEmpty() ? tr("получено %1 из %2: коды ещё не готовы, повторите позже").arg(received).arg(l.qty)
                                      : tr("получено %1 из %2: %3").arg(received).arg(l.qty).arg(err),
                     !err.isEmpty());
            if (l.orderId.isEmpty())
                m_table->item(r, ColOrder)->setText(order);
        }
    }
    m_busy = false;
    m_stopBtn->setEnabled(false);
    m_fetchBtn->setEnabled(true);
    save();
    refreshSummary();
    appendLog(m_stop ? tr("Остановлено. Новых кодов добавлено: %1.").arg(addedNow)
                     : tr("Готово: новых кодов добавлено %1 (всего получено %2); позиций, по которым кодов пока не хватает: %3%4.")
                           .arg(addedNow).arg(fetched).arg(waiting).arg(failed ? tr(", с ошибкой GTIN: %1").arg(failed) : QString()));
    return addedNow;
}

void MarkingOrderDialog::checkStatus()
{
    const ChestnyZnak::Settings s = freshSettings();
    if (!s.isConfigured()) {
        appendLog(tr("СУЗ не настроен: кнопка «Настройки СУЗ…» на странице «Честный знак»."));
        return;
    }
    const QString common = m_order->text().trimmed();
    int asked = 0;
    for (const Line &l : lines()) {
        const QString order = l.orderId.isEmpty() ? common : l.orderId;
        if (order.isEmpty() || validGtin(l.gtin).isEmpty())
            continue;
        const ChestnyZnak::Reply r = ChestnyZnak::orderStatus(s, order, validGtin(l.gtin));
        ++asked;
        appendLog(r.ok ? QStringLiteral("%1 (%2): %3").arg(l.sku, order, QString::fromUtf8(r.body.left(220)).simplified())
                       : tr("%1 (%2): ошибка - %3").arg(l.sku, order, r.error));
    }
    if (asked == 0)
        appendLog(tr("Нет заказанных позиций: создайте заказ или впишите номер заказа из личного кабинета."));
}
