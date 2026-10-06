'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { startSite } = require('./helpers');

let site;
let admin; // клиент-администратор
test.before(async () => {
  site = await startSite();
  await site.addUser('boss', 'bosspass1', true);
  await site.addUser('staff', 'staffpass1', false);
  admin = site.client();
  const r = await admin.login('boss', 'bosspass1');
  assert.equal(r.status, 302);
});
test.after(() => site.stop());

const flashOf = async (c, r) => (await c.follow(r)).text;
const ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;

test('доступ: без входа - на страницу входа; заголовки безопасности', async () => {
  const c = site.client();
  const r = await c.get('/stock');
  assert.equal(r.status, 302);
  assert.match(r.location, /^\/login\?next=%2Fstock/);
  const login = await c.get('/login');
  assert.equal(login.status, 200);
  assert.match(login.headers.get('content-security-policy'), /script-src 'self'/);
  assert.equal(login.headers.get('x-frame-options'), 'DENY');
  assert.equal((await c.get('/healthz')).text, 'ok');
  assert.equal((await c.post('/products', { sku: 'X', name: 'Y' }, { csrf: false })).status, 302); // без входа POST не проходит
});

test('вход: неверный пароль, несуществующий логин, редирект next, защита от open redirect', async () => {
  const c = site.client();
  assert.equal((await c.login('boss', 'wrong')).status, 401);
  assert.equal((await c.login('nobody', 'x')).status, 401);
  const ok = await c.req('POST', '/login', { form: { username: 'boss', password: 'bosspass1', next: '/products' } });
  assert.equal(ok.location, '/products');
  const c2 = site.client();
  const evil = await c2.req('POST', '/login', { form: { username: 'boss', password: 'bosspass1', next: '//evil.com' } });
  assert.equal(evil.location, '/');
  const evil2 = await site.client().req('POST', '/login', { form: { username: 'boss', password: 'bosspass1', next: 'https://evil.com' } });
  assert.equal(evil2.location, '/');
});

test('вход: после 5 неудач логин блокируется на время окна, даже с верным паролем', async () => {
  await site.addUser('victim', 'victimpass', false);
  const c = site.client();
  for (let i = 0; i < 5; i++) assert.equal((await c.login('victim', 'bad' + i)).status, 401);
  assert.equal((await c.login('victim', 'victimpass')).status, 429);
  // другой логин с того же IP не затронут
  assert.equal((await site.client().login('staff', 'staffpass1')).status, 302);
});

test('CSRF: без токена и с чужим токеном - 403; межсайтовый запрос - 403', async () => {
  assert.equal((await admin.post('/warehouses', { name: 'Z' }, { csrf: false })).status, 403);
  assert.equal((await admin.post('/warehouses', { name: 'Z' }, { csrf: 'bad' })).status, 403);
  const r = await admin.req('POST', '/warehouses', { form: { name: 'Z' }, headers: { 'sec-fetch-site': 'cross-site' } });
  assert.equal(r.status, 403);
  const r2 = await admin.req('POST', '/warehouses', { form: { name: 'Z' }, headers: { origin: 'https://evil.example' } });
  assert.equal(r2.status, 403);
  const login = await site.client().req('POST', '/login', { form: { username: 'boss', password: 'bosspass1' }, headers: { 'sec-fetch-site': 'cross-site' } });
  assert.equal(login.status, 403);
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM warehouses"))[0].n, 0);
});

