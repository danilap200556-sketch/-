#include "marketautosync.h"
#include "applog.h"
#include "marketapi.h"
#include "marketsync.h"

#include <QSettings>

#include <limits>

namespace {
const char kGroup[] = "marketAuto/";
constexpr int kFirstRunDelayMs = 45 * 1000; // после запуска приложения: дать окну открыться

// Для автоматических тестов: переменные окружения ускоряют расписание (в обычной работе не задаются).
int firstRunDelayMs()
{
    const int v = qEnvironmentVariableIntValue("INVENTORY_AUTOSYNC_FIRST_RUN_MS");
    return v > 0 ? v : kFirstRunDelayMs;
}
qint64 intervalMs(int minutes)
{
    const int v = qEnvironmentVariableIntValue("INVENTORY_AUTOSYNC_INTERVAL_MS");
    return v > 0 ? v : qint64(minutes) * 60 * 1000;
}
} // namespace

MarketAutoSync::MarketAutoSync(QObject *parent)
    : QObject(parent)
{
    QSettings st;
    m_settings.enabled = st.value(QString::fromLatin1(kGroup) + "enabled", false).toBool();
    m_settings.intervalMinutes = qBound(kMinIntervalMinutes, st.value(QString::fromLatin1(kGroup) + "intervalMin", 30).toInt(),
                                        kMaxIntervalMinutes);
    for (const QString &id : st.value(QString::fromLatin1(kGroup) + "accounts").toStringList())
        m_settings.accountIds << id.toInt();
    m_settings.zeroMissing = st.value(QString::fromLatin1(kGroup) + "zeroMissing", false).toBool();
    m_lastRun = QDateTime::fromString(st.value(QString::fromLatin1(kGroup) + "lastRun").toString(), Qt::ISODate);
    m_lastResult = st.value(QString::fromLatin1(kGroup) + "lastResult").toString();

    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this]() { run(false); });
    if (m_settings.enabled) {
        // Первый запуск - вскоре после старта, дальше по расписанию.
        m_timer.start(firstRunDelayMs());
        m_nextRun = QDateTime::currentDateTime().addMSecs(firstRunDelayMs());
    }
}

void MarketAutoSync::setSettings(const Settings &s)
{
    m_settings = s;
    m_settings.intervalMinutes = qBound(kMinIntervalMinutes, s.intervalMinutes, kMaxIntervalMinutes);
    QSettings st;
    st.setValue(QString::fromLatin1(kGroup) + "enabled", m_settings.enabled);
    st.setValue(QString::fromLatin1(kGroup) + "intervalMin", m_settings.intervalMinutes);
    QStringList ids;
    for (int id : m_settings.accountIds)
        ids << QString::number(id);
    st.setValue(QString::fromLatin1(kGroup) + "accounts", ids);
    st.setValue(QString::fromLatin1(kGroup) + "zeroMissing", m_settings.zeroMissing);
    AppLog::write(QStringLiteral("market-auto"),
                  m_settings.enabled ? QStringLiteral("включено: каждые %1 мин").arg(m_settings.intervalMinutes)
                                     : QStringLiteral("выключено"));
    schedule();
}

void MarketAutoSync::schedule()
{
    m_timer.stop();
    if (m_settings.enabled && !m_running) {
        const qint64 ms = intervalMs(m_settings.intervalMinutes);
        m_timer.start(int(qMin<qint64>(ms, std::numeric_limits<int>::max())));
        m_nextRun = QDateTime::currentDateTime().addMSecs(ms);
    } else if (!m_settings.enabled) {
        m_nextRun = QDateTime();
    }
    emit statusChanged();
}

QString MarketAutoSync::statusText() const
{
    QString s;
    if (m_running)
        s = tr("Идёт обновление остатков…");
    else if (!m_settings.enabled)
        s = tr("Автообновление выключено.");
    else
        s = tr("Автообновление включено: каждые %1.").arg(m_settings.intervalMinutes % 60 == 0 && m_settings.intervalMinutes >= 60
                                                           ? tr("%1 ч").arg(m_settings.intervalMinutes / 60)
                                                           : tr("%1 мин").arg(m_settings.intervalMinutes));
    if (m_lastRun.isValid())
        s += tr(" Последний запуск: %1 - %2.").arg(m_lastRun.toString("dd.MM HH:mm"), m_lastResult);
    if (m_settings.enabled && m_nextRun.isValid() && !m_running)
        s += tr(" Следующий: %1.").arg(m_nextRun.toString("HH:mm"));
    return s;
}

void MarketAutoSync::runNow()
{
    run(true);
}

void MarketAutoSync::run(bool manual)
{
    if (m_running)
        return; // предыдущий запуск ещё идёт (сеть ждёт ответа)
    m_running = true;
    m_timer.stop();
    emit statusChanged();

    const QList<MarketAccount> all = MarketAccount::loadAll();
    QList<MarketAccount> accounts;
    for (const MarketAccount &a : all)
        if (m_settings.accountIds.isEmpty() || m_settings.accountIds.contains(a.id))
            accounts << a;

    int pushed = 0, failed = 0, inSync = 0;
    QStringList errors;
    if (accounts.isEmpty()) {
        errors << tr("нет кабинетов Маркета");
        failed = 1;
    }
    for (const MarketAccount &acc : std::as_const(accounts)) {
        emit message(tr("[авто] «%1»: сверяю остатки…").arg(acc.name));
        MarketApi api(acc.apiKey, [this](const QString &s) { emit message(s); });
        QList<MarketSync::StockRow> rows;
        QString err;
        if (!MarketSync::compareStocks(acc, api, &rows, &err)) {
            ++failed;
            errors << QStringLiteral("«%1»: %2").arg(acc.name, err);
            emit message(tr("[авто] «%1»: ошибка - %2").arg(acc.name, err));
            continue;
        }
        const auto items = MarketSync::itemsToPush(rows, m_settings.zeroMissing);
        if (items.isEmpty()) {
            ++inSync;
            emit message(tr("[авто] «%1»: остатки совпадают").arg(acc.name));
            continue;
        }
        if (!api.updateStocks(acc, items, &err)) {
            ++failed;
            errors << QStringLiteral("«%1»: %2").arg(acc.name, err);
            emit message(tr("[авто] «%1»: не удалось отправить - %2").arg(acc.name, err));
            continue;
        }
        pushed += int(items.size());
        emit message(tr("[авто] «%1»: отправлено позиций - %2").arg(acc.name).arg(items.size()));
    }

    QString result;
    if (failed == 0)
        result = pushed > 0 ? tr("отправлено позиций: %1").arg(pushed) : tr("остатки совпадают");
    else
        result = tr("ошибка: %1").arg(errors.join(QStringLiteral("; ")));
    m_lastRun = QDateTime::currentDateTime();
    m_lastResult = result;
    QSettings st;
    st.setValue(QString::fromLatin1(kGroup) + "lastRun", m_lastRun.toString(Qt::ISODate));
    st.setValue(QString::fromLatin1(kGroup) + "lastResult", m_lastResult);
    AppLog::write(QStringLiteral("market-auto"), (manual ? QStringLiteral("вручную: ") : QString()) + result);
    if (failed > 0)
        emit problem(result);
    m_running = false;
    schedule();
}
