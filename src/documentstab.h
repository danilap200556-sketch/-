#pragma once

#include "documents.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QMenu;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;

// Раздел «Документы»: все списания, приходы, перемещения, инвентаризации, импорты и отмены - отдельными документами.
// Выбранный документ открывается ниже (его записи). Новый приход, списание или перемещение создаётся здесь же, проведённый
// документ можно изменить (состав и комментарий), отменить целиком (остатки возвращаются, остаётся документ «Отмена») или
// удалить совсем - с возвратом остатков или без него.
class DocumentsTab : public QWidget
{
    Q_OBJECT
public:
    explicit DocumentsTab(QWidget *parent = nullptr);
    void setReadOnly(bool readOnly);
    // Удалять документы может только администратор: удаление стирает документ из истории.
    void setAdmin(bool admin);

public slots:
    void refresh();
    // Открывает документ с этим номером (после операции в других разделах).
    void showDocument(int id);

signals:
    // Остатки изменились (документ создан, изменён, отменён или удалён с возвратом остатков): другие страницы устарели.
    void stockChanged();

private slots:
    void documentChanged();
    void cancelSelected();
    void cancelLast();
    void exportSelected();
    void editSelected();
    void editComment();
    void deleteSelected();
    void createDocument(int kind); // BatchDialog::Kind

private:
    void fillDocuments();
    void fillLines(const Documents::Info &d);
    int selectedDocumentId() const;
    void cancelDocument(int id);
    void updateButtons();
    void showContextMenu(const QPoint &pos);

    QComboBox *m_kind;
    QLineEdit *m_search;
    QTableWidget *m_docs;
    QTableWidget *m_lines;
    QLabel *m_header;
    QPushButton *m_cancelBtn;
    QPushButton *m_lastBtn;
    QPushButton *m_exportBtn;
    QPushButton *m_newBtn;
    QPushButton *m_editBtn;
    QPushButton *m_commentBtn;
    QPushButton *m_deleteBtn;
    QTimer *m_searchTimer;
    QList<Documents::Info> m_list;
    bool m_readOnly = false;
    bool m_admin = false;
    bool m_filling = false;
};