test('склады и места хранения', async () => {
  let r = await admin.post('/warehouses', { name: 'Склад А', address: 'Москва' });
  assert.equal(r.status, 302);
  r = await admin.post('/warehouses', { name: 'Склад Б', address: '' });
  assert.equal((await admin.post('/warehouses', { name: 'Склад А' })).status, 409);
  assert.equal((await admin.post('/warehouses', { name: '   ' })).status, 400);
  const w = await q('SELECT id, name FROM warehouses ORDER BY name');
  ids.A = w[0].id; ids.B = w[1].id;
  assert.equal((await admin.post(`/warehouses/${ids.A}/locations`, { code: 'A1-03', description: 'полка' })).status, 302);
  assert.equal((await admin.post(`/warehouses/${ids.A}/locations`, { code: 'A1-03' })).status, 409);
  assert.equal((await admin.post(`/warehouses/${ids.B}/locations`, { code: 'B1' })).status, 302);
  ids.locA = (await q('SELECT id FROM locations WHERE warehouse_id = $1', [ids.A]))[0].id;
  ids.locB = (await q('SELECT id FROM locations WHERE warehouse_id = $1', [ids.B]))[0].id;
  assert.equal((await admin.post(`/warehouses/${ids.A}`, { name: 'Склад А (главный)', address: 'СПб' })).status, 302);
  assert.equal((await admin.post(`/warehouses/${ids.A}`, { name: 'Склад Б' })).status, 409);
  const page = await admin.get(`/warehouses/${ids.A}`);
  assert.match(page.text, /A1-03/);
  assert.match((await admin.get('/warehouses')).text, /Склад А \(главный\)/);
});

test('товары: создание, дубликат артикула, правка, валидация цены', async () => {
  let r = await admin.post('/products', { sku: 'NK-AF1-42', name: 'Nike AF1 42', price: '12 990,50'.replace(' ', ''), market_sku: '', custom_code: '', description: 'белые' });
  assert.equal(r.status, 302);
  ids.nk = Number(/\/products\/(\d+)/.exec(r.location)[1]);
  const row = (await q('SELECT * FROM products WHERE id = $1', [ids.nk]))[0];
  assert.equal(row.price, 12990.5); assert.equal(row.photo_path, ''); assert.equal(row.market_sku, '');
  assert.equal((await admin.post('/products', { sku: 'NK-AF1-42', name: 'Дубль', price: '1' })).status, 409);
  assert.equal((await admin.post('/products', { sku: 'A', name: 'B', price: 'abc' })).status, 400);
  assert.equal((await admin.post('/products', { sku: '', name: 'B', price: '1' })).status, 400);
  r = await admin.post(`/products/${ids.nk}`, { sku: 'NK-AF1-42', name: 'Nike Air Force 1 (42)', price: '13000', market_sku: 'MKT-1', custom_code: 'QR1', description: '' });
  assert.equal(r.status, 302);
  const upd = (await q('SELECT name, market_sku, price FROM products WHERE id = $1', [ids.nk]))[0];
  assert.deepEqual(upd, { name: 'Nike Air Force 1 (42)', market_sku: 'MKT-1', price: 13000 });
  r = await admin.post('/products', { sku: 'AD-S-40', name: 'Adidas Samba 40', price: '9000' });
  ids.ad = Number(/\/products\/(\d+)/.exec(r.location)[1]);
  assert.equal((await admin.post(`/products/${ids.ad}`, { sku: 'NK-AF1-42', name: 'x', price: '1' })).status, 409);
  assert.equal((await admin.get('/products/abc')).status, 302);
  assert.equal((await admin.get('/products/999999')).status, 302);
});

test('XSS: название товара с HTML выводится экранированным', async () => {
  const r = await admin.post('/products', { sku: 'XSS-1', name: '<img src=x onerror=alert(1)>', price: '1', description: '"><script>x</script>' });
  const id = /\/products\/(\d+)/.exec(r.location)[1];
  for (const url of [`/products/${id}`, '/products?q=XSS', '/stock']) {
    const t = (await admin.get(url)).text;
    assert.ok(!t.includes('<img src=x'), url);
    assert.ok(!t.includes('<script>x'), url);
  }
  assert.match((await admin.get(`/products/${id}`)).text, /&lt;img src=x onerror=alert\(1\)&gt;/);
});

