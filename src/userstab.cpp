#include "userstab.h"
#include "authservice.h"

#include <QComboBox>
#include <QInputDialog>
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

// Диалог с логином (опционально), паролем с подтверждением и выбором роли
// (опционально). Проверки выполняются до закрытия окна,
// чтобы при ошибке не приходилось вводить всё заново.
class UserDialog : public QDialog
{
public:
    UserDialog(const QString &title, const QString &fixedUsername, bool askRole, QWidget *parent)
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

        if (askRole) {
            m_role = new QComboBox(this);
            m_role->addItem(tr("Сотрудник - редактирует данные"), int(AuthService::Role::Editor));
            m_role->addItem(tr("Только просмотр - ничего не меняет"), int(AuthService::Role::Viewer));
            m_role->addItem(tr("Администратор - ещё и пользователи, кабинеты Маркета"), int(AuthService::Role::Admin));
            form->addRow(tr("Роль"), m_role);
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
    AuthService::Role role() const
    {
        return m_role ? AuthService::Role(m_role->currentData().toInt()) : AuthService::Role::Editor;
    }

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
    QComboBox *m_role = nullptr;
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
    auto *adminBtn = new QPushButton(tr("Изменить роль"), this);
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
                               "Последнего администратора удалить или лишить прав нельзя. Роль «Только просмотр» "
                               "действует и в приложении, и на сайте (изменения вступят в силу при следующем входе)."),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    connect(addBtn, &QPushButton::clicked, this, &UsersTab::addUser);
    connect(passwordBtn, &QPushButton::clicked, this, &UsersTab::changeSelectedPassword);
    connect(adminBtn, &QPushButton::clicked, this, &UsersTab::changeRole);
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
        login->setData(Qt::UserRole + 1, int(AuthService::roleOf(u)));
        login->setData(Qt::UserRole + 2, u.username);
        if (u.username == m_currentUsername)
            login->setText(tr("%1 (вы)").arg(u.username));
        m_table->setItem(row, ColLogin, login);
        m_table->setItem(row, ColRole, new QTableWidgetItem(AuthService::roleName(AuthService::roleOf(u))));
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
    if (!AuthService::createUser(dlg.username(), dlg.password(), dlg.role(), &err)) {
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

void UsersTab::changeRole()
{
    const int row = selectedRow();
    if (row < 0)
        return;
    const auto *item = m_table->item(row, ColLogin);
    const int id = item->data(Qt::UserRole).toInt();
    const auto current = AuthService::Role(item->data(Qt::UserRole + 1).toInt());
    const QList<AuthService::Role> roles{AuthService::Role::Editor, AuthService::Role::Viewer, AuthService::Role::Admin};
    QStringList names;
    for (auto r : roles)
        names << AuthService::roleName(r);
    bool ok = false;
    const QString choice = QInputDialog::getItem(this, tr("Роль пользователя"),
                                                 tr("Роль для %1:").arg(item->data(Qt::UserRole + 2).toString()),
                                                 names, int(roles.indexOf(current)), false, &ok);
    if (!ok)
        return;
    QString err;
    if (!AuthService::setRole(id, roles.value(names.indexOf(choice)), &err)) {
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
