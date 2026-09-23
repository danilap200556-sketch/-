#include "productdialog.h"
#include "barcode.h"

#include <QKeyEvent>
#include <QListWidget>

#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QUrl>
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

    auto *photoRow = new QWidget(this);
    auto *photoLayout = new QHBoxLayout(photoRow);
    photoLayout->setContentsMargins(0, 0, 0, 0);
    m_photoPath = new QLineEdit(photoRow);
    m_photoPath->setReadOnly(true);
    auto *pickBtn = new QPushButton(tr("Выбрать..."), photoRow);
    auto *openBtn = new QPushButton(tr("Открыть"), photoRow);
    photoLayout->addWidget(m_photoPath);
    photoLayout->addWidget(pickBtn);
    photoLayout->addWidget(openBtn);
    form->addRow(tr("Фото"), photoRow);
    connect(pickBtn, &QPushButton::clicked, this, &ProductDialog::pickPhoto);
    connect(openBtn, &QPushButton::clicked, this, &ProductDialog::openPhoto);

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
    for (auto *b : {addBarcodeBtn, genBarcodeBtn, delBarcodeBtn})
        b->setAutoDefault(false);
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

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, buttons]() {
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

void ProductDialog::pickPhoto()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Выберите фото товара"), QString(),
        tr("Изображения (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (!path.isEmpty())
        m_photoPath->setText(path);
}

void ProductDialog::openPhoto()
{
    const QString path = m_photoPath->text();
    if (path.isEmpty()) {
        QMessageBox::information(this, tr("Нет фото"), tr("Сначала выберите файл фото."));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void ProductDialog::setData(const ProductData &data)
{
    m_sku->setText(data.sku);
    m_name->setText(data.name);
    m_description->setPlainText(data.description);
    m_price->setValue(data.price);
    m_photoPath->setText(data.photoPath);
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
    d.photoPath = m_photoPath->text();
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
