#include "labelstab.h"
#include "qrlabel.h"

#include <QComboBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSqlQuery>
#include <QVBoxLayout>

namespace {
constexpr int TypeProduct = 0;
constexpr int TypeLocation = 1;
}

LabelsTab::LabelsTab(QWidget *parent)
    : QWidget(parent)
{
    auto *mainLayout = new QHBoxLayout(this);

    // --- Левая колонка: форма добавления в очередь печати ---
    auto *formBox = new QGroupBox(tr("Добавить этикетку"), this);
    auto *formLayout = new QVBoxLayout(formBox);

    m_type = new QComboBox(formBox);
    m_type->addItem(tr("Этикетка товара"), TypeProduct);
    m_type->addItem(tr("Этикетка места хранения (полки)"), TypeLocation);
    formLayout->addWidget(new QLabel(tr("Тип этикетки:"), formBox));
    formLayout->addWidget(m_type);

    m_productRow = new QWidget(formBox);
    auto *productRowLayout = new QVBoxLayout(m_productRow);
    productRowLayout->setContentsMargins(0, 0, 0, 0);
    m_product = new QComboBox(m_productRow);
    productRowLayout->addWidget(new QLabel(tr("Товар:"), m_productRow));
    productRowLayout->addWidget(m_product);
    formLayout->addWidget(m_productRow);

    m_warehouse = new QComboBox(formBox);
    formLayout->addWidget(new QLabel(tr("Склад:"), formBox));
    formLayout->addWidget(m_warehouse);

    m_location = new QComboBox(formBox);
    formLayout->addWidget(new QLabel(tr("Место хранения:"), formBox));
    formLayout->addWidget(m_location);

    m_copies = new QSpinBox(formBox);
    m_copies->setRange(1, 500);
    m_copies->setValue(1);
    formLayout->addWidget(new QLabel(tr("Копий:"), formBox));
    formLayout->addWidget(m_copies);

    auto *addBtn = new QPushButton(tr("Добавить в очередь печати"), formBox);
    formLayout->addWidget(addBtn);
    formLayout->addStretch();
    mainLayout->addWidget(formBox, 1);

    // --- Средняя колонка: очередь печати ---
    auto *queueBox = new QGroupBox(tr("Очередь печати"), this);
    auto *queueLayout = new QVBoxLayout(queueBox);
    m_queueList = new QListWidget(queueBox);
    queueLayout->addWidget(m_queueList);
    auto *queueButtons = new QHBoxLayout();
    auto *removeBtn = new QPushButton(tr("Убрать"), queueBox);
    auto *clearBtn = new QPushButton(tr("Очистить"), queueBox);
    queueButtons->addWidget(removeBtn);
    queueButtons->addWidget(clearBtn);
    queueLayout->addLayout(queueButtons);
    auto *buildBtn = new QPushButton(tr("Сформировать предпросмотр"), queueBox);
    queueLayout->addWidget(buildBtn);
    auto *pngBtn = new QPushButton(tr("Сохранить PNG"), queueBox);
    auto *pdfBtn = new QPushButton(tr("Экспорт в PDF"), queueBox);
    queueLayout->addWidget(pngBtn);
    queueLayout->addWidget(pdfBtn);
    mainLayout->addWidget(queueBox, 1);

    // --- Правая колонка: предпросмотр ---
    auto *previewBox = new QGroupBox(tr("Предпросмотр листа"), this);
    auto *previewLayout = new QVBoxLayout(previewBox);
    m_previewArea = new QScrollArea(previewBox);
    m_previewLabel = new QLabel(previewBox);
    m_previewLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_previewArea->setWidget(m_previewLabel);
    m_previewArea->setWidgetResizable(true);
    previewLayout->addWidget(m_previewArea);
    mainLayout->addWidget(previewBox, 2);

    refresh();
    onTypeChanged();

    connect(m_type, &QComboBox::currentIndexChanged, this, &LabelsTab::onTypeChanged);
    connect(m_warehouse, &QComboBox::currentIndexChanged, this, &LabelsTab::onWarehouseChanged);
    connect(addBtn, &QPushButton::clicked, this, &LabelsTab::addToQueue);
    connect(removeBtn, &QPushButton::clicked, this, &LabelsTab::removeSelected);
    connect(clearBtn, &QPushButton::clicked, this, &LabelsTab::clearQueue);
    connect(buildBtn, &QPushButton::clicked, this, &LabelsTab::buildPreview);
    connect(pngBtn, &QPushButton::clicked, this, &LabelsTab::exportPng);
    connect(pdfBtn, &QPushButton::clicked, this, &LabelsTab::exportPdf);
}

void LabelsTab::refresh()
{
    fillProductCombo();
    fillWarehouseCombo();
    fillLocationCombo();
}

void LabelsTab::fillProductCombo()
{
    m_product->clear();
    QSqlQuery q("SELECT id, sku, name, custom_code FROM products ORDER BY sku");
    while (q.next()) {
        const QString code = q.value(3).toString().isEmpty() ? q.value(1).toString() : q.value(3).toString();
        m_product->addItem(QStringLiteral("%1 — %2").arg(q.value(1).toString(), q.value(2).toString()),
                            QVariant::fromValue(QStringList{QString::number(q.value(0).toInt()),
                                                             q.value(1).toString(), q.value(2).toString(), code}));
    }
}

void LabelsTab::fillWarehouseCombo()
{
    m_warehouse->clear();
    m_warehouse->addItem(tr("— не указывать —"), -1);
    QSqlQuery q("SELECT id, name FROM warehouses ORDER BY name");
    while (q.next())
        m_warehouse->addItem(q.value(1).toString(), q.value(0).toInt());
}

