#include "chestnyznak.h"

#include "appsettings.h"
#include "cryptosign.h"

#include <QEventLoop>
#include <QJsonObject>
#include <QSettings>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace ChestnyZnak {

namespace {

constexpr int kTimeoutMs = 60000;
constexpr int kCodesPerRequest = 10000;
const char kStopped[] = "остановлено";

const char kKeyUrl[] = "cz_suz_url";
const char kKeyOms[] = "cz_oms_id";
const char kKeyToken[] = "cz_client_token";
const char kKeySigner[] = "cz_signer_url";
const char kKeyTemplate[] = "cz_template_id";
const char kKeyConnection[] = "cz_oms_connection";
const char kKeyTrueApi[] = "cz_true_api";
const char kKeyTokenAt[] = "cz_token_issued_at";
const char kLocalCert[] = "cz/certThumbprint"; // QSettings этого компьютера

struct Http {
    int status = 0;
    QString contentType;
    QByteArray body;
    QString error; // сетевая ошибка (не HTTP-код)
    bool stopped = false;
};

Http send(const QString &method, const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &body, const Cancel &cancelled)
{
    Http out;
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setTransferTimeout(kTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("Accept", "application/json");
    for (const auto &h : headers)
        req.setRawHeader(h.first, h.second);
    QNetworkReply *reply = method == QLatin1String("POST") ? nam.post(req, body) : nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer watch;
    watch.setInterval(150);
    bool polling = false;
    QObject::connect(&watch, &QTimer::timeout, &loop, [&]() {
        if (polling || !cancelled)
            return;
        polling = true;
        const bool asked = cancelled();
        polling = false;
        if (asked) {
            out.stopped = true;
            reply->abort();
        }
    });
    watch.start();
    loop.exec();
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    out.body = reply->readAll();
    if (!out.stopped && out.status == 0)
        out.error = reply->error() == QNetworkReply::TimeoutError ? QStringLiteral("сервер не ответил за %1 с").arg(kTimeoutMs / 1000) : reply->errorString();
    delete reply;
    return out;
}

// Веб-страница вместо ответа API (адрес указывает на сайт, а не на API).
bool looksLikeHtml(const QString &contentType, const QByteArray &body)
{
    if (contentType.contains(QLatin1String("html"), Qt::CaseInsensitive))
        return true;
    const QByteArray head = body.left(200).trimmed().toLower();
    return head.startsWith("<!doctype") || head.startsWith("<html") || head.startsWith("<head") || head.startsWith("<body");
}

const char kHtmlHint[] = "по этому адресу отвечает веб-страница, а не API СУЗ. В настройках адрес СУЗ должен быть адресом API, например "
                         "https://suzgrid.crpt.ru/api/v3 (а не адресом страницы «Управление заказами» личного кабинета, который кончается на /management/orders)";

// Понятное сообщение из ответа с ошибкой.
QString errorText(int status, const QByteArray &body, bool tokenHint = true)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    QStringList parts;
    if (doc.isObject()) {
        const QJsonObject o = doc.object();
        // Единая аутентификация (True API): error_message - что случилось, description - подробности; нужны оба.
        const QString trueApiMessage = o.value(QStringLiteral("error_message")).toString();
        if (!trueApiMessage.isEmpty()) {
            parts << trueApiMessage;
            const QString details = o.value(QStringLiteral("description")).toString();
            if (!details.isEmpty() && details != trueApiMessage)
                parts << details;
        }
        for (const char *key : {"message", "description", "error", "errorMessage", "error_description"}) {
            if (!parts.isEmpty())
                break;
            const QJsonValue v = o.value(QLatin1String(key));
            if (v.isString() && !v.toString().isEmpty()) {
                parts << v.toString();
                break;
            }
        }
        for (const QJsonValue &g : o.value(QStringLiteral("globalErrors")).toArray()) {
            const QJsonObject ge = g.toObject();
            const QString text = ge.value(QStringLiteral("error")).toString();
            if (!text.isEmpty())
                parts << QStringLiteral("%1%2").arg(ge.contains(QStringLiteral("errorCode")) ? QString::number(ge.value(QStringLiteral("errorCode")).toInt()) + QStringLiteral(": ") : QString(), text);
        }
    }
    if (parts.isEmpty()) {
        QString text = QString::fromUtf8(body).simplified();
        if (text.size() > 200)
            text = text.left(200) + QStringLiteral("…");
        if (!text.isEmpty())
            parts << text;
    }
    if (tokenHint && (status == 401 || status == 403))
        parts << QStringLiteral("проверьте clientToken (он может быть временным - до 10 часов - и устареть: получите новый) и идентификатор СУЗ (omsId)");
    return QStringLiteral("HTTP %1%2").arg(status).arg(parts.isEmpty() ? QString() : QStringLiteral(": ") + parts.join(QStringLiteral("; ")));
}

QUrl endpoint(const Settings &s, const QString &path, const QList<QPair<QString, QString>> &extra = {})
{
    QString base = s.baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    QUrl url(base + path);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("omsId"), s.omsId.trimmed());
    for (const auto &kv : extra)
        q.addQueryItem(kv.first, kv.second);
    url.setQuery(q);
    return url;
}

