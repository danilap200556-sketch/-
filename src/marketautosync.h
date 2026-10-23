#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QTimer>

// Автоматическое обновление остатков на Яндекс Маркете по расписанию: раз в выбранный период приложение
// само сверяет остатки с Маркетом и отправляет расхождения (то же, что кнопки «Сверить» и «Отправить» на
// вкладке «Яндекс Маркет», только без вопросов). Работает, пока запущено приложение (при закрытии окна оно
// остаётся в трее). Настройки хранятся на этом компьютере.
class MarketAutoSync : public QObject
{
    Q_OBJECT
public:
    struct Settings {
        bool enabled = false;
        int intervalMinutes = 30;   // не меньше kMinIntervalMinutes
        QList<int> accountIds;      // какие кабинеты; пусто - все
        bool zeroMissing = false;   // обнулять на Маркете то, чего нет в приложении
    };
    static constexpr int kMinIntervalMinutes = 5;
    static constexpr int kMaxIntervalMinutes = 7 * 24 * 60;

    explicit MarketAutoSync(QObject *parent = nullptr);

    Settings settings() const { return m_settings; }
    // Запоминает настройки и перезапускает расписание.
    void setSettings(const Settings &s);

    bool isRunning() const { return m_running; }
    QDateTime lastRun() const { return m_lastRun; }
    QString lastResult() const { return m_lastResult; }
    // Когда запустится следующий раз (невалидное время - автообновление выключено).
    QDateTime nextRun() const { return m_nextRun; }
    // Строка состояния для окна и трея.
    QString statusText() const;

public slots:
    // Запустить сейчас (кнопка или трей). Возвращает итог в lastResult().
    void runNow();

signals:
    void message(const QString &text);  // строка в журнал обмена с Маркетом
    void statusChanged();
    void problem(const QString &text);  // ошибка (для всплывающего сообщения в трее)

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
