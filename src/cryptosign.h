#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

// Электронная подпись (УКЭП) через КриптоПро CSP: сертификаты берутся из хранилища «Личное» Windows (в том числе с USB-токена, Рутокен /
// JaCarta и т. п.), подписывает сам КриптоПро через системный CryptoAPI (CryptSignMessage): ПИН-код у токена спросит окно КриптоПро.
// Только Windows с установленным КриптоПро CSP; в других системах функции сообщают, что подпись недоступна.
namespace CryptoSign {

struct Certificate {
    QString subject;      // кому выдан (владелец)
    QString issuer;
    QString thumbprint;   // отпечаток SHA-1, 40 шестнадцатеричных знаков
    QDateTime validTo;
    bool hasKey = false;  // есть закрытый ключ (токен вставлен или ключ в контейнере)
    bool gost = false;    // ГОСТ Р 34.10 (бывают и RSA-сертификаты)
    bool expired() const { return validTo.isValid() && validTo < QDateTime::currentDateTimeUtc(); }
};

// Подпись доступна в этой программе (Windows).
bool available();
// Сертификаты хранилища «Личное», у которых есть закрытый ключ; сначала действующие. При ошибке пусто, причина в *error.
QList<Certificate> certificates(QString *error = nullptr);
// Подписывает данные сертификатом с отпечатком thumbprint. detached - откреплённая подпись (данные не включены), иначе присоединённая.
// Результат - DER (CMS / PKCS#7); для HTTP его кодируют в base64. false - причина по-русски в *error.
bool sign(const QByteArray &data, const QString &thumbprint, bool detached, QByteArray *signature, QString *error = nullptr);

} // namespace CryptoSign
