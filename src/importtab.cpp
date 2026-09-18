#include "importtab.h"
#include "csvimport.h"
#include "database.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QSplitter>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

const QStringList kRoleLabels = {
    QStringLiteral("Игнорировать"),
    QStringLiteral("Ключ товара (артикул)"),
    QStringLiteral("Название"),
    QStringLiteral("Остаток"),
    QStringLiteral("Склад"),
};

int guessRoleForHeader(const QString &header)
{
    const QString h = header.toLower();
    if (h.contains(QStringLiteral("артикул")))
        return ImportTab::RoleKey;
    if (h.contains(QStringLiteral("наимен")) || h.contains(QStringLiteral("назв")))
        return ImportTab::RoleName;
    if (h.contains(QStringLiteral("остаток")))
        return ImportTab::RoleStock;
    if (h.trimmed().startsWith(QStringLiteral("склад")))
        return ImportTab::RoleWarehouse;
    return ImportTab::RoleIgnore;
}

} // namespace

ImportTab::ImportTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(
        tr("Сохраните отчёт (например, «Остатки» из МойСклад) в формате CSV "
           "(Файл → Сохранить как → CSV) и выберите файл ниже."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    auto *fileRow = new QHBoxLayout();
    m_fileLabel = new QLabel(tr("Файл не выбран"), this);
    auto *pickBtn = new QPushButton(tr("Выбрать файл..."), this);
    fileRow->addWidget(m_fileLabel, 1);
    fileRow->addWidget(pickBtn);
    layout->addLayout(fileRow);

    auto *splitter = new QSplitter(this);
    layout->addWidget(splitter, 1);

    // --- Левая часть: сырой предпросмотр + номер строки заголовков ---
    auto *previewBox = new QGroupBox(tr("Предпросмотр файла"), this);
    auto *previewLayout = new QVBoxLayout(previewBox);

    auto *headerRowLayout = new QHBoxLayout();
    headerRowLayout->addWidget(new QLabel(tr("Строка с названиями колонок:"), previewBox));
    m_headerRow = new QSpinBox(previewBox);
    m_headerRow->setMinimum(1);
    headerRowLayout->addWidget(m_headerRow);
    headerRowLayout->addStretch();
    previewLayout->addLayout(headerRowLayout);

    m_rawPreview = new QTableWidget(previewBox);
    m_rawPreview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    previewLayout->addWidget(m_rawPreview);
    splitter->addWidget(previewBox);

    // --- Правая часть: сопоставление колонок + параметры импорта ---
    auto *rightBox = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightBox);

    auto *mappingGroup = new QGroupBox(tr("Что означает каждая колонка"), rightBox);
    auto *mappingGroupLayout = new QVBoxLayout(mappingGroup);
    m_mappingBox = new QWidget(mappingGroup);
    m_mappingLayout = new QVBoxLayout(m_mappingBox);
    mappingGroupLayout->addWidget(m_mappingBox);
    rightLayout->addWidget(mappingGroup);

    auto *paramsGroup = new QGroupBox(tr("Параметры импорта"), rightBox);
    auto *paramsLayout = new QVBoxLayout(paramsGroup);

    auto *whRow = new QHBoxLayout();
    whRow->addWidget(new QLabel(tr("Склад (если в файле нет колонки «Склад»):"), paramsGroup));
    m_warehouseCombo = new QComboBox(paramsGroup);
    whRow->addWidget(m_warehouseCombo);
    paramsLayout->addLayout(whRow);

    m_modeInventory = new QRadioButton(tr("Задать как фактический остаток (рекомендуется)"), paramsGroup);
    m_modeReceipt = new QRadioButton(tr("Добавить как приход"), paramsGroup);
    m_modeInventory->setChecked(true);
    paramsLayout->addWidget(m_modeInventory);
    paramsLayout->addWidget(m_modeReceipt);

    m_createMissing = new QCheckBox(tr("Создавать новые товары, если ключ не найден"), paramsGroup);
    m_createMissing->setChecked(true);
    paramsLayout->addWidget(m_createMissing);

    auto *importBtn = new QPushButton(tr("Импортировать"), paramsGroup);
    paramsLayout->addWidget(importBtn);
    rightLayout->addWidget(paramsGroup);

    auto *logGroup = new QGroupBox(tr("Результат импорта"), rightBox);
    auto *logLayout = new QVBoxLayout(logGroup);
    m_log = new QPlainTextEdit(logGroup);
    m_log->setReadOnly(true);
    logLayout->addWidget(m_log);
    rightLayout->addWidget(logGroup, 1);

    splitter->addWidget(rightBox);

    refresh();

    connect(pickBtn, &QPushButton::clicked, this, &ImportTab::pickFile);
    connect(m_headerRow, &QSpinBox::valueChanged, this, &ImportTab::onHeaderRowChanged);
    connect(importBtn, &QPushButton::clicked, this, &ImportTab::runImport);
}

