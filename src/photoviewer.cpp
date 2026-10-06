#include "photoviewer.h"
#include "photostore.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

PhotoViewer::PhotoViewer(const QList<int> &photoIds, const QString &title, QWidget *parent)
    : QDialog(parent), m_ids(photoIds)
{
    setWindowTitle(title);
    resize(760, 700);
    auto *layout = new QVBoxLayout(this);

    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumSize(200, 200);
    m_image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(m_image, 1);

    auto *bar = new QHBoxLayout();
    m_prev = new QPushButton(QStringLiteral("←"), this);
    m_next = new QPushButton(QStringLiteral("→"), this);
    m_counter = new QLabel(this);
    m_counter->setAlignment(Qt::AlignCenter);
    auto *close = new QPushButton(tr("Закрыть"), this);
    bar->addWidget(m_prev);
    bar->addWidget(m_counter, 1);
    bar->addWidget(m_next);
    bar->addWidget(close);
    layout->addLayout(bar);

    connect(m_prev, &QPushButton::clicked, this, [this]() { showPhoto(m_index - 1); });
    connect(m_next, &QPushButton::clicked, this, [this]() { showPhoto(m_index + 1); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    showPhoto(0);
}

void PhotoViewer::showPhoto(int index)
{
    if (m_ids.isEmpty()) {
        m_image->setText(tr("У товара нет фото"));
        m_counter->clear();
        m_prev->setEnabled(false);
        m_next->setEnabled(false);
        return;
    }
    m_index = qBound(0, index, int(m_ids.size()) - 1);
    m_pixmap.loadFromData(PhotoStore::fullData(m_ids[m_index]));
    m_counter->setText(tr("%1 из %2").arg(m_index + 1).arg(m_ids.size()));
    m_prev->setEnabled(m_index > 0);
    m_next->setEnabled(m_index + 1 < m_ids.size());
    refit();
}

void PhotoViewer::refit()
{
    if (m_pixmap.isNull()) {
        m_image->setText(tr("Не удалось показать фото"));
        return;
    }
    m_image->setPixmap(m_pixmap.scaled(m_image->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void PhotoViewer::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (!m_ids.isEmpty())
        refit();
}

void PhotoViewer::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Left && m_prev->isEnabled())
        showPhoto(m_index - 1);
    else if (event->key() == Qt::Key_Right && m_next->isEnabled())
        showPhoto(m_index + 1);
    else
        QDialog::keyPressEvent(event);
}
