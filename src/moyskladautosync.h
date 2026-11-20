#pragma once

#include <QDateTime>
#include <QObject>
#include <QTimer>

// Автоматическая загрузка остатков из МойСклад по расписанию: раз в выбранный период программа сама берёт остатки сопоставленных
// складов и приводит к ним остатки в программе (то же, что кнопка «Загрузить остатки сейчас» на странице «МойСклад», только без
// вопросов). Результат, похожий на ошибку сопоставления (например, обнуляется больше половины товаров), не применяется, а
// сообщается. Работает, пока запущена программа (при закрытии окна она остаётся в трее). Включено ли расписание и как часто -
// настройка этого компьютера; токен и сопоставление складов общие (в базе).
class MoySkladAutoSync : public QObject
{
    Q_OBJECT
public:
    struct Settings {
        bool enabled = false;
        int intervalMinutes = 30;   // не меньше kMinIntervalMinutes
    };
    static constexpr int kMinIntervalMinutes = 5;
    static constexpr int kMaxIntervalMinutes = 7 * 24 * 60;

    explicit MoySkladAutoSync(QObject *parent = nullptr);

    Settings settings() const { return m_settings; }
    void setSettings(const Settings &s);

    bool isRunning() const { return m_running; }
    QDateTime lastRun() const { return m_lastRun; }
    QString lastResult() const { return m_lastResult; }
    QDateTime nextRun() const { return m_nextRun; }
    QString statusText() const;

public slots:
    // Загрузить сейчас (трей); итог в lastResult().
    void runNow();

signals:
    void message(const QString &text);       // строка в журнал страницы
    void statusChanged();
    void problem(const QString &text);       // ошибка (для сообщения в трее)
    void stockChanged(int documentId);       // остатки в программе изменились (документ, 0 - не создан)

private:
    void schedule();
    void run(bool manual);

    Settings m_settings;
    QTimer m_timer;
    bool m_running = false;
    QDateTime m_lastRun;
    QDateTime m_nextRun;
    QString m_lastResult;
};
