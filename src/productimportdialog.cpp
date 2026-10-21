#include "productimportdialog.h"
#include "csvimport.h"
#include "xlsx.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString clip(const QString &s, int n = 90) { return s.size() > n ? s.left(n) + QStringLiteral("…") : s; }
}

ProductImportDialog::ProductImportDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Обновить товары из Excel"));
    resize(1100, 820);

    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Название, описание, цена, артикул на Маркете, свой код, штрихкоды - сразу для многих товаров. "
                               "<b>Остатки не меняются.</b> Подойдёт файл, выгруженный кнопкой «Выгрузить в Excel» на вкладке «Товары»: "
                               "измените в нём нужные ячейки и загрузите обратно. Перед записью покажем, что изменится."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    auto *fileRow = new QHBoxLayout();
    m_fileLabel = new QLabel(tr("Файл не выбран"), this);
    auto *pick = new QPushButton(tr("Выбрать файл..."), this);
    fileRow->addWidget(m_fileLabel, 1);
    fileRow->addWidget(pick);
    layout->addLayout(fileRow);

    auto *top = new QSplitter(Qt::Horizontal, this);
    // слева - файл как есть
    auto *rawBox = new QGroupBox(tr("Файл"), this);
    auto *rawLayout = new QVBoxLayout(rawBox);
    auto *headRow = new QHBoxLayout();
    headRow->addWidget(new QLabel(tr("Строка с названиями колонок:"), rawBox));
    m_headerRow = new QSpinBox(rawBox);
    m_headerRow->setMinimum(1);
    headRow->addWidget(m_headerRow);
    headRow->addStretch(1);
    rawLayout->addLayout(headRow);
    m_raw = new QTableWidget(rawBox);
    m_raw->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rawLayout->addWidget(m_raw, 1);
    top->addWidget(rawBox);
    // справа - что значит каждая колонка и параметры
    auto *mapBox = new QGroupBox(tr("Что означает каждая колонка"), this);
    auto *mapOuter = new QVBoxLayout(mapBox);
    auto *scroll = new QScrollArea(mapBox);
    scroll->setWidgetResizable(true);
    auto *inner = new QWidget(scroll);
    m_mappingLayout = new QVBoxLayout(inner);
    scroll->setWidget(inner);
    mapOuter->addWidget(scroll, 1);
    m_createMissing = new QCheckBox(tr("Создавать новые товары, если артикула нет в базе"), mapBox);
    m_clearEmpty = new QCheckBox(tr("Пустая ячейка очищает поле (описание, артикул на Маркете, свой код)"), mapBox);
    mapOuter->addWidget(m_createMissing);
    mapOuter->addWidget(m_clearEmpty);
    top->addWidget(mapBox);
    top->setStretchFactor(0, 3);
    top->setStretchFactor(1, 2);
    layout->addWidget(top, 4);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);
    m_problems = new QPlainTextEdit(this);
    m_problems->setReadOnly(true);
    m_problems->setMaximumHeight(64);
    m_problems->setStyleSheet("color: #b00020;");
    m_problems->hide();
    layout->addWidget(m_problems);
    m_diff = new QTableWidget(0, 4, this);
    m_diff->setHorizontalHeaderLabels({tr("Строка"), tr("Артикул"), tr("Название"), tr("Что изменится")});
    m_diff->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diff->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_diff->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(m_diff, 3);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    m_applyButton = new QPushButton(tr("Применить"), this);
    m_applyButton->setEnabled(false);
    auto *close = new QPushButton(tr("Закрыть"), this);
    buttons->addWidget(m_applyButton);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, &ProductImportDialog::refreshPlan);
    connect(pick, &QPushButton::clicked, this, &ProductImportDialog::chooseFile);
    connect(m_headerRow, &QSpinBox::valueChanged, this, &ProductImportDialog::rebuildMapping);
    connect(m_createMissing, &QCheckBox::toggled, this, &ProductImportDialog::schedule);
    connect(m_clearEmpty, &QCheckBox::toggled, this, &ProductImportDialog::schedule);
    connect(m_applyButton, &QPushButton::clicked, this, &ProductImportDialog::applyClicked);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
}

void ProductImportDialog::schedule()
{
    if (!m_building)
        m_timer->start();
}

void ProductImportDialog::chooseFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Файл с товарами"), QString(),
                                                      tr("Excel и CSV (*.xlsx *.csv);;Все файлы (*)"));
    if (!path.isEmpty())
        loadFile(path);
}

bool ProductImportDialog::loadFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("xls")) {
        QMessageBox::warning(this, tr("Старый формат Excel"),
                             tr("Файлы .xls (Excel 97-2003) не поддерживаются. Откройте файл в Excel и сохраните как «Книга Excel (.xlsx)»."));
        return false;
    }
    QVector<QStringList> rows;
    QString readError;
    if (suffix == QLatin1String("xlsx")) {
        const Xlsx::Table t = Xlsx::readFirstSheet(path);
        rows = t.rows;
        readError = t.ok ? QString() : t.error;
    } else {
        const CsvImport::Table t = CsvImport::readFile(path);
        rows = t.rows;
        readError = t.ok ? QString() : t.error;
    }
    if (!readError.isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось прочитать файл:\n%1").arg(readError));
        return false;
    }
    if (rows.isEmpty()) {
        QMessageBox::warning(this, tr("Пустой файл"), tr("В файле не найдено ни одной строки."));
        return false;
    }
    return loadRows(rows, QFileInfo(path).fileName());
}

