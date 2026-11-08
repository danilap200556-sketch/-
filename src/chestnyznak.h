#pragma once

#include <QDateTime>
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
    QString clientToken; // динамический (из единой аутентификации, живёт 10 часов) или статичный
    QString signerUrl;   // необязательно: POST тела запроса -> подпись (base64) в ответе
    int templateId = 0;  // шаблон кода (0 - не указывать)

    // Динамический токен (единая аутентификация, True API). omsConnection - «Идентификатор соединения» устройства в разделе «Устройства» СУЗ.
    QString omsConnection;
    QString trueApiUrl = QStringLiteral("https://markirovka.crpt.ru/api/v3/true-api");
    QDateTime tokenIssuedAt;   // когда программа получила токен (пусто - токен вписан вручную)
    // Сертификат УКЭП, которым подписывать (отпечаток), хранится ТОЛЬКО на этом компьютере: токен с подписью вставлен в него, а не во все.
    QString certThumbprint;
    bool isConfigured() const { return !baseUrl.trimmed().isEmpty() && !omsId.trimmed().isEmpty() && !clientToken.trimmed().isEmpty(); }
};

Settings loadSettings();
void saveSettings(const Settings &settings);

// Адрес API СУЗ из того, что вписал человек: часто берут адрес веб-страницы «Управление заказами» личного кабинета
// (https://suzgrid.crpt.ru/management/orders) - это не API, а сайт: он отвечает HTML с кодом 200 на любой путь. Такой адрес (и адрес
// без пути, и «…/api») приводится к виду <сервер>/api/v3; свой нестандартный путь остаётся как есть. Применяется при чтении и записи настроек.
QString normalizeBaseUrl(const QString &url);

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

// Подпись данных: возвращает подпись (CMS, DER) в base64. Для единой аутентификации - КриптоПро на этом компьютере или внешний «подписант».
using Signer = std::function<bool(const QByteArray &data, QByteArray *signatureBase64, QString *error)>;
// Подписант по настройкам: сертификат КриптоПро (если выбран и мы в Windows), иначе внешний «подписант» по адресу из настроек; пустая функция - подписать нечем.
Signer makeSigner(const Settings &settings, bool detached);

struct TokenReply {
    bool ok = false;
    QString token;
    QString error;
    QStringList trace;
};
// Получение динамического clientToken (инструкция «Получение динамического клиентского токена посредством методов единой аутентификации»):
//   1) GET <True API>/auth/key -> {uuid, data}; 2) подписать data УКЭП; 3) POST <True API>/auth/simpleSignIn/<omsConnection> {uuid, data: подпись base64}
//   -> {token}. Токен действует 10 часов; на одно соединение выдаётся один токен (новый отменяет прежний).
TokenReply obtainToken(const Settings &settings, const Signer &sign, const Cancel &cancelled = {});
// Токен старше этого срока (или его нет) надо получить заново; у вписанного вручную токена срок неизвестен - не обновляется.
bool tokenNeedsRefresh(const Settings &settings);
// Если настроено автоматическое получение (omsConnection и подпись) и токен просрочен/отсутствует - получает новый и сохраняет в настройки.
// *note - что произошло (для журнала). false - обновить было нужно, но не удалось (причина в *note).
bool ensureFreshToken(Settings *settings, QString *note = nullptr);

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
