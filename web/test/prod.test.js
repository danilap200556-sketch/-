'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { startSite } = require('./helpers');

test('прод-режим: cookie с префиксом __Host-, Secure, HttpOnly, SameSite; HSTS', async () => {
  const site = await startSite({ env: { COOKIE_SECURE: 'true' } });
  try {
    await site.addUser('boss', 'bosspass1', true);
    const c = site.client();
    const r = await c.login('boss', 'bosspass1');
    assert.equal(r.status, 302);
    const sc = r.headers.getSetCookie().find((x) => x.startsWith('__Host-session='));
    assert.ok(sc, 'cookie сессии');
    assert.match(sc, /; Path=\//); assert.match(sc, /HttpOnly/); assert.match(sc, /Secure/); assert.match(sc, /SameSite=Lax/);
    assert.ok(!/Domain=/i.test(sc), '__Host- не допускает Domain');
    assert.match(r.headers.get('strict-transport-security'), /max-age=/);
    assert.equal((await c.get('/stock')).status, 200);
  } finally { await site.stop(); }
});

test('за прокси (TRUST_PROXY=1): ограничитель считает попытки по реальному IP клиента', async () => {
  const site = await startSite({ env: { TRUST_PROXY: '1' } });
  try {
    await site.addUser('victim', 'victimpass', false);
    const attempt = (ip, pass) => site.client().req('POST', '/login', { form: { username: 'victim', password: pass }, headers: { 'x-forwarded-for': ip } });
    for (let i = 0; i < 5; i++) assert.equal((await attempt('1.2.3.4', 'bad')).status, 401);
    assert.equal((await attempt('1.2.3.4', 'victimpass')).status, 429);      // этот клиент заблокирован
    assert.equal((await attempt('5.6.7.8', 'victimpass')).status, 302);      // другой клиент - нет
  } finally { await site.stop(); }
});

test('без прокси заголовок X-Forwarded-For игнорируется (нельзя обойти ограничитель подменой)', async () => {
  const site = await startSite();
  try {
    await site.addUser('victim', 'victimpass', false);
    for (let i = 0; i < 5; i++) await site.client().req('POST', '/login', { form: { username: 'victim', password: 'bad' }, headers: { 'x-forwarded-for': `9.9.9.${i}` } });
    const r = await site.client().req('POST', '/login', { form: { username: 'victim', password: 'victimpass' }, headers: { 'x-forwarded-for': '8.8.8.8' } });
    assert.equal(r.status, 429);
  } finally { await site.stop(); }
});

test('база без таблиц приложения: сайт отвечает 503 и не падает', async () => {
  const site = await startSite({ schema: false });
  try {
    const r = await site.client().get('/login');
    assert.equal(r.status, 503);
    assert.ok(!/products|users/.test(r.text), 'внутренние детали не показываются посетителю');
    assert.equal((await site.client().get('/healthz')).text, 'ok');
  } finally { await site.stop(); }
});

test('конфигурация: без SESSION_SECRET прод не стартует; короткий секрет и плохой DB_SSL отвергаются', () => {
  const { load } = require('../src/config');
  assert.throws(() => load({ NODE_ENV: 'production' }), /SESSION_SECRET/);
  assert.throws(() => load({ SESSION_SECRET: 'short' }), /короткий/);
  assert.throws(() => load({ SESSION_SECRET: 'x'.repeat(40), DB_SSL: 'maybe' }), /DB_SSL/);
  const c = load({ NODE_ENV: 'production', SESSION_SECRET: 'x'.repeat(40) });
  assert.equal(c.secureCookies, true);
  assert.equal(load({ NODE_ENV: 'production', SESSION_SECRET: 'x'.repeat(40), COOKIE_SECURE: 'false' }).secureCookies, false);
});

test('мусорные запросы не роняют сайт: POST без тела, JSON вместо формы, огромное тело', async () => {
  const site = await startSite();
  try {
    await site.addUser('boss', 'bosspass1', true);
    const c = site.client();
    assert.equal((await c.req('POST', '/login')).status, 401);
    assert.equal((await c.req('POST', '/login', { headers: { 'content-type': 'application/json' } })).status, 401);
    const big = await fetch(site.base + '/login', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: 'username=' + 'a'.repeat(700_000) });
    assert.equal(big.status, 413);
    assert.equal((await c.get('/%E0%A4%A')).status < 500, true);
    await c.login('boss', 'bosspass1');
    assert.equal((await c.req('POST', '/products')).status, 403);   // без CSRF-токена
    assert.equal((await c.get('/products?q[]=a&q[]=b&page=-5')).status, 200);
    assert.equal((await c.get('/stock?w=1%27%20OR%201=1')).status, 200);
  } finally { await site.stop(); }
});
