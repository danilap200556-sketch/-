'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('http');
const sharp = require('sharp');
const xlsx = require('../src/xlsx');
const { addDays } = require('../src/market');
const { startSite } = require('./helpers');

// ---- Поддельный Маркет: те же адреса и формы ответов, что в спецификации API
const KEY = 'test-key';
const mk = (orderId, campaignId, status, substatus, items, ship) => ({
  orderId, campaignId, status, substatus, creationDate: '2026-09-23T10:15:00+03:00',
  items: items.map(([offerId, offerName, count], i) => ({ id: i + 1, offerId, offerName, count })),
  delivery: { serviceName: 'Маркет Курьер', shipment: { id: 1, shipmentDate: ship } },
});
const ORDERS = [
  mk(1001, 222, 'PROCESSING', 'STARTED', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 1], ['MKT-NB', 'NB 373 43', 2]], '2026-09-25'),
  mk(1002, 222, 'PROCESSING', 'READY_TO_SHIP', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 2]], '2026-09-25'),
  mk(1003, 224, 'PROCESSING', 'STARTED', [['UNKNOWN-SKU', 'Чего-то нет у нас', 1]], '2026-09-26'),
  mk(2001, 225, 'PROCESSING', 'STARTED', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 3]], '2026-09-25'),
  mk(3001, 222, 'PROCESSING', 'STARTED', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 1]], '2026-09-27'),
  mk(3003, 222, 'DELIVERY', 'DELIVERY_SERVICE_RECEIVED', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 1]], '2026-09-25'),
  ...Array.from({ length: 1100 }, (_, i) => mk(7000 + i, 226, 'PROCESSING', 'STARTED', [[`BULK-${i}`, `Bulk ${i}`, 1]], '2026-09-30')),
];
const BUSINESS = { 222: 111, 224: 111, 225: 112, 226: 113 };

function startFakeMarket() {
  const state = { requests: [], files: [], reports: new Map(), failLabels: null };
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      const body = chunks.length ? JSON.parse(Buffer.concat(chunks).toString()) : null;
      const send = (code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); };
      if (url.pathname.startsWith('/files/')) {
        state.files.push({ path: url.pathname, apiKey: req.headers['api-key'] ?? null, host: req.headers.host });
        const r = state.reports.get(url.pathname.split('/')[2].replace('.pdf', ''));
        res.writeHead(200, { 'Content-Type': 'application/pdf' });
        return res.end(`%PDF-1.4\n% format=${r.format} orders=${r.ids.join(',')}\n`);
      }
      state.requests.push({ method: req.method, path: url.pathname, query: Object.fromEntries(url.searchParams), body, apiKey: req.headers['api-key'] });
      if (req.headers['api-key'] !== KEY) return send(401, { status: 'ERROR', errors: [{ code: 'UNAUTHORIZED', message: 'bad key' }] });
      let m;
      if ((m = /^\/v1\/businesses\/(\d+)\/orders$/.exec(url.pathname)) && req.method === 'POST') {
        const biz = Number(m[1]);
        const d = body.dates;
        let hi = d?.shipmentDateTo;
        if (d && (!hi || (new Date(hi) - new Date(d.shipmentDateFrom)) < 86_400_000)) hi = addDays(d.shipmentDateFrom, 1);
        const list = ORDERS.filter((o) => BUSINESS[o.campaignId] === biz
          && (!body.campaignIds || body.campaignIds.includes(o.campaignId))
          && (!body.statuses || body.statuses.includes(o.status))
          && (!body.substatuses || body.substatuses.includes(o.substatus))
          && (!d || (o.delivery.shipment.shipmentDate >= d.shipmentDateFrom && o.delivery.shipment.shipmentDate < hi)));
        const start = Number(url.searchParams.get('page_token') || 0);
        const limit = Math.min(Number(url.searchParams.get('limit') || 50), 50);
        const chunk = list.slice(start, start + limit);
        return send(200, { orders: chunk, paging: start + limit < list.length ? { nextPageToken: String(start + limit) } : {} });
      }
      if (url.pathname === '/v2/reports/documents/labels/generate') {
        if (state.failLabels === 'NO_DATA') { const id = `r${state.reports.size + 1}`; state.reports.set(id, { fail: 'NO_DATA', polls: 0 }); return send(200, { status: 'OK', result: { reportId: id, estimatedGenerationTime: 1 } }); }
        const id = `r${state.reports.size + 1}`;
        state.reports.set(id, { ids: body.orderIds, format: url.searchParams.get('format'), polls: 0 });
        return send(200, { status: 'OK', result: { reportId: id, estimatedGenerationTime: 1 } });
      }
      if ((m = /^\/v2\/reports\/info\/(.+)$/.exec(url.pathname))) {
        const r = state.reports.get(m[1]);
        if (++r.polls < 2) return send(200, { status: 'OK', result: { status: 'PROCESSING' } });
        if (r.fail) return send(200, { status: 'OK', result: { status: 'FAILED', subStatus: r.fail } });
        // ссылка на файл ведёт на другое имя хоста (localhost), чем адрес API (127.0.0.1)
        return send(200, { status: 'OK', result: { status: 'DONE', file: `http://localhost:${server.address().port}/files/${m[1]}.pdf` } });
      }
      return send(404, { status: 'ERROR', errors: [{ code: 'NOT_FOUND', message: url.pathname }] });
    });
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({ server, state, url: `http://127.0.0.1:${server.address().port}` })));
}

