#include "photoexceldialog.h"

#include "photofetch.h"
#include "photostore.h"

#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

PhotoExcelDialog::PhotoExcelDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Фото из Excel"));
    resize(820, 640);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Файл Excel (.xlsx) или CSV: в одной колонке артикул товара («Артикул», «Артикул на Маркете» или «Штрихкод»), в других - "
                                "ссылки на фото («Фото 1», «Фото 2», «Фото 3»…; в одной ячейке можно несколько ссылок через пробел или перевод "
                                "строки). Картинки скачиваются, приводятся к JPEG и сохраняются в карточки товаров - их увидят все компьютеры и сайт. "
                                "Одна и та же картинка у нескольких товаров (размеры одной модели) скачивается один раз."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *fileRow = new QHBoxLayout();
    m_pickBtn = new QPushButton(tr("Выбрать файл..."), this);
    m_fileLabel = new QLabel(tr("Файл не выбран"), this);
    fileRow->addWidget(m_pickBtn);
    fileRow->addWidget(m_fileLabel, 1);
    layout->addLayout(fileRow);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_replace = new QCheckBox(tr("Заменять прежние фото: у товара, для которого фото скачались, старые удаляются (новое - обложка)"), this);
    m_replace->setChecked(true);
    m_replace->setToolTip(tr("Выключите, чтобы только добавить недостающие фото к уже имеющимся (такие же фото повторно не кладутся; не больше %1 на товар).")
                              .arg(PhotoStore::kMaxPhotosPerProduct));
    layout->addWidget(m_replace);

    auto *countRow = new QHBoxLayout();
    countRow->addWidget(new QLabel(tr("Не больше фото на товар (берутся первые ссылки в строке):"), this));
    m_perProduct = new QSpinBox(this);
    m_perProduct->setRange(1, PhotoStore::kMaxPhotosPerProduct);
    m_perProduct->setValue(3);
    m_perProduct->setToolTip(tr("Каждое фото занимает место в общей базе (около 100-200 КБ): для сотен товаров 2-3 фото достаточно."));
    countRow->addWidget(m_perProduct);
    countRow->addStretch();
    layout->addLayout(countRow);

    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 1);
    m_bar->setValue(0);
    layout->addWidget(m_bar);
    m_status = new QLabel(this);
    layout->addWidget(m_status);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    layout->addWidget(m_log, 1);

    auto *bottom = new QHBoxLayout();
    m_startBtn = new QPushButton(tr("Загрузить фото"), this);
    m_startBtn->setEnabled(false);
    m_stopBtn = new QPushButton(tr("Остановить"), this);
    m_stopBtn->setEnabled(false);
    m_closeBtn = new QPushButton(tr("Закрыть"), this);
    bottom->addStretch();
    bottom->addWidget(m_startBtn);
    bottom->addWidget(m_stopBtn);
    bottom->addWidget(m_closeBtn);
    layout->addLayout(bottom);
    for (QPushButton *b : {m_pickBtn, m_startBtn, m_stopBtn, m_closeBtn})
        b->setAutoDefault(false);

    connect(m_pickBtn, &QPushButton::clicked, this, &PhotoExcelDialog::pickFile);
    connect(m_startBtn, &QPushButton::clicked, this, &PhotoExcelDialog::start);
    connect(m_stopBtn, &QPushButton::clicked, this, [this]() { m_stop = true; m_stopBtn->setEnabled(false); });
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void PhotoExcelDialog::reject()
{
    if (m_running) { // закрыли окно во время работы: сначала останавливаемся
        m_stop = true;
        m_stopBtn->setEnabled(false);
        return;
    }
    QDialog::reject();
}

void PhotoExcelDialog::log(const QString &line)
{
    m_log->appendPlainText(line);
}

void PhotoExcelDialog::setBusy(bool busy)
{
    m_running = busy;
    for (QWidget *w : {static_cast<QWidget *>(m_pickBtn), static_cast<QWidget *>(m_replace), static_cast<QWidget *>(m_perProduct), static_cast<QWidget *>(m_closeBtn)})
        w->setEnabled(!busy);
    m_startBtn->setEnabled(!busy && m_parsed.ok && !m_parsed.links.isEmpty());
    m_stopBtn->setEnabled(busy);
}

void PhotoExcelDialog::pickFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Файл со ссылками на фото"), QString(), tr("Excel и CSV (*.xlsx *.csv);;Все файлы (*)"));
    if (!path.isEmpty())
        loadFile(path);
}

