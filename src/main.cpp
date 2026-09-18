#include "database.h"
#include "logindialog.h"
#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("InventoryManager");
    QApplication::setOrganizationName("InventoryManager");

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    const QString dbPath = dataDir + "/inventory.db";

    QString error;
    if (!Database::open(dbPath, &error)) {
        QMessageBox::critical(nullptr, QObject::tr("Ошибка базы данных"),
                               QObject::tr("Не удалось открыть базу данных:\n%1\n\n%2").arg(dbPath, error));
        return 1;
    }

    LoginDialog login;
    if (login.exec() != QDialog::Accepted)
        return 0;

    MainWindow window(login.username());
    window.show();

    return app.exec();
}
