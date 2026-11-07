#pragma once

#include "documents.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QRadioButton;
class QTableWidget;

// Удаление документа остатков целиком: показывает, что исчезнет из журнала и что станет с остатками, и просит выбрать - вернуть
// остатки, как будто документа не было, или оставить их как есть (просто убрать документ из истории). Если вернуть остатки нельзя
// (товар уже расходован), объясняет почему. Удаление нельзя отменить.
class DocumentDeleteDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DocumentDeleteDialog(int documentId, QWidget *parent = nullptr);

    bool deleted() const { return m_deleted; }
    // Для проверок: выбрать вариант и выполнить удаление без вопросов. При отказе - false, причины в problems().
    void setRestoreStock(bool restore);
    bool apply();
    QStringList problems() const { return m_plan.problems; }
    int rowCount() const;

private:
    void reload();

    int m_id = 0;
    Documents::Info m_info;
    Documents::DeletePlan m_plan;
    QRadioButton *m_restore = nullptr;
    QRadioButton *m_keep = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_problems = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    bool m_deleted = false;
};
