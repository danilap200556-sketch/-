#include "userstab.h"
#include "authservice.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Col { ColLogin = 0, ColRole, ColCreated };

// Диалог с логином (опционально), паролем с подтверждением и галкой
// "администратор" (опционально). Проверки выполняются до закрытия окна,
// чтобы при ошибке не приходилось вводить всё заново.
class UserDialog : public QDialog
{
public:
    UserDialog(const QString &title, const QString &fixedUsername, bool askAdmin, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(title);
        setMinimumWidth(340);
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout();
        layout->addLayout(form);

        m_username = new QLineEdit(fixedUsername, this);
        m_username->setReadOnly(!fixedUsername.isEmpty());
        form->addRow(tr("Логин"), m_username);

        m_password = new QLineEdit(this);
        m_password->setEchoMode(QLineEdit::Password);
        form->addRow(tr("Пароль"), m_password);

        m_confirm = new QLineEdit(this);
        m_confirm->setEchoMode(QLineEdit::Password);
        form->addRow(tr("Повторите пароль"), m_confirm);

        if (askAdmin) {
            m_admin = new QCheckBox(tr("Администратор (может управлять пользователями и кабинетами Маркета)"), this);
            layout->addWidget(m_admin);
        }

        m_error = new QLabel(this);
        m_error->setStyleSheet("color: #c0392b;");
        m_error->setWordWrap(true);
        layout->addWidget(m_error);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &UserDialog::validateAndAccept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);

        (fixedUsername.isEmpty() ? m_username : m_password)->setFocus();
    }

    QString username() const { return m_username->text().trimmed(); }
    QString password() const { return m_password->text(); }
    bool isAdmin() const { return m_admin && m_admin->isChecked(); }

private:
    void validateAndAccept()
    {
        QString err;
        if (username().isEmpty())
            err = tr("Введите логин");
        else if (password() != m_confirm->text())
            err = tr("Пароли не совпадают");
        else
            AuthService::validatePassword(password(), &err);
        if (!err.isEmpty()) {
            m_error->setText(err);
            return;
        }
        accept();
    }

    QLineEdit *m_username;
    QLineEdit *m_password;
    QLineEdit *m_confirm;
    QCheckBox *m_admin = nullptr;
    QLabel *m_error;
};

} // namespace

UsersTab::UsersTab(const QString &currentUsername, QWidget *parent)
    : QWidget(parent), m_currentUsername(currentUsername)
{
    auto *layout = new QVBoxLayout(this);

    auto *toolbar = new QHBoxLayout();
    auto *addBtn = new QPushButton(tr("Добавить пользователя"), this);
    auto *passwordBtn = new QPushButton(tr("Сменить пароль"), this);
    auto *adminBtn = new QPushButton(tr("Дать / снять права администратора"), this);
    auto *deleteBtn = new QPushButton(tr("Удалить"), this);
    toolbar->addWidget(addBtn);
    toolbar->addWidget(passwordBtn);
    toolbar->addWidget(adminBtn);
    toolbar->addWidget(deleteBtn);
    toolbar->addStretch();
    layout->addLayout(toolbar);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({tr("Логин"), tr("Роль"), tr("Создан")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table);

    auto *hint = new QLabel(tr("Пользователи общие для всех компьютеров, подключённых к этому серверу. "
                               "Последнего администратора удалить или лишить прав нельзя."),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    connect(addBtn, &QPushButton::clicked, this, &UsersTab::addUser);
    connect(passwordBtn, &QPushButton::clicked, this, &UsersTab::changeSelectedPassword);
    connect(adminBtn, &QPushButton::clicked, this, &UsersTab::toggleAdmin);
    connect(deleteBtn, &QPushButton::clicked, this, &UsersTab::deleteUser);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &UsersTab::changeSelectedPassword);

    refresh();
}

void UsersTab::refresh()
{
    const auto users = AuthService::listUsers();
    m_table->setRowCount(0);
    for (const auto &u : users) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *login = new QTableWidgetItem(u.username);
        login->setData(Qt::UserRole, u.id);
        login->setData(Qt::UserRole + 1, u.isAdmin);
        login->setData(Qt::UserRole + 2, u.username);
        if (u.username == m_currentUsername)
            login->setText(tr("%1 (вы)").arg(u.username));
        m_table->setItem(row, ColLogin, login);
        m_table->setItem(row, ColRole, new QTableWidgetItem(u.isAdmin ? tr("Администратор") : tr("Пользователь")));
        m_table->setItem(row, ColCreated,
                         new QTableWidgetItem(u.createdAt.toLocalTime().toString("dd.MM.yyyy HH:mm")));
    }
    m_table->resizeColumnsToContents();
}

int UsersTab::selectedRow() const
{
    const auto sel = m_table->selectionModel()->selectedRows();
    return sel.isEmpty() ? -1 : sel.first().row();
}

bool UsersTab::changePassword(QWidget *parent, int userId, const QString &username)
{
    UserDialog dlg(tr("Новый пароль — %1").arg(username), username, false, parent);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    QString err;
    if (!AuthService::setPassword(userId, dlg.password(), &err)) {
        QMessageBox::warning(parent, tr("Ошибка"), err);
        return false;
    }
    QMessageBox::information(parent, tr("Готово"), tr("Пароль пользователя %1 изменён.").arg(username));
    return true;
}

void UsersTab::addUser()
{
    UserDialog dlg(tr("Новый пользователь"), QString(), true, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!AuthService::createUser(dlg.username(), dlg.password(), dlg.isAdmin(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

void UsersTab::changeSelectedPassword()
{
    const int row = selectedRow();
    if (row < 0)
        return;
    const auto *item = m_table->item(row, ColLogin);
    const QString username = item->data(Qt::UserRole + 2).toString();
    changePassword(this, item->data(Qt::UserRole).toInt(), username);
}

void UsersTab::toggleAdmin()
{
    const int row = selectedRow();
    if (row < 0)
        return;
    const auto *item = m_table->item(row, ColLogin);
    const int id = item->data(Qt::UserRole).toInt();
    const bool wasAdmin = item->data(Qt::UserRole + 1).toBool();
    QString err;
    if (!AuthService::setAdmin(id, !wasAdmin, &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}

void UsersTab::deleteUser()
{
    const int row = selectedRow();
    if (row < 0)
        return;
    const auto *item = m_table->item(row, ColLogin);
    const QString username = item->data(Qt::UserRole + 2).toString();
    if (username == m_currentUsername) {
        QMessageBox::warning(this, tr("Нельзя"), tr("Нельзя удалить учётную запись, под которой вы сейчас работаете."));
        return;
    }
    if (QMessageBox::question(this, tr("Удалить пользователя"),
                              tr("Удалить пользователя %1? Он больше не сможет войти в приложение.").arg(username))
        != QMessageBox::Yes)
        return;
    QString err;
    if (!AuthService::deleteUser(item->data(Qt::UserRole).toInt(), &err)) {
        QMessageBox::warning(this, tr("Ошибка"), err);
        return;
    }
    refresh();
}
