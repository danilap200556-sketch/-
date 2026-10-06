#include "productdialog.h"
#include "barcode.h"
#include "photoviewer.h"

#include <QFileInfo>
#include <QPixmap>

#include <QKeyEvent>
#include <QListWidget>

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

ProductDialog::ProductDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Товар"));
    setMinimumWidth(420);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    layout->addLayout(form);

    m_sku = new QLineEdit(this);
    m_sku->setPlaceholderText(tr("напр. SHOE-042-BLK-42"));
    form->addRow(tr("Артикул (SKU)"), m_sku);

    m_name = new QLineEdit(this);
    form->addRow(tr("Название"), m_name);

    m_description = new QTextEdit(this);
    m_description->setFixedHeight(80);
    form->addRow(tr("Описание"), m_description);

    m_price = new QDoubleSpinBox(this);
    m_price->setRange(0, 10'000'000);
    m_price->setDecimals(2);
    m_price->setSuffix(tr(" ₽"));
    form->addRow(tr("Цена"), m_price);

    // --- Фото: галерея миниатюр, первое - главное ---
    auto *photoBox = new QWidget(this);
    auto *photoLayout = new QVBoxLayout(photoBox);
    photoLayout->setContentsMargins(0, 0, 0, 0);
    m_gallery = new QListWidget(photoBox);
    m_gallery->setViewMode(QListView::IconMode);
    m_gallery->setIconSize(QSize(88, 88));
    m_gallery->setResizeMode(QListView::Adjust);
    m_gallery->setMovement(QListView::Static);
    m_gallery->setWrapping(true);
    m_gallery->setFixedHeight(122);
    m_gallery->setSelectionMode(QAbstractItemView::SingleSelection);
    photoLayout->addWidget(m_gallery);
    auto *photoButtons = new QHBoxLayout();
    auto *addPhotoBtn = new QPushButton(tr("Добавить фото..."), photoBox);
    auto *delPhotoBtn = new QPushButton(tr("Убрать"), photoBox);
    auto *coverBtn = new QPushButton(tr("Сделать главным"), photoBox);
    auto *viewBtn = new QPushButton(tr("Смотреть крупно"), photoBox);
    for (auto *b : {addPhotoBtn, delPhotoBtn, coverBtn, viewBtn}) {
        b->setAutoDefault(false);
        photoButtons->addWidget(b);
    }
    photoButtons->addStretch();
    photoLayout->addLayout(photoButtons);
    m_editButtons << addPhotoBtn << delPhotoBtn << coverBtn;
    form->addRow(tr("Фото"), photoBox);
    connect(addPhotoBtn, &QPushButton::clicked, this, &ProductDialog::addPhotos);
    connect(delPhotoBtn, &QPushButton::clicked, this, &ProductDialog::removePhoto);
    connect(coverBtn, &QPushButton::clicked, this, &ProductDialog::makeCoverPhoto);
    connect(viewBtn, &QPushButton::clicked, this, &ProductDialog::viewPhotos);
    connect(m_gallery, &QListWidget::itemDoubleClicked, this, &ProductDialog::viewPhotos);

    m_customCode = new QLineEdit(this);
    m_customCode->setPlaceholderText(tr("оставьте пустым - в QR пойдёт артикул"));
    form->addRow(tr("Свой код маркировки"), m_customCode);

    m_marketSku = new QLineEdit(this);
    m_marketSku->setPlaceholderText(tr("оставьте пустым, если совпадает с артикулом"));
    form->addRow(tr("Артикул на Маркете"), m_marketSku);

    // --- Штрихкоды: можно просто сканировать сканером в поле ввода ---
    auto *barcodeBox = new QWidget(this);
    auto *barcodeLayout = new QVBoxLayout(barcodeBox);
    barcodeLayout->setContentsMargins(0, 0, 0, 0);
    auto *inputRow = new QHBoxLayout();
    m_barcodeInput = new QLineEdit(barcodeBox);
    m_barcodeInput->setPlaceholderText(tr("отсканируйте или введите 13 цифр и нажмите Enter"));
    m_barcodeInput->installEventFilter(this);
    auto *addBarcodeBtn = new QPushButton(tr("Добавить"), barcodeBox);
    auto *genBarcodeBtn = new QPushButton(tr("Сгенерировать свой"), barcodeBox);
    auto *delBarcodeBtn = new QPushButton(tr("Удалить"), barcodeBox);
    for (auto *b : {addBarcodeBtn, genBarcodeBtn, delBarcodeBtn}) {
        b->setAutoDefault(false);
        m_editButtons << b;
    }
    inputRow->addWidget(m_barcodeInput, 1);
    inputRow->addWidget(addBarcodeBtn);
    barcodeLayout->addLayout(inputRow);
    m_barcodes = new QListWidget(barcodeBox);
    m_barcodes->setMaximumHeight(80);
    barcodeLayout->addWidget(m_barcodes);
    auto *barcodeButtons = new QHBoxLayout();
    barcodeButtons->addWidget(genBarcodeBtn);
    barcodeButtons->addWidget(delBarcodeBtn);
    barcodeButtons->addStretch();
    barcodeLayout->addLayout(barcodeButtons);
    form->addRow(tr("Штрихкоды (EAN-13)"), barcodeBox);
    connect(addBarcodeBtn, &QPushButton::clicked, this, &ProductDialog::addBarcode);
    connect(genBarcodeBtn, &QPushButton::clicked, this, &ProductDialog::generateBarcode);
    connect(delBarcodeBtn, &QPushButton::clicked, this, &ProductDialog::removeBarcode);

    m_photoStatus = new QLabel(this);
    m_photoStatus->setStyleSheet("color: gray; font-size: 11px;");
    layout->addWidget(m_photoStatus);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *buttons = m_buttons;
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (m_sku->text().trimmed().isEmpty() || m_name->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Проверьте поля"), tr("Артикул и название обязательны."));
            return;
        }
        // Недобавленный код в поле ввода - почти наверняка забыли нажать Enter.
        if (!m_barcodeInput->text().trimmed().isEmpty() && !addBarcodeCode(m_barcodeInput->text()))
            return;
        QString code, otherSku;
        if (Barcode::findConflict(m_productId, data().barcodes, &code, &otherSku)) {
            QMessageBox::warning(this, tr("Штрихкод занят"),
                                 tr("Штрихкод %1 уже привязан к товару %2. Один штрихкод может быть только у "
                                    "одного товара.").arg(code, otherSku));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ProductDialog::setProductId(int id)
{
    m_productId = id;
    loadExistingPhotos();
}

void ProductDialog::addGalleryItem(const QByteArray &thumbJpeg, int existingId, int addedIndex, bool atFront)
{
    QPixmap pm;
    pm.loadFromData(thumbJpeg);
    auto *item = new QListWidgetItem(QIcon(pm), QString());
    item->setData(Qt::UserRole, existingId);
    item->setData(Qt::UserRole + 1, addedIndex);
    item->setSizeHint(QSize(96, 96));
    if (atFront)
        m_gallery->insertItem(0, item);
    else
        m_gallery->addItem(item);
    m_gallery->setCurrentItem(item);
}

void ProductDialog::loadExistingPhotos()
{
    m_gallery->clear();
    m_changes = PhotoStore::Changes();
    for (int id : PhotoStore::ids(m_productId))
        addGalleryItem(PhotoStore::thumbData(id), id, -1);
}

void ProductDialog::addPhotos()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Выберите фото товара"), QString(), tr("Изображения (*.png *.jpg *.jpeg *.webp *.bmp)"));
    QStringList problems;
    for (const QString &path : files) {
        if (m_gallery->count() >= PhotoStore::kMaxPhotosPerProduct) {
            problems << tr("Достигнут максимум - %1 фото на товар.").arg(PhotoStore::kMaxPhotosPerProduct);
            break;
        }
        PhotoStore::Prepared p;
        QString err;
        if (!PhotoStore::prepare(path, &p, &err)) {
            problems << tr("%1: %2").arg(QFileInfo(path).fileName(), err);
            continue;
        }
        m_changes.added.append(p);
        addGalleryItem(p.thumb, 0, int(m_changes.added.size()) - 1);
    }
    if (!problems.isEmpty())
        QMessageBox::warning(this, tr("Не все фото добавлены"), problems.join('\n'));
}

