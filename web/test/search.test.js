'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { startSite } = require('./helpers');
const { startFakeMarket, KEY } = require('./fakemarket');

let site; let market; let staff; let viewer; let admin;
const ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const stockOf = async (sku, wh) => (await q('SELECT COALESCE((SELECT s.quantity FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id WHERE p.sku = $1 AND w.name = $2), 0)::int AS n', [sku, wh]))[0].n;

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
  await site.db.query("INSERT INTO locations (warehouse_id, code) VALUES ($1, 'A1-03'), ($1, 'A2-07'), ($2, 'B1')", [ids['Склад А'], ids['Склад Б']]);
  for (const l of await q('SELECT id, code FROM locations')) ids[l.code] = l.id;
  await site.db.query(`INSERT INTO products (sku, name, price, market_sku) VALUES
    ('NK-AF1-42', 'Nike AF1 42', 100, ''), ('NB-373-43', 'NB 373 43', 250, 'MKT-NB'), ('AD-S-40', 'Adidas Samba 40', 90, 'DUP'), ('AD-S-41', 'Adidas Samba 41', 95, 'DUP')`);
  for (const p of await q('SELECT id, sku FROM products')) ids[p.sku] = p.id;
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) VALUES ('4006381333931', $1), ('036000291452', $2)", [ids['NK-AF1-42'], ids['NB-373-43']]);
  await site.db.query('INSERT INTO stock (product_id, warehouse_id, quantity, location_id) VALUES ($1, $2, 5, $3), ($1, $4, 2, NULL), ($5, $2, 4, NULL)',
    [ids['NK-AF1-42'], ids['Склад А'], ids['A1-03'], ids['Склад Б'], ids['NB-373-43']]);
  await site.sys(`INSERT INTO market_accounts (name, api_key, business_id, campaign_id) VALUES
    ('Магазин 1', '${KEY}', 111, 222), ('Магазин 2', '${KEY}', 111, 224), ('Другой кабинет', '${KEY}', 112, 225)`);
});
test.after(async () => { await site.stop(); await new Promise((r) => market.server.close(r)); delete process.env.MARKET_API_BASE_URL; });

// ------------------------------------------------------------------ поиск везде
test('строка поиска в шапке есть на всех страницах после входа, но не на странице входа', async () => {
  for (const url of ['/stock', '/products', '/warehouses', '/movements', '/import', '/orders', '/users', '/account']) {
    assert.match((await admin.get(url)).text, /<form method="get" action="\/find" class="navsearch"/, url);
  }
  assert.ok(!(await site.client().get('/login')).text.includes('navsearch'));
  assert.match((await viewer.get('/stock')).text, /action="\/find"/, 'просмотр тоже ищет');
});

test('/find: штрихкод, артикул (любой регистр), артикул на Маркете, часть названия -> карточка', async () => {
  const to = async (t) => { const r = await staff.get(`/find?q=${encodeURIComponent(t)}`); return r.status === 302 ? r.location : `${r.status}`; };
  assert.equal(await to('4006381333931'), `/products/${ids['NK-AF1-42']}`);
  assert.equal(await to(' 4006-381 333931 '), `/products/${ids['NK-AF1-42']}`, 'пробелы и дефисы из сканера');
  assert.equal(await to('nk-af1-42'), `/products/${ids['NK-AF1-42']}`);
  assert.equal(await to('mkt-nb'), `/products/${ids['NB-373-43']}`);
  assert.equal(await to('036000291452'), `/products/${ids['NB-373-43']}`);
  assert.equal(await to('Nike'), `/products/${ids['NK-AF1-42']}`, 'единственное совпадение по названию');
  assert.equal(await to('40063813'), `/products/${ids['NK-AF1-42']}`, 'часть штрихкода');
  assert.equal(await to('Samba'), '/products?q=Samba', 'несколько -> список');
  assert.equal(await to('DUP'), '/products?q=DUP', 'один артикул Маркета у двух товаров -> список');
  assert.equal((await staff.get('/find?q=')).location, '/products');
  for (const bad of ['%', '_', "'; DROP TABLE products;--", '\\', 'a'.repeat(300)]) {
    const r = await staff.get(`/find?q=${encodeURIComponent(bad)}`);
    assert.ok(r.status === 404 || r.status === 302, `${bad.slice(0, 10)} -> ${r.status}`);
  }
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM products'))[0].n, 4);
  assert.equal((await site.client().get('/find?q=4006381333931')).status, 302, 'без входа - на страницу входа');
  assert.match((await site.client().get('/find?q=4006381333931')).location, /^\/login/);
});

