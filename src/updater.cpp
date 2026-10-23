#include "updater.h"
#include "appversion.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <memory>

// QZipReader: до Qt 6.6 лежал в QtGui, с Qt 6.6 - в QtCore (тот же приём, что в xlsx.cpp).
#if __has_include(<QtCore/private/qzipreader_p.h>)
#include <QtCore/private/qzipreader_p.h>
#else
#include <QtGui/private/qzipreader_p.h>
#endif

namespace {

const char kDefaultBase[] = "https://github.com/danilap200556-sketch/-/releases/download/app-latest/";

QString friendlyNetworkError(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 404)
        return QStringLiteral("Обновлений на GitHub пока нет: релиз «app-latest» не найден. Он появится после ближайшей сборки программы.");
    if (status >= 400)
        return QStringLiteral("Сервер обновлений ответил ошибкой %1.").arg(status);
    return reply->errorString();
}

bool safeFileName(const QString &name)
{
    return !name.isEmpty() && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) &&
           !name.contains(QLatin1Char(':')) && name != QLatin1String("..") && name != QLatin1String(".");
}

QString exeName() { return QFileInfo(QCoreApplication::applicationFilePath()).fileName(); }

} // namespace

QString Updater::baseUrl()
{
    QString base = qEnvironmentVariable("INVENTORY_UPDATE_URL");
    if (base.isEmpty())
        base = QString::fromLatin1(kDefaultBase);
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    return base;
}

QString Updater::stagingRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("InventoryManagerUpdate"));
}

Updater::Updater(QObject *parent)
    : QObject(parent)
{
}

Updater::~Updater()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

bool Updater::newerThanCurrent() const { return hasLatest() && m_latest.build > AppVersion::build(); }
bool Updater::updateAvailable() const { return AppVersion::build() > 0 && newerThanCurrent(); }

QString Updater::buildDir() const
{
    return QDir(stagingRoot()).filePath(QStringLiteral("b%1").arg(m_latest.build));
}

QString Updater::stagedDir() const
{
    return hasLatest() ? QDir(buildDir()).filePath(QStringLiteral("files")) : QString();
}

void Updater::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}

bool Updater::parseManifest(const QByteArray &json, Release *out, QString *error)
{
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("Файл version.json на сервере обновлений повреждён."));
    const QJsonObject o = doc.object();
    Release r;
    r.build = o.value(QStringLiteral("build")).toInt();
    r.commit = o.value(QStringLiteral("commit")).toString();
    r.date = QDateTime::fromString(o.value(QStringLiteral("date")).toString(), Qt::ISODate);
    r.file = o.value(QStringLiteral("file")).toString();
    r.sha256 = o.value(QStringLiteral("sha256")).toString().toLower();
    r.size = qint64(o.value(QStringLiteral("size")).toDouble());
    r.notes = o.value(QStringLiteral("notes")).toString();
    if (r.build <= 0)
        return fail(QStringLiteral("В version.json нет номера сборки."));
    if (!safeFileName(r.file))
        return fail(QStringLiteral("В version.json неверное имя файла обновления."));
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    if (!hex.match(r.sha256).hasMatch())
        return fail(QStringLiteral("В version.json нет контрольной суммы обновления."));
    for (const QJsonValue &v : o.value(QStringLiteral("history")).toArray()) {
        const QJsonObject h = v.toObject();
        HistoryItem item;
        item.build = h.value(QStringLiteral("build")).toInt();
        if (item.build <= 0)
            continue;
        item.commit = h.value(QStringLiteral("commit")).toString();
        item.date = QDateTime::fromString(h.value(QStringLiteral("date")).toString(), Qt::ISODate);
        item.title = h.value(QStringLiteral("title")).toString();
        item.notes = h.value(QStringLiteral("notes")).toString();
        r.history << item;
    }
    if (out)
        *out = r;
    return true;
}

void Updater::check()
{
    if (m_state == State::Checking || m_state == State::Downloading || m_state == State::Unpacking)
        return;
    m_error.clear();
    setState(State::Checking);
    QNetworkRequest req{QUrl(baseUrl() + QStringLiteral("version.json"))};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("InventoryManager/%1").arg(AppVersion::build()));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setTransferTimeout(20000);
    m_reply = m_nam.get(req);
    connect(m_reply, &QNetworkReply::finished, this, &Updater::onChecked);
}

void Updater::onChecked()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    Release rel;
    QString err;
    bool ok = false;
    if (reply->error() != QNetworkReply::NoError)
        err = friendlyNetworkError(reply);
    else
        ok = parseManifest(reply->readAll(), &rel, &err);
    if (ok)
        m_latest = rel;
    m_error = ok ? QString() : err;
    setState(State::Idle);
    emit checkFinished(ok, err);
}

