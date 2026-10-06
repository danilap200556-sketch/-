#include "bulkphotosdialog.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
const QStringList kImageFilters{"*.jpg", "*.jpeg", "*.png", "*.webp", "*.bmp"};
}

BulkPhotosDialog::BulkPhotosDialog(const QList<int> &selectedProductIds, QWidget *parent)
    : QDialog(parent), m_selected(selectedProductIds)
{
    setWindowTitle(tr("Фото пачкой"));
    resize(820, 640);
    auto *layout = new QVBoxLayout(this);

    m_byName = new QRadioButton(tr("Подобрать товары по имени файла: артикул, артикул на Маркете или штрихкод "
                                   "(напр. NK-AF1-42.jpg, NK-AF1-42_2.jpg - второе фото того же товара)"),
                                this);
    m_toSelected = new QRadioButton(tr("Добавить эти фото всем выделенным товарам (%1 шт.)").arg(m_selected.size()), this);
    m_byName->setChecked(true);
    if (m_selected.isEmpty()) {
        m_toSelected->setEnabled(false);
        m_toSelected->setToolTip(tr("Сначала выделите товары в списке (Ctrl или Shift)"));
    }
    layout->addWidget(m_byName);
    layout->addWidget(m_toSelected);

    auto *pickRow = new QHBoxLayout();
    auto *filesBtn = new QPushButton(tr("Выбрать фото..."), this);
    auto *folderBtn = new QPushButton(tr("Выбрать папку..."), this);
    pickRow->addWidget(filesBtn);
    pickRow->addWidget(folderBtn);
    pickRow->addStretch();
    layout->addLayout(pickRow);

    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({tr("Файл"), tr("Товар"), tr("№"), tr("Что будет")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 1);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(120);
    layout->addWidget(m_log);

    auto *bottom = new QHBoxLayout();
    m_uploadBtn = new QPushButton(tr("Загрузить"), this);
    m_uploadBtn->setEnabled(false);
    auto *closeBtn = new QPushButton(tr("Закрыть"), this);
    bottom->addStretch();
    bottom->addWidget(m_uploadBtn);
    bottom->addWidget(closeBtn);
    layout->addLayout(bottom);

    connect(filesBtn, &QPushButton::clicked, this, &BulkPhotosDialog::pickFiles);
    connect(folderBtn, &QPushButton::clicked, this, &BulkPhotosDialog::pickFolder);
    connect(m_uploadBtn, &QPushButton::clicked, this, &BulkPhotosDialog::upload);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_byName, &QRadioButton::toggled, this, &BulkPhotosDialog::rebuildPreview);
}

void BulkPhotosDialog::pickFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Выберите фото"), QString(), tr("Изображения (*.jpg *.jpeg *.png *.webp *.bmp)"));
    if (!files.isEmpty())
        setFiles(files);
}

void BulkPhotosDialog::pickFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Папка с фото"));
    if (dir.isEmpty())
        return;
    QStringList files;
    for (const QFileInfo &fi : QDir(dir).entryInfoList(kImageFilters, QDir::Files, QDir::Name | QDir::IgnoreCase))
        files << fi.absoluteFilePath();
    if (files.isEmpty()) {
        QMessageBox::information(this, tr("Нет фото"), tr("В этой папке нет файлов jpg, png, webp или bmp."));
        return;
    }
    setFiles(files);
}

void BulkPhotosDialog::setFiles(const QStringList &files)
{
    m_files = files;
    m_log->clear();
    rebuildPreview();
}

void BulkPhotosDialog::rebuildPreview()
{
    m_table->setRowCount(0);
    if (m_files.isEmpty())
        return;
    delete m_index;
    m_index = new PhotoStore::SkuIndex();
    int ok = 0, bad = 0;
    for (const QString &f : m_files) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(QFileInfo(f).fileName()));
        if (m_toSelected->isChecked()) {
            m_table->setItem(row, 1, new QTableWidgetItem(tr("все выделенные (%1)").arg(m_selected.size())));
            m_table->setItem(row, 2, new QTableWidgetItem(QString()));
            m_table->setItem(row, 3, new QTableWidgetItem(tr("будет добавлено")));
            ++ok;
            continue;
        }
        const auto hit = m_index->match(QFileInfo(f).fileName());
        m_table->setItem(row, 1, new QTableWidgetItem(hit.productId >= 0 ? hit.sku : QString()));
        m_table->setItem(row, 2, new QTableWidgetItem(hit.productId >= 0 && hit.order > 0 ? QString::number(hit.order) : QString()));
        m_table->setItem(row, 3, new QTableWidgetItem(hit.productId >= 0 ? tr("будет добавлено") : tr("товар не найден - пропуск")));
        if (hit.productId >= 0) ++ok; else { ++bad; m_table->item(row, 3)->setForeground(QColor(0xc0, 0x39, 0x2b)); }
    }
    m_table->resizeColumnsToContents();
    m_summary->setText(tr("Файлов: %1, подойдёт: %2, без товара: %3").arg(m_files.size()).arg(ok).arg(bad));
    m_uploadBtn->setEnabled(ok > 0);
}

void BulkPhotosDialog::upload()
{
    if (m_files.isEmpty())
        return;
    const bool toSelected = m_toSelected->isChecked();
    struct Item { QString file; int productId; int order; };
    QList<Item> items;
    QStringList unmatched;
    if (toSelected) {
        for (const QString &f : m_files)
            items.append({f, 0, 0});
    } else {
        PhotoStore::SkuIndex index;
        for (const QString &f : m_files) {
            const auto hit = index.match(QFileInfo(f).fileName());
            if (hit.productId >= 0)
                items.append({f, hit.productId, hit.order});
            else
                unmatched << QFileInfo(f).fileName();
        }
        std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
            return a.productId != b.productId ? a.productId < b.productId : a.order < b.order;
        });
    }
    if (items.isEmpty()) {
        m_log->setPlainText(tr("Ни к одному файлу не нашлось товара: артикул (или штрихкод) должен быть в имени файла."));
        return;
    }

    QProgressDialog progress(tr("Загрузка фото..."), tr("Отмена"), 0, int(items.size()), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    int added = 0, duplicates = 0, failed = 0;
    QStringList log;
    for (int i = 0; i < items.size(); ++i) {
        progress.setValue(i);
        if (progress.wasCanceled()) {
            log << tr("Загрузка прервана.");
            break;
        }
        const QString name = QFileInfo(items[i].file).fileName();
        PhotoStore::Prepared prepared;
        QString err;
        if (!PhotoStore::prepare(items[i].file, &prepared, &err)) {
            ++failed;
            log << tr("%1: %2").arg(name, err);
            continue;
        }
        const QList<int> targets = toSelected ? m_selected : QList<int>{items[i].productId};
        for (int productId : targets) {
            if (PhotoStore::exists(productId, prepared)) {
                ++duplicates;
                continue;
            }
            if (PhotoStore::add(productId, prepared, &err) < 0) {
                ++failed;
                log << tr("%1: %2").arg(name, err);
            } else {
                ++added;
            }
        }
        QApplication::processEvents();
    }
    progress.setValue(int(items.size()));
    m_changed = m_changed || added > 0;
    if (!unmatched.isEmpty())
        log << tr("Пропущено - не найден товар: %1").arg(unmatched.join(", "));
    log.prepend(tr("Готово: добавлено фото %1, уже были %2, ошибок %3.").arg(added).arg(duplicates).arg(failed));
    m_log->setPlainText(log.join('\n'));
}
