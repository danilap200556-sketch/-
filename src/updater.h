#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class QNetworkReply;
class QThread;

// Обновление программы: проверка новой сборки, загрузка, проверка контрольной суммы, распаковка и замена файлов.
//
// Откуда берутся обновления: CI (.github/workflows/build-windows.yml) после каждой сборки кладёт в релиз GitHub «app-latest»
// архив inventory_manager-windows.zip и version.json (номер сборки, контрольная сумма, «что нового», история).
// Программа знает номер своей сборки (AppVersion::build()): если в version.json номер больше - есть обновление.
//
// Установка: файлы нельзя заменить, пока программа запущена, поэтому новая версия сначала скачивается и распаковывается
// во временную папку, а потом отдельный скрипт (cmd, см. scriptText()) ждёт закрытия программы, делает резервную копию
// текущих файлов, копирует новые поверх, запускает программу снова, а при неудаче возвращает прежние файлы.
// Результат скрипт записывает в update_result.txt рядом с программой - его читает takeResult() после перезапуска.
class Updater : public QObject
{
    Q_OBJECT
public:
    struct HistoryItem {
        int build = 0;
        QString commit;
        QDateTime date;
        QString title;
        QString notes;
    };
    struct Release {
        int build = 0;
        QString commit;
        QDateTime date;
        QString file;   // имя архива
        QString sha256; // контрольная сумма архива
        qint64 size = 0;
        QString notes;
        QList<HistoryItem> history; // от новых к старым
    };
    enum class State { Idle, Checking, Downloading, Unpacking, Ready };

    explicit Updater(QObject *parent = nullptr);
    ~Updater() override;

    // Адрес папки с version.json и архивом (с «/» на конце). Переменная INVENTORY_UPDATE_URL подменяет его (для тестов).
    static QString baseUrl();
    // Временная папка с загруженными обновлениями.
    static QString stagingRoot();
    // Разбор version.json; при ошибке - false и причина.
    static bool parseManifest(const QByteArray &json, Release *out, QString *error);
    // Распаковывает zip в папку dest; пути с «..», абсолютные и с диском отвергаются.
    static bool unpackZip(const QString &zipPath, const QString &dest, QString *error);
    // Текст скрипта замены файлов (cmd). Только ASCII: пути и номера приходят аргументами командной строки.
    static QString scriptText();
    // Итог прошлого обновления (update_result.txt рядом с программой). Файл после чтения удаляется.
    struct Result {
        bool present = false;
        bool ok = false;
        int build = 0;
        QString message;
    };
    static Result takeResult();
    // Удаляет загруженные раньше и уже ненужные обновления (номер не новее установленного).
    static void cleanupOld();

    State state() const { return m_state; }
    bool hasLatest() const { return m_latest.build > 0; }
    const Release &latest() const { return m_latest; }
    // В релизе сборка новее установленной (у версии для разработки номер 0 - любая новее).
    bool newerThanCurrent() const;
    // То же, но только для настоящих сборок: версия для разработки обновлений не предлагает сама.
    bool updateAvailable() const;
    QString lastError() const { return m_error; }
    // Папка с распакованными новыми файлами (после downloadFinished(true)).
    QString stagedDir() const;

public slots:
    void check();
    void download();
    void cancel();
    // Запускает скрипт замены. После true программа должна сразу закрыться.
    bool install(QString *error);

signals:
    void checkFinished(bool ok, const QString &error);
    void progress(qint64 done, qint64 total);
    void downloadFinished(bool ok, const QString &error);
    void stateChanged();

private:
    void setState(State s);
    void failDownload(const QString &text);
    void onChecked();
    void onDownloaded();
    QString buildDir() const;
    static QString nativeQuoted(const QString &path);

    QNetworkAccessManager m_nam;
    QNetworkReply *m_reply = nullptr;
    State m_state = State::Idle;
    Release m_latest;
    QString m_error;
    QFile m_file;
    QThread *m_thread = nullptr;
    qint64 m_expected = 0;
};
