#pragma once

#include <QDialog>
#include "serverconfig.h"

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;

// Диалог ввода параметров подключения к серверу PostgreSQL (адрес, порт,
// имя базы, пользователь, пароль). Показывается при первом запуске и по
// запросу из меню, умеет проверить соединение перед сохранением.
class ConnectionDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget *parent = nullptr);

    ServerConfig config() const;

private slots:
    void testConnection();
    void trySave();

private:
    QLineEdit *m_host;
    QSpinBox *m_port;
    QLineEdit *m_database;
    QLineEdit *m_user;
    QLineEdit *m_password;
    QCheckBox *m_useSsl;
    QLabel *m_status;
};
