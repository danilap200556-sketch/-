'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { startSite } = require('./helpers');

// Приём заказов от сайта-магазина: очередь на списание со склада (списывает приложение), отмена, повторы, токен.
const TOKEN = 'orders-token-0123456789abcdef0123456789';
let site; let off;
const q = async (sql, p) => (await site.db.query(sql, p)).rows;

test.before(async () => {
  site = await startSite({ env: { SITE_ORDERS_TOKEN: TOKEN } });
  off = await startSite(); // без токена: приём заказов выключен
});
test.after(async () => { await site.stop(); await off.stop(); });

const call = (s, method, path, body, headers = { authorization: `Bearer ${TOKEN}` }) =>
  fetch(s.base + path, { method, headers: { 'content-type': 'application/json', ...headers }, body: body === undefined ? undefined : (typeof body === 'string' ? body : JSON.stringify(body)) })
    .then(async (r) => ({ status: r.status, json: await r.json().catch(() => null) }));

test('без токена на сервере приём заказов выключен', async () => {
  const r = await call(off, 'POST', '/api/orders', { order_id: 'A-1', items: [{ sku: 'X', qty: 1 }] });
  assert.equal(r.status, 503); assert.match(r.json.error, /SITE_ORDERS_TOKEN/);
});

test('токен: нет, неверный и верный', async () => {
  const body = { order_id: 'T-1', items: [{ sku: 'X-1', qty: 1 }] };
  assert.equal((await call(site, 'POST', '/api/orders', body, {})).status, 401);
  assert.equal((await call(site, 'POST', '/api/orders', body, { authorization: 'Bearer wrong' })).status, 401);
  assert.equal((await call(site, 'POST', '/api/orders', body, { 'x-orders-token': TOKEN })).status, 201, 'токен можно и в заголовке X-Orders-Token');
  assert.equal((await call(site, 'POST', '/api/orders', { ...body, order_id: 'T-2' })).status, 201);
});

test('заказ попадает в очередь со складом позиций', async () => {
  const r = await call(site, 'POST', '/api/orders', { order_id: 'A-1001', items: [{ sku: 'DD1391-100_42', qty: 1 }, { barcode: '2000000000017', qty: 2 }, { sku: 'dd1391-100_42', qty: 1, name: 'Nike' }] });
  assert.equal(r.status, 201); assert.deepEqual(r.json, { ok: true, order_id: 'A-1001', status: 'queued' });
  const row = (await q("SELECT source, account_id, state, lines FROM order_writeoffs WHERE order_ref = 'A-1001'"))[0];
  assert.equal(row.source, 'site'); assert.equal(row.account_id, 0); assert.equal(row.state, 'pending');
  assert.equal(row.lines.length, 2, 'одинаковые артикулы (без учёта регистра) складываются');
  assert.deepEqual(row.lines.map((l) => [l.key, l.qty]).sort(), [['2000000000017', 2], ['DD1391-100_42', 2]]);
  assert.equal(row.lines.find((l) => l.qty === 2 && l.name === 'Nike')?.key, 'DD1391-100_42', 'название сохраняется');
});

test('повтор того же заказа безопасен, другой состав под тем же номером - 409', async () => {
  const body = { order_id: 'R-1', items: [{ sku: 'A', qty: 1 }, { sku: 'B', qty: 3 }] };
  assert.equal((await call(site, 'POST', '/api/orders', body)).status, 201);
  const again = await call(site, 'POST', '/api/orders', { order_id: 'R-1', items: [{ sku: 'b', qty: 3 }, { sku: 'A', qty: 1 }] });
  assert.equal(again.status, 200); assert.equal(again.json.status, 'exists'); assert.equal(again.json.state, 'queued');
  assert.equal((await q("SELECT count(*)::int AS n FROM order_writeoffs WHERE order_ref = 'R-1'"))[0].n, 1, 'в очереди один заказ');
  const other = await call(site, 'POST', '/api/orders', { order_id: 'R-1', items: [{ sku: 'A', qty: 2 }] });
  assert.equal(other.status, 409); assert.match(other.json.error, /другим составом/);
});