void ImportTab::refresh()
{
    const int previous = m_warehouseCombo->currentData().toInt();
    m_warehouseCombo->clear();
    QSqlQuery q("SELECT id, name FROM warehouses ORDER BY name");
    int indexToSelect = -1;
    while (q.next()) {
        m_warehouseCombo->addItem(q.value(1).toString(), q.value(0).toInt());
        if (q.value(0).toInt() == previous)
            indexToSelect = m_warehouseCombo->count() - 1;
    }
    if (indexToSelect >= 0)
        m_warehouseCombo->setCurrentIndex(indexToSelect);
}

void ImportTab::pickFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Выберите CSV-файл"), QString(),
                                                        tr("CSV файлы (*.csv);;Все файлы (*)"));
    if (path.isEmpty())
        return;

    const CsvImport::Table table = CsvImport::readFile(path);
    if (!table.ok) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось прочитать файл:\n%1").arg(table.error));
        return;
    }
    if (table.rows.isEmpty()) {
        QMessageBox::warning(this, tr("Пустой файл"), tr("В файле не найдено ни одной строки."));
        return;
    }

    m_rows = table.rows;
    m_fileName = QFileInfo(path).fileName();
    m_fileLabel->setText(tr("%1 (%2 строк)").arg(m_fileName).arg(m_rows.size()));

    m_headerRow->setMaximum(m_rows.size());
    showRawPreview();
    guessHeaderRow();
    rebuildColumnMapping();
}

void ImportTab::showRawPreview()
{
    int maxCols = 0;
    for (const QStringList &row : m_rows)
        maxCols = qMax(maxCols, row.size());

    const int rowCount = qMin(m_rows.size(), 30);
    m_rawPreview->clear();
    m_rawPreview->setRowCount(rowCount);
    m_rawPreview->setColumnCount(maxCols);

    QStringList vHeaders;
    QStringList hHeaders;
    for (int c = 0; c < maxCols; ++c)
        hHeaders << tr("Колонка %1").arg(c + 1);
    m_rawPreview->setHorizontalHeaderLabels(hHeaders);

    for (int r = 0; r < rowCount; ++r) {
        vHeaders << QString::number(r + 1);
        const QStringList &row = m_rows[r];
        for (int c = 0; c < maxCols; ++c)
            m_rawPreview->setItem(r, c, new QTableWidgetItem(row.value(c)));
    }
    m_rawPreview->setVerticalHeaderLabels(vHeaders);
}

void ImportTab::guessHeaderRow()
{
    int bestRow = 0;
    int bestScore = 0;
    const int scanLimit = qMin(m_rows.size(), 30);
    for (int r = 0; r < scanLimit; ++r) {
        int score = 0;
        for (const QString &cell : m_rows[r]) {
            if (guessRoleForHeader(cell) != RoleIgnore)
                ++score;
        }
        if (score > bestScore) {
            bestScore = score;
            bestRow = r;
        }
    }
    if (bestScore >= 2)
        m_headerRow->setValue(bestRow + 1);
    else
        m_headerRow->setValue(1);
}

void ImportTab::onHeaderRowChanged(int)
{
    rebuildColumnMapping();
}

