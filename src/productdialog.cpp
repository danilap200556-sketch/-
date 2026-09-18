#include "productdialog.h"

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

    m_photoStatus = new QLabel(this);
    m_photoStatus->setStyleSheet("color: gray; font-size: 11px;");
    layout->addWidget(m_photoStatus);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, buttons]() {
        if (m_sku->text().trimmed().isEmpty() || m_name->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Проверьте поля"), tr("Артикул и название обязательны."));
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
    return d;
}