test('проверка данных: понятные ошибки 400, в очередь ничего не попадает', async () => {
  const bad = async (body, re) => {
    const r = await call(site, 'POST', '/api/orders', body);
    assert.equal(r.status, 400, JSON.stringify(body)); assert.match(r.json.error, re);
  };
  await bad({ items: [{ sku: 'A', qty: 1 }] }, /order_id/);
  await bad({ order_id: 'bad id!', items: [{ sku: 'A', qty: 1 }] }, /order_id/);
  await bad({ order_id: 'V-1' }, /items/);
  await bad({ order_id: 'V-1', items: [] }, /items/);
  await bad({ order_id: 'V-1', items: [{ qty: 1 }] }, /sku/);
  await bad({ order_id: 'V-1', items: [{ sku: 'A', qty: 0 }] }, /qty/);
  await bad({ order_id: 'V-1', items: [{ sku: 'A', qty: 1.5 }] }, /qty/);
  await bad({ order_id: 'V-1', items: [{ sku: 'A', qty: 5000 }] }, /qty/);
  await bad({ order_id: 'V-1', items: Array.from({ length: 201 }, (_, i) => ({ sku: `S${i}`, qty: 1 })) }, /не больше 200/);
  await bad([1, 2], /JSON-объект/);
  const broken = await call(site, 'POST', '/api/orders', '{"order_id": ');
  assert.equal(broken.status, 400); assert.match(broken.json.error, /JSON/);
  assert.equal((await q("SELECT count(*)::int AS n FROM order_writeoffs WHERE order_ref = 'V-1'"))[0].n, 0);
  const ok = await call(site, 'POST', '/api/orders', { order_id: 'V-2', items: [{ sku: 'A', qty: '2' }] });
  assert.equal(ok.status, 201, 'количество строкой из цифр допустимо');
});

test('состояние заказа', async () => {
  await call(site, 'POST', '/api/orders', { order_id: 'G-1', items: [{ sku: 'A', qty: 1 }] });
  assert.deepEqual((await call(site, 'GET', '/api/orders/G-1')).json, { order_id: 'G-1', state: 'queued' });
  await site.db.query("UPDATE order_writeoffs SET state = 'written_off' WHERE order_ref = 'G-1'");
  assert.equal((await call(site, 'GET', '/api/orders/G-1')).json.state, 'written_off');
  assert.equal((await call(site, 'GET', '/api/orders/NOPE')).status, 404);
  assert.equal((await call(site, 'GET', '/api/orders/G-1', undefined, {})).status, 401, 'состояние тоже по токену');
});

test('отмена: до списания снимает с очереди, после списания просит вернуть', async () => {
  await call(site, 'POST', '/api/orders', { order_id: 'C-1', items: [{ sku: 'A', qty: 1 }] });
  const c1 = await call(site, 'POST', '/api/orders/C-1/cancel');
  assert.equal(c1.status, 200); assert.equal(c1.json.state, 'cancelled');
  assert.equal((await q("SELECT state FROM order_writeoffs WHERE order_ref = 'C-1'"))[0].state, 'cancelled');
  assert.equal((await call(site, 'POST', '/api/orders/C-1/cancel')).json.state, 'cancelled', 'повторная отмена безопасна');

  await call(site, 'POST', '/api/orders', { order_id: 'C-2', items: [{ sku: 'A', qty: 1 }] });
  await site.db.query("UPDATE order_writeoffs SET state = 'written_off' WHERE order_ref = 'C-2'");
  const c2 = await call(site, 'POST', '/api/orders/C-2/cancel');
  assert.equal(c2.status, 200); assert.equal(c2.json.state, 'cancel_requested'); assert.match(c2.json.note, /вернётся на склад/);
  assert.equal((await q("SELECT state FROM order_writeoffs WHERE order_ref = 'C-2'"))[0].state, 'cancel_requested');

  await call(site, 'POST', '/api/orders', { order_id: 'C-3', items: [{ sku: 'A', qty: 1 }] });
  await site.db.query("UPDATE order_writeoffs SET state = 'writing' WHERE order_ref = 'C-3'");
  const c3 = await call(site, 'POST', '/api/orders/C-3/cancel');
  assert.equal(c3.status, 409); assert.match(c3.json.error, /обрабатывается/);
  assert.equal((await q("SELECT state FROM order_writeoffs WHERE order_ref = 'C-3'"))[0].state, 'writing', 'во время списания заказ не трогаем');
  assert.equal((await call(site, 'POST', '/api/orders/NOPE/cancel')).status, 404);
});

test('неверные токены блокируют адрес, верный токен после этого тоже ждёт', async () => {
  const s2 = await startSite({ env: { SITE_ORDERS_TOKEN: TOKEN } });
  try {
    for (let i = 0; i < 20; i++) assert.equal((await call(s2, 'POST', '/api/orders', {}, { authorization: 'Bearer nope' })).status, 401);
    const blocked = await call(s2, 'POST', '/api/orders', { order_id: 'L-1', items: [{ sku: 'A', qty: 1 }] });
    assert.equal(blocked.status, 429);
  } finally { await s2.stop(); }
});

test('слишком короткий токен в настройках - сайт не запускается', async () => {
  const { load } = require('../src/config');
  assert.throws(() => load({ SESSION_SECRET: 'x'.repeat(40), SITE_ORDERS_TOKEN: 'short' }), /SITE_ORDERS_TOKEN/);
  assert.equal(load({ SESSION_SECRET: 'x'.repeat(40) }).siteOrdersToken, '');
});

test('заказы сайта не видны и не нужны вошедшим пользователям: обычные страницы работают как раньше', async () => {
  await site.addUser('boss', 'bosspass1', true);
  const c = site.client(); await c.login('boss', 'bosspass1');
  assert.equal((await c.get('/stock')).status, 200);
});
