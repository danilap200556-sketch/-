#pragma once

#include "documents.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;

// Раздел «Документы»: все списания, приходы, перемещения, инвентаризации, импорты и отмены - отдельными документами.
// Выбранный документ открывается ниже (его записи); отменить можно документ целиком или последний документ одной кнопкой.
class DocumentsTab : public QWidget
{
    Q_OBJECT
public:
    explicit DocumentsTab(QWidget *parent = nullptr);
    void setReadOnly(bool readOnly);

public slots:
    void refresh();
    // Открывает документ с этим номером (после операции в других разделах).
    void showDocument(int id);

private slots:
    void documentChanged();
    void cancelSelected();
    void cancelLast();
    void exportSelected();

private:
    void fillDocuments();
    void fillLines(const Documents::Info &d);
    int selectedDocumentId() const;
    void cancelDocument(int id);
    void updateButtons();

    QComboBox *m_kind;
    QLineEdit *m_search;
    QTableWidget *m_docs;
    QTableWidget *m_lines;
    QLabel *m_header;
    QPushButton *m_cancelBtn;
    QPushButton *m_lastBtn;
    QPushButton *m_exportBtn;
    QTimer *m_searchTimer;
    QList<Documents::Info> m_list;
    bool m_readOnly = false;
    bool m_filling = false;
};