void LabelsTab::fillLocationCombo()
{
    m_location->clear();
    const int whId = m_warehouse->currentData().toInt();
    if (whId <= 0) {
        m_location->addItem(tr("— не указывать —"), -1);
        return;
    }
    QSqlQuery q;
    q.prepare("SELECT id, code FROM locations WHERE warehouse_id = ? ORDER BY code");
    q.addBindValue(whId);
    q.exec();
    bool any = false;
    while (q.next()) {
        m_location->addItem(q.value(1).toString(), q.value(0).toInt());
        any = true;
    }
    if (!any)
        m_location->addItem(tr("— нет мест хранения —"), -1);
}

void LabelsTab::onWarehouseChanged()
{
    fillLocationCombo();
}

void LabelsTab::onTypeChanged()
{
    const bool isProduct = m_type->currentData().toInt() == TypeProduct;
    m_productRow->setVisible(isProduct);
}

void LabelsTab::addToQueue()
{
    const int type = m_type->currentData().toInt();
    const QString warehouseName = m_warehouse->currentData().toInt() > 0 ? m_warehouse->currentText() : QString();
    const QString locationCode = m_location->currentData().toInt() > 0 ? m_location->currentText() : QString();

    QString content;
    QStringList captions;

    if (type == TypeProduct) {
        if (m_product->count() == 0) {
            QMessageBox::information(this, tr("Нет товаров"), tr("Сначала добавьте товары на вкладке «Товары»."));
            return;
        }
        const QStringList meta = m_product->currentData().toStringList();
        const QString sku = meta.value(1);
        const QString name = meta.value(2);
        const QString code = meta.value(3);

        content = QStringLiteral("PROD|%1").arg(code);
        if (!warehouseName.isEmpty())
            content += "|" + warehouseName;
        if (!locationCode.isEmpty())
            content += "|" + locationCode;

        captions << sku << name;
        if (!warehouseName.isEmpty())
            captions << warehouseName + (locationCode.isEmpty() ? QString() : " / " + locationCode);
    } else {
        if (warehouseName.isEmpty() || locationCode.isEmpty()) {
            QMessageBox::information(this, tr("Укажите склад и место"),
                                      tr("Для этикетки места хранения нужно выбрать и склад, и место хранения."));
            return;
        }
        content = QStringLiteral("LOC|%1|%2").arg(warehouseName, locationCode);
        captions << warehouseName << locationCode;
    }

    QueueEntry entry{content, captions, m_copies->value()};
    m_queue.append(entry);
    m_queueList->addItem(QStringLiteral("%1  x%2").arg(captions.join(" / ")).arg(entry.copies));
}

void LabelsTab::removeSelected()
{
    const int row = m_queueList->currentRow();
    if (row < 0)
        return;
    delete m_queueList->takeItem(row);
    m_queue.remove(row);
}

void LabelsTab::clearQueue()
{
    m_queueList->clear();
    m_queue.clear();
}

QList<QPixmap> LabelsTab::expandedLabels() const
{
    QList<QPixmap> result;
    for (const QueueEntry &e : m_queue) {
        const QPixmap pm = QrLabel::renderLabel(e.content, e.captions);
        for (int i = 0; i < e.copies; ++i)
            result.append(pm);
    }
    return result;
}

void LabelsTab::buildPreview()
{
    const auto labels = expandedLabels();
    if (labels.isEmpty()) {
        QMessageBox::information(this, tr("Очередь пуста"), tr("Добавьте хотя бы одну этикетку в очередь."));
        return;
    }

    int cellW = 0, cellH = 0;
    for (const QPixmap &p : labels) {
        cellW = qMax(cellW, p.width());
        cellH = qMax(cellH, p.height());
    }
    const int columns = qMax(1, 4);
    const int rows = (labels.size() + columns - 1) / columns;
    const int spacing = 12;
    QPixmap sheet(columns * cellW + (columns + 1) * spacing, rows * cellH + (rows + 1) * spacing);
    sheet.fill(Qt::white);
    QPainter painter(&sheet);
    for (int i = 0; i < labels.size(); ++i) {
        const int col = i % columns;
        const int row = i / columns;
        painter.drawPixmap(spacing + col * (cellW + spacing), spacing + row * (cellH + spacing), labels[i]);
    }
    painter.end();

    m_previewLabel->setPixmap(sheet);
    m_previewLabel->resize(sheet.size());
}

void LabelsTab::exportPng()
{
    const auto labels = expandedLabels();
    if (labels.isEmpty()) {
        QMessageBox::information(this, tr("Очередь пуста"), tr("Добавьте хотя бы одну этикетку в очередь."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Сохранить лист этикеток"), "labels.png",
                                                        tr("Изображение PNG (*.png)"));
    if (path.isEmpty())
        return;
    if (!QrLabel::savePngSheet(path, labels))
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сохранить файл."));
    else
        QMessageBox::information(this, tr("Готово"), tr("Лист этикеток сохранён: %1").arg(path));
}

void LabelsTab::exportPdf()
{
    const auto labels = expandedLabels();
    if (labels.isEmpty()) {
        QMessageBox::information(this, tr("Очередь пуста"), tr("Добавьте хотя бы одну этикетку в очередь."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Экспорт в PDF"), "labels.pdf",
                                                        tr("PDF-документ (*.pdf)"));
    if (path.isEmpty())
        return;
    if (!QrLabel::exportPdf(path, labels))
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось сформировать PDF."));
    else
        QMessageBox::information(this, tr("Готово"), tr("PDF сохранён: %1").arg(path));
}