void Updater::failDownload(const QString &text)
{
    m_file.close();
    QDir(buildDir()).removeRecursively();
    m_error = text;
    setState(State::Idle);
    emit downloadFinished(false, text);
}

void Updater::download()
{
    if (!hasLatest() || m_state == State::Checking || m_state == State::Downloading || m_state == State::Unpacking)
        return;
    m_error.clear();
    QDir(buildDir()).removeRecursively();
    if (!QDir().mkpath(buildDir())) {
        failDownload(QStringLiteral("Не удалось создать временную папку %1.").arg(QDir::toNativeSeparators(buildDir())));
        return;
    }
    m_file.setFileName(QDir(buildDir()).filePath(QStringLiteral("pkg.zip")));
    if (!m_file.open(QIODevice::WriteOnly)) {
        failDownload(QStringLiteral("Не удалось записать файл %1: %2").arg(QDir::toNativeSeparators(m_file.fileName()), m_file.errorString()));
        return;
    }
    m_expected = m_latest.size;
    setState(State::Downloading);
    QNetworkRequest req{QUrl(baseUrl() + m_latest.file)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("InventoryManager/%1").arg(AppVersion::build()));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000); // нет данных минуту - считаем, что связь пропала
    m_reply = m_nam.get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (!m_reply)
            return;
        const QByteArray chunk = m_reply->readAll();
        if (m_file.write(chunk) != chunk.size())
            m_reply->abort(); // диск заполнен: finished придёт с ошибкой
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 done, qint64 total) {
        emit progress(done, total > 0 ? total : m_expected);
    });
    connect(m_reply, &QNetworkReply::finished, this, &Updater::onDownloaded);
}

void Updater::onDownloaded()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() == QNetworkReply::OperationCanceledError && m_state != State::Downloading)
            return; // отменили сами
        failDownload(QStringLiteral("Не удалось скачать обновление: %1").arg(friendlyNetworkError(reply)));
        return;
    }
    m_file.write(reply->readAll()); // остаток, если он не пришёл через readyRead
    const qint64 got = m_file.size();
    m_file.close();
    if (m_latest.size > 0 && got != m_latest.size) {
        failDownload(QStringLiteral("Скачано %1 байт вместо %2 - файл неполный. Попробуйте ещё раз.").arg(got).arg(m_latest.size));
        return;
    }
    // Контрольная сумма - по скачанному файлу целиком.
    QFile again(m_file.fileName());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!again.open(QIODevice::ReadOnly) || !hash.addData(&again)) {
        failDownload(QStringLiteral("Не удалось прочитать скачанный файл для проверки."));
        return;
    }
    again.close();
    if (QString::fromLatin1(hash.result().toHex()) != m_latest.sha256) {
        failDownload(QStringLiteral("Контрольная сумма не совпала - файл повреждён при загрузке. Попробуйте ещё раз."));
        return;
    }

    setState(State::Unpacking);
    const QString zip = m_file.fileName();
    const QString dest = stagedDir();
    auto result = std::make_shared<QPair<bool, QString>>(false, QString());
    m_thread = QThread::create([zip, dest, result]() {
        QString err;
        result->first = unpackZip(zip, dest, &err);
        result->second = err;
    });
    connect(m_thread, &QThread::finished, this, [this, result]() {
        m_thread->deleteLater();
        m_thread = nullptr;
        if (!result->first) {
            failDownload(result->second);
            return;
        }
        if (!QFileInfo::exists(QDir(stagedDir()).filePath(exeName()))) {
            failDownload(QStringLiteral("В архиве обновления нет файла %1.").arg(exeName()));
            return;
        }
        QFile::remove(m_file.fileName()); // архив больше не нужен: остались распакованные файлы
        setState(State::Ready);
        emit downloadFinished(true, QString());
    });
    m_thread->start();
}

void Updater::cancel()
{
    if (m_state == State::Downloading && m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
        m_file.close();
        QDir(buildDir()).removeRecursively();
        setState(State::Idle);
    }
}

