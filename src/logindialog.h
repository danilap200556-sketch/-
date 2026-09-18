#pragma once

#include <QDialog>

class QLineEdit;
class QLabel;

// Показывается перед главным окном. Если пользователей в базе ещё нет -
// работает как форма создания первой (администраторской) учётной записи,
// иначе - как обычная форма логина. Простая локальная реализация "на
// будущее": позже сюда можно подключить сетевую проверку/несколько
// кабинетов без изменения остального приложения (см. authservice.h).
class LoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LoginDialog(QWidget *parent = nullptr);

    QString username() const;

private slots:
    void trySubmit();

private:
    bool m_registerMode;
    QLineEdit *m_username;
    QLineEdit *m_password;
    QLineEdit *m_confirmPassword = nullptr;
    QLabel *m_error;
};
