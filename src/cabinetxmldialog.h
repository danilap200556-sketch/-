#pragma once

#include "cabinetxml.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

// «XML для кабинета»: файл заказа кодов (urn:oms.order) для загрузки в «Заказы → Активные → Импортировать» личного кабинета «Честного знака».
// Подписывать ничего не нужно: файл загружается в кабинете, а заказ подписывает и отправляет сам кабинет.
class CabinetOrderXmlDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CabinetOrderXmlDialog(const QList<CabinetXml::OrderItem> &items, QWidget *parent = nullptr);

    CabinetXml::OrderOptions options() const;
    void setOptions(const CabinetXml::OrderOptions &options);
    void setDirectory(const QString &directory);
    // Записывает файлы в выбранную папку. Возвращает число файлов (0 - не получилось, причина в lastError()).
    int exportFiles();
    QStringList files() const { return m_files; }
    QString lastError() const { return m_error; }
    QString stateText() const;

private:
    void groupChanged();
    void refreshSummary();
    void saveSettings() const;
    void chooseDirectory();
    void openDirectory();

    QList<CabinetXml::OrderItem> m_items;
    QComboBox *m_group = nullptr, *m_release = nullptr, *m_create = nullptr, *m_template = nullptr, *m_cis = nullptr;
    QLineEdit *m_contact = nullptr, *m_producer = nullptr, *m_productionOrder = nullptr, *m_dir = nullptr;
    QRadioButton *m_operatorSerials = nullptr, *m_ownSerials = nullptr;
    QLabel *m_cisLabel = nullptr, *m_summary = nullptr, *m_state = nullptr;
    QPushButton *m_openBtn = nullptr;
    QStringList m_files;
    QString m_error;
};

// «Ввод в оборот (XML)»: файл vvod_individuals (action_id 5.3) со списком кодов идентификации, ИНН участника и датой.
class CabinetIntroXmlDialog : public QDialog
{
    Q_OBJECT
public:
    CabinetIntroXmlDialog(const QStringList &codes, const QString &source, QWidget *parent = nullptr);

    void setInn(const QString &inn);
    void setDate(const QDate &date);
    void setDirectory(const QString &directory);
    int exportFiles();
    QStringList files() const { return m_files; }
    QString lastError() const { return m_error; }
    QString stateText() const;

private:
    void refreshSummary();
    void chooseDirectory();
    void openDirectory();

    QStringList m_codes;
    QLineEdit *m_inn = nullptr, *m_dir = nullptr;
    QDateEdit *m_date = nullptr;
    QLabel *m_summary = nullptr, *m_state = nullptr;
    QPushButton *m_openBtn = nullptr;
    QStringList m_files;
    QString m_error;
};