void ProductDialog::removePhoto()
{
    QListWidgetItem *item = m_gallery->currentItem();
    if (!item)
        return;
    const int existingId = item->data(Qt::UserRole).toInt();
    const int addedIndex = item->data(Qt::UserRole + 1).toInt();
    if (existingId > 0) {
        m_changes.removed << existingId;
        if (m_changes.coverExistingId == existingId)
            m_changes.coverExistingId = 0;
    } else if (addedIndex >= 0 && addedIndex < m_changes.added.size()) {
        m_changes.added[addedIndex] = PhotoStore::Prepared(); // индексы остальных не сдвигаем
        if (m_changes.coverAddedIndex == addedIndex)
            m_changes.coverAddedIndex = -1;
    }
    delete m_gallery->takeItem(m_gallery->row(item));
}

void ProductDialog::makeCoverPhoto()
{
    QListWidgetItem *item = m_gallery->currentItem();
    if (!item || m_gallery->row(item) == 0)
        return;
    const int existingId = item->data(Qt::UserRole).toInt();
    const int addedIndex = item->data(Qt::UserRole + 1).toInt();
    m_changes.coverExistingId = existingId > 0 ? existingId : 0;
    m_changes.coverAddedIndex = existingId > 0 ? -1 : addedIndex;
    QListWidgetItem *moved = m_gallery->takeItem(m_gallery->row(item));
    m_gallery->insertItem(0, moved);
    m_gallery->setCurrentItem(moved);
}