void ImportTab::rebuildColumnMapping()
{
    QLayoutItem *item;
    while ((item = m_mappingLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    m_roleCombos.clear();

    const int headerIdx = m_headerRow->value() - 1;
    if (headerIdx < 0 || headerIdx >= m_rows.size())
        return;

    const QStringList &headerRow = m_rows[headerIdx];
    for (int c = 0; c < headerRow.size(); ++c) {
        const QString headerText = headerRow.value(c).trimmed();
        auto *rowLayout = new QHBoxLayout();
        auto *label = new QLabel(headerText.isEmpty() ? tr("Колонка %1").arg(c + 1) : headerText, m_mappingBox);
        label->setMinimumWidth(160);
        auto *combo = new QComboBox(m_mappingBox);
        combo->addItem(kRoleLabels[RoleIgnore], RoleIgnore);
        combo->addItem(kRoleLabels[RoleKey], RoleKey);
        combo->addItem(kRoleLabels[RoleName], RoleName);
        combo->addItem(kRoleLabels[RoleStock], RoleStock);
        combo->addItem(kRoleLabels[RoleWarehouse], RoleWarehouse);
        combo->setCurrentIndex(guessRoleForHeader(headerText));
        rowLayout->addWidget(label);
        rowLayout->addWidget(combo, 1);
        m_mappingLayout->addLayout(rowLayout);
        m_roleCombos << combo;
    }
}

void ImportTab::runImport()
{
    const int headerIdx = m_headerRow->value() - 1;
    if (headerIdx < 0 || headerIdx >= m_rows.size() || m_roleCombos.isEmpty()) {
        QMessageBox::warning(this, tr("Нет данных"), tr("Сначала выберите файл."));
        return;
    }

    int keyCol = -1, stockCol = -1, nameCol = -1, warehouseCol = -1;
    for (int c = 0; c < m_roleCombos.size(); ++c) {
        switch (m_roleCombos[c]->currentData().toInt()) {
        case RoleKey: keyCol = c; break;
        case RoleStock: stockCol = c; break;
        case RoleName: nameCol = c; break;
        case RoleWarehouse: warehouseCol = c; break;
        default: break;
        }
    }

    if (keyCol < 0 || stockCol < 0) {
        QMessageBox::warning(this, tr("Не хватает колонок"),
                              tr("Укажите, какая колонка - «Ключ товара (артикул)», а какая - «Остаток»."));
        return;
    }

    const int defaultWarehouseId = m_warehouseCombo->currentData().toInt();
    if (warehouseCol < 0 && m_warehouseCombo->count() == 0) {
        QMessageBox::warning(this, tr("Нет складов"), tr("Сначала добавьте хотя бы один склад на вкладке «Склады»."));
        return;
    }

    QMap<QString, int> warehouseByName;
    {
        QSqlQuery q("SELECT id, name FROM warehouses");
        while (q.next())
            warehouseByName[q.value(1).toString()] = q.value(0).toInt();
    }

    QMap<QString, int> productBySku;
    {
        QSqlQuery q("SELECT id, sku FROM products");
        while (q.next())
            productBySku[q.value(1).toString()] = q.value(0).toInt();
    }

    const bool asInventory = m_modeInventory->isChecked();
    const bool createMissing = m_createMissing->isChecked();
    const QString comment = tr("импорт из %1").arg(m_fileName);

    int processed = 0, created = 0, updated = 0, skipped = 0, errors = 0;
    QStringList logLines;

    for (int r = headerIdx + 1; r < m_rows.size(); ++r) {
        const QStringList &row = m_rows[r];
        const QString key = row.value(keyCol).trimmed();
        if (key.isEmpty())
            continue;
        if (key.toLower().startsWith(QStringLiteral("итого")))
            continue;

        ++processed;

        QString stockRaw = row.value(stockCol).trimmed();
        stockRaw.replace(QChar(','), QChar('.'));
        bool okNum = false;
        const double qtyD = stockRaw.toDouble(&okNum);
        if (!okNum) {
            ++errors;
            logLines << tr("Строка %1: не удалось прочитать остаток «%2» для «%3»").arg(r + 1).arg(row.value(stockCol), key);
            continue;
        }
        const int qty = qRound(qtyD);

        int warehouseId = defaultWarehouseId;
        if (warehouseCol >= 0) {
            const QString whName = row.value(warehouseCol).trimmed();
            if (!warehouseByName.contains(whName)) {
                ++errors;
                logLines << tr("Строка %1: склад «%2» не найден").arg(r + 1).arg(whName);
                continue;
            }
            warehouseId = warehouseByName[whName];
        }
        if (warehouseId <= 0) {
            ++errors;
            logLines << tr("Строка %1: не выбран склад для «%2»").arg(r + 1).arg(key);
            continue;
        }

        int productId = productBySku.value(key, -1);
        if (productId < 0) {
            if (!createMissing) {
                ++skipped;
                continue;
            }
            QString name = nameCol >= 0 ? row.value(nameCol).trimmed() : QString();
            if (name.isEmpty())
                name = key;
            QSqlQuery ins;
            ins.prepare("INSERT INTO products (sku, name) VALUES (?, ?)");
            ins.addBindValue(key);
            ins.addBindValue(name);
            if (!ins.exec()) {
                ++errors;
                logLines << tr("Строка %1: не удалось создать товар «%2»: %3").arg(r + 1).arg(key, ins.lastError().text());
                continue;
            }
            productId = ins.lastInsertId().toInt();
            productBySku[key] = productId;
            ++created;
            logLines << tr("Строка %1: создан новый товар «%2»").arg(r + 1).arg(key);
        }

        QString err;
        bool ok;
        if (asInventory)
            ok = Database::inventoryAdjust(productId, warehouseId, qty, comment, &err);
        else
            ok = Database::adjustStock(productId, warehouseId, qty, Database::MovementType::Receipt, comment, &err);

        if (!ok) {
            ++errors;
            logLines << tr("Строка %1: ошибка обновления остатка для «%2»: %3").arg(r + 1).arg(key, err);
            continue;
        }
        ++updated;
    }

    logLines << QString();
    logLines << tr("Готово: обработано %1, создано товаров %2, обновлено остатков %3, пропущено %4, ошибок %5")
                     .arg(processed).arg(created).arg(updated).arg(skipped).arg(errors);
    m_log->setPlainText(logLines.join('\n'));

    if (updated > 0 || created > 0)
        emit dataImported();
}