let site; let market; let staff; let viewer; let admin; let ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;

test.before(async () => {
  market = await startFakeMarket();
  process.env.MARKET_API_BASE_URL = market.url;
  site = await startSite();
  await site.addUser('boss', 'bosspass1', true); await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  admin = site.client(); staff = site.client(); viewer = site.client();
  await admin.login('boss', 'bosspass1'); await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад Б')");
  for (const w of await q('SELECT id, name FROM warehouses')) ids[w.name] = w.id;
  await site.db.query("INSERT INTO locations (warehouse_id, code) VALUES ($1, 'A1-03')", [ids['Склад А']]);
  await site.db.query("INSERT INTO products (sku, name, price, market_sku) VALUES ('NK-AF1-42', 'Nike AF1 42', 100, ''), ('NB-373-43', 'NB 373 43', 250, 'MKT-NB')");
  await site.db.query("INSERT INTO stock (product_id, warehouse_id, quantity, location_id) SELECT p.id, $1, 6, (SELECT id FROM locations LIMIT 1) FROM products p WHERE p.sku = 'NK-AF1-42'", [ids['Склад А']]);
  await site.db.query("INSERT INTO stock (product_id, warehouse_id, quantity) SELECT p.id, $1, 2 FROM products p WHERE p.sku = 'NK-AF1-42'", [ids['Склад Б']]);
  await site.db.query("INSERT INTO stock (product_id, warehouse_id, quantity) SELECT p.id, $1, 4 FROM products p WHERE p.sku = 'NB-373-43'", [ids['Склад А']]);
  await site.sys(`INSERT INTO market_accounts (name, api_key, business_id, campaign_id) VALUES
    ('Магазин 1', '${KEY}', 111, 222), ('Магазин 2', '${KEY}', 111, 224), ('Другой кабинет', '${KEY}', 112, 225), ('Большой', '${KEY}', 113, 226)`);
  const photo = await sharp({ create: { width: 50, height: 50, channels: 3, background: '#ff0000' } }).jpeg().toBuffer();
  await site.db.query("INSERT INTO product_photos (product_id, data, thumb) SELECT id, $1, $1 FROM products WHERE sku = 'NK-AF1-42'", [photo]);
});
test.after(async () => { await site.stop(); await new Promise((r) => market.server.close(r)); delete process.env.MARKET_API_BASE_URL; });

const page = async (client, params) => (await client.get(`/orders?${new URLSearchParams({ go: '1', ...params }).toString()}`));
const orderNumbers = (html) => [...new Set([...html.matchAll(/<tr><td>(?:<a [^>]*>.*?<\/a>)?<\/td><td>[^<]*<\/td><td>(\d+)<\/td>/g)].map((m) => Number(m[1])))].sort((a, b) => a - b);
const lastOrdersRequest = () => market.state.requests.filter((r) => r.path.endsWith('/orders')).at(-1);

