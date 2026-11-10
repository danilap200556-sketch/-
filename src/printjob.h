#pragma once

#include "thermallabel.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QWidget;

namespace ThermalLabel {

// Печать этикеток в ФОНОВОМ потоке: окно программы остаётся живым, пока этикетки рисуются и уходят в драйвер принтера (на сотни
// этикеток и «тяжёлый» драйвер это минуты - раньше всё это шло в потоке окна, и окно «зависало»).
// render(i, dpi) вызывается в фоновом потоке: она не должна трогать виджеты - всё нужное заранее копируется в неё.
class PrintJob : public QObject
{
    Q_OBJECT
public:
    using Render = std::function<QImage(int index, int dpi)>;
    PrintJob(const Settings &settings, int count, Render render, const QString &pdfPath, QObject *parent = nullptr);

    void start();
    // Просит остановиться: печать прервётся перед следующей страницей, задание принтера отменяется.
    void cancel();

signals:
    void progress(int done, int total);
    void finished(bool ok, const QString &error, bool canceled);

private:
    Settings m_settings;
    int m_count;
    Render m_render;
    QString m_pdf;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};

// Сколько ждать ответа принтера после «Отмена», миллисекунд (по умолчанию 8000; меняется для проверок).
int &cancelWaitMs();

// Показывает окно «Печать» с полосой и кнопкой «Отмена», печатает в фоне и вызывает onDone(ok, error, canceled) в потоке окна ровно один раз.
// После «Отмена» принтер может не ответить (драйвер завис): через несколько секунд окно закрывается само, а печать остаётся в фоне
// (onDone вызывается с canceled=true и пояснением), так что программа не зависает при любом поведении драйвера.
void printWithProgress(QWidget *parent, const QString &title, const Settings &settings, int count, PrintJob::Render render, const QString &pdfPath,
                       const std::function<void(bool ok, const QString &error, bool canceled)> &onDone);

} // namespace ThermalLabel
