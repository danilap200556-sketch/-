#include "connectiondialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVBoxLayout>

namespace {
constexpr auto kTestConnectionName = "connection_test";
}

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Подключение к серверу"));
    setMinimumWidth(380);

    const ServerConfig current = ServerConfig::load();

    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(
        tr("Данные для подключения к серверу PostgreSQL (например, из панели Neon: "
           "Connection string / Connection details)."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    auto *form = new QFormLayout();
    layout->addLayout(form);

    m_host = new QLineEdit(current.host, this);
    m_host->setPlaceholderText(tr("напр. ep-xxx.eu-central-1.aws.neon.tech"));
    form->addRow(tr("Адрес сервера (host)"), m_host);

    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(current.port > 0 ? current.port : 5432);
    form->addRow(tr("Порт"), m_port);

    m_database = new QLineEdit(current.database, this);
    form->addRow(tr("База данных"), m_database);

    m_user = new QLineEdit(current.user, this);
    form->addRow(tr("Пользователь"), m_user);

    m_password = new QLineEdit(current.password, this);
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Пароль"), m_password);

    m_useSsl = new QCheckBox(tr("Использовать SSL (обязательно для Neon/Supabase)"), this);
    m_useSsl->setChecked(current.useSsl);
    layout->addWidget(m_useSsl);

    auto *testBtn = new QPushButton(tr("Проверить соединение"), this);
    layout->addWidget(testBtn);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ConnectionDialog::trySave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(testBtn, &QPushButton::clicked, this, &ConnectionDialog::testConnection);
}

ServerConfig ConnectionDialog::config() const
{
    ServerConfig cfg;
    cfg.host = m_host->text().trimmed();
    cfg.port = m_port->value();
    cfg.database = m_database->text().trimmed();
    cfg.user = m_user->text().trimmed();
    cfg.password = m_password->text();
    cfg.useSsl = m_useSsl->isChecked();
    return cfg;
}

void ConnectionDialog::testConnection()
{
    const ServerConfig cfg = config();
    if (!cfg.isComplete()) {
        m_status->setStyleSheet("color: #c0392b;");
        m_status->setText(tr("Заполните адрес, базу и пользователя."));
        return;
    }

    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", kTestConnectionName);
        db.setHostName(cfg.host);
        db.setPort(cfg.port);
        db.setDatabaseName(cfg.database);
        db.setUserName(cfg.user);
        db.setPassword(cfg.password);
        if (cfg.useSsl)
            db.setConnectOptions("sslmode=require;sslnegotiation=postgres;connect_timeout=20");

        if (db.open()) {
            m_status->setStyleSheet("color: #27ae60;");
            m_status->setText(tr("Соединение установлено успешно."));
            db.close();
        } else {
            m_status->setStyleSheet("color: #c0392b;");
            m_status->setText(tr("Не удалось подключиться:\n%1").arg(db.lastError().text()));
        }
    }
    QSqlDatabase::removeDatabase(kTestConnectionName);
}

void ConnectionDialog::trySave()
{
    const ServerConfig cfg = config();
    if (!cfg.isComplete()) {
        QMessageBox::warning(this, tr("Проверьте поля"), tr("Заполните адрес, базу и пользователя."));
        return;
    }
    accept();
}
