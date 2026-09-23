#pragma once

#include <QWidget>

class QTableWidget;

// Управление учётными записями (видна только администраторам).
class UsersTab : public QWidget
{
    Q_OBJECT
public:
    explicit UsersTab(const QString &currentUsername, QWidget *parent = nullptr);

    // Диалог смены пароля для указанного пользователя; true - пароль изменён.
    static bool changePassword(QWidget *parent, int userId, const QString &username);

public slots:
    void refresh();

private slots:
    void addUser();
    void changeSelectedPassword();
    void toggleAdmin();
    void deleteUser();

private:
    int selectedRow() const;

    QString m_currentUsername;
    QTableWidget *m_table;
};