bool ProductImportDialog::loadRows(const QVector<QStringList> &rows, const QString &fileName)
{
    m_rows = rows;
    m_fileName = fileName;
    m_applied = false;
    m_fileLabel->setText(tr("%1 (%2 строк)").arg(fileName).arg(rows.size()));
    m_building = true;
    m_headerRow->setMaximum(qMax<int>(1, rows.size()));
    m_headerRow->setValue(ProductSync::guessHeaderRow(rows) + 1);
    m_building = false;

    int maxCols = 0;
    for (const QStringList &r : rows)
        maxCols = qMax<int>(maxCols, r.size());
    maxCols = qMin(maxCols, 40);
    const int shown = qMin<int>(rows.size(), 12);
    m_raw->clear();
    m_raw->setRowCount(shown);
    m_raw->setColumnCount(maxCols);
    QStringList h;
    for (int c = 0; c < maxCols; ++c)
        h << tr("Колонка %1").arg(c + 1);
    m_raw->setHorizontalHeaderLabels(h);
    for (int r = 0; r < shown; ++r)
        for (int c = 0; c < maxCols; ++c)
            m_raw->setItem(r, c, new QTableWidgetItem(clip(rows[r].value(c), 60)));
    rebuildMapping();
    return true;
}

void ProductImportDialog::rebuildMapping()
{
    if (m_building)
        return;
    QLayoutItem *item;
    while ((item = m_mappingLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    m_combos.clear();
    const int hIdx = m_headerRow->value() - 1;
    if (hIdx < 0 || hIdx >= m_rows.size())
        return;
    const QStringList header = m_rows[hIdx];
    int cols = 0;
    for (int r = 0; r < qMin<int>(m_rows.size(), 50); ++r)
        cols = qMax<int>(cols, m_rows[r].size());
    cols = qMin(cols, 40);
    const QList<int> roles = ProductSync::guessRoles(header, cols);
    const QStringList labels = ProductSync::roleLabels();
    m_building = true;
    for (int c = 0; c < cols; ++c) {
        auto *row = new QHBoxLayout();
        const QString t = header.value(c).trimmed();
        auto *label = new QLabel(t.isEmpty() ? tr("Колонка %1").arg(c + 1) : t);
        label->setMinimumWidth(150);
        auto *combo = new QComboBox();
        for (int r = 0; r < labels.size(); ++r)
            combo->addItem(labels[r], r);
        combo->setCurrentIndex(roles.value(c));
        connect(combo, &QComboBox::currentIndexChanged, this, &ProductImportDialog::schedule);
        row->addWidget(label);
        row->addWidget(combo, 1);
        m_mappingLayout->addLayout(row);
        m_combos << combo;
    }
    m_mappingLayout->addStretch(1);
    m_building = false;
    refreshPlan();
}

void ProductImportDialog::setRole(int column, int role)
{
    if (column >= 0 && column < m_combos.size())
        m_combos[column]->setCurrentIndex(role);
    refreshPlan();
}

void ProductImportDialog::setOptions(bool createMissing, bool clearEmpty)
{
    m_building = true;
    m_createMissing->setChecked(createMissing);
    m_clearEmpty->setChecked(clearEmpty);
    m_building = false;
    refreshPlan();
}

void ProductImportDialog::setHeaderRow(int row)
{
    m_headerRow->setValue(row);
}

void ProductImportDialog::refreshPlan()
{
    m_timer->stop();
    m_diff->setRowCount(0);
    m_problems->clear();
    m_problems->hide();
    m_applyButton->setEnabled(false);
    m_applyButton->setText(tr("Применить"));
    if (m_rows.isEmpty() || m_combos.isEmpty()) {
        m_summary->clear();
        return;
    }
    QList<int> roles;
    for (QComboBox *c : std::as_const(m_combos))
        roles << c->currentData().toInt();
    ProductSync::Options o;
    o.createMissing = m_createMissing->isChecked();
    o.clearEmpty = m_clearEmpty->isChecked();
    m_plan = ProductSync::plan(m_rows, m_headerRow->value() - 1, roles, o);
    if (!m_plan.fatal.isEmpty()) {
        m_summary->setText(QStringLiteral("<span style='color:#b00020'>%1</span>").arg(m_plan.fatal.toHtmlEscaped()));
        return;
    }
    m_summary->setText(tr("Будет обновлено: <b>%1</b> · создано: <b>%2</b> · без изменений: <b>%3</b> · не найдено: <b>%4</b> · с ошибками: <b>%5</b>")
                           .arg(m_plan.update).arg(m_plan.create).arg(m_plan.same).arg(m_plan.missing).arg(m_plan.error));
    if (!m_plan.problems.isEmpty()) {
        QStringList lines;
        for (int i = 0; i < qMin<int>(m_plan.problems.size(), 50); ++i)
            lines << tr("строка %1: %2").arg(m_plan.problems[i].n).arg(m_plan.problems[i].text);
        if (m_plan.problems.size() > 50)
            lines << tr("...и ещё %1").arg(m_plan.problems.size() - 50);
        m_problems->setPlainText(lines.join(QLatin1Char('\n')));
        m_problems->show();
    }
    int shown = 0;
    for (const ProductSync::Item &it : std::as_const(m_plan.items)) {
        if (it.status != QLatin1String("update") && it.status != QLatin1String("create"))
            continue;
        if (shown >= 300)
            break;
        QStringList what;
        if (it.status == QLatin1String("create")) {
            what << tr("новый товар") + (it.newCodes.isEmpty() ? QString() : tr(", штрихкоды: %1").arg(it.newCodes.join(QStringLiteral(", "))));
        } else {
            for (const ProductSync::Change &c : it.changes)
                what << QStringLiteral("%1: %2 → %3").arg(c.label, c.from.isEmpty() ? QStringLiteral("—") : clip(c.from), c.to.isEmpty() ? QStringLiteral("—") : clip(c.to));
        }
        const int row = m_diff->rowCount();
        m_diff->insertRow(row);
        m_diff->setItem(row, 0, new QTableWidgetItem(QString::number(it.n)));
        m_diff->setItem(row, 1, new QTableWidgetItem(it.sku + (it.byCode ? tr(" (найден по штрихкоду)") : QString())));
        m_diff->setItem(row, 2, new QTableWidgetItem(clip(it.name, 60)));
        m_diff->setItem(row, 3, new QTableWidgetItem(what.join(QLatin1Char('\n'))));
        ++shown;
    }
    if (m_plan.update + m_plan.create > 0) {
        m_applyButton->setEnabled(true);
        m_applyButton->setText(m_plan.create ? tr("Применить: обновить %1, создать %2").arg(m_plan.update).arg(m_plan.create)
                                             : tr("Применить: обновить %1").arg(m_plan.update));
    }
}

bool ProductImportDialog::apply()
{
    m_error.clear();
    QList<int> roles;
    for (QComboBox *c : std::as_const(m_combos))
        roles << c->currentData().toInt();
    ProductSync::Options o;
    o.createMissing = m_createMissing->isChecked();
    o.clearEmpty = m_clearEmpty->isChecked();
    ProductSync::Result res;
    if (!ProductSync::apply(m_rows, m_headerRow->value() - 1, roles, o, &res, &m_error))
        return false;
    m_result = res;
    m_applied = true;
    return true;
}

void ProductImportDialog::applyClicked()
{
    const QString q = tr("Обновить товаров: %1%2. Остатки не изменятся. Продолжить?")
                          .arg(m_plan.update).arg(m_plan.create ? tr(", создать: %1").arg(m_plan.create) : QString());
    if (QMessageBox::question(this, windowTitle(), q) != QMessageBox::Yes)
        return;
    if (!apply()) {
        QMessageBox::warning(this, tr("Не обновлено"), tr("Ничего не изменилось.\n\n%1").arg(m_error));
        return;
    }
    QString text = tr("Готово. Обновлено товаров: %1, создано: %2, без изменений: %3, не найдено: %4, с ошибками: %5. Остатки не менялись.")
                       .arg(m_result.updated).arg(m_result.created).arg(m_result.plan.same).arg(m_result.plan.missing).arg(m_result.plan.error);
    if (m_result.backup.rows.size() > 1)
        text += tr("\n\nСохранить файл с прежними значениями («как было»)? Загрузив его так же, можно вернуть всё обратно "
                   "(для возврата пустых полей отметьте «Пустая ячейка очищает поле»).");
    const bool offer = m_result.backup.rows.size() > 1;
    const auto answer = QMessageBox::question(this, windowTitle(), text,
                                              offer ? QMessageBox::Yes | QMessageBox::No : QMessageBox::Ok,
                                              offer ? QMessageBox::Yes : QMessageBox::Ok);
    if (offer && answer == QMessageBox::Yes)
        saveBackup();
    accept();
}

void ProductImportDialog::saveBackup()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Сохранить «как было»"),
                                                tr("Товары_как_было_%1.xlsx").arg(QDate::currentDate().toString("yyyy-MM-dd")),
                                                tr("Excel (*.xlsx)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".xlsx"), Qt::CaseInsensitive))
        path += QLatin1String(".xlsx");
    QString err;
    Xlsx::Sheet sheet = m_result.backup;
    sheet.name = tr("Товары (как было)");
    if (!Xlsx::write(path, {sheet}, &err))
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сохранить файл:\n%1").arg(err));
}
