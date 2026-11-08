#include "cryptosign.h"

#include <algorithm>
#include <cstring>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace CryptoSign {

#ifdef Q_OS_WIN

namespace {

QString windowsMessage(DWORD code)
{
    switch (static_cast<unsigned long>(code)) {
    case 0x80090016: // NTE_BAD_KEYSET
        return QStringLiteral("не найден закрытый ключ сертификата: вставьте токен (флешку) с электронной подписью");
    case 0x8010002E: // SCARD_E_NO_READERS_AVAILABLE
    case 0x80100069: // SCARD_W_REMOVED_CARD
    case 0x8010000C: // SCARD_E_NO_SMARTCARD
        return QStringLiteral("токен с электронной подписью не найден: вставьте его в USB-порт");
    case 0x8010006E: // SCARD_W_CANCELLED_BY_USER
    case 0x800704C7: // ERROR_CANCELLED
        return QStringLiteral("ввод ПИН-кода отменён");
    case 0x80090010: // NTE_PERM
    case 0x8009000F: // NTE_EXISTS
        return QStringLiteral("нет доступа к закрытому ключу");
    case 0x80090019: // NTE_KEYSET_NOT_DEF
    case 0x80090017: // NTE_PROV_TYPE_NOT_DEF
    case 0x80090021: // NTE_PROVIDER_DLL_FAIL
        return QStringLiteral("не найден криптопровайдер: установите и настройте КриптоПро CSP");
    case 0x80090008: // NTE_BAD_ALGID
        return QStringLiteral("криптопровайдер не поддерживает алгоритм сертификата (нужен КриптоПро CSP с поддержкой ГОСТ Р 34.10-2012)");
    default:
        break;
    }
    wchar_t buf[512] = {};
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 511, nullptr);
    QString text = n ? QString::fromWCharArray(buf).trimmed() : QString();
    return QStringLiteral("%1 (код 0x%2)").arg(text.isEmpty() ? QStringLiteral("ошибка Windows") : text).arg(static_cast<qulonglong>(static_cast<unsigned long>(code)), 8, 16, QLatin1Char('0'));
}

QDateTime fromFileTime(const FILETIME &ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 100-нс интервалы с 1601-01-01 -> миллисекунды от эпохи Unix.
    const qint64 ms = static_cast<qint64>(u.QuadPart / 10000ULL) - 11644473600000LL;
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC);
}

QString nameOf(PCCERT_CONTEXT ctx, DWORD type, bool issuer)
{
    wchar_t buf[512] = {};
    CertGetNameStringW(ctx, type, issuer ? CERT_NAME_ISSUER_FLAG : 0, nullptr, buf, 511);
    return QString::fromWCharArray(buf);
}

QString thumbprintOf(PCCERT_CONTEXT ctx)
{
    BYTE hash[32] = {};
    DWORD size = sizeof(hash);
    if (!CertGetCertificateContextProperty(ctx, CERT_HASH_PROP_ID, hash, &size))
        return QString();
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(hash), static_cast<int>(size)).toHex()).toUpper();
}

// ГОСТ-сертификат: хэш подписи должен быть ГОСТ Р 34.11 того же поколения, что и ключ.
QByteArray hashOidFor(PCCERT_CONTEXT ctx, bool *gost)
{
    const QByteArray alg = ctx->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId ? QByteArray(ctx->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId) : QByteArray();
    *gost = true;
    if (alg == "1.2.643.7.1.1.1.1")
        return "1.2.643.7.1.1.2.2";   // ГОСТ Р 34.10-2012 256 -> Стрибог-256
    if (alg == "1.2.643.7.1.1.1.2")
        return "1.2.643.7.1.1.2.3";   // ГОСТ Р 34.10-2012 512 -> Стрибог-512
    if (alg == "1.2.643.2.2.19")
        return "1.2.643.2.2.9";       // ГОСТ Р 34.10-2001 -> ГОСТ Р 34.11-94
    *gost = false;
    return "2.16.840.1.101.3.4.2.1";  // SHA-256 (RSA-сертификат)
}

} // namespace

bool available()
{
    return true;
}