bool Updater::unpackZip(const QString &zipPath, const QString &dest, QString *error)
{
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    QZipReader zip(zipPath);
    if (!zip.isReadable() || zip.status() != QZipReader::NoError)
        return fail(QStringLiteral("Архив обновления не открывается."));
    if (!QDir().mkpath(dest))
        return fail(QStringLiteral("Не удалось создать папку %1.").arg(QDir::toNativeSeparators(dest)));
    const QString root = QDir(dest).canonicalPath() + QLatin1Char('/');
    const auto entries = zip.fileInfoList();
    if (entries.isEmpty())
        return fail(QStringLiteral("Архив обновления пуст."));
    for (const QZipReader::FileInfo &fi : entries) {
        const QString name = fi.filePath;
        // абсолютные пути, «..», диск и обратные косые - так файл мог бы оказаться вне папки обновления
        if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.startsWith(QLatin1Char('\\')) || name.contains(QLatin1Char(':')) ||
            name.contains(QLatin1Char('\\')) || name.split(QLatin1Char('/')).contains(QStringLiteral("..")))
            return fail(QStringLiteral("В архиве обновления небезопасный путь «%1» - установка отменена.").arg(name));
        const QString target = QDir::cleanPath(root + name);
        if (!(target + QLatin1Char('/')).startsWith(root))
            return fail(QStringLiteral("В архиве обновления небезопасный путь «%1» - установка отменена.").arg(name));
        if (fi.isSymLink)
            continue;
        if (fi.isDir) {
            QDir().mkpath(target);
            continue;
        }
        if (!fi.isFile)
            continue;
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("Не удалось записать файл %1: %2").arg(QDir::toNativeSeparators(target), out.errorString()));
        const QByteArray data = zip.fileData(name);
        if (data.size() != fi.size || out.write(data) != data.size())
            return fail(QStringLiteral("Не удалось распаковать файл %1: архив повреждён или содержит небезопасные пути.").arg(name));
    }
    return true;
}

QString Updater::scriptText()
{
    // cmd читает скрипт в кодовой странице консоли, поэтому он целиком на латинице; пути (в них бывает кириллица)
    // передаются аргументами командной строки в Юникоде. Коды robocopy 0-7 - успех, 8 и больше - ошибка.
    const QStringList lines = {
        QStringLiteral("@echo off"),
        QStringLiteral("setlocal EnableExtensions"),
        QStringLiteral("rem Args: 1=program folder, 2=folder with new files, 3=backup folder, 4=program exe, 5=program process id, 6=new build"),
        QStringLiteral("set \"APP=%~1\""),
        QStringLiteral("set \"STAGE=%~2\""),
        QStringLiteral("set \"BAK=%~3\""),
        QStringLiteral("set \"EXE=%~4\""),
        QStringLiteral("set \"PID=%~5\""),
        QStringLiteral("set \"BUILD=%~6\""),
        QStringLiteral("set \"RESULT=%APP%\\update_result.txt\""),
        QStringLiteral("rem wait until the program exits (up to about 90 seconds)"),
        QStringLiteral("set /a TRIES=0"),
        QStringLiteral(":wait"),
        QStringLiteral("tasklist /FI \"PID eq %PID%\" /NH 2>nul | find \"%PID%\" >nul"),
        QStringLiteral("if errorlevel 1 goto copy"),
        QStringLiteral("set /a TRIES+=1"),
        QStringLiteral("if %TRIES% GTR 90 goto notclosed"),
        QStringLiteral("ping -n 2 127.0.0.1 >nul"),
        QStringLiteral("goto wait"),
        QStringLiteral(":copy"),
        QStringLiteral("rem backup of the current files, then copy the new ones over them"),
        QStringLiteral("rd /s /q \"%BAK%\" 2>nul"),
        QStringLiteral("robocopy \"%APP%\" \"%BAK%\" /E /XD \"%BAK%\" /R:2 /W:1 /NFL /NDL /NJH /NJS /NP >nul"),
        QStringLiteral("if errorlevel 8 goto nobackup"),
        QStringLiteral("robocopy \"%STAGE%\" \"%APP%\" /E /R:30 /W:1 /NFL /NDL /NJH /NJS /NP >nul"),
        QStringLiteral("if errorlevel 8 goto rollback"),
        QStringLiteral("> \"%RESULT%\" echo ok^|%BUILD%"),
        QStringLiteral("rd /s /q \"%BAK%\" 2>nul"),
        QStringLiteral("for %%I in (\"%STAGE%\\..\") do rd /s /q \"%%~fI\" 2>nul"),
        QStringLiteral("goto start"),
        QStringLiteral(":rollback"),
        QStringLiteral("robocopy \"%BAK%\" \"%APP%\" /E /R:5 /W:1 /NFL /NDL /NJH /NJS /NP >nul"),
        QStringLiteral("> \"%RESULT%\" echo fail^|%BUILD%^|copy failed, the previous version was restored"),
        QStringLiteral("goto start"),
        QStringLiteral(":nobackup"),
        QStringLiteral("> \"%RESULT%\" echo fail^|%BUILD%^|backup failed, nothing was changed"),
        QStringLiteral("goto start"),
        QStringLiteral(":notclosed"),
        QStringLiteral("> \"%RESULT%\" echo fail^|%BUILD%^|the program did not close in time"),
        QStringLiteral(":start"),
        QStringLiteral("cd /d \"%APP%\""),
        QStringLiteral("start \"\" \"%EXE%\""),
        QStringLiteral("(goto) 2>nul & del \"%~f0\""),
    };
    return lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n");
}

