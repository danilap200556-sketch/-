/* Диагностика подключения к Postgres на уровне TCP + TLS, используя ту же
 * OpenSSL, что и libpq в inventory_manager.exe. Не требует пароля/базы -
 * только адрес сервера, поэтому её безопасно собирать и распространять
 * отдельно. Воспроизводит "классическое" согласование TLS для Postgres:
 * TCP-коннект -> 8-байтовый SSLRequest -> ждём 'S' -> TLS handshake.
 *
 * Использование: tlsdiag.exe [host] [port]
 * По умолчанию host - Neon-пулер из этого проекта, port - 5432.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#pragma comment(lib, "ws2_32.lib")

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "ep-bitter-brook-b14fjtla-pooler.c-5.eu-central-1.aws.neon.tech";
    const char *port = argc > 2 ? argv[2] : "5432";

    printf("Проверяю %s:%s ...\n\n", host, port);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        printf("DNS FAILED: код %d\n", rc);
        return 1;
    }
    printf("DNS OK\n");

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        printf("socket() FAILED: %d\n", WSAGetLastError());
        return 1;
    }

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        printf("TCP CONNECT FAILED: код ошибки %d\n", WSAGetLastError());
        return 1;
    }
    printf("TCP подключение установлено\n");

    /* SSLRequest: длина пакета 8 (big-endian), код 80877103 (0x04D2162F) */
    unsigned char sslrequest[8] = {0, 0, 0, 8, 0x04, 0xD2, 0x16, 0x2F};
    if (send(s, (const char *)sslrequest, 8, 0) != 8) {
        printf("Не удалось отправить SSLRequest: %d\n", WSAGetLastError());
        return 1;
    }

    char resp = 0;
    int n = recv(s, &resp, 1, 0);
    if (n != 1) {
        printf("Сервер не ответил на SSLRequest (recv=%d, ошибка %d)\n", n, WSAGetLastError());
        printf("\n>>> Похоже, соединение обрывается именно тут, до начала TLS. <<<\n");
        return 1;
    }
    printf("Ответ сервера на SSLRequest: '%c' (0x%02X)\n", resp, (unsigned char)resp);

    if (resp != 'S') {
        printf("Сервер отказался использовать SSL для этого соединения\n");
        return 1;
    }

    SSL_library_init();
    SSL_load_error_strings();
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        printf("SSL_CTX_new failed\n");
        return 1;
    }

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, (int)s);
    SSL_set_tlsext_host_name(ssl, host);

    int ret = SSL_connect(ssl);
    if (ret != 1) {
        int err = SSL_get_error(ssl, ret);
        printf("TLS HANDSHAKE FAILED: SSL_get_error=%d\n", err);
        printf("\nOpenSSL error queue:\n");
        ERR_print_errors_fp(stdout);
        return 1;
    }

    printf("\n>>> TLS handshake прошёл успешно: %s, шифр=%s <<<\n",
           SSL_get_version(ssl), SSL_get_cipher(ssl));

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    closesocket(s);
    WSACleanup();
    return 0;
}