Reply finish(const Http &http, const QString &label, const QUrl &url, const QByteArray &sentBody, bool tokenHint = true)
{
    Reply r;
    r.status = http.status;
    r.body = http.body;
    r.trace << QStringLiteral("%1 %2%3 -> %4, %5 байт").arg(label, url.toString(QUrl::FullyEncoded), sentBody.isEmpty() ? QString() : QStringLiteral(" (тело %1 байт)").arg(sentBody.size()),
                                                         http.stopped ? QString::fromUtf8(kStopped) : (http.status ? QStringLiteral("HTTP %1").arg(http.status) : http.error)).arg(http.body.size());
    if (http.stopped) {
        r.error = QString::fromUtf8(kStopped);
        return r;
    }
    if (http.status == 0) {
        r.error = http.error;
        return r;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(http.body);
    if (doc.isObject())
        r.json = doc.object();
    else if (doc.isArray())
        r.json.insert(QStringLiteral("_array"), doc.array());
    // Сайт вместо API отвечает 200 и страницей: это не «связь есть», а неверный адрес.
    if (!doc.isObject() && !doc.isArray() && looksLikeHtml(http.contentType, http.body)) {
        r.error = http.status >= 400 ? QStringLiteral("HTTP %1: %2").arg(http.status).arg(QString::fromUtf8(kHtmlHint)) : QString::fromUtf8(kHtmlHint);
        return r;
    }
    if (http.status >= 400) {
        r.error = errorText(http.status, http.body, tokenHint);
        return r;
    }
    // Любой настоящий ответ API - JSON; всё остальное (пустая страница, текст) - не тот адрес или не тот сервер.
    if (!http.body.trimmed().isEmpty() && !doc.isObject() && !doc.isArray()) {
        r.error = QStringLiteral("ответ не похож на ответ API СУЗ (не JSON): %1. Проверьте адрес СУЗ в настройках (обычно https://suzgrid.crpt.ru/api/v3)")
                      .arg(QString::fromUtf8(http.body.left(120)).simplified());
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace

QString normalizeBaseUrl(const QString &input)
{
    QString text = input.trimmed();
    while (text.endsWith(QLatin1Char('/')))
        text.chop(1);
    if (text.isEmpty())
        return text;
    QUrl url(text);
    if (!url.isValid() || url.host().isEmpty())
        return text;
    const QString path = url.path().toLower();
    if (path.isEmpty() || path.contains(QLatin1String("/management")))
        url.setPath(QStringLiteral("/api/v3"));   // сайт кабинета или голый адрес сервера -> API
    else if (path == QLatin1String("/api"))
        url.setPath(QStringLiteral("/api/v3"));
    url.setQuery(QString());
    url.setFragment(QString());
    QString out = url.toString(QUrl::FullyEncoded);
    while (out.endsWith(QLatin1Char('/')))
        out.chop(1);
    return out;
}

Settings loadSettings()
{
    Settings s;
    const QString url = AppSettings::get(QString::fromLatin1(kKeyUrl));
    if (!url.trimmed().isEmpty())
        s.baseUrl = normalizeBaseUrl(url);
    s.omsId = AppSettings::get(QString::fromLatin1(kKeyOms)).trimmed();
    s.clientToken = AppSettings::get(QString::fromLatin1(kKeyToken)).trimmed();
    s.signerUrl = AppSettings::get(QString::fromLatin1(kKeySigner)).trimmed();
    s.templateId = AppSettings::get(QString::fromLatin1(kKeyTemplate)).toInt();
    s.omsConnection = AppSettings::get(QString::fromLatin1(kKeyConnection)).trimmed();
    const QString trueApi = AppSettings::get(QString::fromLatin1(kKeyTrueApi)).trimmed();
    if (!trueApi.isEmpty())
        s.trueApiUrl = trueApi;
    s.tokenIssuedAt = QDateTime::fromString(AppSettings::get(QString::fromLatin1(kKeyTokenAt)), Qt::ISODate);
    s.certThumbprint = QSettings().value(QString::fromLatin1(kLocalCert)).toString().trimmed();
    return s;
}

void saveSettings(const Settings &s)
{
    AppSettings::set(QString::fromLatin1(kKeyUrl), normalizeBaseUrl(s.baseUrl));
    AppSettings::set(QString::fromLatin1(kKeyOms), s.omsId.trimmed());
    AppSettings::set(QString::fromLatin1(kKeyToken), s.clientToken.trimmed());
    AppSettings::set(QString::fromLatin1(kKeySigner), s.signerUrl.trimmed());
    AppSettings::set(QString::fromLatin1(kKeyTemplate), QString::number(s.templateId));
    AppSettings::set(QString::fromLatin1(kKeyConnection), s.omsConnection.trimmed());
    AppSettings::set(QString::fromLatin1(kKeyTrueApi), s.trueApiUrl.trimmed());
    AppSettings::set(QString::fromLatin1(kKeyTokenAt), s.tokenIssuedAt.isValid() ? s.tokenIssuedAt.toString(Qt::ISODate) : QString(QLatin1String("")));
    QSettings().setValue(QString::fromLatin1(kLocalCert), s.certThumbprint.trimmed());
}

static QList<QPair<QByteArray, QByteArray>> authHeaders(const Settings &s)
{
    return {{"clientToken", s.clientToken.trimmed().toUtf8()}};
}

Reply ping(const Settings &s, const Cancel &cancelled)
{
    const QUrl url = endpoint(s, QStringLiteral("/ping"));
    return finish(send(QStringLiteral("GET"), url, authHeaders(s), {}, cancelled), QStringLiteral("GET"), url, {});
}

Reply createOrder(const Settings &s, const QList<OrderLine> &lines, const Cancel &cancelled)
{
    QJsonArray products;
    for (const OrderLine &l : lines) {
        QJsonObject p{{QStringLiteral("gtin"), l.gtin}, {QStringLiteral("quantity"), l.quantity},
                      {QStringLiteral("serialNumberType"), QStringLiteral("OPERATOR")}, {QStringLiteral("cisType"), QStringLiteral("UNIT")}};
        const int tpl = l.templateId > 0 ? l.templateId : s.templateId;
        if (tpl > 0)
            p.insert(QStringLiteral("templateId"), tpl);
        products << p;
    }
    const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("products"), products}}).toJson(QJsonDocument::Compact);
    const QUrl url = endpoint(s, QStringLiteral("/order"));
    auto headers = authHeaders(s);
    headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
    Reply signerFail;
    if (!s.certThumbprint.trimmed().isEmpty() && CryptoSign::available()) {
        // Подпись КриптоПро с токена на этом компьютере: откреплённая, в заголовок X-Signature (base64).
        QByteArray signature;
        QString signError;
        if (!CryptoSign::sign(body, s.certThumbprint, true, &signature, &signError)) {
            signerFail.error = QStringLiteral("заказ не подписан: %1").arg(signError);
            return signerFail;
        }
        signerFail.trace << QStringLiteral("подписано сертификатом КриптоПро (%1 байт)").arg(signature.size());
        headers.append(qMakePair(QByteArray("X-Signature"), signature.toBase64()));
    } else if (!s.signerUrl.trimmed().isEmpty()) {
        // Подпись тела запроса (УКЭП) делает отдельная программа на компьютере с сертификатом.
        const Http sig = send(QStringLiteral("POST"), QUrl(s.signerUrl.trimmed()), {{"Content-Type", "application/octet-stream"}, {"Accept", "text/plain"}}, body, cancelled);
        signerFail.trace << QStringLiteral("подписант %1 -> %2, %3 байт").arg(s.signerUrl.trimmed(), sig.status ? QStringLiteral("HTTP %1").arg(sig.status) : sig.error).arg(sig.body.size());
        if (sig.stopped) {
            signerFail.error = QString::fromUtf8(kStopped);
            return signerFail;
        }
        const QByteArray signature = sig.body.trimmed();
        if (sig.status != 200 || signature.isEmpty()) {
            signerFail.error = QStringLiteral("подписант не вернул подпись (%1)").arg(sig.status ? QStringLiteral("HTTP %1").arg(sig.status) : sig.error);
            return signerFail;
        }
        headers.append(qMakePair(QByteArray("X-Signature"), signature));
    }
    Reply r = finish(send(QStringLiteral("POST"), url, headers, body, cancelled), QStringLiteral("POST"), url, body);
    r.trace = signerFail.trace + r.trace;
    if (!r.ok && (r.status == 400 || r.status == 401 || r.status == 403) && s.signerUrl.trimmed().isEmpty())
        r.error += QStringLiteral(" (заказ подписывается электронной подписью: задайте подписант в настройках или создайте заказ в личном кабинете и скачайте коды по его номеру)");
    return r;
}

