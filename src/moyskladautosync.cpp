#include "moyskladautosync.h"

#include "applog.h"
#include "moysklad.h"

#include <QSettings>

#include <limits>

namespace {
const char kGroup[] = "moyskladAuto/";
constexpr int kFirstRunDelayMs = 60 * 1000; // после запуска программы: дать окну открыться

// Для автоматических тестов: переменные окружения ускоряют расписание (в обычной работе не задаются).
int firstRunDelayMs()
{
    const int v = qEnvironmentVariableIntValue("INVENTORY_MS_FIRST_RUN_MS");
    return v > 0 ? v : kFirstRunDelayMs;
}
qint64 intervalMs(int minutes)
{
    const int v = qEnvironmentVariableIntValue("INVENTORY_MS_INTERVAL_MS");
    return v > 0 ? v : qint64(minutes) * 60 * 1000;
}
} // namespace

MoySkladAutoSync::MoySkladAutoSync(QObject *parent)
    : QObject(parent)
{
    QSettings st;
    m_settings.enabled = st.value(QString::fromLatin1(kGroup) + "enabled", false).toBool();
    m_settings.intervalMinutes = qBound(kMinIntervalMinutes, st.value(QString::fromLatin1(kGroup) + "intervalMin", 30).toInt(), kMaxIntervalMinutes);
    m_lastRun = QDateTime::fromString(st.value(QString::fromLatin1(kGroup) + "lastRun").toString(), Qt::ISODate);
    m_lastResult = st.value(QString::fromLatin1(kGroup) + "lastResult").toString();

    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this]() { run(false); });
    if (m_settings.enabled) {
        m_timer.start(firstRunDelayMs());
        m_nextRun = QDateTime::currentDateTime().addMSecs(firstRunDelayMs());
    }
}

void MoySkladAutoSync::setSettings(const Settings &s)
{
    m_settings = s;
    m_settings.intervalMinutes = qBound(kMinIntervalMinutes, s.intervalMinutes, kMaxIntervalMinutes);
    QSettings st;
    st.setValue(QString::fromLatin1(kGroup) + "enabled", m_settings.enabled);
    st.setValue(QString::fromLatin1(kGroup) + "intervalMin", m_settings.intervalMinutes);
    AppLog::write(QStringLiteral("moysklad-auto"),
                  m_settings.enabled ? QStringLiteral("включено: каждые %1 мин").arg(m_settings.intervalMinutes) : QStringLiteral("выключено"));
    schedule();
}

void MoySkladAutoSync::schedule()
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

QString MoySkladAutoSync::statusText() const
{
    QString s;
    if (m_running)
        s = tr("Идёт загрузка остатков из МойСклад…");
    else if (!m_settings.enabled)
        s = tr("Автозагрузка выключена.");
    else
        s = tr("Автозагрузка включена: каждые %1.").arg(m_settings.intervalMinutes % 60 == 0 && m_settings.intervalMinutes >= 60
                                                         ? tr("%1 ч").arg(m_settings.intervalMinutes / 60)
                                                         : tr("%1 мин").arg(m_settings.intervalMinutes));
    if (m_lastRun.isValid())
        s += tr(" Последний запуск: %1 - %2.").arg(m_lastRun.toString("dd.MM HH:mm"), m_lastResult);
    if (m_settings.enabled && m_nextRun.isValid() && !m_running)
        s += tr(" Следующий: %1.").arg(m_nextRun.toString("HH:mm"));
    return s;
}

void MoySkladAutoSync::runNow()
{
    run(true);
}

void MoySkladAutoSync::run(bool manual)
{
    if (m_running)
        return;
    m_running = true;
    m_timer.stop();
    emit statusChanged();

    QString result;
    bool failed = false;
    int documentId = 0;
    bool changed = false;
    const MoySklad::Settings cfg = MoySklad::loadSettings();
    if (!cfg.isConfigured() || cfg.mappedStores() == 0) {
        result = tr("не настроено: нужны токен и сопоставление складов");
        failed = true;
    } else {
        emit message(tr("[авто] загружаю остатки из МойСклад…"));
        MoySklad::Api api(cfg.token, [this](const QString &s) { emit message(s); });
        MoySklad::Plan plan;
        QString err;
        if (!MoySklad::buildPlan(cfg, api, &plan, &err)) {
            result = tr("ошибка: %1").arg(err);
            failed = true;
        } else if (plan.suspicious) {
            result = tr("не применено: %1. Проверьте сопоставление и загрузите вручную на странице «МойСклад»").arg(plan.suspicion);
            failed = true;
        } else if (plan.changes.isEmpty()) {
            result = tr("остатки совпадают");
        } else {
            MoySklad::ApplyResult applied;
            if (!MoySklad::apply(plan, tr("Синхронизация с МойСклад (авто)"), &applied, &err)) {
                result = tr("ошибка записи: %1").arg(err);
                failed = true;
            } else {
                result = tr("изменено позиций: %1").arg(applied.changed);
                if (applied.created > 0)
                    result += tr(", создано карточек: %1").arg(applied.created);
                changed = applied.changed > 0 || applied.created > 0;
                documentId = applied.documentId;
            }
        }
        if (!failed)
            emit message(tr("[авто] %1 (%2)").arg(result, MoySklad::summary(plan)));
    }
    if (failed)
        emit message(tr("[авто] %1").arg(result));

    m_lastRun = QDateTime::currentDateTime();
    m_lastResult = result;
    QSettings st;
    st.setValue(QString::fromLatin1(kGroup) + "lastRun", m_lastRun.toString(Qt::ISODate));
    st.setValue(QString::fromLatin1(kGroup) + "lastResult", m_lastResult);
    AppLog::write(QStringLiteral("moysklad-auto"), (manual ? QStringLiteral("вручную: ") : QString()) + result);
    m_running = false;
    schedule();
    if (changed)
        emit stockChanged(documentId);
    if (failed)
        emit problem(result);
}
