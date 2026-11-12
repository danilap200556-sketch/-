#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QFrame;
class QHBoxLayout;
class QPushButton;
class TabStrip;

// Верхнее меню окна вместо боковой панели: тёмно-зелёная полоса с группами разделов (Склад, Маркетплейс, Маркировка,
// Система) и под ней белая полоса вкладок выбранной группы. Линия под выбранной вкладкой плавно переезжает к новой
// (или «вырастает» слева, если сменилась группа). Страницы нумеруются в порядке addPage - так же, как в QStackedWidget.
class TopNav : public QWidget
{
    Q_OBJECT
public:
    explicit TopNav(QWidget *parent = nullptr);

    // Добавляет страницу в группу и возвращает её номер. Пустая группа - та же, что у предыдущей страницы.
    // Имя группы можно передавать заглавными («СКЛАД»): на кнопке оно пишется обычно («Склад»).
    int addPage(const QString &title, const QString &group = QString());
    // Делает страницу текущей (подсвечивает её группу и вкладку) и сообщает currentChanged. Под QSignalBlocker - без сообщения.
    void setCurrentIndex(int index);
    int currentIndex() const { return m_current; }
    int count() const { return m_pages.size(); }
    QString title(int index) const;
    // Точка «есть новое» у вкладки и у её группы (например, «Обновления»).
    void setBadge(int index, bool on);
    // Правая часть верхней полосы: сюда окно кладёт строку сканера, пользователя и кнопку темы.
    QHBoxLayout *headerTrailing() const { return m_trailing; }
    QFrame *header() const { return m_header; }

signals:
    void currentChanged(int index);

private:
    struct Page {
        QString title;
        int group = 0;
        QPushButton *tab = nullptr;
        bool badge = false;
    };
    struct Group {
        QString name;
        QPushButton *button = nullptr;
        int lastPage = -1;
    };
    int groupIndex(const QString &name);
    void showGroup(int group);
    void updateBadges();

    QFrame *m_header = nullptr;
    QHBoxLayout *m_groupsLayout = nullptr;
    QHBoxLayout *m_trailing = nullptr;
    TabStrip *m_strip = nullptr;
    QList<Page> m_pages;
    QList<Group> m_groups;
    int m_current = -1;
    int m_shownGroup = -1;
};