test('/find: незнакомый штрихкод можно привязать к товару, повторный скан открывает его карточку', async () => {
  const code = '4600702084030';
  const r = await staff.get(`/find?q=${code}`);
  assert.equal(r.status, 404);
  assert.match(r.text, /Привязать штрихкод 4600702084030 к товару/);
  assert.ok(!r.text.includes('name="barcode" value="4600702084030"><button'), 'пока товар не выбран, кнопок «Привязать» нет');
  const found = await staff.get(`/find?q=${code}&p=samba`);
  assert.match(found.text, /AD-S-40/); assert.match(found.text, /AD-S-41/);
  const form = new RegExp(`action="/products/${ids['AD-S-41']}/barcodes"[\\s\\S]*?name="barcode" value="${code}"`);
  assert.match(found.text, form);
  assert.match((await staff.get(`/find?q=${code}&p=zzzz`)).text, /Такого товара нет/);
  const bind = await staff.post(`/products/${ids['AD-S-41']}/barcodes`, { barcode: code }, { from: `/find?q=${code}&p=samba` });
  assert.equal(bind.status, 302);
  assert.equal((await q('SELECT product_id FROM product_barcodes WHERE barcode = $1', [code]))[0].product_id, ids['AD-S-41']);
  assert.equal((await staff.get(`/find?q=${code}`)).location, `/products/${ids['AD-S-41']}`);
  // неверный штрихкод / просмотр: привязки нет
  assert.ok(!(await staff.get('/find?q=4006381333932')).text.includes('Привязать штрихкод'), 'неверная контрольная цифра');
  const v = await viewer.get('/find?q=5901234123457&p=nike');
  assert.equal(v.status, 404); assert.ok(!v.text.includes('Привязать штрихкод'));
});

test('операции: товар по штрихкоду/артикулу/артикулу Маркета - только точное совпадение', async () => {
  const op = (product, extra = {}) => staff.post('/operation', { kind: 'receipt', product, warehouse: String(ids['Склад А']), quantity: '1', comment: '', ...extra }, { from: '/operation' });
  const before = await stockOf('NB-373-43', 'Склад А');
  assert.equal((await op('036000291452')).status, 302);
  assert.equal((await op('mkt-nb')).status, 302, 'артикул Маркета, любой регистр');
  assert.equal((await op('nb-373-43')).status, 302, 'артикул, любой регистр');
  assert.equal(await stockOf('NB-373-43', 'Склад А'), before + 3);
  const partial = await op('NB 373');
  assert.equal(partial.status, 404); assert.match(partial.text, /нужен артикул или штрихкод целиком/);
  const dup = await op('DUP');
  assert.equal(dup.status, 404); assert.match(dup.text, /несколько товаров/);
  assert.equal(await stockOf('NB-373-43', 'Склад А'), before + 3, 'частичное совпадение ничего не списало и не добавило');
});

// ------------------------------------------------------------------ быстрая правка
const quick = (client, id, form, from) => client.post(`/products/${id}/quick`, form, { from: from || `/products/${id}/quick` });
const formFor = async (client, id) => {
  const html = (await client.get(`/products/${id}/quick`)).text;
  const form = {};
  for (const m of html.matchAll(/<input[^>]*name="([^"]+)"[^>]*value="([^"]*)"/g)) if (!['_csrf', 'rm', 'generate'].includes(m[1])) form[m[1]] = m[2].replace(/&quot;/g, '"').replace(/&amp;/g, '&');
  return { html, form };
};