QString Updater::nativeQuoted(const QString &path)
{
    return QLatin1Char('"') + QDir::toNativeSeparators(path) + QLatin1Char('"');
}

bool Updater::install(QString *error)
{
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (m_state != State::Ready)
        return fail(QStringLiteral("Обновление ещё не скачано."));
    const QString appDir = QCoreApplication::applicationDirPath();
    {
        // Заменить файлы можно, только если в папку программы разрешено писать (в «Program Files» - нет).
        QFile probe(QDir(appDir).filePath(QStringLiteral(".update_write_test")));
        if (!probe.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("В папку программы (%1) нельзя писать, поэтому заменить файлы автоматически не получится.\n\n"
                                       "Запустите программу от имени администратора или перенесите её в обычную папку (например, на Рабочий стол) "
                                       "и повторите. Скачанная новая версия лежит здесь: %2")
                            .arg(QDir::toNativeSeparators(appDir), QDir::toNativeSeparators(stagedDir())));
        probe.close();
        probe.remove();
    }
    const QString scriptPath = QDir(stagingRoot()).filePath(QStringLiteral("apply_update.cmd"));
    {
        QSaveFile script(scriptPath);
        if (!script.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("Не удалось записать скрипт обновления: %1").arg(script.errorString()));
        script.write(scriptText().toLatin1());
        if (!script.commit())
            return fail(QStringLiteral("Не удалось записать скрипт обновления: %1").arg(script.errorString()));
    }
    const QString backup = QDir(appDir).filePath(QStringLiteral("_update_backup"));
    const QString exe = QCoreApplication::applicationFilePath();
    const qint64 pid = QCoreApplication::applicationPid();
#ifdef Q_OS_WIN
    // «cmd /d /s /c "<скрипт> <аргументы>"»: при /s внешние кавычки снимаются, а внутренние остаются как есть.
    QProcess process;
    process.setProgram(qEnvironmentVariable("ComSpec", QStringLiteral("C:\\Windows\\System32\\cmd.exe")));
    process.setNativeArguments(QStringLiteral("/d /s /c \"%1 %2 %3 %4 %5 %6 %7\"")
                                   .arg(nativeQuoted(scriptPath), nativeQuoted(appDir), nativeQuoted(stagedDir()), nativeQuoted(backup),
                                        nativeQuoted(exe), QString::number(pid), QString::number(m_latest.build)));
    if (!process.startDetached())
        return fail(QStringLiteral("Не удалось запустить установку обновления."));
    return true;
#else
    Q_UNUSED(backup)
    Q_UNUSED(exe)
    Q_UNUSED(pid)
    if (qEnvironmentVariableIsSet("INVENTORY_UPDATE_DRYRUN"))
        return true; // для проверок: скрипт записан, но не запускается
    return fail(QStringLiteral("Автоматическая установка работает только в Windows. Новая версия лежит здесь: %1")
                    .arg(QDir::toNativeSeparators(stagedDir())));
#endif
}

Updater::Result Updater::takeResult()
{
    Result r;
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("update_result.txt"));
    QFile f(path);
    if (!f.exists())
        return r;
    if (f.open(QIODevice::ReadOnly)) {
        const QStringList parts = QString::fromUtf8(f.readAll()).trimmed().split(QLatin1Char('|'));
        r.present = true;
        r.ok = parts.value(0).trimmed() == QLatin1String("ok");
        r.build = parts.value(1).trimmed().toInt();
        r.message = parts.mid(2).join(QLatin1Char('|')).trimmed();
        f.close();
    }
    QFile::remove(path);
    return r;
}

void Updater::cleanupOld()
{
    const QDir root(stagingRoot());
    const int current = AppVersion::build();
    for (const QFileInfo &fi : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString n = fi.fileName();
        if (n.startsWith(QLatin1Char('b')) && n.mid(1).toInt() <= current)
            QDir(fi.absoluteFilePath()).removeRecursively();
    }
}
