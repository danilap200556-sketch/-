#pragma once

#include <QWidget>
#include <QStringList>
#include <QVector>

class QComboBox;
class QSpinBox;
class QListWidget;
class QLabel;
class QScrollArea;

// Вкладка "Этикетки": генерация QR-этикеток для товаров и/или мест
// хранения (полок), чтобы обклеить склад и не путать позиции при
// пересчёте/инвентаризации. Печать листом в PDF или сохранение в PNG.
class LabelsTab : public QWidget
{
    Q_OBJECT
public:
    explicit LabelsTab(QWidget *parent = nullptr);

    void refresh(); // перечитать списки товаров/складов (после изменений на других вкладках)

private slots:
    void onTypeChanged();
    void onWarehouseChanged();
    void fillBarcodeCombo();
    void addToQueue();
    void removeSelected();
    void clearQueue();
    void buildPreview();
    void exportPng();
    void exportPdf();

private:
    struct QueueEntry {
        QString content;
        QStringList captions;
        int copies;
        bool ean13 = false;
    };

    void fillProductCombo();
    void fillWarehouseCombo();
    void fillLocationCombo();
    QList<QPixmap> expandedLabels() const;

    QComboBox *m_type;
    QWidget *m_productRow;
    QComboBox *m_product;
    QWidget *m_barcodeRow;
    QComboBox *m_barcode;
    QComboBox *m_warehouse;
    QComboBox *m_location;
    QSpinBox *m_copies;
    QListWidget *m_queueList;
    QLabel *m_previewLabel;
    QScrollArea *m_previewArea;

    QVector<QueueEntry> m_queue;
};