Signer makeSigner(const Settings &s, bool detached)
{
    if (!s.certThumbprint.trimmed().isEmpty() && CryptoSign::available()) {
        const QString thumb = s.certThumbprint;
        return [thumb, detached](const QByteArray &data, QByteArray *out, QString *error) {
            QByteArray sig;
            if (!CryptoSign::sign(data, thumb, detached, &sig, error))
                return false;
            *out = sig.toBase64();
            return true;
        };
    }
    if (!s.signerUrl.trimmed().isEmpty()) {
        const QString url = s.signerUrl.trimmed();
        return [url](const QByteArray &data, QByteArray *out, QString *error) {
            const Http h = send(QStringLiteral("POST"), QUrl(url), {{"Content-Type", "application/octet-stream"}, {"Accept", "text/plain"}}, data, {});
            const QByteArray sig = h.body.trimmed();
            if (h.status != 200 || sig.isEmpty()) {
                if (error)
                    *error = QStringLiteral("подписант не вернул подпись (%1)").arg(h.status ? QStringLiteral("HTTP %1").arg(h.status) : h.error);
                return false;
            }
            *out = sig;
            return true;
        };
    }
    return {};
}

TokenReply obtainToken(const Settings &s, const Signer &sign, const Cancel &cancelled)
{
    TokenReply out;
    if (s.omsConnection.trimmed().isEmpty()) {
        out.error = QStringLiteral("не указан идентификатор соединения (omsConnection): он в разделе «Устройства» станции управления заказами, столбец «Идентификатор соединения»");
        return out;
    }
    if (!sign) {
        out.error = QStringLiteral("нечем подписать: выберите сертификат электронной подписи (Windows с КриптоПро) или укажите адрес «подписанта»");
        return out;
    }
    QString base = s.trueApiUrl.trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    if (base.isEmpty())
        base = QStringLiteral("https://markirovka.crpt.ru/api/v3/true-api");

    // 1) случайные данные для подписи
    const QUrl keyUrl(base + QStringLiteral("/auth/key"));
    const Reply key = finish(send(QStringLiteral("GET"), keyUrl, {}, {}, cancelled), QStringLiteral("GET"), keyUrl, {}, false);
    out.trace << key.trace;
    if (!key.ok) {
        out.error = QStringLiteral("не получены данные для подписи: %1").arg(key.error);
        return out;
    }
    const QString uuid = key.json.value(QStringLiteral("uuid")).toString();
    const QString data = key.json.value(QStringLiteral("data")).toString();
    if (uuid.isEmpty() || data.isEmpty()) {
        out.error = QStringLiteral("сервер вернул данные для подписи не в ожидаемом виде: %1").arg(QString::fromUtf8(key.body.left(200)));
        return out;
    }
    // 2) подпись (присоединённая: в ней сами данные)
    QByteArray signature;
    QString signError;
    if (!sign(data.toUtf8(), &signature, &signError)) {
        out.error = signError.isEmpty() ? QStringLiteral("не удалось подписать данные") : signError;
        return out;
    }
    // 3) обмен подписи на токен
    const QUrl signInUrl(base + QStringLiteral("/auth/simpleSignIn/") + s.omsConnection.trimmed());
    const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("uuid"), uuid}, {QStringLiteral("data"), QString::fromLatin1(signature)}}).toJson(QJsonDocument::Compact);
    const Reply tok = finish(send(QStringLiteral("POST"), signInUrl, {{"Content-Type", "application/json"}, {"Accept", "application/json"}}, body, cancelled),
                             QStringLiteral("POST"), signInUrl, body, false);
    out.trace << tok.trace;
    if (!tok.ok) {
        out.error = QStringLiteral("токен не получен: %1").arg(tok.error);
        return out;
    }
    const QString token = tok.json.value(QStringLiteral("token")).toString();
    if (token.isEmpty()) {
        const QString detail = tok.json.value(QStringLiteral("error_message")).toString() + QStringLiteral(" ") + tok.json.value(QStringLiteral("description")).toString();
        out.error = QStringLiteral("в ответе нет токена: %1").arg(detail.simplified().isEmpty() ? QString::fromUtf8(tok.body.left(200)) : detail.simplified());
        return out;
    }
    out.ok = true;
    out.token = token;
    return out;
}

