#pragma once

#include "thermallabel.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

// Печать штрихкодов EAN-13 товаров на термоэтикетках (по умолчанию 58x40 мм): артикул сверху, описание посередине,
// штрихкод снизу. Штрихкоды берутся из уже привязанных к карточкам товаров; тем, у кого EAN-13 нет, можно присвоить
// свой. Число копий задаётся по каждому товару (можно поставить по остатку на складе).
class ThermalPrintDialog : public QDialog
{
    Q_OBJECT
public:
    struct Item {
        int productId = 0;
        int copies = 1;
    };
    explicit ThermalPrintDialog(const QList<Item> &items, QWidget *parent = nullptr);

    int totalLabels() const;
    int rowCount() const;
    // То же, что «Печать», но в PDF-файл (и для проверок): возвращает false и причину в *error при неудаче.
    bool printToPdf(const QString &path, QString *error = nullptr);
    ThermalLabel::Settings currentSettings() const;
    void setCopies(int row, int copies);

private slots:
    void print();
    void savePdf();
    void updatePreview();
    void applyCopiesToAll();
    void copiesFromStock();
    void createOwnCodes();
    void settingsChanged();

private:
    struct Row {
        int productId = 0;
        QString sku, name, description;
    };
    ThermalLabel::Ean13Label labelFor(int row) const;
    QString codeFor(int row) const;
    QString middleText(const Row &r) const;
    bool askAboutMissingCodes(); // false - отмена
    void reloadCodes();
    void updateSummary();
    bool doPrint(const QString &pdfPath, QString *error);

    QTableWidget *m_table;
    QDoubleSpinBox *m_width;
    QDoubleSpinBox *m_height;
    QCheckBox *m_rotate;
    QComboBox *m_printer;
    QComboBox *m_middle;
    QSpinBox *m_allCopies;
    QComboBox *m_stockWarehouse;
    QLabel *m_preview;
    QLabel *m_summary;
    QPushButton *m_printBtn;
    QPushButton *m_ownBtn;

    QList<Row> m_rows;
    bool m_loading = false;
};
