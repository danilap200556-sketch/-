#pragma once

#include <QDialog>

class QLineEdit;
class QTextEdit;
class QDoubleSpinBox;
class QLabel;

// Форма добавления/редактирования товара: артикул, название, описание,
// цена, путь к фото (открывается во внешнем просмотрщике) и собственный
// код маркировки (если нужно закодировать в QR не сам артикул, а что-то ещё).
class ProductDialog : public QDialog
{
    Q_OBJECT
public:
    struct ProductData {
        QString sku;
        QString name;
        QString description;
        QString photoPath;
        double price = 0.0;
        QString customCode;
        QString marketSku;
    };

    explicit ProductDialog(QWidget *parent = nullptr);

    void setData(const ProductData &data);
    ProductData data() const;

private slots:
    void pickPhoto();
    void openPhoto();

private:
    QLineEdit *m_sku;
    QLineEdit *m_name;
    QTextEdit *m_description;
    QDoubleSpinBox *m_price;
    QLineEdit *m_photoPath;
    QLineEdit *m_customCode;
    QLineEdit *m_marketSku;
    QLabel *m_photoStatus;
};