bool PhotoExcelDialog::loadFile(const QString &path)
{
    m_log->clear();
    m_parsed = PhotoExcel::parseFile(path);
    m_fileLabel->setText(path);
    if (!m_parsed.ok) {
        m_summary->setText(tr("Не удалось разобрать файл: %1").arg(m_parsed.error));
        m_startBtn->setEnabled(false);
        return false;
    }
    QString text = tr("Колонка товара: «%1»; колонки с фото: %2.\nСтрок с артикулом: %3 · товаров найдено в базе: %4 · ссылок: %5 · строк без ссылок: %6 · артикулов нет в базе: %7")
                       .arg(m_parsed.keyHeader, m_parsed.photoHeaders.join(QStringLiteral(", ")))
                       .arg(m_parsed.rows).arg(m_parsed.links.size()).arg(m_parsed.linkCount).arg(m_parsed.withoutLinks).arg(m_parsed.unknownCount);
    if (!m_parsed.unknown.isEmpty())
        text += tr("\nНет в базе, например: %1%2").arg(m_parsed.unknown.mid(0, 8).join(QStringLiteral(", ")), m_parsed.unknownCount > 8 ? QStringLiteral(" …") : QString());
    m_summary->setText(text);
    m_startBtn->setEnabled(!m_parsed.links.isEmpty());
    if (m_parsed.links.isEmpty())
        m_status->setText(tr("В файле нет ссылок на фото для товаров из базы."));
    else
        m_status->setText(tr("Готово к загрузке: товаров %1.").arg(m_parsed.links.size()));
    return true;
}

void PhotoExcelDialog::start()
{
    if (!m_parsed.ok || m_parsed.links.isEmpty())
        return;
    PhotoFetch::Options opt;
    opt.productIds = m_parsed.links.keys();
    opt.onlyWithoutPhotos = false;
    opt.fromMarket = false;
    opt.fromCards = false;
    opt.fromWeb = false;
    opt.perProduct = m_perProduct->value();
    opt.replaceExisting = m_replace->isChecked();
    opt.extraLinks = m_parsed.links;

    m_log->clear();
    m_stop = false;
    m_bar->setRange(0, 0);
    m_status->setText(tr("Готовлю список товаров…"));
    setBusy(true);
    QApplication::processEvents();

    const PhotoFetch::Result r = PhotoFetch::run(opt, [this](int done, int total, const QString &text) {
        if (total > 0) {
            m_bar->setRange(0, total);
            m_bar->setValue(done);
        }
        if (!text.isEmpty())
            m_status->setText(text);
        QApplication::processEvents(); // перерисовка и кнопка «Остановить»
        return !m_stop;
    });
    setBusy(false);
    m_bar->setRange(0, 1);
    m_bar->setValue(r.cancelled ? 0 : 1);
    if (r.added > 0)
        m_changed = true;
    m_status->setText(r.cancelled ? tr("Остановлено. Добавлено фото: %1.").arg(r.added) : tr("Готово. Добавлено фото: %1.").arg(r.added));

    log(tr("Товаров в файле: %1").arg(r.considered));
    log(tr("Фото добавлено: %1 (у %2 товаров)").arg(r.added).arg(r.productsFilled));
    if (r.replaced > 0)
        log(tr("Удалено прежних фото при замене: %1").arg(r.replaced));
    if (r.unchanged > 0)
        log(tr("Без изменений (в карточке уже те же фото): %1 товаров").arg(r.unchanged));
    if (r.duplicates > 0)
        log(tr("Пропущено, такие фото уже есть: %1").arg(r.duplicates));
    if (r.failed > 0)
        log(tr("Не скачалось или не картинка: %1").arg(r.failed));
    if (!r.allFailed.isEmpty()) {
        const QStringList shown = r.allFailed.mid(0, 20);
        log(tr("Ни одна ссылка не сработала (%1): %2%3").arg(r.allFailed.size()).arg(shown.join(QStringLiteral(", ")), r.allFailed.size() > shown.size() ? QStringLiteral(" …") : QString()));
    }
    if (!r.problems.isEmpty()) {
        log(QString());
        log(tr("Подробности ошибок (первые %1):").arg(r.problems.size()));
        for (const QString &p : r.problems)
            log(QStringLiteral("  ") + p);
    }
}