test('штрихкоды: добавление, неверная цифра, конфликт, генерация, удаление', async () => {
  let r = await admin.post(`/products/${ids.nk}/barcodes`, { barcode: '4006-381 333931' });
  assert.equal(r.status, 302);
  assert.deepEqual((await q('SELECT barcode FROM product_barcodes WHERE product_id = $1', [ids.nk])).map((x) => x.barcode), ['4006381333931']);
  r = await admin.post(`/products/${ids.nk}/barcodes`, { barcode: '4006381333932' });
  assert.equal(r.status, 400); assert.match(r.text, /контрольная/);
  r = await admin.post(`/products/${ids.ad}/barcodes`, { barcode: '4006381333931' });
  assert.equal(r.status, 409); assert.match(r.text, /уже привязан к товару NK-AF1-42/);
  assert.equal((await admin.post(`/products/${ids.nk}/barcodes`, { barcode: '4006381333931' })).status, 302); // повтор того же - не ошибка
  r = await admin.post(`/products/${ids.nk}/barcodes`, { generate: '1' });
  assert.equal(r.status, 302);
  const codes = (await q('SELECT barcode FROM product_barcodes WHERE product_id = $1 ORDER BY created_at', [ids.nk])).map((x) => x.barcode);
  assert.equal(codes.length, 2); assert.ok(codes[1].startsWith('2') && codes[1].length === 13);
  assert.equal((await admin.post(`/products/${ids.ad}/barcodes`, { barcode: '036000291452' })).status, 302);
  assert.equal((await admin.post(`/products/${ids.nk}/barcodes/delete`, { barcode: codes[1] })).status, 302);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_barcodes WHERE product_id = $1', [ids.nk]))[0].n, 1);
  assert.equal((await admin.post('/products/999999/barcodes', { barcode: '4006381333931' })).status, 302); // нет товара - без падения
});

test('поиск: по артикулу, названию, штрихкоду, спецсимволам', async () => {
  assert.match((await admin.get('/products?q=samba')).text, /AD-S-40/);
  assert.ok(!(await admin.get('/products?q=samba')).text.includes('NK-AF1-42'));
  assert.match((await admin.get('/products?q=4006381333931')).text, /NK-AF1-42/);   // сканер
  assert.match((await admin.get('/products?q=MKT-1')).text, /NK-AF1-42/);
  for (const bad of ["%", "_", "'; DROP TABLE products;--", "\\", "ё%ё"]) {
    const r = await admin.get('/products?q=' + encodeURIComponent(bad));
    assert.equal(r.status, 200, bad);
    assert.match(r.text, /Ничего не найдено/);
  }
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM products'))[0].n, 3);
});

test('остатки: приход, списание, нехватка, перемещение, инвентаризация + журнал', async () => {
  const op = (f) => admin.post('/operation', { product: 'NK-AF1-42', warehouse: String(ids.A), quantity: '1', comment: '', ...f }, { from: '/operation' });
  const qty = async (w) => (await q('SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2', [ids.nk, w]))[0]?.quantity;

  let r = await op({ kind: 'receipt', quantity: '10', comment: 'партия 1' });
  assert.equal(r.status, 302); assert.equal(await qty(ids.A), 10);
  r = await op({ kind: 'writeoff', quantity: '4' });
  assert.equal(await qty(ids.A), 6);
  r = await op({ kind: 'writeoff', quantity: '7' });
  assert.equal(r.status, 409); assert.match(r.text, /Недостаточно остатка: на складе 6 шт, требуется списать 7 шт/);
  assert.equal(await qty(ids.A), 6);
  r = await op({ kind: 'transfer', quantity: '2', dest: String(ids.B) });
  assert.equal(await qty(ids.A), 4); assert.equal(await qty(ids.B), 2);
  assert.equal((await op({ kind: 'transfer', quantity: '1', dest: String(ids.A) })).status, 409);
  r = await op({ kind: 'inventory', quantity: '9', comment: 'пересчёт' });
  assert.equal(await qty(ids.A), 9);
  r = await op({ kind: 'inventory', quantity: '9' }); // без расхождений - ничего не пишем
  const moves = await q('SELECT type, delta, comment, warehouse_id, related_warehouse_id FROM stock_movements WHERE product_id = $1 ORDER BY id', [ids.nk]);
  assert.deepEqual(moves.map((m) => [m.type, m.delta]), [['receipt', 10], ['writeoff', -4], ['transfer_out', -2], ['transfer_in', 2], ['inventory_adjust', 5]]);
  assert.equal(moves[2].related_warehouse_id, ids.B); assert.equal(moves[3].related_warehouse_id, ids.A);
  assert.equal(moves[4].comment, 'пересчёт (инвентаризация: было 4, стало 9)');
  // по штрихкоду и неизвестный товар
  r = await op({ kind: 'receipt', product: '4006-381333931', quantity: '1' });
  assert.equal(r.status, 302); assert.equal(await qty(ids.A), 10);
  assert.equal((await op({ kind: 'receipt', product: 'NOPE' })).status, 404);
  // мусор в количестве
  for (const bad of ['0', '-1', '1.5', 'abc', '', '99999999999', '1e3']) assert.equal((await op({ kind: 'receipt', quantity: bad })).status, 400, bad);
  assert.equal((await op({ kind: 'bogus' })).status, 400);
  assert.equal((await op({ kind: 'receipt', warehouse: '99999' })).status, 400);
});

