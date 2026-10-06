#pragma once

#include "photostore.h"

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QTextEdit;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QDialogButtonBox;
class QPushButton;

// Форма добавления/редактирования товара: артикул, название, описание,
// цена, фото (лежат в общей базе, до 12 штук, первое - главное), штрихкоды и
// собственный код маркировки (если нужно закодировать в QR не сам артикул).
// Изменения фото накапливаются и применяются при сохранении товара.
class ProductDialog : public QDialog
{
    Q_OBJECT
public:
    struct ProductData {
        QString sku;
        QString name;
        QString description;
        QString photoPath; // старое поле (путь к файлу на одном компьютере), только для переноса в общую базу
        double price = 0.0;
        QString customCode;
        QString marketSku;
        QStringList barcodes;
    };

    explicit ProductDialog(QWidget *parent = nullptr);

    void setData(const ProductData &data);
    // id редактируемого товара (0 - новый), нужен для проверки штрихкодов.
    void setProductId(int id);
    ProductData data() const;
    // Что сделали с фото в диалоге (применяется после сохранения товара).
    PhotoStore::Changes photoChanges() const { return m_changes; }
    // Режим просмотра: ничего нельзя менять (роль "только просмотр").
    void setReadOnly(bool readOnly);

private slots:
    void addPhotos();
    void removePhoto();
    void makeCoverPhoto();
    void viewPhotos();
    void addBarcode();
    void removeBarcode();
    void generateBarcode();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    bool addBarcodeCode(const QString &input);
    void addGalleryItem(const QByteArray &thumbJpeg, int existingId, int addedIndex, bool atFront = false);
    void loadExistingPhotos();

    int m_productId = 0;
    QListWidget *m_barcodes;
    QLineEdit *m_barcodeInput;
    QLineEdit *m_sku;
    QLineEdit *m_name;
    QTextEdit *m_description;
    QDoubleSpinBox *m_price;
    QString m_legacyPhotoPath;
    PhotoStore::Changes m_changes;
    QListWidget *m_gallery;
    QList<QPushButton *> m_editButtons; // кнопки, скрываемые в режиме просмотра
    QDialogButtonBox *m_buttons;
    QLineEdit *m_customCode;
    QLineEdit *m_marketSku;
    QLabel *m_photoStatus;
};
