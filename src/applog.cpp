#include "applog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

namespace {

constexpr qint64 kMaxBytes = 2 * 1024 * 1024;

QMutex g_mutex;
QFile g_file;
QString g_path;
QtMessageHandler g_previous = nullptr;

bool tryOpen(const QString &path)
{
    QFileInfo info(path);
    if (info.exists() && info.size() > kMaxBytes) {
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    g_file.setFileName(path);
    if (!g_file.open(QIODevice::Append | QIODevice::Text))
        return false;
    g_path = path;
    return true;
}

void writeLine(const QString &level, const QString &text)
{
    QMutexLocker lock(&g_mutex);
    if (!g_file.isOpen())
        return;
    QTextStream out(&g_file);
    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << " [" << level << "] " << text << '\n';
    out.flush();
    g_file.flush();
}

void handler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *level = "INFO";
    switch (type) {
    case QtDebugMsg: level = "DEBUG"; break;
    case QtInfoMsg: level = "INFO"; break;
    case QtWarningMsg: level = "WARN"; break;
    case QtCriticalMsg: level = "ERROR"; break;
    case QtFatalMsg: level = "FATAL"; break;
    }
    writeLine(QLatin1String(level), msg);
    if (type == QtFatalMsg)
        abort();
}

} // namespace

namespace AppLog {

void install()
{
    {
        QMutexLocker lock(&g_mutex);
        if (g_file.isOpen())
            return;
        const QString name = QStringLiteral("inventory_manager.log");
        bool ok = tryOpen(QDir(QCoreApplication::applicationDirPath()).filePath(name));
        if (!ok) {
            const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            QDir().mkpath(dir);
            ok = tryOpen(QDir(dir).filePath(name));
        }
        if (!ok)
            return; // писать некуда - работаем без журнала
    }
    g_previous = qInstallMessageHandler(handler);
    writeLine(QStringLiteral("INFO"), QStringLiteral("Запуск приложения (Qt %1)").arg(QString::fromLatin1(qVersion())));
}

void write(const QString &section, const QString &text)
{
    writeLine(QStringLiteral("INFO"), section.isEmpty() ? text : QStringLiteral("%1: %2").arg(section, text));
}

QString path()
{
    return g_path;
}

} // namespace AppLog
