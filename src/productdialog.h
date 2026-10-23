#pragma once

#include "photostore.h"
#include "productmarket.h"

#include <QByteArray>
#include <QDialog>
#include <QList>
#include <QStringList>

class QLineEdit;
class QTextEdit;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QDialogButtonBox;
class QPushButton;
class QTableWidget;
class ProductMarketBox;

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
        ProductMarketInfo market; // параметры для карточки Яндекс Маркета
    };

    // Что исправили в остатках на одном складе (только изменённое).
    struct StockEdit {
        int warehouseId = 0;
        bool setCount = false;
        int counted = 0;
        bool setLocation = false;
        int locationId = 0; // 0 - место не указано
    };

    // Всё, что форме нужно показать кроме самих полей товара: фото (уменьшенные), остатки по складам
    // и места хранения. Читается вместе с карточкой одним обращением к базе (см. loadCard).
    struct PhotoThumb {
        int id = 0;
        QByteArray thumb;
    };
    struct StockRow {
        int warehouseId = 0;
        QString warehouseName;
        int quantity = 0;
        int locationId = 0; // 0 - не указано
    };
    struct Location {
        int id = 0;
        int warehouseId = 0;
        QString code;
    };
    struct Card {
        ProductData data;
        QList<PhotoThumb> photos;
        QList<StockRow> stock;
        QList<Location> locations;
    };

    explicit ProductDialog(QWidget *parent = nullptr);

    // Читает всю карточку одним запросом к базе (а не по запросу на штрихкоды, фото, склады и места).
    static bool loadCard(int productId, Card *out);
    // Показывает прочитанную карточку (id товара нужен для проверки штрихкодов).
    void setCard(int productId, const Card &card);

    void setData(const ProductData &data);
    // id редактируемого товара (0 - новый), нужен для проверки штрихкодов.
    void setProductId(int id);
    ProductData data() const;
    // Штрихкоды, с которыми карточку открыли (для сохранения изменений без перечитывания базы).
    QStringList originalBarcodes() const { return m_originalBarcodes; }
    // Исправления остатков и мест хранения (пустой список - не трогали).
    QList<StockEdit> stockEdits() const;
    QString stockComment() const;
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
    void loadStock();
    void fillStock(const QList<StockRow> &stock, const QList<Location> &locations);

    int m_productId = 0;
    QStringList m_originalBarcodes;
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
    QTableWidget *m_stockTable = nullptr;
    QLineEdit *m_stockComment = nullptr;
    QWidget *m_stockBox = nullptr;
    QLineEdit *m_customCode;
    QLineEdit *m_marketSku;
    QLabel *m_photoStatus;
    ProductMarketBox *m_market;
};
