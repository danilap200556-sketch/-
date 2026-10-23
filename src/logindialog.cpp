#include "logindialog.h"
#include "authservice.h"
#include "theme.h"

#include <QAction>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadialGradient>
#include <QSequentialAnimationGroup>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kMargin = 30;   // поле вокруг окна под тень
constexpr qreal kRadius = 26;

const char *kStyle = R"(
QLabel { color: rgba(255,255,255,0.92); background: transparent; }
QLabel#brandTitle { font-size: 30pt; font-weight: 800; color: white; }
QLabel#brandText { font-size: 12pt; color: rgba(214,220,255,0.82); }
QLabel#chip { color: rgba(255,255,255,0.88); background: rgba(255,255,255,0.10); border: 1px solid rgba(255,255,255,0.16);
  border-radius: 13px; padding: 5px 12px; font-size: 9.5pt; }
QLabel#cardTitle { font-size: 20pt; font-weight: 700; color: white; }
QLabel#cardHint { color: rgba(214,220,255,0.78); font-size: 10pt; }
QLabel#fieldLabel { color: rgba(214,220,255,0.78); font-size: 9pt; font-weight: 600; }
QLabel#error { color: #ff9aa0; font-size: 9.5pt; }
QFrame#glass { background: rgba(255,255,255,0.10); border: 1px solid rgba(255,255,255,0.22); border-radius: 22px; }
QLineEdit { background: rgba(8,12,32,0.38); border: 1px solid rgba(255,255,255,0.20); border-radius: 12px; color: white;
  padding: 11px 14px; font-size: 11pt; selection-background-color: #7c8cff; selection-color: white; }
QLineEdit:focus { border: 1px solid #8e9bff; background: rgba(8,12,32,0.52); }
QPushButton#submit { border: none; border-radius: 12px; padding: 12px; font-size: 11.5pt; font-weight: 700; color: white;
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #5b73ff, stop:1 #9b5cf6); }
QPushButton#submit:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #6f84ff, stop:1 #ac72ff); }
QPushButton#submit:pressed { background: #4a5fe0; }
QPushButton#link { border: none; background: transparent; color: rgba(214,220,255,0.82); padding: 6px; font-size: 9.5pt; }
QPushButton#link:hover { color: white; text-decoration: underline; }
QToolButton#close { border: none; border-radius: 14px; background: rgba(255,255,255,0.08); }
QToolButton#close:hover { background: rgba(255,255,255,0.20); }
)";
} // namespace

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent), m_registerMode(!AuthService::hasAnyUser())
{
    setWindowTitle(m_registerMode ? tr("Создание учётной записи") : tr("Вход"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(920 + 2 * kMargin, (m_registerMode ? 600 : 560) + 2 * kMargin);
    setStyleSheet(QString::fromUtf8(kStyle));

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(kMargin + 40, kMargin + 34, kMargin + 34, kMargin + 34);
    root->setSpacing(36);

    // --- слева: бренд
    auto *brand = new QVBoxLayout();
    brand->setSpacing(10);
    auto *logo = new QLabel(this);
    logo->setPixmap(Theme::logo(64));
    logo->setFixedSize(64, 64);
    brand->addWidget(logo);
    brand->addSpacing(8);
    auto *title = new QLabel(tr("Склад"), this);
    title->setObjectName("brandTitle");
    brand->addWidget(title);
    auto *text = new QLabel(tr("Остатки, заказы и Яндекс Маркет\nв одном окне. Быстро и без лишнего."), this);
    text->setObjectName("brandText");
    brand->addWidget(text);
    brand->addSpacing(14);
    auto *chips = new QHBoxLayout();
    chips->setSpacing(8);
    for (const QString &t : {tr("Остатки"), tr("Заказы"), tr("Маркет")}) {
        auto *c = new QLabel(t, this);
        c->setObjectName("chip");
        chips->addWidget(c);
    }
    chips->addStretch(1);
    brand->addLayout(chips);
    brand->addStretch(1);
    auto *foot = new QLabel(tr("Данные хранятся на вашем сервере"), this);
    foot->setObjectName("cardHint");
    brand->addWidget(foot);
    root->addLayout(brand, 1);

    // --- справа: «стеклянная» карточка
    auto *glass = new QFrame(this);
    glass->setObjectName("glass");
    glass->setFixedWidth(380);
    auto *card = new QVBoxLayout(glass);
    card->setContentsMargins(30, 30, 30, 24);
    card->setSpacing(6);
    auto *cardTitle = new QLabel(m_registerMode ? tr("Первый запуск") : tr("Вход"), glass);
    cardTitle->setObjectName("cardTitle");
    card->addWidget(cardTitle);
    auto *hint = new QLabel(m_registerMode ? tr("Создайте учётную запись администратора") : tr("Введите логин и пароль"), glass);
    hint->setObjectName("cardHint");
    card->addWidget(hint);
    card->addSpacing(14);

    auto addField = [&](const QString &caption, QLineEdit *edit) {
        auto *l = new QLabel(caption, glass);
        l->setObjectName("fieldLabel");
        card->addWidget(l);
        card->addWidget(edit);
        card->addSpacing(8);
    };
    m_username = new QLineEdit(glass);
    m_username->setPlaceholderText(tr("Логин"));
    addField(tr("ЛОГИН"), m_username);
    m_password = new QLineEdit(glass);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(tr("Пароль"));
    auto *eye = m_password->addAction(Theme::icon(Theme::Icon::Eye, QColor(214, 220, 255, 200), 18), QLineEdit::TrailingPosition);
    connect(eye, &QAction::triggered, this, [this, eye]() {
        const bool show = m_password->echoMode() == QLineEdit::Password;
        m_password->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
        eye->setIcon(Theme::icon(show ? Theme::Icon::EyeOff : Theme::Icon::Eye, QColor(214, 220, 255, 200), 18));
    });
    addField(tr("ПАРОЛЬ"), m_password);
    if (m_registerMode) {
        m_confirmPassword = new QLineEdit(glass);
        m_confirmPassword->setEchoMode(QLineEdit::Password);
        m_confirmPassword->setPlaceholderText(tr("Пароль ещё раз"));
        addField(tr("ПОВТОРИТЕ ПАРОЛЬ"), m_confirmPassword);
    }

    m_error = new QLabel(glass);
    m_error->setObjectName("error");
    m_error->setWordWrap(true);
    m_error->setMinimumHeight(18);
    card->addWidget(m_error);
    card->addSpacing(4);

    m_submit = new QPushButton(m_registerMode ? tr("Создать и войти") : tr("Войти"), glass);
    m_submit->setObjectName("submit");
    m_submit->setCursor(Qt::PointingHandCursor);
    m_submit->setDefault(true);
    card->addWidget(m_submit);
    auto *changeServer = new QPushButton(tr("Сменить сервер…"), glass);
    changeServer->setObjectName("link");
    changeServer->setCursor(Qt::PointingHandCursor);
    changeServer->setAutoDefault(false);
    card->addWidget(changeServer, 0, Qt::AlignHCenter);
    root->addWidget(glass, 0, Qt::AlignVCenter);

    auto *shadow = new QGraphicsDropShadowEffect(glass);
    shadow->setBlurRadius(40);
    shadow->setOffset(0, 12);
    shadow->setColor(QColor(0, 0, 0, 110));
    glass->setGraphicsEffect(shadow);

    // кнопка закрытия
    auto *close = new QToolButton(this);
    close->setObjectName("close");
    close->setIcon(Theme::icon(Theme::Icon::Close, QColor(255, 255, 255, 220), 16));
    close->setIconSize(QSize(16, 16));
    close->setFixedSize(28, 28);
    close->setCursor(Qt::PointingHandCursor);
    close->move(width() - kMargin - 28 - 14, kMargin + 14);
    close->raise();
    connect(close, &QToolButton::clicked, this, &QDialog::reject);

    connect(changeServer, &QPushButton::clicked, this, [this] { done(ChangeServer); });
    connect(m_submit, &QPushButton::clicked, this, &LoginDialog::trySubmit);
    connect(m_username, &QLineEdit::returnPressed, m_password, qOverload<>(&QLineEdit::setFocus));
    connect(m_password, &QLineEdit::returnPressed, this, &LoginDialog::trySubmit);
    if (m_confirmPassword)
        connect(m_confirmPassword, &QLineEdit::returnPressed, this, &LoginDialog::trySubmit);

    // последний вход запоминается: остаётся ввести пароль
    const QString last = QSettings().value("lastUser").toString();
    if (!m_registerMode && !last.isEmpty()) {
        m_username->setText(last);
        m_password->setFocus();
    } else {
        m_username->setFocus();
    }
}

void LoginDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF body = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);

    // мягкая тень вокруг окна
    for (int i = 0; i < 14; ++i) {
        const qreal grow = i * 1.7;
        QColor c(10, 14, 40, qMax(0, 11 - i));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(body.adjusted(-grow, -grow + 6, grow, grow + 6), kRadius + grow, kRadius + grow);
    }

    QPainterPath clip;
    clip.addRoundedRect(body, kRadius, kRadius);
    p.setClipPath(clip);
    QLinearGradient bg(body.topLeft(), body.bottomRight());
    bg.setColorAt(0.0, QColor("#0b1022"));
    bg.setColorAt(0.55, QColor("#17154a"));
    bg.setColorAt(1.0, QColor("#2a1458"));
    p.fillRect(body, bg);

    auto glow = [&](QPointF c, qreal r, QColor col) {
        QRadialGradient g(c, r);
        QColor inner = col;
        QColor outer = col;
        outer.setAlpha(0);
        g.setColorAt(0, inner);
        g.setColorAt(1, outer);
        p.fillRect(body, g);
    };
    glow(body.topLeft() + QPointF(body.width() * 0.18, body.height() * 0.22), 430, QColor(99, 102, 241, 150));
    glow(body.topLeft() + QPointF(body.width() * 0.86, body.height() * 0.86), 380, QColor(236, 72, 153, 90));
    glow(body.topLeft() + QPointF(body.width() * 0.60, body.height() * 0.02), 300, QColor(34, 211, 238, 70));

    // тонкие круги на фоне
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < 4; ++i) {
        p.setPen(QPen(QColor(255, 255, 255, 14 - i * 2), 1.2));
        p.drawEllipse(body.center() + QPointF(-210, 80), 190 + i * 70, 190 + i * 70);
    }
    p.setClipping(false);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.2));
    p.drawRoundedRect(body, kRadius, kRadius);
}

void LoginDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    setWindowOpacity(0.0);
    auto *fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(260);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void LoginDialog::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOffset = e->globalPosition().toPoint() - frameGeometry().topLeft();
    }
}

void LoginDialog::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton))
        move(e->globalPosition().toPoint() - m_dragOffset);
}

void LoginDialog::shake()
{
    const QPoint base = pos();
    auto *seq = new QSequentialAnimationGroup(this);
    const int steps[] = {-12, 10, -8, 6, -3, 0};
    for (int dx : steps) {
        auto *a = new QPropertyAnimation(this, "pos");
        a->setDuration(45);
        a->setEndValue(base + QPoint(dx, 0));
        seq->addAnimation(a);
    }
    seq->start(QAbstractAnimation::DeleteWhenStopped);
}

void LoginDialog::fail(const QString &text)
{
    m_error->setText(text);
    shake();
}

void LoginDialog::trySubmit()
{
    m_error->clear();
    QString err;

    if (m_registerMode) {
        if (m_password->text() != m_confirmPassword->text()) {
            fail(tr("Пароли не совпадают"));
            return;
        }
        // Первая учётная запись всегда администраторская - иначе заводить
        // остальных пользователей было бы некому.
        if (!AuthService::createUser(m_username->text(), m_password->text(), true, &err)) {
            fail(err);
            return;
        }
    } else {
        if (!AuthService::verifyLogin(m_username->text(), m_password->text(), &err)) {
            fail(err);
            m_password->clear();
            m_password->setFocus();
            return;
        }
    }

    QSettings().setValue("lastUser", m_username->text().trimmed());
    accept();
}

QString LoginDialog::username() const
{
    return m_username->text().trimmed();
}
