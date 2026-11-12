#include "pagetransition.h"

#include "theme.h"

#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QVariantAnimation>
#include <QWidget>

namespace {

constexpr int kDurationMs = 260;
constexpr qreal kShiftPx = 8.0;

class Overlay : public QWidget
{
public:
    Overlay(QWidget *parent, const QPixmap &shot)
        : QWidget(parent), m_shot(shot)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents); // клики проходят к настоящей странице под снимком
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }

    void setProgress(qreal t)
    {
        m_t = t;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme::Colors &c = Theme::colors();
        QPainter p(this);
        p.fillRect(rect(), c.bg);
        p.setOpacity(m_t);
        p.drawPixmap(QPointF(0, (1.0 - m_t) * kShiftPx), m_shot);
        // Полоска «загрузки» сверху: пробегает за первые ~3/4 перехода и гаснет к концу.
        const qreal run = qMin(1.0, m_t / 0.75);
        const qreal fade = m_t < 0.75 ? 1.0 : qMax(0.0, 1.0 - (m_t - 0.75) / 0.25);
        p.setOpacity(fade);
        p.fillRect(QRectF(0, 0, width() * run, 2), c.accent);
    }

private:
    QPixmap m_shot;
    qreal m_t = 0.0;
};

QPointer<Overlay> g_current;

} // namespace

namespace PageTransition {

void play(QWidget *area)
{
    if (!area || !area->isVisible() || !area->window()->isVisible() || !Theme::animations())
        return;
    QWidget *host = area->parentWidget();
    if (!host)
        return;
    if (g_current)
        delete g_current.data(); // быстрый повторный переход: прежний снимок убираем сразу
    const QPixmap shot = area->grab();
    auto *overlay = new Overlay(host, shot);
    overlay->setGeometry(area->geometry());
    overlay->show();
    overlay->raise();
    g_current = overlay;

    auto *anim = new QVariantAnimation(overlay);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setDuration(kDurationMs);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(anim, &QVariantAnimation::valueChanged, overlay, [overlay](const QVariant &v) { overlay->setProgress(v.toReal()); });
    QObject::connect(anim, &QVariantAnimation::finished, overlay, [overlay]() { overlay->deleteLater(); });
    anim->start(); // анимация - дочерний объект снимка и удаляется вместе с ним
}

} // namespace PageTransition