test('быстрая правка: страница показывает всё нужное и защищена', async () => {
  const id = ids['NK-AF1-42'];
  const { html, form } = await formFor(staff, id);
  assert.equal(form.sku, 'NK-AF1-42'); assert.equal(form.name, 'Nike AF1 42'); assert.equal(form.price, '100');
  assert.equal(form[`qty_${ids['Склад А']}`], '5'); assert.equal(form[`was_${ids['Склад А']}`], '5'); assert.equal(form[`was_${ids['Склад Б']}`], '2');
  assert.match(html, /4006381333931/); assert.match(html, /A1-03/); assert.match(html, /A2-07/);
  assert.match(html, /Полная карточка/);
  assert.match((await staff.get(`/products/${id}/quick?back=${encodeURIComponent('/stock?q=nike')}`)).text, /name="back" value="\/stock\?q=nike"/);
  assert.match((await staff.get(`/products/${id}/quick?back=${encodeURIComponent('//evil.com')}`)).text, /name="back" value="\/products\/\d+"/, 'чужой адрес возврата отбрасывается');
  assert.match((await staff.get(`/products/${id}/quick?back=${encodeURIComponent('https://evil.com')}`)).text, /name="back" value="\/products\/\d+"/);
  assert.equal((await staff.get('/products/99999/quick')).status, 302);
  assert.equal((await staff.get('/products/abc/quick')).status, 302);
  assert.equal((await viewer.get(`/products/${id}/quick`)).location, '/', 'просмотр: страницы правки нет');
  assert.equal((await site.client().get(`/products/${id}/quick`)).status, 302);
  const before = (await q('SELECT name FROM products WHERE id = $1', [id]))[0].name;
  assert.equal((await viewer.post(`/products/${id}/quick`, { sku: 'X', name: 'Хак', price: '1' }, { from: '/stock' })).location, '/');
  assert.equal((await staff.req('POST', `/products/${id}/quick`, { form: { sku: 'X', name: 'Хак', price: '1' } })).status, 403, 'без CSRF-токена');
  assert.equal((await q('SELECT name FROM products WHERE id = $1', [id]))[0].name, before);
});

test('быстрая правка: сохраняет всё сразу - поля, штрихкоды, остатки, места; журнал и возврат назад', async () => {
  const id = ids['NK-AF1-42']; const A = ids['Склад А']; const B = ids['Склад Б'];
  const { form } = await formFor(staff, id);
  const res = await quick(staff, id, {
    ...form, back: '/stock?q=nike', name: 'Nike Air Force 1 (42) — белые', price: '12 990,5'.replace(' ', ''), market_sku: 'MKT-NK',
    add: '9780201379624, 5901234123457', rm: '4006381333931',
    [`qty_${A}`]: '8', [`loc_${A}`]: String(ids['A2-07']), [`loc_${B}`]: String(ids.B1), reason: 'пересчитали полку',
  });
  assert.equal(res.status, 302); assert.equal(res.location, '/stock?q=nike');
  assert.match(decodeURIComponent((await staff.follow(res)).text), /Сохранено: NK-AF1-42/);
  const p = (await q('SELECT name, price, market_sku FROM products WHERE id = $1', [id]))[0];
  assert.deepEqual([p.name, Number(p.price), p.market_sku], ['Nike Air Force 1 (42) — белые', 12990.5, 'MKT-NK']);
  assert.deepEqual((await q('SELECT barcode FROM product_barcodes WHERE product_id = $1 ORDER BY barcode', [id])).map((r) => r.barcode), ['5901234123457', '9780201379624']);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 8); assert.equal(await stockOf('NK-AF1-42', 'Склад Б'), 2);
  const st = await q('SELECT warehouse_id, location_id FROM stock WHERE product_id = $1 ORDER BY warehouse_id', [id]);
  assert.deepEqual(st.map((r) => [r.warehouse_id, r.location_id]), [[A, ids['A2-07']], [B, ids.B1]]);
  const mv = await q("SELECT type, delta, comment FROM stock_movements WHERE product_id = $1 AND comment LIKE '%инвентаризация%'", [id]);
  assert.deepEqual(mv, [{ type: 'inventory_adjust', delta: 3, comment: 'пересчитали полку (инвентаризация: было 5, стало 8)' }]);
  // ничего не меняли -> ни журнала, ни ошибок
  const again = await formFor(staff, id);
  const n0 = (await q('SELECT COUNT(*)::int AS n FROM stock_movements'))[0].n;
  assert.equal((await quick(staff, id, again.form)).status, 302);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_movements'))[0].n, n0, 'нетронутые остатки не пишутся в журнал');
  // свой штрихкод, пустой артикул Маркета, остаток в ноль
  const g = await formFor(staff, id);
  const r2 = await quick(staff, id, { ...g.form, market_sku: '', generate: '1', [`qty_${B}`]: '0' });
  assert.equal(r2.status, 302); assert.equal(await stockOf('NK-AF1-42', 'Склад Б'), 0);
  assert.equal((await q('SELECT market_sku FROM products WHERE id = $1', [id]))[0].market_sku, '');
  const codes = (await q('SELECT barcode FROM product_barcodes WHERE product_id = $1', [id])).map((r) => r.barcode);
  assert.equal(codes.length, 3); assert.ok(codes.some((c) => c.startsWith('2') && c.length === 13));
  assert.match(decodeURIComponent((await staff.follow(r2)).text), /Новый штрихкод 2\d{12}/);
  // по умолчанию возврат на карточку
  assert.equal((await quick(staff, id, (await formFor(staff, id)).form)).location, `/products/${id}`);
  // чужой адрес возврата не используется
  assert.equal((await quick(staff, id, { ...(await formFor(staff, id)).form, back: '//evil.com/x' })).location, `/products/${id}`);
});