test('остатки: параллельные списания не уводят остаток в минус', async () => {
  await admin.post('/operation', { kind: 'receipt', product: 'AD-S-40', warehouse: String(ids.A), quantity: '5', comment: '' }, { from: '/operation' });
  const token = await admin.csrf('/operation');
  const results = await Promise.all(Array.from({ length: 12 }, () =>
    admin.req('POST', '/operation', { form: { _csrf: token, kind: 'writeoff', product: 'AD-S-40', warehouse: String(ids.A), quantity: '1', comment: '' } })));
  assert.equal(results.filter((x) => x.status === 302).length, 5);
  assert.equal(results.filter((x) => x.status === 409).length, 7);
  assert.equal((await q('SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2', [ids.ad, ids.A]))[0].quantity, 0);
  const sum = (await q("SELECT SUM(delta)::int AS s FROM stock_movements WHERE product_id = $1", [ids.ad]))[0].s;
  assert.equal(sum, 0); // журнал сходится с остатком
});

test('страницы остатков, журнала, CSV', async () => {
  const stock = await admin.get('/stock');
  assert.match(stock.text, /NK-AF1-42/);
  assert.match((await admin.get(`/stock?w=${ids.B}`)).text, /NK-AF1-42/);
  assert.ok(!(await admin.get(`/stock?w=${ids.B}&q=samba`)).text.includes('AD-S-40'));
  assert.match((await admin.get('/movements?q=NK-AF1')).text, /Инвентаризация/);
  await admin.post('/products', { sku: '=CMD|x', name: '+SUM(1)', price: '1' });
  await admin.post('/operation', { kind: 'receipt', product: '=CMD|x', warehouse: String(ids.A), quantity: '1', comment: '' }, { from: '/operation' });
  const csv = await admin.get('/stock.csv');
  assert.equal(csv.status, 200);
  assert.ok(csv.text.startsWith('﻿Артикул;Название;Склад;Место хранения;Остаток'));
  assert.match(csv.text, /NK-AF1-42;/);
  assert.ok(csv.text.includes("'=CMD|x;'+SUM(1);"), 'формулы в CSV обезврежены: ' + csv.text);
  assert.equal(csv.headers.get('content-type'), 'text/csv; charset=utf-8');
});

