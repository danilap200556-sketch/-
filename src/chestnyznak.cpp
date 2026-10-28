#include "chestnyznak.h"

#include "appsettings.h"

#include <QEventLoop>
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

struct Http {
    int status = 0;
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
    out.body = reply->readAll();
    if (!out.stopped && out.status == 0)
        out.error = reply->error() == QNetworkReply::TimeoutError ? QStringLiteral("сервер не ответил за %1 с").arg(kTimeoutMs / 1000) : reply->errorString();
    delete reply;
    return out;
}

// Понятное сообщение из ответа с ошибкой.
QString errorText(int status, const QByteArray &body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    QStringList parts;
    if (doc.isObject()) {
        const QJsonObject o = doc.object();
        for (const char *key : {"message", "description", "error", "errorMessage", "error_description"}) {
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
    if (status == 401 || status == 403)
        parts << QStringLiteral("проверьте clientToken и идентификатор СУЗ (omsId)");
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

Reply finish(const Http &http, const QString &label, const QUrl &url, const QByteArray &sentBody)
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
    if (http.status >= 400) {
        r.error = errorText(http.status, http.body);
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace

Settings loadSettings()
{
    Settings s;
    const QString url = AppSettings::get(QString::fromLatin1(kKeyUrl));
    if (!url.trimmed().isEmpty())
        s.baseUrl = url.trimmed();
    s.omsId = AppSettings::get(QString::fromLatin1(kKeyOms)).trimmed();
    s.clientToken = AppSettings::get(QString::fromLatin1(kKeyToken)).trimmed();
    s.signerUrl = AppSettings::get(QString::fromLatin1(kKeySigner)).trimmed();
    s.templateId = AppSettings::get(QString::fromLatin1(kKeyTemplate)).toInt();
    return s;
}

void saveSettings(const Settings &s)
{
    AppSettings::set(QString::fromLatin1(kKeyUrl), s.baseUrl.trimmed());
    AppSettings::set(QString::fromLatin1(kKeyOms), s.omsId.trimmed());
    AppSettings::set(QString::fromLatin1(kKeyToken), s.clientToken.trimmed());
    AppSettings::set(QString::fromLatin1(kKeySigner), s.signerUrl.trimmed());
    AppSettings::set(QString::fromLatin1(kKeyTemplate), QString::number(s.templateId));
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
    if (!s.signerUrl.trimmed().isEmpty()) {
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
