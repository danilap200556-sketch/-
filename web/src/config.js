'use strict';
const crypto = require('crypto');

function intEnv(v, def) {
  const n = parseInt(v, 10);
  return Number.isFinite(n) ? n : def;
}

function load(env = process.env) {
  const prod = env.NODE_ENV === 'production';
  let secret = env.SESSION_SECRET;
  if (!secret) {
    if (prod) throw new Error('SESSION_SECRET не задан (нужна случайная строка от 32 символов)');
    secret = crypto.randomBytes(32).toString('hex');
  }
  if (secret.length < 32) throw new Error('SESSION_SECRET слишком короткий (нужно от 32 символов)');

  const ssl = (env.DB_SSL || 'require').toLowerCase();
  if (!['require', 'verify', 'disable'].includes(ssl)) {
    throw new Error('DB_SSL должен быть require, verify или disable');
  }
  return {
    prod,
    port: intEnv(env.PORT, 3000),
    host: env.HOST || '0.0.0.0',
    secret,
    // Cookie только по HTTPS. На проде по умолчанию включено; для проверки по
    // обычному http можно выключить COOKIE_SECURE=false (пароли пойдут открытым текстом!).
    secureCookies: env.COOKIE_SECURE ? env.COOKIE_SECURE !== 'false' : prod,
    // Сколько прокси стоит перед сайтом (в docker-compose это Caddy - 1).
    trustProxy: intEnv(env.TRUST_PROXY, 0),
    sessionHours: intEnv(env.SESSION_HOURS, 12),
    db: {
      host: env.DB_HOST || 'localhost',
      port: intEnv(env.DB_PORT, 5432),
      database: env.DB_NAME || 'postgres',
      user: env.DB_USER || 'postgres',
      password: env.DB_PASSWORD || '',
      ssl,
      sslCaFile: env.DB_SSL_CA || '',
      poolSize: intEnv(env.DB_POOL, 5),
    },
  };
}

module.exports = { load };