void ProductDialog::viewPhotos()
{
    // Крупно можно смотреть только сохранённые фото; новые - после сохранения товара.
    QList<int> ids;
    int start = 0;
    for (int i = 0; i < m_gallery->count(); ++i) {
        const int id = m_gallery->item(i)->data(Qt::UserRole).toInt();
        if (id > 0) {
            if (m_gallery->item(i) == m_gallery->currentItem())
                start = int(ids.size());
            ids << id;
        }
    }
    if (ids.isEmpty()) {
        QMessageBox::information(this, tr("Фото"), tr("Крупно можно смотреть фото, уже сохранённые в базе. "
                                                      "Сохраните товар и откройте его снова."));
        return;
    }
    PhotoViewer viewer(ids.mid(start) + ids.mid(0, start), m_sku->text(), this);
    viewer.exec();
}

void ProductDialog::setReadOnly(bool readOnly)
{
    for (auto *e : {m_sku, m_name, m_customCode, m_marketSku, m_barcodeInput})
        e->setReadOnly(readOnly);
    m_description->setReadOnly(readOnly);
    m_price->setReadOnly(readOnly);
    m_price->setButtonSymbols(readOnly ? QAbstractSpinBox::NoButtons : QAbstractSpinBox::UpDownArrows);
    for (auto *b : m_editButtons)
        b->setVisible(!readOnly);
    m_barcodeInput->setVisible(!readOnly);
    m_buttons->button(QDialogButtonBox::Ok)->setVisible(!readOnly);
    m_buttons->button(QDialogButtonBox::Cancel)->setText(readOnly ? tr("Закрыть") : tr("Отмена"));
    setWindowTitle(readOnly ? tr("Товар (только просмотр)") : tr("Товар"));
}

void ProductDialog::setData(const ProductData &data)
{
    m_sku->setText(data.sku);
    m_name->setText(data.name);
    m_description->setPlainText(data.description);
    m_price->setValue(data.price);
    m_legacyPhotoPath = data.photoPath;
    // Старое фото-путь с этого компьютера переносим в общую базу, если там у товара ещё нет фото.
    if (m_gallery->count() == 0 && !data.photoPath.isEmpty() && QFileInfo::exists(data.photoPath)) {
        PhotoStore::Prepared p;
        if (PhotoStore::prepare(data.photoPath, &p)) {
            m_changes.added.append(p);
            addGalleryItem(p.thumb, 0, int(m_changes.added.size()) - 1);
        }
    }
    m_customCode->setText(data.customCode);
    m_marketSku->setText(data.marketSku);
    m_barcodes->clear();
    m_barcodes->addItems(data.barcodes);
}

ProductDialog::ProductData ProductDialog::data() const
{
    ProductData d;
    d.sku = m_sku->text().trimmed();
    d.name = m_name->text().trimmed();
    d.description = m_description->toPlainText();
    d.price = m_price->value();
    d.photoPath = m_legacyPhotoPath;
    d.customCode = m_customCode->text().trimmed();
    d.marketSku = m_marketSku->text().trimmed();
    for (int i = 0; i < m_barcodes->count(); ++i)
        d.barcodes << m_barcodes->item(i)->text();
    return d;
}

bool ProductDialog::eventFilter(QObject *obj, QEvent *event)
{
    // Сканер штрихкодов "печатает" цифры и нажимает Enter - Enter здесь должен
    // добавить код в список, а не закрыть карточку товара.
    if (obj == m_barcodeInput && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            addBarcode();
            return true;
        }
    }
    return QDialog::eventFilter(obj, event);
}

bool ProductDialog::addBarcodeCode(const QString &input)
{
    const QString code = Barcode::normalize(input);
    QString err;
    if (!Barcode::isValid(code, &err)) {
        QMessageBox::warning(this, tr("Неверный штрихкод"), tr("%1: %2").arg(code, err));
        m_barcodeInput->selectAll();
        return false;
    }
    if (m_barcodes->findItems(code, Qt::MatchExactly).isEmpty())
        m_barcodes->addItem(code);
    m_barcodeInput->clear();
    return true;
}

void ProductDialog::addBarcode()
{
    if (!m_barcodeInput->text().trimmed().isEmpty())
        addBarcodeCode(m_barcodeInput->text());
    m_barcodeInput->setFocus();
}

void ProductDialog::removeBarcode()
{
    delete m_barcodes->takeItem(m_barcodes->currentRow());
}

void ProductDialog::generateBarcode()
{
    const QString code = Barcode::generateInternalEan13();
    if (code.isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось подобрать свободный штрихкод."));
        return;
    }
    m_barcodes->addItem(code);
}
