#pragma once

#include <QDialog>
#include <QList>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStackedWidget;

// Массовое редактирование товаров: выбранные поля меняются сразу у всех
// выделенных товаров.
namespace BulkEdit {

struct Spec {
    enum PriceMode { PriceSet, PricePercent, PriceAdd };
    enum NameMode { NamePrefix, NameSuffix, NameReplace };

    bool changePrice = false;
    PriceMode priceMode = PriceSet;
    double priceValue = 0;
    bool roundPrice = false; // до целых рублей

    bool changeName = false;
    NameMode nameMode = NamePrefix;
    QString nameA; // текст / что заменить
    QString nameB; // на что заменить

    bool changeDescription = false;
    QString description;

    bool changeMarketSku = false;
    QString marketSku; // пусто - очистить

    bool changeLocation = false;
    int warehouseId = 0;
    int locationId = 0; // 0 - "не указано"

    bool any() const { return changePrice || changeName || changeDescription || changeMarketSku || changeLocation; }
};

double newPrice(double oldPrice, const Spec &spec);
QString newName(const QString &oldName, const Spec &spec);
QString describe(const Spec &spec);
// Все изменения - одной транзакцией: либо у всех товаров, либо ни у кого.
bool apply(const QList<int> &productIds, const Spec &spec, QString *error);

} // namespace BulkEdit

class BulkEditDialog : public QDialog
{
    Q_OBJECT
public:
    explicit BulkEditDialog(const QList<int> &productIds, QWidget *parent = nullptr);

    void accept() override;

private slots:
    void updatePreview();
    void onWarehouseChanged();

private:
    BulkEdit::Spec spec() const;

    QList<int> m_ids;
    QString m_sampleSku, m_sampleName;
    double m_samplePrice = 0;

    QCheckBox *m_priceOn;
    QComboBox *m_priceMode;
    QDoubleSpinBox *m_priceValue;
    QCheckBox *m_priceRound;
    QCheckBox *m_nameOn;
    QComboBox *m_nameMode;
    QLineEdit *m_nameA;
    QLineEdit *m_nameB;
    QCheckBox *m_descOn;
    QLineEdit *m_desc;
    QCheckBox *m_marketOn;
    QLineEdit *m_market;
    QCheckBox *m_locOn;
    QComboBox *m_warehouse;
    QComboBox *m_location;
    QLabel *m_preview;
};
