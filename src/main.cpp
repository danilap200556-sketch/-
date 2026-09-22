#include "connectiondialog.h"
#include "database.h"
#include "logindialog.h"
#include "mainwindow.h"
#include "serverconfig.h"

#include <QApplication>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("InventoryManager");
    QApplication::setOrganizationName("InventoryManager");

    ServerConfig config = ServerConfig::load();
    bool askServer = false;

    for (;;) {
        // Первый запуск, прошлые данные больше не подходят или пользователь
        // сам попросил сменить сервер - спрашиваем адрес, пока не подключимся
        // или пользователь не откажется.
        QString error;
        while (askServer || !config.isComplete() || !Database::open(config, &error)) {
            if (!error.isEmpty()) {
                QMessageBox::warning(nullptr, QObject::tr("Не удалось подключиться"),
                                      QObject::tr("Не удалось подключиться к серверу:\n%1").arg(error));
            }
            ConnectionDialog dlg(nullptr, &config);
            if (dlg.exec() != QDialog::Accepted)
                return 0;
            config = dlg.config();
            error.clear();
            askServer = false;
        }
        config.save();

        LoginDialog login;
        const int result = login.exec();
        if (result == LoginDialog::ChangeServer) {
            askServer = true;
            continue;
        }
        if (result != QDialog::Accepted)
            return 0;

        MainWindow window(login.username());
        window.show();
        return app.exec();
    }
}
