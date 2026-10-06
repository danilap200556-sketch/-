'use strict';
const { load } = require('./src/config');
const { createDb } = require('./src/db');
const { createApp } = require('./src/app');

const cfg = load();
const db = createDb(cfg.db);
const app = createApp(cfg, db);

const server = app.listen(cfg.port, cfg.host, () => {
  console.log(`Сайт склада запущен: http://${cfg.host}:${cfg.port} (БД ${cfg.db.host}:${cfg.db.port}/${cfg.db.database}, TLS: ${cfg.db.ssl})`);
  if (cfg.db.ssl === 'require') console.log('[db] TLS без проверки сертификата (как в приложении). Для проверки: DB_SSL=verify и DB_SSL_CA.');
  if (!cfg.secureCookies) console.warn('[!] COOKIE_SECURE выключен: без HTTPS пароли передаются открытым текстом.');
});
server.requestTimeout = 30_000;
server.headersTimeout = 15_000;

for (const sig of ['SIGTERM', 'SIGINT']) {
  process.on(sig, () => {
    console.log(`${sig}: останавливаюсь`);
    server.close(() => db.end().finally(() => process.exit(0)));
    setTimeout(() => process.exit(0), 5000).unref();
  });
}
