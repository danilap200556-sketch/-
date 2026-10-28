#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Станция управления заказами «Честного знака» (СУЗ, API v3): заказать коды маркировки для товара и скачать готовые.
// Настройки (адрес СУЗ, идентификатор СУЗ omsId, clientToken) берутся в личном кабинете «Честного знака» и хранятся в общей
// базе (AppSettings). Заказ коды создаётся подписанным запросом (УКЭП): подписать можно только на компьютере с сертификатом,
// поэтому есть необязательный «подписант» - адрес маленькой программы, которая получает тело запроса и возвращает подпись;
// без него заказ создают в личном кабинете, а здесь по номеру заказа скачивают коды (для этого подпись не нужна).
// Адреса и форматы заданы по документации ЦРПТ и проверены только на тестовом сервере-заглушке.
namespace ChestnyZnak {

struct Settings {
    QString baseUrl = QStringLiteral("https://suzgrid.crpt.ru/api/v3"); // без слеша в конце
    QString omsId;
    QString clientToken;
    QString signerUrl;   // необязательно: POST тела запроса -> подпись (base64) в ответе
    int templateId = 0;  // шаблон кода (0 - не указывать)
    bool isConfigured() const { return !baseUrl.trimmed().isEmpty() && !omsId.trimmed().isEmpty() && !clientToken.trimmed().isEmpty(); }
};

Settings loadSettings();
void saveSettings(const Settings &settings);

struct Reply {
    bool ok = false;
    int status = 0;
    QString error;        // понятное сообщение, если не ok
    QJsonObject json;     // ответ (массив лежит в ключе "_array")
    QByteArray body;
    QStringList trace;    // что отправили и что ответили (токен скрыт)
};

struct OrderLine {
    QString gtin;         // GTIN-14
    int quantity = 0;
    int templateId = 0;   // 0 - взять из настроек
};

using Cancel = std::function<bool()>;

// Проверка связи и настроек.
Reply ping(const Settings &settings, const Cancel &cancelled = {});
// Заказ кодов (POST /order). Номер заказа - reply.json["orderId"].
Reply createOrder(const Settings &settings, const QList<OrderLine> &lines, const Cancel &cancelled = {});
// Состояние заказа по GTIN (GET /order/status).
Reply orderStatus(const Settings &settings, const QString &orderId, const QString &gtin, const Cancel &cancelled = {});
// Скачивает до quantity кодов заказа порциями (GET /codes, lastBlockId). Возвращает полученное даже при ошибке посреди пути;
// false - ни одного кода не получено или была ошибка (причина в *error). progress(got, total) false - остановить.
bool fetchCodes(const Settings &settings, const QString &orderId, const QString &gtin, int quantity, QStringList *codes, QString *error,
                QStringList *trace = nullptr, const std::function<bool(int got, int total)> &progress = {});

} // namespace ChestnyZnak