bool tokenNeedsRefresh(const Settings &s)
{
    if (s.clientToken.trimmed().isEmpty())
        return true;
    if (!s.tokenIssuedAt.isValid())
        return false; // вписан вручную - срок неизвестен
    return s.tokenIssuedAt.secsTo(QDateTime::currentDateTimeUtc()) > 9 * 3600 + 30 * 60; // токен живёт 10 часов: обновляем за полчаса до конца
}

bool ensureFreshToken(Settings *s, QString *note)
{
    if (!tokenNeedsRefresh(*s))
        return true;
    const Signer sign = makeSigner(*s, false);
    if (s->omsConnection.trimmed().isEmpty() || !sign)
        return true; // автоматическое получение не настроено - работаем с тем токеном, что есть
    const TokenReply r = obtainToken(*s, sign);
    if (!r.ok) {
        if (note)
            *note = QStringLiteral("Токен СУЗ не обновлён: %1").arg(r.error);
        return false;
    }
    s->clientToken = r.token;
    s->tokenIssuedAt = QDateTime::currentDateTimeUtc();
    saveSettings(*s);
    if (note)
        *note = QStringLiteral("Получен новый токен СУЗ (действует 10 часов).");
    return true;
}

Reply orderStatus(const Settings &s, const QString &orderId, const QString &gtin, const Cancel &cancelled)
{
    const QUrl url = endpoint(s, QStringLiteral("/order/status"), {{QStringLiteral("orderId"), orderId.trimmed()}, {QStringLiteral("gtin"), gtin.trimmed()}});
    return finish(send(QStringLiteral("GET"), url, authHeaders(s), {}, cancelled), QStringLiteral("GET"), url, {});
}

