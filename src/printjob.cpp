#include "printjob.h"

#include "applog.h"

#include <QCoreApplication>
#include <QPointer>
#include <QProgressDialog>
#include <QTimer>

#include <thread>

namespace ThermalLabel {

int &cancelWaitMs()
{
    static int ms = 8000;
    return ms;
}

PrintJob::PrintJob(const Settings &settings, int count, Render render, const QString &pdfPath, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_count(count)
    , m_render(std::move(render))
    , m_pdf(pdfPath)
{
}

void PrintJob::cancel()
{
    m_cancel->store(true);
}

void PrintJob::start()
{
    // Обычный отсоединённый поток: если драйвер принтера завис, программу всё равно можно закрыть (QThread в такой момент разрушать нельзя).
    // Результаты уходят в поток окна через очередь событий; к объекту обращаемся только там и только если он ещё жив.
    QPointer<PrintJob> self(this);
    const Settings settings = m_settings;
    const int count = m_count;
    const Render render = m_render;
    const QString pdf = m_pdf;
    const std::shared_ptr<std::atomic<bool>> cancel = m_cancel;
    std::thread([self, settings, count, render, pdf, cancel]() {
        auto post = [](const std::function<void()> &fn) {
            if (QCoreApplication *app = QCoreApplication::instance())
                QMetaObject::invokeMethod(app, fn, Qt::QueuedConnection);
        };
        QString error;
        const bool ok = printPages(settings, count, render, &error, pdf, [&](int done, int total) {
            post([self, done, total]() {
                if (self)
                    emit self->progress(done, total);
            });
            return !cancel->load();
        });
        const bool canceled = cancel->load();
        post([self, ok, error, canceled]() {
            if (self)
                emit self->finished(ok, error, canceled);
        });
    }).detach();
}

void printWithProgress(QWidget *parent, const QString &title, const Settings &settings, int count, PrintJob::Render render, const QString &pdfPath,
                       const std::function<void(bool, const QString &, bool)> &onDone)
{
    auto *dlg = new QProgressDialog(QObject::tr("Подготовка печати…"), QObject::tr("Отмена"), 0, qMax(1, count), parent);
    dlg->setWindowTitle(title);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->setValue(0);
    auto *job = new PrintJob(settings, count, std::move(render), pdfPath, dlg);
    auto finished = std::make_shared<bool>(false);
    auto finish = [dlg, finished, onDone](bool ok, const QString &error, bool canceled) {
        if (*finished)
            return;
        *finished = true;
        QObject::disconnect(dlg, &QProgressDialog::canceled, nullptr, nullptr); // закрытие окна не должно считаться отменой
        dlg->hide();
        dlg->deleteLater();
        onDone(ok, error, canceled);
    };
    QObject::connect(job, &PrintJob::progress, dlg, [dlg](int done, int total) {
        dlg->setMaximum(qMax(1, total));
        dlg->setValue(done);
        dlg->setLabelText(QObject::tr("Печать этикеток: %1 из %2…").arg(done).arg(total));
    });
    QObject::connect(job, &PrintJob::finished, dlg, [finish](bool ok, const QString &error, bool canceled) { finish(ok, error, canceled); });
    QObject::connect(dlg, &QProgressDialog::canceled, dlg, [dlg, job, finish]() {
        job->cancel();
        dlg->setLabelText(QObject::tr("Отмена… Жду, пока принтер остановится."));
        dlg->setCancelButton(nullptr);
        AppLog::write(QStringLiteral("Печать"), QStringLiteral("пользователь нажал «Отмена»"));
        // Драйвер мог зависнуть и не ответить: через несколько секунд не ждём его, окно программы освобождается.
        QTimer::singleShot(cancelWaitMs(), dlg, [finish]() {
            AppLog::write(QStringLiteral("Печать"), QStringLiteral("принтер не ответил на отмену за %1 мс: печать брошена в фоне").arg(cancelWaitMs()));
            finish(false, QObject::tr("Принтер не ответил на отмену: печать брошена. Проверьте очередь печати Windows (Пуск → Принтеры и сканеры → ваш принтер → "
                                      "Открыть очередь печати) и очистите её. Коды не отмечены напечатанными."), true);
        });
    });
    job->start();
    dlg->show();
}

} // namespace ThermalLabel