test('быстрая правка: любая ошибка - ничего не сохранено, форма возвращается с введённым и понятным текстом', async () => {
  const id = ids['NK-AF1-42']; const A = ids['Склад А'];
  const snap = async () => JSON.stringify([await q('SELECT id, sku, name, price, market_sku FROM products ORDER BY id'), await q('SELECT * FROM stock ORDER BY product_id, warehouse_id'),
    await q('SELECT barcode, product_id FROM product_barcodes ORDER BY barcode'), (await q('SELECT COUNT(*)::int AS n FROM stock_movements'))[0].n]);
  const before = await snap();
  const base = (await formFor(staff, id)).form;
  const cases = [
    ['занятый артикул', { sku: 'NB-373-43' }, 409, /Товар с артикулом NB-373-43 уже есть/],
    ['штрихкод другого товара', { add: '036000291452' }, 409, /уже привязан к товару NB-373-43/],
    ['неверная контрольная цифра', { add: '4006381333932' }, 400, /контрольная/],
    ['мусор в штрихкоде', { add: '12ab' }, 400, /только из цифр/],
    ['пустое название', { name: '  ' }, 400, /Введите название/],
    ['пустой артикул', { sku: '' }, 400, /Введите артикул/],
    ['цена - не число', { price: 'abc' }, 400, /Цена/],
    ['отрицательный остаток', { [`qty_${A}`]: '-3' }, 400, /Склад А: Количество/],
    ['дробный остаток', { [`qty_${A}`]: '2.5' }, 400, /Склад А: Количество/],
    ['огромный остаток', { [`qty_${A}`]: '999999999999' }, 400, /Слишком большое|Склад А/],
    ['место чужого склада', { [`loc_${A}`]: String(ids.B1) }, 400, /другому складу/],
  ];
  for (const [label, patch, status, re] of cases) {
    // изменения в нескольких полях сразу: если хоть одно плохое - не должно сохраниться и "хорошее"
    const r = await quick(staff, id, { ...base, name: base.name === '  ' ? 'x' : 'ХОРОШЕЕ НАЗВАНИЕ', price: '1', [`qty_${A}`]: '9', ...patch });
    assert.equal(r.status, status, `${label}: ${r.status}`);
    assert.match(r.text, re, label);
    assert.equal(await snap(), before, `${label}: ничего не изменилось`);
  }
  // форма вернулась с тем, что вводили (и экранирована)
  const xss = await quick(staff, id, { ...base, name: '<script>alert(1)</script>"', sku: 'NB-373-43' });
  assert.equal(xss.status, 409); assert.ok(!xss.text.includes('<script>alert(1)'));
  assert.match(xss.text, /value="&lt;script&gt;alert\(1\)&lt;\/script&gt;&quot;"/);
  assert.equal(await snap(), before);
  // товар удалили, пока форма была открыта
  const tmp = (await q("INSERT INTO products (sku, name) VALUES ('TMP-1', 'Временный') RETURNING id"))[0].id;
  const tf = (await formFor(staff, tmp)).form;
  await site.db.query('DELETE FROM products WHERE id = $1', [tmp]);
  const gone = await quick(staff, tmp, tf, '/stock');
  assert.equal(gone.status, 302); assert.equal(gone.location, '/products');
  assert.match(decodeURIComponent((await staff.follow(gone)).text), /уже удалён|не найден/);
});