QList<Certificate> certificates(QString *error)
{
    QList<Certificate> out;
    HCERTSTORE store = CertOpenSystemStoreW(0, L"MY");
    if (!store) {
        if (error)
            *error = QStringLiteral("не удалось открыть хранилище сертификатов Windows: %1").arg(windowsMessage(GetLastError()));
        return out;
    }
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr) {
        DWORD size = 0;
        const bool hasKey = CertGetCertificateContextProperty(ctx, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) && size > 0;
        if (!hasKey)
            continue; // чужие сертификаты (без закрытого ключа) для подписи не нужны
        Certificate c;
        c.subject = nameOf(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, false);
        c.issuer = nameOf(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, true);
        c.thumbprint = thumbprintOf(ctx);
        c.validTo = fromFileTime(ctx->pCertInfo->NotAfter);
        c.hasKey = true;
        hashOidFor(ctx, &c.gost);
        if (!c.thumbprint.isEmpty())
            out << c;
    }
    CertCloseStore(store, 0);
    // Сначала действующие, потом по сроку (дальше - раньше).
    std::stable_sort(out.begin(), out.end(), [](const Certificate &a, const Certificate &b) {
        if (a.expired() != b.expired())
            return !a.expired();
        return a.validTo > b.validTo;
    });
    if (out.isEmpty() && error)
        *error = QStringLiteral("в хранилище «Личное» нет сертификатов с закрытым ключом: вставьте токен и установите сертификат в КриптоПро (Сервис → Просмотреть сертификаты в контейнере → Установить)");
    return out;
}

bool sign(const QByteArray &data, const QString &thumbprint, bool detached, QByteArray *signature, QString *error)
{
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    const QByteArray hex = QByteArray::fromHex(thumbprint.trimmed().remove(QLatin1Char(' ')).toLatin1());
    if (hex.size() != 20)
        return fail(QStringLiteral("не выбран сертификат электронной подписи"));
    HCERTSTORE store = CertOpenSystemStoreW(0, L"MY");
    if (!store)
        return fail(QStringLiteral("не удалось открыть хранилище сертификатов Windows: %1").arg(windowsMessage(GetLastError())));
    CRYPT_HASH_BLOB blob;
    blob.cbData = static_cast<DWORD>(hex.size());
    blob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(hex.constData()));
    PCCERT_CONTEXT ctx = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
    if (!ctx) {
        CertCloseStore(store, 0);
        return fail(QStringLiteral("сертификат с отпечатком %1 не найден в хранилище «Личное» этого компьютера").arg(thumbprint));
    }
    bool gost = false;
    QByteArray hashOid = hashOidFor(ctx, &gost);
    CRYPT_SIGN_MESSAGE_PARA para;
    memset(&para, 0, sizeof(para));
    para.cbSize = sizeof(para);
    para.dwMsgEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    para.pSigningCert = ctx;
    para.HashAlgorithm.pszObjId = hashOid.data();
    para.cMsgCert = 1;
    para.rgpMsgCert = &ctx;
    const BYTE *input = reinterpret_cast<const BYTE *>(data.constData());
    DWORD inputSize = static_cast<DWORD>(data.size());
    DWORD outSize = 0;
    bool ok = CryptSignMessage(&para, detached ? TRUE : FALSE, 1, &input, &inputSize, nullptr, &outSize) != FALSE;
    QByteArray result;
    if (ok) {
        result.resize(static_cast<int>(outSize));
        ok = CryptSignMessage(&para, detached ? TRUE : FALSE, 1, &input, &inputSize, reinterpret_cast<BYTE *>(result.data()), &outSize) != FALSE;
        if (ok)
            result.resize(static_cast<int>(outSize));
    }
    const DWORD lastError = ok ? 0 : GetLastError();
    CertFreeCertificateContext(ctx);
    CertCloseStore(store, 0);
    if (!ok)
        return fail(QStringLiteral("не удалось подписать: %1").arg(windowsMessage(lastError)));
    if (signature)
        *signature = result;
    return true;
}

#else // не Windows

bool available()
{
    return false;
}

QList<Certificate> certificates(QString *error)
{
    if (error)
        *error = QStringLiteral("подпись через КриптоПро доступна только в Windows");
    return {};
}

bool sign(const QByteArray &, const QString &, bool, QByteArray *, QString *error)
{
    if (error)
        *error = QStringLiteral("подпись через КриптоПро доступна только в Windows");
    return false;
}

#endif

} // namespace CryptoSign