bool fetchCodes(const Settings &s, const QString &orderId, const QString &gtin, int quantity, QStringList *codes, QString *error,
                QStringList *trace, const std::function<bool(int, int)> &progress)
{
    codes->clear();
    QString lastBlock = QStringLiteral("0");
    QSet<QString> seen;
    bool stop = false;
    const Cancel cancelled = [&]() { return stop; };
    while (codes->size() < quantity) {
        const int want = qMin(kCodesPerRequest, quantity - int(codes->size()));
        const QUrl url = endpoint(s, QStringLiteral("/codes"), {{QStringLiteral("orderId"), orderId.trimmed()}, {QStringLiteral("quantity"), QString::number(want)},
                                                               {QStringLiteral("gtin"), gtin.trimmed()}, {QStringLiteral("lastBlockId"), lastBlock}});
        const Reply r = finish(send(QStringLiteral("GET"), url, authHeaders(s), {}, cancelled), QStringLiteral("GET"), url, {});
        if (trace)
            *trace << r.trace;
        if (!r.ok) {
            if (error)
                *error = r.error;
            return false;
        }
        int got = 0;
        for (const QJsonValue &v : r.json.value(QStringLiteral("codes")).toArray()) {
            const QString c = v.toString();
            if (!c.isEmpty() && !seen.contains(c)) { // сервер, игнорирующий lastBlockId, не должен зациклить загрузку
                seen.insert(c);
                *codes << c;
                ++got;
            }
        }
        const QString block = r.json.value(QStringLiteral("blockId")).toVariant().toString();
        if (got == 0) {
            if (codes->isEmpty() && error)
                *error = QStringLiteral("сервер пока не отдал ни одного кода - заказ ещё готовится или все коды уже получены");
            return !codes->isEmpty();
        }
        if (!block.isEmpty())
            lastBlock = block;
        if (progress && !progress(int(codes->size()), quantity)) {
            stop = true;
            break;
        }
    }
    return !codes->isEmpty();
}

} // namespace ChestnyZnak
