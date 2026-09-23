#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QTextEdit;
class QDoubleSpinBox;
class QLabel;
class QListWidget;

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
        QStringList barcodes;
    };

    explicit ProductDialog(QWidget *parent = nullptr);

    void setData(const ProductData &data);
    // id редактируемого товара (0 - новый), нужен для проверки штрихкодов.
    void setProductId(int id) { m_productId = id; }
    ProductData data() const;

private slots:
    void pickPhoto();
    void openPhoto();
    void addBarcode();
    void removeBarcode();
    void generateBarcode();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    bool addBarcodeCode(const QString &input);

    int m_productId = 0;
    QListWidget *m_barcodes;
    QLineEdit *m_barcodeInput;
    QLineEdit *m_sku;
    QLineEdit *m_name;
    QTextEdit *m_description;
    QDoubleSpinBox *m_price;
    QLineEdit *m_photoPath;
    QLineEdit *m_customCode;
    QLineEdit *m_marketSku;
    QLabel *m_photoStatus;
};
