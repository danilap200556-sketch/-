#include "updatestab.h"
#include "appversion.h"
#include "updater.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
QString when(const QDateTime &d) { return d.isValid() ? d.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")) : QString(); }
QString megabytes(qint64 bytes) { return QLocale().toString(double(bytes) / (1024.0 * 1024.0), 'f', 1); }
} // namespace

bool UpdatesTab::autoCheckEnabled()
{
    return QSettings().value(QStringLiteral("updates/autoCheck"), true).toBool();
}

UpdatesTab::UpdatesTab(Updater *updater, QWidget *parent)
    : QWidget(parent), m_updater(updater)
{
    auto *layout = new QVBoxLayout(this);

    auto *versionBox = new QGroupBox(tr("Установленная версия"), this);
    auto *vl = new QVBoxLayout(versionBox);
    m_current = new QLabel(versionBox);
    m_current->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_current->setWordWrap(true);
    vl->addWidget(m_current);
    layout->addWidget(versionBox);

    auto *updateBox = new QGroupBox(tr("Обновление"), this);
    auto *ul = new QVBoxLayout(updateBox);
    m_status = new QLabel(updateBox);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    ul->addWidget(m_status);
    m_progress = new QProgressBar(updateBox);
    m_progress->setTextVisible(true);
    m_progress->hide();
    ul->addWidget(m_progress);
    auto *buttons = new QHBoxLayout();
    m_check = new QPushButton(tr("Проверить обновления"), updateBox);
    m_download = new QPushButton(tr("Скачать и установить"), updateBox);
    m_download->setProperty("primary", true);
    m_install = new QPushButton(tr("Установить и перезапустить"), updateBox);
    m_install->setProperty("primary", true);
    m_cancel = new QPushButton(tr("Отменить загрузку"), updateBox);
    buttons->addWidget(m_check);
    buttons->addWidget(m_download);
    buttons->addWidget(m_install);
    buttons->addWidget(m_cancel);
    buttons->addStretch(1);
    ul->addLayout(buttons);
    m_auto = new QCheckBox(tr("Проверять обновления при запуске программы (и раз в несколько часов, пока она открыта)"), updateBox);
    m_auto->setChecked(autoCheckEnabled());
    ul->addWidget(m_auto);
    auto *how = new QLabel(tr("Новая версия скачивается с GitHub, проверяется контрольная сумма, затем программа закрывается, подменяет свои "
                              "файлы и запускается снова - это занимает несколько секунд. Настройки, подключение к базе и файл журнала "
                              "сохраняются. Если замена не удалась, возвращается прежняя версия."),
                           updateBox);
    how->setWordWrap(true);
    how->setObjectName("muted");
    ul->addWidget(how);
    layout->addWidget(updateBox);

    auto *notesBox = new QGroupBox(tr("Что нового"), this);
    auto *nl = new QVBoxLayout(notesBox);
    m_notes = new QTextBrowser(notesBox);
    m_notes->setOpenLinks(false);
    nl->addWidget(m_notes);
    layout->addWidget(notesBox, 1);

    connect(m_check, &QPushButton::clicked, this, &UpdatesTab::checkNow);
    connect(m_download, &QPushButton::clicked, this, &UpdatesTab::startDownload);
    connect(m_install, &QPushButton::clicked, this, [this]() { offerInstall(); });
    connect(m_cancel, &QPushButton::clicked, m_updater, &Updater::cancel);
    connect(m_auto, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("updates/autoCheck"), on); });
    connect(m_updater, &Updater::stateChanged, this, &UpdatesTab::refresh);
    connect(m_updater, &Updater::checkFinished, this, [this](bool, const QString &) {
        m_checkedOnce = true;
        refresh();
    });
    connect(m_updater, &Updater::progress, this, [this](qint64 done, qint64 total) {
        m_progress->setRange(0, total > 0 ? 1000 : 0);
        if (total > 0)
            m_progress->setValue(int(done * 1000 / total));
        m_progress->setFormat(total > 0 ? tr("%1 из %2 МБ").arg(megabytes(done), megabytes(total)) : tr("%1 МБ").arg(megabytes(done)));
    });
    connect(m_updater, &Updater::downloadFinished, this, [this](bool ok, const QString &error) {
        m_downloadError = ok ? QString() : error;
        refresh();
        if (ok)
            offerInstall();
    });
    refresh();
}

void UpdatesTab::checkNow()
{
    m_downloadError.clear();
    m_updater->check();
    refresh();
}

void UpdatesTab::startDownload()
{
    if (!m_updater->hasLatest()) {
        checkNow();
        return;
    }
    m_downloadError.clear();
    m_progress->setRange(0, 0);
    m_progress->setFormat(QString());
    m_updater->download();
    refresh();
}