test('быстрая правка: чужие свежие изменения не затираются (остаток трогаем только если его меняли)', async () => {
  const id = ids['NB-373-43']; const A = ids['Склад А'];
  const { form } = await formFor(staff, id);
  await site.db.query('UPDATE stock SET quantity = 40 WHERE product_id = $1 AND warehouse_id = $2', [id, A]); // другой человек поправил
  const r = await quick(staff, id, { ...form, name: 'Только название' });
  assert.equal(r.status, 302);
  assert.equal(await stockOf('NB-373-43', 'Склад А'), 40, 'остаток не перезаписан старым значением из формы');
  const f2 = await formFor(staff, id);
  await quick(staff, id, { ...f2.form, [`qty_${A}`]: '41' });
  assert.equal(await stockOf('NB-373-43', 'Склад А'), 41);
});

test('быстрая правка: параллельные сохранения одной карточки не ломают остаток и журнал', async () => {
  const id = ids['AD-S-40']; const A = ids['Склад А'];
  const { form } = await formFor(staff, id);
  const token = await staff.csrf('/stock');
  const results = await Promise.all(Array.from({ length: 6 }, (_, i) => staff.req('POST', `/products/${id}/quick`, { form: { ...form, _csrf: token, [`qty_${A}`]: String(10 + i), reason: `p${i}` } })));
  assert.ok(results.every((r) => r.status === 302), results.map((r) => r.status).join());
  const final = await stockOf('AD-S-40', 'Склад А');
  assert.ok(final >= 10 && final <= 15);
  const sum = (await q('SELECT COALESCE(SUM(delta), 0)::int AS s FROM stock_movements WHERE product_id = $1', [id]))[0].s;
  assert.equal(sum, final, 'сумма движений = остаток');
});

test('ссылки «✎» быстрой правки есть в списках у сотрудника и нет у «только просмотр»', async () => {
  const id = ids['NK-AF1-42'];
  for (const [url, re] of [['/products?q=Nike', `/products/${id}/quick\\?back=%2Fproducts%3Fq%3DNike`], ['/products?view=cards', `/products/${id}/quick\\?back=`],
    ['/stock?q=Nike', `/products/${id}/quick\\?back=%2Fstock%3Fq%3DNike`], ['/movements', `/products/${id}/quick\\?back=%2Fmovements`]]) {
    assert.match((await staff.get(url)).text, new RegExp(re), url);
    assert.ok(!(await viewer.get(url)).text.includes('/quick'), `${url}: у просмотра ссылок нет`);
  }
  assert.match((await staff.get(`/products/${id}`)).text, /Быстрая правка: название, цена, штрихкоды, остатки/);
  assert.ok(!(await viewer.get(`/products/${id}`)).text.includes('Быстрая правка'));
});

// ------------------------------------------------------------------ поиск в заказах
const orderNumbers = (html) => [...new Set([...html.matchAll(/<tr><td>(?:<a [^>]*>.*?<\/a>(?: <a class="editlink"[^>]*>✎<\/a>)?)?<\/td><td>[^<]*<\/td><td>(\d+)<\/td>/g)].map((m) => Number(m[1])))].sort((a, b) => a - b);
const ordersPage = (client, params) => client.get(`/orders?${new URLSearchParams({ go: '1', from: '2026-09-25', to: '2026-09-26', ...params }).toString()}`);