test('заказы на дату отгрузки: фильтр уходит в API (дата "по" - следующий день), список только нужной даты', async () => {
  market.state.requests.length = 0;
  const r = await page(staff, { acc: '', status: '0', from: '2026-09-25', to: '2026-09-25' });
  assert.equal(r.status, 200);
  assert.deepEqual(orderNumbers(r.text), [1001, 1002, 2001], '25.09 в обработке: 3 заказа (доставка 3003 и другие даты не входят)');
  const sent = market.state.requests.filter((x) => x.path.endsWith('/orders'));
  assert.ok(sent.length >= 2, 'запросы по каждому кабинету (бизнесу)');
  for (const s of sent) {
    assert.equal(s.apiKey, KEY);
    assert.deepEqual(s.body.dates, { shipmentDateFrom: '2026-09-25', shipmentDateTo: '2026-09-26' });
    assert.equal(s.body.fake, false); assert.deepEqual(s.body.statuses, ['PROCESSING']);
  }
  assert.match(r.text, /Заказов: <b>3<\/b>, товаров: <b>8<\/b> шт/);
  assert.match(r.text, /Отгрузка 25\.09\.2026/);
  // диапазон
  assert.deepEqual(orderNumbers((await page(staff, { from: '2026-09-25', to: '2026-09-26' })).text), [1001, 1002, 1003, 2001]);
  assert.deepEqual(lastOrdersRequest().body.dates, { shipmentDateFrom: '2026-09-25', shipmentDateTo: '2026-09-27' });
  // только "с" -> один день
  assert.deepEqual(orderNumbers((await page(staff, { from: '2026-09-27' })).text), [3001]);
  // кабинет и статус
  assert.deepEqual(orderNumbers((await page(staff, { acc: String((await q("SELECT id FROM market_accounts WHERE name = 'Магазин 1'"))[0].id), from: '2026-09-25', to: '2026-09-26' })).text), [1001, 1002]);
  assert.equal(lastOrdersRequest().body.campaignIds.join(), '222', 'для кабинета - только его магазин');
  assert.deepEqual(orderNumbers((await page(staff, { status: '1', from: '2026-09-25', to: '2026-09-25' })).text), [1002]);
  assert.deepEqual(lastOrdersRequest().body.substatuses, ['READY_TO_SHIP']);
  assert.deepEqual(orderNumbers((await page(staff, { status: '3', from: '2026-09-25', to: '2026-09-25' })).text), [3003]);
  // без дат - все PROCESSING без фильтра по дате
  const all = await page(staff, { status: '0', acc: String((await q("SELECT id FROM market_accounts WHERE name = 'Магазин 1'"))[0].id) });
  assert.equal(lastOrdersRequest().body.dates, undefined);
  assert.deepEqual(orderNumbers(all.text), [1001, 1002, 3001]);
});

test('заказы: где лежит товар, миниатюры, товары без остатка, ключ нигде не показывается', async () => {
  const r = await page(staff, { from: '2026-09-25', to: '2026-09-26' });
  assert.match(r.text, /Склад А: A1-03 \(6 шт\); Склад Б \(2 шт\)/);
  assert.match(r.text, /Склад А \(4 шт\)/, 'по артикулу на Маркете (MKT-NB)');
  assert.match(r.text, /нет в наличии у нас/);
  assert.match(r.text, /<img class="thumb" src="\/photos\/\d+\/thumb"/, 'миниатюра товара с фото');
  assert.ok(!r.text.includes(KEY), 'ключ не попадает в страницу');
  for (const url of ['/orders', '/stock', '/users', '/import']) assert.ok(!(await admin.get(url)).text.includes(KEY), url);
  assert.match(r.text, /Магазин 1/); assert.match(r.text, /Другой кабинет/);
});

test('сайт может только читать кабинеты Маркета (минимальные права БД)', { skip: !process.env.WEB_TEST_RESTRICTED }, async () => {
  assert.ok((await site.db.query('SELECT api_key FROM market_accounts')).rowCount > 0);
  for (const sql of ["UPDATE market_accounts SET name = 'x'", "DELETE FROM market_accounts", "INSERT INTO market_accounts (name, api_key, business_id, campaign_id) VALUES ('a','b',1,2)"]) {
    await assert.rejects(site.db.query(sql), /permission denied/, sql);
  }
  await assert.rejects(site.db.query('SELECT * FROM market_account_warehouses'), /permission denied/);
});

