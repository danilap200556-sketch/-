#include "logindialog.h"
#include "authservice.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent), m_registerMode(!AuthService::hasAnyUser())
{
    setWindowTitle(m_registerMode ? tr("Создание учётной записи") : tr("Вход"));
    setMinimumWidth(340);

    auto *layout = new QVBoxLayout(this);

    if (m_registerMode) {
        auto *hint = new QLabel(tr("Первый запуск - создайте учётную запись, под которой вы будете\n"
                                    "заходить в приложение."),
                                 this);
        hint->setWordWrap(true);
        hint->setStyleSheet("color: gray;");
        layout->addWidget(hint);
    }

    auto *form = new QFormLayout();
    layout->addLayout(form);

    m_username = new QLineEdit(this);
    form->addRow(tr("Логин"), m_username);

    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Пароль"), m_password);

    if (m_registerMode) {
        m_confirmPassword = new QLineEdit(this);
        m_confirmPassword->setEchoMode(QLineEdit::Password);
        form->addRow(tr("Повторите пароль"), m_confirmPassword);
    }

    m_error = new QLabel(this);
    m_error->setStyleSheet("color: #c0392b;");
    m_error->setWordWrap(true);
    layout->addWidget(m_error);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(m_registerMode ? tr("Создать") : tr("Войти"));
    auto *changeServer = buttons->addButton(tr("Сменить сервер…"), QDialogButtonBox::ResetRole);
    connect(changeServer, &QPushButton::clicked, this, [this] { done(ChangeServer); });
    connect(buttons, &QDialogButtonBox::accepted, this, &LoginDialog::trySubmit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_password, &QLineEdit::returnPressed, this, &LoginDialog::trySubmit);
    if (m_confirmPassword)
        connect(m_confirmPassword, &QLineEdit::returnPressed, this, &LoginDialog::trySubmit);
}

void LoginDialog::trySubmit()
{
    m_error->clear();
    QString err;

    if (m_registerMode) {
        if (m_password->text() != m_confirmPassword->text()) {
            m_error->setText(tr("Пароли не совпадают"));
            return;
        }
        if (!AuthService::createUser(m_username->text(), m_password->text(), &err)) {
            m_error->setText(err);
            return;
        }
    } else {
        if (!AuthService::verifyLogin(m_username->text(), m_password->text(), &err)) {
            m_error->setText(err);
            m_password->clear();
            m_password->setFocus();
            return;
        }
    }

    accept();
}

QString LoginDialog::username() const
{
    return m_username->text().trimmed();
}
