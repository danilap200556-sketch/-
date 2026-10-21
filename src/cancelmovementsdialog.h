#pragma once

#include "database.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

// Отмена записей журнала: показывает, что изменится (остаток сейчас -> станет), и, если отменить нельзя
// (товар уже расходован, запись уже отменена), объясняет почему. Журнал не стирается: добавляются записи
// «Отмена» с противоположным изменением.
class CancelMovementsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CancelMovementsDialog(const QList<int> &movementIds, QWidget *parent = nullptr);

    bool cancelled() const { return m_cancelled; }
    int rowCount() const;
    QStringList problems() const { return m_plan.problems; }
    void setReason(const QString &reason);
    // Отменяет без вопросов (то же, что кнопка). При отказе - false, причины в problems().
    bool apply();

private:
    void reload();

    QList<int> m_ids;
    Database::CancelPlan m_plan;
    QTableWidget *m_table = nullptr;
    QLabel *m_problems = nullptr;
    QLineEdit *m_reason = nullptr;
    QPushButton *m_apply = nullptr;
    bool m_cancelled = false;
};