void UpdatesTab::offerInstall()
{
    if (m_updater->state() != Updater::State::Ready)
        return;
    const auto answer = QMessageBox::question(
        this, tr("Установить обновление"),
        tr("Сборка %1 скачана и проверена.\n\nЧтобы установить её, программа закроется и через несколько секунд запустится заново. "
           "Несохранённых данных в окнах быть не должно. Установить сейчас?").arg(m_updater->latest().build),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes)
        emit installRequested();
    refresh();
}

void UpdatesTab::refresh()
{
    const int build = AppVersion::build();
    QString cur = build > 0 ? tr("Сборка %1").arg(build) : tr("Версия для разработки (собрана не в CI)");
    if (!AppVersion::commit().isEmpty())
        cur += tr("  ·  коммит %1").arg(AppVersion::commit());
    if (!AppVersion::builtOn().isEmpty())
        cur += tr("  ·  собрана %1").arg(AppVersion::builtOn());
    cur += tr("\nПапка программы: %1").arg(QDir::toNativeSeparators(QCoreApplication::applicationDirPath()));
    m_current->setText(cur);

    const Updater::State st = m_updater->state();
    const bool busy = st == Updater::State::Checking || st == Updater::State::Downloading || st == Updater::State::Unpacking;
    QString text;
    bool bad = false;
    if (st == Updater::State::Checking) {
        text = tr("Проверяю, есть ли новая версия…");
    } else if (st == Updater::State::Downloading) {
        text = tr("Скачиваю сборку %1…").arg(m_updater->latest().build);
    } else if (st == Updater::State::Unpacking) {
        text = tr("Распаковываю и проверяю файлы…");
    } else if (st == Updater::State::Ready) {
        text = tr("Сборка %1 скачана и проверена. Нажмите «Установить и перезапустить».").arg(m_updater->latest().build);
    } else if (!m_downloadError.isEmpty()) {
        text = m_downloadError;
        bad = true;
    } else if (!m_updater->lastError().isEmpty()) {
        text = tr("Не удалось проверить обновления: %1").arg(m_updater->lastError());
        bad = true;
    } else if (!m_updater->hasLatest()) {
        text = m_checkedOnce ? tr("Нет данных об обновлениях.") : tr("Проверка ещё не выполнялась. Нажмите «Проверить обновления».");
    } else if (m_updater->newerThanCurrent()) {
        const auto &r = m_updater->latest();
        text = build > 0 ? tr("Доступна новая версия: сборка %1 от %2 (у вас сборка %3).").arg(r.build).arg(when(r.date)).arg(build)
                         : tr("На GitHub есть сборка %1 от %2. У вас версия для разработки.").arg(r.build).arg(when(r.date));
    } else {
        text = tr("Установлена последняя версия (сборка %1, на GitHub - сборка %2).").arg(build).arg(m_updater->latest().build);
    }
    m_status->setText(text);
    m_status->setStyleSheet(bad ? QStringLiteral("color: #d64545;") : QString());

    const bool newer = m_updater->newerThanCurrent();
    m_check->setEnabled(!busy);
    m_download->setVisible(newer && st == Updater::State::Idle);
    m_install->setVisible(st == Updater::State::Ready);
    m_cancel->setVisible(st == Updater::State::Downloading);
    m_progress->setVisible(st == Updater::State::Downloading || st == Updater::State::Unpacking);
    if (st == Updater::State::Unpacking) {
        m_progress->setRange(0, 0);
        m_progress->setFormat(QString());
    }

    // Что нового: история сборок, новые (новее установленной) отмечены.
    QString htmlText;
    auto addItem = [&](int b, const QString &commit, const QDateTime &date, const QString &title, const QString &notes) {
        const bool fresh = b > build;
        htmlText += QStringLiteral("<p><b>Сборка %1</b> <span style='color:gray'>%2%3</span>%4<br>")
                        .arg(b).arg(commit.toHtmlEscaped(), date.isValid() ? QStringLiteral(" · ") + when(date) : QString(),
                             fresh ? QStringLiteral(" <b style='color:#4f6bf5'>НОВОЕ</b>") : (b == build ? QStringLiteral(" <i>(у вас)</i>") : QString()));
        QString body = notes.isEmpty() ? title : notes;
        htmlText += body.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")) + QStringLiteral("</p>");
    };
    if (m_updater->hasLatest()) {
        const auto &r = m_updater->latest();
        if (r.history.isEmpty())
            addItem(r.build, r.commit, r.date, QString(), r.notes);
        for (const auto &h : r.history)
            addItem(h.build, h.commit, h.date, h.title, h.notes);
    } else {
        htmlText = QStringLiteral("<p style='color:gray'>%1</p>").arg(tr("Описание изменений появится после проверки обновлений.").toHtmlEscaped());
    }
    m_notes->setHtml(htmlText);
}
