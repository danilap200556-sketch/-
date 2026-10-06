'use strict';
const fs = require('fs');
const path = require('path');
const { Pool } = require('pg');
const { load } = require('../src/config');
const { createDb } = require('../src/db');
const { createApp } = require('../src/app');
const pw = require('../src/passwords');

const PG = {
  host: process.env.TEST_DB_HOST || '127.0.0.1',
  port: Number(process.env.TEST_DB_PORT || 5432),
  user: process.env.TEST_DB_USER || 'postgres',
  password: process.env.TEST_DB_PASSWORD ?? 'postgres',
};

// Отдельная база на каждый тестовый файл; схема - та же, что создаёт приложение.
async function startSite({ schema = true, env = {} } = {}) {
  const dbName = `webtest_${process.pid}_${Math.random().toString(36).slice(2, 8)}`;
  const admin = new Pool({ ...PG, database: 'postgres' });
  await admin.query(`CREATE DATABASE ${dbName}`);
  const cfg = load({
    SESSION_SECRET: 'x'.repeat(40), DB_SSL: 'disable', DB_HOST: PG.host, DB_PORT: String(PG.port),
    DB_USER: PG.user, DB_PASSWORD: PG.password, DB_NAME: dbName, ...env,
  });
  let db = createDb(cfg.db);
  if (schema) await db.pool.query(fs.readFileSync(path.join(__dirname, 'schema.sql'), 'utf8'));
  // WEB_TEST_RESTRICTED=1: сайт работает под пользователем только с правами из grants.sql
  // (как в проде). Заодно проверяется, что сайту не нужен доступ к API-ключам Маркета.
  let roleName = null;
  if (process.env.WEB_TEST_RESTRICTED && schema) {
    roleName = `web_test_${process.pid}_${Math.random().toString(36).slice(2, 8)}`;
    await admin.query(`CREATE ROLE ${roleName} LOGIN PASSWORD 'rolepass'`);
    await db.pool.query(fs.readFileSync(path.join(__dirname, '..', 'grants.sql'), 'utf8').replace(/web_user/g, roleName));
    cfg.db.user = roleName; cfg.db.password = 'rolepass';
    const adminDb = db;
    db = createDb(cfg.db);
    db.adminQuery = (...a) => adminDb.query(...a);
    db.adminEnd = () => adminDb.end();
  }
  const app = createApp(cfg, db);
  const server = await new Promise((resolve) => { const s = app.listen(0, '127.0.0.1', () => resolve(s)); });
  const base = `http://127.0.0.1:${server.address().port}`;

  async function addUser(username, password, isAdmin = false) {
    const salt = pw.newSalt();
    const r = await db.query('INSERT INTO users (username, password_hash, salt, is_admin) VALUES ($1,$2,$3,$4) RETURNING id',
      [username, pw.hashPassword(password, salt), salt, isAdmin]);
    return r.rows[0].id;
  }
  async function stop() {
    await new Promise((r) => server.close(r));
    server.closeAllConnections?.();
    await db.end();
    if (db.adminEnd) await db.adminEnd();
    await admin.query(`DROP DATABASE ${dbName} WITH (FORCE)`);
    if (roleName) await admin.query(`DROP ROLE ${roleName}`);
    await admin.end();
  }
  // Запросы от имени владельца базы (в режиме WEB_TEST_RESTRICTED у сайта нет прав менять market_accounts).
  const sys = (...a) => (db.adminQuery ? db.adminQuery(...a) : db.query(...a));
  return { base, db, cfg, addUser, stop, sys, restricted: Boolean(db.adminQuery), client: () => new Client(base) };
}

// Мини-браузер: хранит cookie, не ходит по редиректам сам, достаёт CSRF-токен со страницы.
class Client {
  constructor(base) { this.base = base; this.jar = new Map(); }
  async req(method, url, { form, headers = {} } = {}) {
    const h = { ...headers };
    if (this.jar.size) h.cookie = [...this.jar].map(([k, v]) => `${k}=${encodeURIComponent(v)}`).join('; ');
    let body;
    if (form) { body = new URLSearchParams(form).toString(); h['content-type'] = 'application/x-www-form-urlencoded'; }
    const res = await fetch(this.base + url, { method, headers: h, body, redirect: 'manual' });
    this.store(res);
    const raw = Buffer.from(await res.arrayBuffer());
    return { status: res.status, headers: res.headers, text: raw.toString('utf8'), raw, location: res.headers.get('location') };
  }
  store(res) {
    for (const c of res.headers.getSetCookie()) {
      const [pair, ...attrs] = c.split(';');
      const i = pair.indexOf('=');
      const name = pair.slice(0, i);
      const value = decodeURIComponent(pair.slice(i + 1));
      const expired = attrs.some((a) => /^\s*max-age=0/i.test(a)) || attrs.some((a) => /^\s*expires=Thu, 01 Jan 1970/i.test(a));
      if (expired || value === '') this.jar.delete(name); else this.jar.set(name, value);
    }
  }
  get(url, o) { return this.req('GET', url, o); }
  // multipart: files = [{ name, data(Buffer), field }]; csrf: 'field' (по умолчанию), 'header' или false.
  async multipart(url, fields, files, { csrf = 'field', from = '/account', headers = {} } = {}) {
    const fd = new FormData();
    const token = csrf ? await this.csrf(from) : null;
    if (csrf === 'field') fd.append('_csrf', token);
    for (const [k, v] of Object.entries(fields)) fd.append(k, v);
    for (const f of files) fd.append(f.field || 'photos', new Blob([f.data]), f.name);
    const h = { ...headers };
    if (csrf === 'header') h['x-csrf-token'] = token;
    if (this.jar.size) h.cookie = [...this.jar].map(([k, v]) => `${k}=${encodeURIComponent(v)}`).join('; ');
    const res = await fetch(this.base + url, { method: 'POST', headers: h, body: fd, redirect: 'manual' });
    this.store(res);
    const raw = Buffer.from(await res.arrayBuffer());
    return { status: res.status, headers: res.headers, text: raw.toString('utf8'), raw, location: res.headers.get('location') };
  }
  async csrf(url = '/account') {
    const r = await this.get(url);
    const m = /name="_csrf" value="([^"]+)"/.exec(r.text);
    if (!m) throw new Error(`нет csrf на ${url}: ${r.status} ${r.location || ''}`);
    return m[1];
  }
  async post(url, form, o = {}) {
    const token = o.csrf === false ? undefined : (o.csrf || await this.csrf(o.from || '/account'));
    return this.req('POST', url, { ...o, form: token ? { _csrf: token, ...form } : form });
  }
  async login(username, password) {
    return this.req('POST', '/login', { form: { username, password } });
  }
  // Следует за одним редиректом (после POST) и возвращает итоговую страницу.
  async follow(r) { return r.location ? this.get(r.location) : r; }
}

module.exports = { startSite, Client, PG };
