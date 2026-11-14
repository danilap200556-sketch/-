#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// «ИП и настройки СУЗ»: справочник ИП (название, ИНН), для каждого - кнопка настроек его СУЗ (omsId, соединение, токен, сертификат).
// Склады привязываются к ИП на вкладке «Склады».
class MarkingOwnersDialog : public QDialog
{
    Q_OBJECT
public:
    // suzButtons - показывать кнопки настроек СУЗ (на вкладке «Склады» они не нужны: окно настроек живёт на вкладке «Честный знак»).
    explicit MarkingOwnersDialog(QWidget *parent = nullptr, bool suzButtons = true);

signals:
    // Запросить окно настроек СУЗ для ИП (открывает вкладка «Честный знак»: окно настроек живёт там).
    void suzSettingsRequested(int ownerId);

private:
    void reload(int selectId = 0);
    void addOwner();
    void editOwner();
    void removeOwner();
    int currentId() const;

    QListWidget *m_list;
    QLabel *m_info;
};
