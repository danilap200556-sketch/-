'use strict';
const crypto = require('crypto');

const b64 = (buf) => Buffer.from(buf).toString('base64url');
const hmac = (secret, data) => crypto.createHmac('sha256', secret).update(data).digest('base64url');

function safeEqual(a, b) {
  const x = Buffer.from(String(a));
  const y = Buffer.from(String(b));
  return x.length === y.length && crypto.timingSafeEqual(x, y);
}

// Подписанное значение "данные.подпись" - состояние хранится в cookie, а не на сервере.
function sign(secret, obj) {
  const body = b64(JSON.stringify(obj));
  return `${body}.${hmac(secret, body)}`;
}
function unsign(secret, token) {
  if (typeof token !== 'string') return null;
  const i = token.lastIndexOf('.');
  if (i < 0) return null;
  const body = token.slice(0, i);
  if (!safeEqual(token.slice(i + 1), hmac(secret, body))) return null;
  try {
    return JSON.parse(Buffer.from(body, 'base64url').toString('utf8'));
  } catch {
    return null;
  }
}

function parseCookies(header) {
  const out = {};
  for (const part of String(header || '').split(';')) {
    const i = part.indexOf('=');
    if (i < 0) continue;
    const k = part.slice(0, i).trim();
    if (k && !(k in out)) {
      try { out[k] = decodeURIComponent(part.slice(i + 1).trim()); } catch { /* битый cookie - игнорируем */ }
    }
  }
  return out;
}

// Версия пароля внутри сессии: после смены пароля все старые входы перестают работать.
const passwordVersion = (u) =>
  crypto.createHash('sha256').update(`${u.password_hash}:${u.salt}`).digest('hex').slice(0, 16);

const csrfToken = (secret, session) => hmac(secret, `csrf:${session.uid}:${session.iat}:${session.v}`);

// Ограничитель попыток входа (в памяти процесса; сайт - один экземпляр).
class RateLimiter {
  constructor(max, windowMs) {
    this.max = max;
    this.windowMs = windowMs;
    this.hits = new Map();
    setInterval(() => this.prune(), 60_000).unref();
  }
  prune(now = Date.now()) {
    for (const [k, v] of this.hits) if (now - v.first > this.windowMs) this.hits.delete(k);
  }
  blocked(key, now = Date.now()) {
    const v = this.hits.get(key);
    if (!v) return false;
    if (now - v.first > this.windowMs) { this.hits.delete(key); return false; }
    return v.count >= this.max;
  }
  fail(key, now = Date.now()) {
    const v = this.hits.get(key);
    if (!v || now - v.first > this.windowMs) this.hits.set(key, { first: now, count: 1 });
    else v.count++;
  }
  reset(key) { this.hits.delete(key); }
}

module.exports = { sign, unsign, parseCookies, passwordVersion, csrfToken, safeEqual, RateLimiter };