test('заказы: поиск по штрихкоду, артикулу, названию, номеру; ярлыки и Excel - только по найденным', async () => {
  await site.db.query("UPDATE products SET market_sku = '' WHERE sku = 'NK-AF1-42'");
  const base = await ordersPage(staff, {});
  assert.deepEqual(orderNumbers(base.text), [1001, 1002, 1003, 2001]);
  const nbBarcode = await ordersPage(staff, { s: '036000291452' }); // NB, артикул на Маркете MKT-NB
  assert.deepEqual(orderNumbers(nbBarcode.text), [1001]);
  assert.match(nbBarcode.text, /Заказов: <b>1<\/b> из 4 загруженных \(поиск «036000291452»\)/);
  assert.match(nbBarcode.text, /NK-AF1-42/, 'заказ показывается целиком, со всеми товарами');
  assert.deepEqual(orderNumbers((await ordersPage(staff, { s: '9780201379624' })).text), [1001, 1002, 2001], 'штрихкод Nike');
  assert.deepEqual(orderNumbers((await ordersPage(staff, { s: '1003' })).text), [1003]);
  assert.deepEqual(orderNumbers((await ordersPage(staff, { s: 'UNKNOWN' })).text), [1003]);
  assert.deepEqual(orderNumbers((await ordersPage(staff, { s: 'кроссовки nike' })).text), [1001, 1002, 2001], 'название товара из заказа');
  assert.deepEqual(orderNumbers((await ordersPage(staff, { s: 'Другой кабинет' })).text), [2001], 'кабинет');
  assert.deepEqual(orderNumbers((await ordersPage(staff, { s: 'nothing-like-this' })).text), []);
  assert.match((await ordersPage(staff, { s: '9780201379624' })).text, /name="s" value="9780201379624"/, 'поле поиска сохраняет введённое');
  // ярлыки: только заказы из найденных
  market.state.requests.length = 0;
  const page = await ordersPage(staff, { s: '9780201379624' });
  const rid = /name="r" value="([0-9a-f]+)"/.exec(page.text)[1];
  const groups = [...page.text.matchAll(/name="g" value="(\d+)"/g)].map((m) => m[1]);
  assert.equal(groups.length, 2);
  const sent = [];
  for (const g of groups) {
    const res = await staff.req('POST', '/orders/labels', { form: { _csrf: await staff.csrf('/orders'), r: rid, g, part: '1', format: 'A7' }, headers: { 'x-requested-with': 'fetch' } });
    assert.equal(res.status, 200);
    sent.push(...market.state.requests.filter((x) => x.path.endsWith('/generate')).at(-1).body.orderIds);
  }
  assert.deepEqual(sent.sort(), [1001, 1002, 2001], 'ярлыки запрошены только для найденных заказов (не для 1003)');
  const xlsx = require('../src/xlsx');
  const t = xlsx.readFirstSheet((await staff.get(`/orders/export.xlsx?r=${rid}`)).raw);
  assert.deepEqual([...new Set(t.rows.slice(1).map((r) => r[1]))].sort(), ['1001', '1002', '2001']);
  // ссылка быстрой правки у товара в заказе; у просмотра заказов нет вовсе
  assert.match((await ordersPage(staff, { s: '9780201379624' })).text, /\/products\/\d+\/quick\?back=%2Forders%3F/);
  assert.equal((await viewer.get('/orders')).status, 302);
});

test('заказы: поле поиска в форме, быстрые кнопки сохраняют поиск', async () => {
  const f = await staff.get('/orders');
  assert.match(f.text, /name="s"/);
  const withS = await staff.get('/orders?go=1&s=9780201379624');
  assert.match(withS.text, /href="\/orders\?[^"]*s=9780201379624/, 'быстрые кнопки «сегодня/завтра» сохраняют поиск');
});

test('страницы не используют inline-стили (их блокирует политика безопасности)', async () => {
  const id = ids['NK-AF1-42'];
  for (const url of ['/stock', '/products', '/products?view=cards', `/products/${id}`, `/products/${id}/quick`, '/find?q=4600702084030', '/orders?go=1&from=2026-09-25&to=2026-09-25', '/import', '/users']) {
    const html = (await admin.get(url)).text;
    assert.ok(!/\sstyle="/.test(html), `${url}: есть style="..."`);
    assert.ok(!/\son(click|submit|change|load)="/.test(html), `${url}: есть inline-обработчик`);
  }
});