test('заказы: проверка дат, ошибки Маркета, права', async () => {
  assert.equal((await page(staff, { from: '2026-09-26', to: '2026-09-25' })).status, 400);
  assert.match((await page(staff, { from: '2026-09-26', to: '2026-09-25' })).text, /раньше/);
  assert.equal((await page(staff, { from: '2026-08-01', to: '2026-09-25' })).status, 400);
  assert.match((await page(staff, { from: '2026-08-01', to: '2026-09-25' })).text, /30 дней/);
  const bad = await page(staff, { from: '2026-02-31', to: 'мусор' });
  assert.equal(bad.status, 200);
  assert.equal(lastOrdersRequest().body.dates, undefined, 'невалидные даты игнорируются, а не уходят в API');
  // неверный ключ -> понятное сообщение, сайт не падает
  await site.sys("UPDATE market_accounts SET api_key = 'wrong' WHERE name = 'Другой кабинет'");
  const r = await page(staff, { from: '2026-09-25', to: '2026-09-25' });
  assert.equal(r.status, 200);
  assert.match(r.text, /Другой кабинет: HTTP 401 - неверный или отозванный API-ключ/);
  assert.deepEqual(orderNumbers(r.text), [1001, 1002], 'остальные кабинеты загрузились');
  assert.ok(!r.text.includes('wrong'));
  await site.sys(`UPDATE market_accounts SET api_key = '${KEY}' WHERE name = 'Другой кабинет'`);
  // права
  assert.equal((await viewer.get('/orders')).status, 302);
  assert.equal((await viewer.get('/orders/export.xlsx?r=x')).status, 302);
  assert.equal((await site.client().get('/orders')).status, 302);
  assert.ok(!(await viewer.get('/stock')).text.includes('href="/orders"'));
  assert.ok((await staff.get('/stock')).text.includes('href="/orders"'));
  // нет кабинетов
  const saved = await q('SELECT * FROM market_accounts');
  await site.sys('DELETE FROM market_accounts');
  assert.match((await page(staff, { from: '2026-09-25' })).text, /не настроены/);
  for (const a of saved) await site.sys('INSERT INTO market_accounts (name, api_key, business_id, campaign_id, warehouse_groups, market_warehouse_id) VALUES ($1,$2,$3,$4,$5,$6)', [a.name, a.api_key, a.business_id, a.campaign_id, a.warehouse_groups, a.market_warehouse_id]);
});

test('заказы: список в Excel (заказы + сборка)', async () => {
  const r = await page(staff, { from: '2026-09-25', to: '2026-09-25' });
  const rid = /\/orders\/export\.xlsx\?r=([0-9a-f]+)/.exec(r.text)[1];
  const x = await staff.get(`/orders/export.xlsx?r=${rid}`);
  assert.equal(x.status, 200);
  assert.match(x.headers.get('content-disposition'), /attachment/);
  const t = xlsx.readFirstSheet(x.raw);
  assert.deepEqual(t.rows[0], ['Кабинет', '№ заказа', 'Статус', 'Оформлен', 'Отгрузка', 'Служба доставки', 'Артикул', 'Товар', 'Кол-во', 'Где лежит у нас']);
  assert.equal(t.rows.length, 1 + 4, '3 заказа, 4 товарные строки');
  const row = t.rows.find((x1) => x1[1] === '1001' && x1[6] === 'NK-AF1-42');
  assert.deepEqual([row[0], row[4], row[8], row[9]], ['Магазин 1', '25.09.2026', '1', 'Склад А: A1-03 (6 шт); Склад Б (2 шт)']);
  // второй лист «Сборка»: артикулы просуммированы
  const { unzipSync, strFromU8 } = require('fflate');
  const sheet2 = strFromU8(unzipSync(new Uint8Array(x.raw))['xl/worksheets/sheet2.xml']);
  assert.match(sheet2, />NK-AF1-42</); assert.match(sheet2, /NK-AF1-42<\/t><\/is><\/c><c r="B3"[^>]*><is><t[^>]*>[^<]*<\/t><\/is><\/c><c r="C3"><v>6<\/v><\/c>/, '1+2+3 = 6 шт NK-AF1-42');
  // чужой список недоступен
  assert.equal((await admin.get(`/orders/export.xlsx?r=${rid}`)).status, 302);
  assert.equal((await staff.get('/orders/export.xlsx?r=deadbeef')).status, 302);
});

