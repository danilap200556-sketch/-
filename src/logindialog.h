#pragma once

#include <QDialog>

class QLineEdit;
class QLabel;
class QPushButton;
class QFrame;

// Показывается перед главным окном. Если пользователей в базе ещё нет - работает как форма создания
// первой (администраторской) учётной записи, иначе - как обычная форма входа. Оформление: тёмный
// градиентный фон со свечением и «стеклянная» полупрозрачная карточка с полями; окно без рамки,
// плавно появляется, а при неверном пароле карточка встряхивается.
class LoginDialog : public QDialog
{
    Q_OBJECT
public:
    // Код возврата exec(), когда пользователь хочет подключиться к другому серверу.
    enum { ChangeServer = 2 };

    explicit LoginDialog(QWidget *parent = nullptr);

    QString username() const;

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

private slots:
    void trySubmit();

private:
    void shake();
    void fail(const QString &text);

    bool m_registerMode;
    QLineEdit *m_username;
    QLineEdit *m_password;
    QLineEdit *m_confirmPassword = nullptr;
    QLabel *m_error;
    QPushButton *m_submit;
    QPoint m_dragOffset;
    bool m_dragging = false;
};
