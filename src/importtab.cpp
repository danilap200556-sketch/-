#include "importtab.h"
#include "csvimport.h"
#include "database.h"
#include "xlsx.h"
#include "barcode.h"

#include <QRegularExpression>

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
    QStringLiteral("Штрихкод"),
};

int guessRoleForHeader(const QString &header)
{
    const QString h = header.toLower();
    if (h.contains(QStringLiteral("штрих")) || h.contains(QStringLiteral("ean")) || h.contains(QStringLiteral("barcode")))
        return ImportTab::RoleBarcode;
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
        tr("Загрузите Excel (.xlsx) или CSV - например, отчёт «Остатки» из МойСклад. "
           "Чтобы поправить остатки в Excel: «Выгрузить остатки в Excel», измените числа, "
           "сохраните и загрузите файл обратно в режиме «фактический остаток». Для прихода "
           "или списания укажите в колонке «Остаток» количество, которое добавить или убрать."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    auto *fileRow = new QHBoxLayout();
    m_fileLabel = new QLabel(tr("Файл не выбран"), this);
    auto *pickBtn = new QPushButton(tr("Выбрать файл..."), this);
    auto *exportBtn = new QPushButton(tr("Выгрузить остатки в Excel..."), this);
    fileRow->addWidget(m_fileLabel, 1);
    fileRow->addWidget(pickBtn);
    fileRow->addWidget(exportBtn);
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
    m_modeReceipt = new QRadioButton(tr("Добавить к остатку (приход)"), paramsGroup);
    m_modeWriteOff = new QRadioButton(tr("Убрать из остатка (списание)"), paramsGroup);
    m_modeInventory->setChecked(true);
    paramsLayout->addWidget(m_modeInventory);
    paramsLayout->addWidget(m_modeReceipt);
    paramsLayout->addWidget(m_modeWriteOff);

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
    connect(exportBtn, &QPushButton::clicked, this, &ImportTab::exportStock);
    connect(m_modeWriteOff, &QRadioButton::toggled, this, [this](bool on) {
        // Списывать товары, которых у нас нет, бессмысленно.
        m_createMissing->setEnabled(!on);
    });
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
    const QString path = QFileDialog::getOpenFileName(this, tr("Выберите файл с остатками"), QString(),
                                                        tr("Excel и CSV (*.xlsx *.csv);;Все файлы (*)"));
    if (path.isEmpty())
        return;

    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("xls")) {
        QMessageBox::warning(this, tr("Старый формат Excel"),
                             tr("Файлы .xls (Excel 97-2003) не поддерживаются. Откройте файл в Excel и "
                                "сохраните как «Книга Excel (.xlsx)»."));
        return;
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
        return;
    }
    if (rows.isEmpty()) {
        QMessageBox::warning(this, tr("Пустой файл"), tr("В файле не найдено ни одной строки."));
        return;
    }

    m_rows = rows;
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
        combo->addItem(kRoleLabels[RoleBarcode], RoleBarcode);
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

    int keyCol = -1, stockCol = -1, nameCol = -1, warehouseCol = -1, barcodeCol = -1;
    for (int c = 0; c < m_roleCombos.size(); ++c) {
        switch (m_roleCombos[c]->currentData().toInt()) {
        case RoleKey: keyCol = c; break;
        case RoleStock: stockCol = c; break;
        case RoleName: nameCol = c; break;
        case RoleWarehouse: warehouseCol = c; break;
        case RoleBarcode: barcodeCol = c; break;
        default: break;
        }
    }

    // Без колонки остатка импорт только привязывает штрихкоды к товарам.
    const bool barcodesOnly = stockCol < 0 && barcodeCol >= 0;
    if ((keyCol < 0 && barcodeCol < 0) || (stockCol < 0 && !barcodesOnly)) {
        QMessageBox::warning(this, tr("Не хватает колонок"),
                              tr("Укажите, какая колонка - «Ключ товара (артикул)» (или «Штрихкод»), а какая - "
                                 "«Остаток». Чтобы только привязать штрихкоды, достаточно колонок артикула и "
                                 "штрихкода."));
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
    const bool asWriteOff = m_modeWriteOff->isChecked();
    const bool createMissing = m_createMissing->isChecked() && !asWriteOff;
    const QString comment = tr("импорт из %1").arg(m_fileName);

    int processed = 0, created = 0, updated = 0, skipped = 0, errors = 0, linked = 0;
    QStringList logLines;

    for (int r = headerIdx + 1; r < m_rows.size(); ++r) {
        const QStringList &row = m_rows[r];
        QString key = keyCol >= 0 ? row.value(keyCol).trimmed() : QString();
        // В одной ячейке может быть несколько штрихкодов через запятую/пробел.
        QStringList codes;
        if (barcodeCol >= 0)
            for (const QString &part : row.value(barcodeCol).split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts))
                codes << Barcode::normalize(part);
        if (key.isEmpty() && codes.isEmpty())
            continue;
        if (key.toLower().startsWith(QStringLiteral("итого")))
            continue;

        ++processed;

        // Товар ищем по артикулу, а если его нет - по штрихкоду.
        int productId = key.isEmpty() ? -1 : productBySku.value(key, -1);
        for (const QString &c : codes)
            if (productId < 0)
                productId = Barcode::productIdFor(c);
        auto linkBarcodes = [&](int pid) {
            for (const QString &c : codes) {
                QString err;
                const bool isNew = Barcode::productIdFor(c) != pid;
                if (!Barcode::attach(pid, c, &err))
                    logLines << tr("Строка %1: штрихкод «%2» не привязан - %3").arg(r + 1).arg(c, err);
                else if (isNew)
                    ++linked;
            }
        };
        if (barcodesOnly) {
            if (productId < 0) {
                ++skipped;
                logLines << tr("Строка %1: товар «%2» не найден").arg(r + 1).arg(key.isEmpty() ? codes.join(", ") : key);
                continue;
            }
            linkBarcodes(productId);
            continue;
        }
        if (key.isEmpty() && productId < 0) {
            ++skipped;
            logLines << tr("Строка %1: штрихкод %2 не привязан ни к одному товару, а артикула нет")
                            .arg(r + 1).arg(codes.join(", "));
            continue;
        }

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
        if (!asInventory && qty == 0)
            continue;
        if (!asInventory && qty < 0) {
            ++errors;
            logLines << tr("Строка %1: отрицательное количество %2 для «%3» - для прихода и списания "
                           "укажите, сколько добавить или убрать").arg(r + 1).arg(qty).arg(key);
            continue;
        }

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
        linkBarcodes(productId);

        QString err;
        bool ok;
        if (asInventory)
            ok = Database::inventoryAdjust(productId, warehouseId, qty, comment, &err);
        else if (asWriteOff)
            ok = Database::adjustStock(productId, warehouseId, -qty, Database::MovementType::WriteOff, comment, &err);
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
    logLines << tr("Готово: обработано %1, создано товаров %2, обновлено остатков %3, привязано штрихкодов %4, "
                   "пропущено %5, ошибок %6")
                     .arg(processed).arg(created).arg(updated).arg(linked).arg(skipped).arg(errors);
    m_log->setPlainText(logLines.join('\n'));

    if (updated > 0 || created > 0 || linked > 0)
        emit dataImported();
}

void ImportTab::exportStock()
{
    QString path = QFileDialog::getSaveFileName(
        this, tr("Выгрузить остатки в Excel"),
        tr("Остатки_%1.xlsx").arg(QDate::currentDate().toString("yyyy-MM-dd")), tr("Excel (*.xlsx)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".xlsx"), Qt::CaseInsensitive))
        path += QLatin1String(".xlsx");

    Xlsx::Sheet sheet;
    sheet.name = tr("Остатки");
    sheet.rows.append({tr("Артикул"), tr("Название"), tr("Склад"), tr("Остаток"), tr("Место хранения")});
    sheet.numericColumns = {3};
    sheet.columnWidths = {22, 45, 20, 10, 16};

    // Товары, которых ещё нет ни на одном складе, тоже попадают в файл - на
    // склад по умолчанию с нулём, чтобы им можно было проставить остаток.
    QSqlQuery q;
    q.prepare("SELECT p.sku, p.name, w.name, s.quantity, l.code "
              "FROM stock s JOIN products p ON p.id = s.product_id "
              "JOIN warehouses w ON w.id = s.warehouse_id "
              "LEFT JOIN locations l ON l.id = s.location_id "
              "UNION ALL "
              "SELECT p.sku, p.name, ?, 0, NULL FROM products p "
              "WHERE NOT EXISTS (SELECT 1 FROM stock s WHERE s.product_id = p.id) "
              "ORDER BY 1, 3");
    q.addBindValue(m_warehouseCombo->currentText());
    if (!q.exec()) {
        QMessageBox::warning(this, tr("Ошибка"), q.lastError().text());
        return;
    }
    while (q.next())
        sheet.rows.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                           QString::number(q.value(3).toInt()), q.value(4).toString()});

    QString err;
    if (!Xlsx::write(path, {sheet}, &err)) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сохранить файл:\n%1").arg(err));
        return;
    }
    QMessageBox::information(this, tr("Готово"),
                             tr("Выгружено строк: %1.\n\nИзмените остатки в Excel, сохраните файл и загрузите его "
                                "здесь же в режиме «Задать как фактический остаток».")
                                 .arg(sheet.rows.size() - 1));
}
