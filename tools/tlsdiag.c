/* Диагностика подключения к Postgres на уровне TCP + TLS + начало протокола,
 * используя ту же OpenSSL, что и libpq в inventory_manager.exe. Не требует
 * пароля - только адрес/пользователя/базу (не секреты), поэтому её безопасно
 * собирать и распространять отдельно. Воспроизводит "классическое"
 * согласование TLS для Postgres: TCP-коннект -> 8-байтовый SSLRequest ->
 * ждём 'S' -> TLS handshake -> StartupMessage -> читаем первый ответ
 * сервера (запрос авторизации или ErrorResponse с текстом причины).
 * Пароль никуда не отправляется - до него просто не доходит дело.
 *
 * Использование: tlsdiag.exe [host] [port] [user] [database]
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <libpq-fe.h>

#pragma comment(lib, "ws2_32.lib")

static void put_be32(unsigned char *buf, unsigned int v)
{
    buf[0] = (unsigned char)(v >> 24);
    buf[1] = (unsigned char)(v >> 16);
    buf[2] = (unsigned char)(v >> 8);
    buf[3] = (unsigned char)(v);
}

static unsigned int get_be32(const unsigned char *buf)
{
    return ((unsigned int)buf[0] << 24) | ((unsigned int)buf[1] << 16) |
           ((unsigned int)buf[2] << 8) | (unsigned int)buf[3];
}

/* Читает ровно n байт из SSL-соединения (SSL_read может вернуть меньше за раз). */
static int ssl_read_exact(SSL *ssl, unsigned char *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = SSL_read(ssl, buf + got, n - got);
        if (r <= 0)
            return got;
        got += r;
    }
    return got;
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "ep-bitter-brook-b14fjtla-pooler.c-5.eu-central-1.aws.neon.tech";
    const char *port = argc > 2 ? argv[2] : "5432";
    const char *user = argc > 3 ? argv[3] : "neondb_owner";
    const char *dbname = argc > 4 ? argv[4] : "neondb";
    const char *password = argc > 5 ? argv[5] : NULL;

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

    printf("\n>>> TLS handshake прошёл успешно: %s, шифр=%s <<<\n\n",
           SSL_get_version(ssl), SSL_get_cipher(ssl));

    /* --- StartupMessage: протокол 3.0, user, database --- */
    {
        unsigned char msg[512];
        int pos = 4; /* первые 4 байта - длина, заполним в конце */
        put_be32(msg + 4, 0x00030000); /* protocol version 3.0 */
        pos = 8;

#define PUT_STR(s) do { size_t l = strlen(s); memcpy(msg + pos, s, l + 1); pos += (int)(l + 1); } while (0)
        PUT_STR("user");
        PUT_STR(user);
        PUT_STR("database");
        PUT_STR(dbname);
        msg[pos++] = 0; /* терминатор списка параметров */
#undef PUT_STR

        put_be32(msg, (unsigned int)pos);

        printf("Отправляю StartupMessage (user=%s, database=%s)...\n", user, dbname);
        if (SSL_write(ssl, msg, pos) <= 0) {
            printf("Не удалось отправить StartupMessage\n");
            ERR_print_errors_fp(stdout);
            return 1;
        }

        unsigned char header[5];
        int hn = ssl_read_exact(ssl, header, 5);
        if (hn < 5) {
            printf(">>> Сервер закрыл соединение сразу после StartupMessage (получено %d байт заголовка) <<<\n", hn);
            printf("Это значит: TLS полностью в порядке, а обрыв происходит именно на этапе\n");
            printf("разбора StartupMessage сервером (пулером Neon).\n");
            return 1;
        }

        char msgtype = (char)header[0];
        unsigned int msglen = get_be32(header + 1); /* включает себя, но не тип */
        int bodylen = (int)msglen - 4;
        printf("Ответ сервера: тип '%c', длина тела %d байт\n", msgtype, bodylen);

        if (bodylen > 0 && bodylen < 8000) {
            unsigned char body[8000];
            int bn = ssl_read_exact(ssl, body, bodylen);
            body[bn] = 0;

            if (msgtype == 'R') {
                unsigned int authType = bn >= 4 ? get_be32(body) : 0xFFFFFFFF;
                printf(">>> AuthenticationRequest, код %u ", authType);
                switch (authType) {
                case 0: printf("(AuthenticationOk - сервер вообще не просит пароль)\n"); break;
                case 3: printf("(CleartextPassword)\n"); break;
                case 5: printf("(MD5Password)\n"); break;
                case 10: printf("(SASL/SCRAM)\n"); break;
                default: printf("(неизвестный/другой метод)\n"); break;
                }
                printf(">>> Хорошая новость: сервер принял StartupMessage и перешёл к авторизации. <<<\n");
            } else if (msgtype == 'E') {
                int i = 0;
                printf(">>> ErrorResponse от сервера:\n");
                while (i < bn && body[i] != 0) {
                    char field = (char)body[i++];
                    const char *text = (const char *)(body + i);
                    size_t l = strlen(text);
                    printf("    [%c] %s\n", field, text);
                    i += (int)l + 1;
                }
            } else {
                printf(">>> Неожиданный тип сообщения, сырые байты (hex):\n    ");
                {
                    int i;
                    for (i = 0; i < bn; ++i) printf("%02X ", body[i]);
                }
                printf("\n");
            }
        }
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    closesocket(s);
    WSACleanup();

    /* --- Тест 2: настоящий libpq (та же PQconnectdb, что и Qt-плагин),
     * без пароля в строке подключения. Если оборвётся так же, до реальной
     * авторизации ещё далеко - значит дело не в пароле/SCRAM, а в том,
     * как именно libpq формирует своё полное StartupMessage. --- */
    {
        char conninfo[512];
        snprintf(conninfo, sizeof(conninfo),
                 "host=%s port=%s dbname=%s user=%s sslmode=require sslnegotiation=postgres connect_timeout=20",
                 host, port, dbname, user);

        printf("\n--- Тест через настоящий libpq (PQconnectdb, без пароля) ---\n");
        PGconn *conn = PQconnectdb(conninfo);
        ConnStatusType st = PQstatus(conn);
        printf("PQstatus: %s\n", st == CONNECTION_OK ? "CONNECTION_OK" : "не OK");
        printf("PQerrorMessage: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
    }

    /* --- Тест 3: полное подключение с паролем + один настоящий запрос,
     * ровно как делает Database::open() в самом приложении (CREATE TABLE
     * ...; открытие в тесте выше не выполняет ни одного запроса, поэтому
     * не могло поймать обрыв именно на этом шаге). Пароль передаётся
     * только через argv - никуда, кроме этого запуска, не попадает. --- */
    if (password) {
        char conninfo[1024];
        snprintf(conninfo, sizeof(conninfo),
                 "host=%s port=%s dbname=%s user=%s password=%s sslmode=require sslnegotiation=postgres connect_timeout=20",
                 host, port, dbname, user, password);

        printf("\n--- Тест через настоящий libpq (PQconnectdb, С паролем) ---\n");
        PGconn *conn = PQconnectdb(conninfo);
        if (PQstatus(conn) != CONNECTION_OK) {
            printf("PQconnectdb FAILED: %s\n", PQerrorMessage(conn));
            PQfinish(conn);
            return 1;
        }
        printf("PQconnectdb OK, авторизация прошла успешно.\n");

        printf("Выполняю тестовый запрос (как при первом запуске приложения)...\n");
        PGresult *res = PQexec(conn,
            "CREATE TABLE IF NOT EXISTS tlsdiag_probe (id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY)");
        ExecStatusType est = PQresultStatus(res);
        printf("PQresultStatus: %d, PQerrorMessage: %s\n", est, PQerrorMessage(conn));
        if (est == PGRES_COMMAND_OK) {
            printf(">>> Запрос выполнен успешно! <<<\n");
            PQclear(res);
            res = PQexec(conn, "DROP TABLE IF EXISTS tlsdiag_probe");
            PQclear(res);
        } else {
            printf(">>> ЗАПРОС НЕ ВЫПОЛНИЛСЯ - вот тут и есть настоящая причина. <<<\n");
            PQclear(res);
        }

        printf("PQstatus после запроса: %s\n", PQstatus(conn) == CONNECTION_OK ? "CONNECTION_OK" : "ОБОРВАНО");
        PQfinish(conn);
    } else {
        printf("\n(Тест 3 с паролем пропущен - запусти с 5-м аргументом = пароль, чтобы проверить и его)\n");
    }

    /* --- Тест 4: та же connection string, что строит сам Qt (с ; в опциях,
     * которые превращаются в пробелы - ровно как в QPSQLDriver::open), и
     * СРАЗУ ПОСЛЕ ПОДКЛЮЧЕНИЯ - те же 5 служебных запросов, которые Qt сам
     * выполняет внутри db.open() ДО того, как наш код в qtpgtest.exe вообще
     * получает управление: SELECT version(), SELECT '\\' x, SET CLIENT_ENCODING,
     * SET DATESTYLE, SET bytea_output. qtpgtest.exe показал db.open()==true, но
     * isOpen()==false сразу после - то есть соединение обрывается именно на
     * одном из этих пяти запросов, ещё до "SELECT 1" из нашего собственного
     * теста. Раз голый libpq в Тесте 3 выполнил CREATE TABLE без проблем,
     * подозрение на сами эти конкретные запросы (или их порядок/количество). */
    if (password) {
        char conninfo[1024];
        snprintf(conninfo, sizeof(conninfo),
                 "host=%s port=%s dbname=%s user=%s password=%s sslmode=require sslnegotiation=postgres connect_timeout=20",
                 host, port, dbname, user, password);

        printf("\n--- Тест 4: повторяю служебные запросы Qt-драйвера один за другим ---\n");
        PGconn *conn = PQconnectdb(conninfo);
        if (PQstatus(conn) != CONNECTION_OK) {
            printf("PQconnectdb FAILED: %s\n", PQerrorMessage(conn));
            PQfinish(conn);
            return 1;
        }
        printf("PQconnectdb OK.\n");

        const char *steps[] = {
            "SELECT version()",
            "SELECT '\\\\' x",
            "SET CLIENT_ENCODING TO 'UNICODE'",
            "SET DATESTYLE TO 'ISO'",
            "SET bytea_output TO escape",
        };
        int i;
        for (i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); ++i) {
            printf("  [%d] %s ... ", i, steps[i]);
            PGresult *r = PQexec(conn, steps[i]);
            ExecStatusType est = PQresultStatus(r);
            int ok = (est == PGRES_COMMAND_OK || est == PGRES_TUPLES_OK);
            printf("%s (status=%d)\n", ok ? "OK" : "FAIL", est);
            if (!ok)
                printf("      PQerrorMessage: %s\n", PQerrorMessage(conn));
            PQclear(r);
            printf("      PQstatus после этого шага: %s\n",
                   PQstatus(conn) == CONNECTION_OK ? "CONNECTION_OK" : "ОБОРВАНО");
            if (PQstatus(conn) != CONNECTION_OK) {
                printf("  >>> Соединение оборвалось именно на шаге [%d] (%s). <<<\n", i, steps[i]);
                break;
            }
        }
        PQfinish(conn);
    }

    return 0;
}