test('место хранения: можно только место этого склада', async () => {
  const loc = async () => (await q('SELECT location_id FROM stock WHERE product_id = $1 AND warehouse_id = $2', [ids.nk, ids.A]))[0].location_id;
  assert.equal((await admin.post(`/products/${ids.nk}/location`, { warehouse: String(ids.A), location: String(ids.locA) }, { from: `/products/${ids.nk}` })).status, 302);
  assert.equal(await loc(), ids.locA);
  await admin.post(`/products/${ids.nk}/location`, { warehouse: String(ids.A), location: String(ids.locB) }, { from: `/products/${ids.nk}` }); // чужой склад
  assert.equal(await loc(), ids.locA);
  await admin.post(`/products/${ids.nk}/location`, { warehouse: String(ids.A), location: '' }, { from: `/products/${ids.nk}` });
  assert.equal(await loc(), null);
  // место на складе, где остатка ещё нет - строка stock создаётся с нулём
  await admin.post(`/products/${ids.ad}/location`, { warehouse: String(ids.B), location: String(ids.locB) }, { from: `/products/${ids.ad}` });
  assert.deepEqual((await q('SELECT quantity, location_id FROM stock WHERE product_id = $1 AND warehouse_id = $2', [ids.ad, ids.B]))[0], { quantity: 0, location_id: ids.locB });
  assert.match((await admin.get(`/products/${ids.nk}`)).text, /Остатки по складам/);
});

test('права: обычный пользователь не удаляет и не видит пользователей', async () => {
  const c = site.client();
  await c.login('staff', 'staffpass1');
  assert.equal((await c.get('/users')).status, 302);
  assert.ok(!(await c.get('/stock')).text.includes('href="/users"'));
  const before = (await q('SELECT COUNT(*)::int AS n FROM products'))[0].n;
  await c.post(`/products/${ids.ad}/delete`, {}, { from: '/stock' });
  await c.post(`/warehouses/${ids.B}/delete`, {}, { from: '/stock' });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM products'))[0].n, before);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM warehouses'))[0].n, 2);
  await c.post('/users', { username: 'hacker', password: 'hackerpass' }, { from: '/stock' });
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM users WHERE username = 'hacker'"))[0].n, 0);
  // но обычные операции доступны
  assert.equal((await c.post('/operation', { kind: 'receipt', product: 'NK-AF1-42', warehouse: String(ids.A), quantity: '1', comment: '' }, { from: '/operation' })).status, 302);
});

test('пользователи: создание, дубликат, админ-гард, пароль, удаление', async () => {
  let r = await admin.post('/users', { username: 'newbie', password: 'newbiepass', is_admin: '' }, { from: '/users' });
  assert.equal(r.status, 302);
  assert.equal((await admin.post('/users', { username: 'newbie', password: 'newbiepass2' }, { from: '/users' })).status, 409);
  assert.equal((await admin.post('/users', { username: 'weak', password: 'short' }, { from: '/users' })).status, 400);
  assert.equal((await admin.post('/users', { username: '', password: 'longenough' }, { from: '/users' })).status, 400);
  // новый пользователь может войти
  assert.equal((await site.client().login('newbie', 'newbiepass')).status, 302);
  const bossId = (await q("SELECT id FROM users WHERE username = 'boss'"))[0].id;
  const newbieId = (await q("SELECT id FROM users WHERE username = 'newbie'"))[0].id;
  // последнего админа нельзя ни разжаловать, ни удалить
  r = await admin.post(`/users/${bossId}/role`, { role: 'editor' }, { from: '/users' });
  assert.match(await flashOf(admin, r), /без администратора/);
  assert.equal((await q('SELECT is_admin FROM users WHERE id = $1', [bossId]))[0].is_admin, true);
  r = await admin.post(`/users/${bossId}/delete`, {}, { from: '/users' });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM users WHERE id = $1', [bossId]))[0].n, 1);
  // два админа: теперь можно разжаловать одного
  await admin.post(`/users/${newbieId}/role`, { role: 'admin' }, { from: '/users' });
  r = await admin.post(`/users/${newbieId}/role`, { role: 'editor' }, { from: '/users' });
  assert.equal((await q('SELECT is_admin FROM users WHERE id = $1', [newbieId]))[0].is_admin, false);
  // сброс пароля чужому пользователю -> старый не подходит, новый подходит
  await admin.post(`/users/${newbieId}/password`, { password: 'resetpass99' }, { from: '/users' });
  assert.equal((await site.client().login('newbie', 'newbiepass')).status, 401);
  assert.equal((await site.client().login('newbie', 'resetpass99')).status, 302);
  // удаление
  await admin.post(`/users/${newbieId}/delete`, {}, { from: '/users' });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM users WHERE id = $1', [newbieId]))[0].n, 0);
  // нельзя удалить себя
  await admin.post(`/users/${bossId}/delete`, {}, { from: '/users' });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM users WHERE id = $1', [bossId]))[0].n, 1);
});