test('ярлыки: PDF на кабинет, формат, имя файла с датой отгрузки, ключ не уходит на сторонний хост', async () => {
  market.state.requests.length = 0; market.state.files.length = 0;
  const r = await page(staff, { from: '2026-09-25', to: '2026-09-25' });
  const rid = /name="r" value="([0-9a-f]+)"/.exec(r.text)[1];
  const groups = [...r.text.matchAll(/name="g" value="(\d+)"/g)].map((m) => m[1]);
  assert.equal(groups.length, 2, 'два кабинета (бизнеса) -> две кнопки');
  const out = [];
  for (const g of groups) {
    const res = await staff.req('POST', '/orders/labels', { form: { _csrf: await staff.csrf('/orders'), r: rid, g, part: '1', format: 'A9_HORIZONTALLY' }, headers: { 'x-requested-with': 'fetch' } });
    assert.equal(res.status, 200, res.text.slice(0, 200));
    assert.equal(res.headers.get('content-type'), 'application/pdf');
    assert.ok(res.text.startsWith('%PDF'));
    const name = decodeURIComponent(/filename\*=UTF-8''([^;]+)/.exec(res.headers.get('content-disposition'))[1]);
    out.push({ name, text: res.text });
  }
  assert.ok(out.every((o) => /^Ярлыки_.+_отгрузка_2026-09-25\.pdf$/.test(o.name)), out.map((o) => o.name).join(' | '));
  assert.deepEqual(out.map((o) => /orders=([\d,]+)/.exec(o.text)[1]).sort(), ['1001,1002', '2001']);
  assert.ok(out.every((o) => o.text.includes('format=A9_HORIZONTALLY')));
  const gen = market.state.requests.filter((x) => x.path.endsWith('/generate'));
  assert.deepEqual(gen.map((x) => x.body.businessId).sort(), [111, 112]);
  assert.ok(gen.every((x) => x.apiKey === KEY && x.body.sortingType === 'SORT_BY_GIVEN_ORDER'));
  assert.equal(market.state.files.length, 2);
  assert.ok(market.state.files.every((f) => f.apiKey === null), 'Api-Key не отправлен на другой хост (localhost vs 127.0.0.1)');
  // без fetch-режима и без токена
  assert.equal((await staff.req('POST', '/orders/labels', { form: { r: rid, g: '0' } })).status, 403);
  assert.equal((await viewer.req('POST', '/orders/labels', { form: { _csrf: await viewer.csrf('/stock'), r: rid, g: '0' } })).status, 302);
  assert.equal((await admin.req('POST', '/orders/labels', { form: { _csrf: await admin.csrf('/orders'), r: rid, g: '0' }, headers: { 'x-requested-with': 'fetch' } })).status, 410, 'чужой список');
  // неизвестный формат заменяется безопасным значением
  const odd = await staff.req('POST', '/orders/labels', { form: { _csrf: await staff.csrf('/orders'), r: rid, g: groups[0], format: 'EVIL&x=1' }, headers: { 'x-requested-with': 'fetch' } });
  assert.match(odd.text, /format=A7/);
});

test('ярлыки: больше 1000 заказов - несколько файлов; ошибки Маркета читаемы', async () => {
  const r = await page(staff, { acc: String((await q("SELECT id FROM market_accounts WHERE name = 'Большой'"))[0].id) });
  assert.match(r.text, /Заказов: <b>1100<\/b>/);
  const rid = /name="r" value="([0-9a-f]+)"/.exec(r.text)[1];
  const parts = [...r.text.matchAll(/name="part" value="(\d+)"/g)].map((m) => m[1]);
  assert.deepEqual(parts, ['1', '2']);
  market.state.requests.length = 0;
  const sizes = [];
  for (const part of parts) {
    const res = await staff.req('POST', '/orders/labels', { form: { _csrf: await staff.csrf('/orders'), r: rid, g: '0', part, format: 'A7' }, headers: { 'x-requested-with': 'fetch' } });
    assert.equal(res.status, 200);
    sizes.push(/orders=([\d,]+)/.exec(res.text)[1].split(',').length);
    assert.match(decodeURIComponent(/filename\*=UTF-8''([^;]+)/.exec(res.headers.get('content-disposition'))[1]), new RegExp(`_часть${part}\\.pdf$`));
  }
  assert.deepEqual(sizes, [1000, 100]);
  // постраничная загрузка 1100 заказов: 22 страницы по 50
  assert.equal((await page(staff, { acc: String((await q("SELECT id FROM market_accounts WHERE name = 'Большой'"))[0].id) })).status, 200);
  // Маркет: нет данных для ярлыков
  market.state.failLabels = 'NO_DATA';
  const bad = await staff.req('POST', '/orders/labels', { form: { _csrf: await staff.csrf('/orders'), r: rid, g: '0', part: '1', format: 'A7' }, headers: { 'x-requested-with': 'fetch' } });
  assert.equal(bad.status, 502);
  assert.match(JSON.parse(bad.text).error, /Большой: для этих заказов ярлыков нет/);
  market.state.failLabels = null;
  // без JS - сообщение на странице заказов
  const noJs = await staff.req('POST', '/orders/labels', { form: { _csrf: await staff.csrf('/orders'), r: 'nope', g: '0' } });
  assert.equal(noJs.status, 302);
  assert.match(decodeURIComponent((await staff.follow(noJs)).text), /Список устарел/);
});
