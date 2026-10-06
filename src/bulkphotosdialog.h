#pragma once

#include "photostore.h"

#include <QDialog>
#include <QList>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;

// Загрузка фото пачкой. Два режима:
//  1) товар определяется по имени файла (артикул, артикул на Маркете или штрихкод;
//     "NK-AF1-42.jpg", "NK-AF1-42_2.jpg" - несколько фото одного товара);
//  2) одни и те же фото добавляются сразу всем выделенным товарам (например, одна модель
//     в разных размерах).
class BulkPhotosDialog : public QDialog
{
    Q_OBJECT
public:
    BulkPhotosDialog(const QList<int> &selectedProductIds, QWidget *parent = nullptr);

    bool changed() const { return m_changed; }

private slots:
    void pickFiles();
    void pickFolder();
    void upload();

private:
    void setFiles(const QStringList &files);
    void rebuildPreview();

    QList<int> m_selected;
    QStringList m_files;
    bool m_changed = false;
    PhotoStore::SkuIndex *m_index = nullptr;

    QRadioButton *m_byName;
    QRadioButton *m_toSelected;
    QTableWidget *m_table;
    QLabel *m_summary;
    QPlainTextEdit *m_log;
    QPushButton *m_uploadBtn;
};