test('смена пароля: сессии на других устройствах закрываются, текущая остаётся', async () => {
  await site.addUser('pwuser', 'oldpassword', false);
  const a = site.client(); const b = site.client();
  await a.login('pwuser', 'oldpassword'); await b.login('pwuser', 'oldpassword');
  assert.equal((await a.get('/stock')).status, 200);
  assert.equal((await a.post('/account/password', { current: 'WRONG', password: 'brandnew123', confirm: 'brandnew123' })).status, 400);
  assert.equal((await a.post('/account/password', { current: 'oldpassword', password: 'brandnew123', confirm: 'other' })).status, 400);
  assert.equal((await a.post('/account/password', { current: 'oldpassword', password: 'short', confirm: 'short' })).status, 400);
  assert.equal((await a.post('/account/password', { current: 'oldpassword', password: 'brandnew123', confirm: 'brandnew123' })).status, 302);
  assert.equal((await a.get('/stock')).status, 200);               // эта сессия продолжается
  assert.equal((await b.get('/stock')).status, 302);               // другая закрыта
  assert.equal((await site.client().login('pwuser', 'oldpassword')).status, 401);
  assert.equal((await site.client().login('pwuser', 'brandnew123')).status, 302);
});

test('сессия: удалённый пользователь теряет доступ сразу; подделанный cookie не работает', async () => {
  const id = await site.addUser('temp', 'temppass12', false);
  const c = site.client(); await c.login('temp', 'temppass12');
  assert.equal((await c.get('/stock')).status, 200);
  await site.db.query('DELETE FROM users WHERE id = $1', [id]);
  assert.equal((await c.get('/stock')).status, 302);
  const forged = site.client();
  forged.jar.set('session', 'eyJ1aWQiOjEsInYiOiJ4IiwiZXhwIjo5OTk5OTk5OTk5OTk5fQ.AAAA');
  assert.equal((await forged.get('/stock')).status, 302);
});

test('выход: cookie сбрасывается', async () => {
  const c = site.client(); await c.login('staff', 'staffpass1');
  assert.equal((await c.post('/logout', {}, { from: '/stock' })).status, 302);
  assert.equal((await c.get('/stock')).status, 302);
});

test('удаление товара и склада админом: каскад остатков и штрихкодов', async () => {
  const before = await q('SELECT COUNT(*)::int AS n FROM stock WHERE product_id = $1', [ids.nk]);
  assert.ok(before[0].n > 0);
  await admin.post(`/products/${ids.nk}/delete`, {}, { from: '/stock' });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock WHERE product_id = $1', [ids.nk]))[0].n, 0);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_barcodes WHERE product_id = $1', [ids.nk]))[0].n, 0);
  await admin.post(`/warehouses/${ids.B}/delete`, {}, { from: '/stock' });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM warehouses WHERE id = $1', [ids.B]))[0].n, 0);
  assert.equal((await admin.get('/stock')).status, 200);
});

test('404 и неожиданные пути', async () => {
  assert.equal((await admin.get('/nothing-here')).status, 404);
  assert.equal((await admin.get('/static/style.css')).status, 200);
  assert.equal((await admin.get('/static/../src/app.js')).status, 404);
});
