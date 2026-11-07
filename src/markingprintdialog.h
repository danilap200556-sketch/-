#pragma once

#include "markingcode.h"
#include "marketcards.h"
#include "thermallabel.h"

#include <QDialog>
#include <QHash>
#include <QList>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

// Печать кодов маркировки «Честного знака» на термоэтикетках (по умолчанию 58x40 мм). Два шаблона: «Стандартный» - Data Matrix слева,
// справа артикул и название товара, внизу (01) GTIN и (21) серийный номер; «Как в кабинете «Честного знака»» - бренд, артикул,
// продукция, цвет, размер, изготовитель и страна в таблице слева, справа Data Matrix, его текст и логотип. Поля второго шаблона
// берутся из карточки товара (бренд, страна, вид товара из «Для Яндекс Маркета»; размер и модель - из артикула; цвет - из названия)
// и при необходимости задаются в окне для всех этикеток сразу. После успешной печати коды получают статус «напечатан».
class MarkingPrintDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MarkingPrintDialog(const QList<int> &codeIds, QWidget *parent = nullptr);

    int count() const { return m_rows.size(); }
    int skippedNoTail() const { return m_skippedNoTail; } // коды без криптохвоста, исключённые из печати
    bool printed() const { return m_printed; }
    ThermalLabel::Settings currentSettings() const;
    // Шаблон и поля (для проверок и настройки без окна): cabinet - «как в кабинете»; поле пусто - берётся из карточки товара.
    void setCabinetTemplate(bool cabinet);
    void setFontFamily(const QString &family);
    void setOverrides(const QString &brand, const QString &product, const QString &color, const QString &manufacturer, const QString &country);
    // Этикетка для строки списка в том виде, как её напечатает окно.
    ThermalLabel::MarkingLabel previewLabel(int row) const { return labelFor(row); }
    // Печатает в PDF-файл (и для проверок). markPrinted - отметить коды напечатанными.
    bool printToPdf(const QString &path, bool markPrinted, QString *error = nullptr);

private slots:
    void print();
    void savePdf();
    void updatePreview();

private:
    ThermalLabel::MarkingLabel labelFor(int row) const;
    bool doPrint(const QString &pdfPath, bool markPrinted, QString *error);
    int alreadyPrinted() const;

    QList<MarkingCode::Row> m_rows;
    QHash<int, ProductMarketInfo> m_info; // карточки товаров (бренд, страны, вид товара)
    QComboBox *m_template;
    QComboBox *m_font;
    QGroupBox *m_fieldsBox;
    QLineEdit *m_brand;
    QLineEdit *m_product;
    QLineEdit *m_color;
    QLineEdit *m_manufacturer;
    QLineEdit *m_country;
    QTableWidget *m_table;
    QDoubleSpinBox *m_width;
    QDoubleSpinBox *m_height;
    QCheckBox *m_rotate;
    QComboBox *m_printer;
    QLabel *m_preview;
    QLabel *m_summary;
    bool m_printed = false;
    int m_skippedNoTail = 0;
};
