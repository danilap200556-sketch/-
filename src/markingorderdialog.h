#pragma once

#include <QDialog>
#include <QList>
#include <QString>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class ProductSuggest;

// Заказ кодов маркировки в СУЗ «Честного знака» СРАЗУ ПО МНОГИМ ТОВАРАМ: список «товар - сколько кодов» (добавляется поиском, вставкой
// «артикул количество» из Excel или кнопкой «Товары с остатком»), GTIN берётся из штрихкода товара. «Создать заказ» отправляет весь
// список одним заказом в СУЗ (длинные списки - несколькими заказами по kLinesPerOrder позиций), «Получить коды» скачивает коды по
// каждой позиции и кладёт их в таблицу «Честный знак». Номер заказа и сколько кодов уже получено хранятся у каждой позиции и
// сохраняются в общей базе: если коды ещё не готовы, окно можно закрыть и вернуться позже. Заказ, созданный в личном кабинете, -
// впишите его номер и добавьте позиции (товар и количество), затем «Получить коды».
class MarkingOrderDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MarkingOrderDialog(QWidget *parent = nullptr);

    // Позиций (кодов товара GTIN) в одном заказе СУЗ: так задано в кабинете для заказа файлом (не больше 10 для всех групп, кроме лекарств);
    // длиннее список - несколько заказов подряд. Предел самого API не проверялся, поэтому берётся тот же.
    static constexpr int kLinesPerOrder = 10;
    static constexpr int kMaxQty = 150000;

    struct Line {
        int productId = 0;
        QString sku, name, gtin;
        int qty = 0;
        QString orderId;    // заказ в СУЗ (пусто - ещё не заказано)
        int received = 0;   // сколько кодов уже скачано
        QString state;
        bool problem = false;
    };

    int added() const { return m_added; }  // новых кодов добавлено в таблицу «Честный знак»
    int lineCount() const;
    QList<Line> lines() const;
    QString log() const;

    // Добавление позиций. Повтор товара складывается с уже добавленным.
    // Текст: по строке «артикул [количество]» (или штрихкод; из Excel - колонки через табуляцию). Возвращает число строк с ошибками.
    int addText(const QString &text);
    // false - у товара нет подходящего штрихкода (позиция всё равно добавлена, с пометкой, GTIN можно вписать вручную).
    bool addProduct(int productId, int qty);
    // Товары, у которых остаток больше, чем свободных (не напечатанных) кодов: заказать недостающее. Возвращает число добавленных позиций.
    int addByStock();
    void setOrderNumber(const QString &orderId);

    // Действия кнопок без вопросов (для проверок). createOrders - сколько заказов создано; fetchCodes - сколько НОВЫХ кодов добавлено.
    int createOrders();
    int fetchCodes();
    void checkStatus();

private slots:
    void scanEntered();
    void pasteFromClipboard();
    void removeSelected();
    void clearAll();
    void createClicked();

private:
    void refreshSummary();
    void appendLog(const QString &line);
    void save() const;
    void restore();
    QString validGtin(const QString &text) const;
    void setState(int row, const QString &text, bool problem);
    int rowOf(int productId) const;
    int addProducts(const QList<QPair<int, int>> &items);

    QLineEdit *m_scan = nullptr;
    QLineEdit *m_order = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_createBtn = nullptr;
    QPushButton *m_fetchBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    ProductSuggest *m_suggest = nullptr;
    int m_added = 0;
    bool m_stop = false;
    bool m_busy = false;
    bool m_updating = false;
};
