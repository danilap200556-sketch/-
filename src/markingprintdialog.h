#pragma once

#include "markingcode.h"
#include "thermallabel.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

// Печать кодов маркировки «Честного знака» на термоэтикетках (по умолчанию 58x40 мм): Data Matrix слева, справа артикул и
// название товара, внизу (01) GTIN и (21) серийный номер. После успешной печати коды получают статус «напечатан».
class MarkingPrintDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MarkingPrintDialog(const QList<int> &codeIds, QWidget *parent = nullptr);

    int count() const { return m_rows.size(); }
    bool printed() const { return m_printed; }
    ThermalLabel::Settings currentSettings() const;
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
    QTableWidget *m_table;
    QDoubleSpinBox *m_width;
    QDoubleSpinBox *m_height;
    QCheckBox *m_rotate;
    QComboBox *m_printer;
    QLabel *m_preview;
    QLabel *m_summary;
    bool m_printed = false;
};
