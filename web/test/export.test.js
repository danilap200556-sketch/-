'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const xlsx = require('../src/xlsx');
const { startSite } = require('./helpers');

let site; let staff; let viewer;
const ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const sheet = (res) => { const t = xlsx.readFirstSheet(res.raw); assert.ok(t.ok, t.error); return t.rows; };
const rowOf = (rows, sku) => rows.find((r) => r[0] === sku);

test.before(async () => {
  site = await startSite();
  await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  staff = site.client(); viewer = site.client();
  await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query(`INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад "Б" & <b>')`);
  for (const w of await q('SELECT id, name FROM warehouses')) ids[w.name] = w.id;
  await site.db.query(`INSERT INTO products (sku, name, description, price, market_sku, custom_code) VALUES
    ('NK-AF1-42', 'Nike AF1 42', 'Белые, кожа', 12990.5, 'MKT-NK', 'QR-1'),
    ('AD-S-40', '=Adidas Samba 40', '', 9000, '', ''),
    ('NB-373-43', 'NB 373 43', 'строка 1
строка 2', 250.99, '', ''),
    ('EMPTY-1', 'Без ничего', '', 0, '', '')`);
  for (const p of await q('SELECT id, sku FROM products')) ids[p.sku] = p.id;
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) VALUES ('4006381333931', $1), ('4600702084030', $1), ('036000291452', $2)", [ids['NK-AF1-42'], ids['AD-S-40']]);
  const loc = (await q("INSERT INTO locations (warehouse_id, code) VALUES ($1, 'A1-03') RETURNING id", [ids['Склад А']]))[0].id;
  await site.db.query('INSERT INTO stock (product_id, warehouse_id, quantity, location_id) VALUES ($1,$2,10,$3),($1,$4,3,NULL),($5,$2,6,NULL)',
    [ids['NK-AF1-42'], ids['Склад А'], loc, ids['Склад "Б" & <b>'], ids['AD-S-40']]);
  await site.db.query("INSERT INTO product_photos (product_id, data, thumb) VALUES ($1, '\\x00', '\\x00'), ($1, '\\x01', '\\x01')", [ids['NK-AF1-42']]);
});
test.after(() => site.stop());

test('Excel: все колонки карточки товара, остатки по складам отдельными колонками', async () => {
  const res = await staff.get('/export/products.xlsx');
  assert.equal(res.status, 200);
  assert.match(res.headers.get('content-type'), /spreadsheetml/);
  assert.match(res.headers.get('content-disposition'), /attachment; filename="products_\d{4}-\d{2}-\d{2}\.xlsx"; filename\*=UTF-8''/);
  const rows = sheet(res);
  assert.deepEqual(rows[0], ['Артикул', 'Название', 'Описание', 'Цена, ₽', 'Артикул на Маркете', 'Свой код', 'Штрихкоды', 'Всего на складах',
    'Остаток: Склад "Б" & <b>', 'Остаток: Склад А', 'Места хранения', 'Фото, шт', 'Добавлен']);
  assert.equal(rows.length, 5, 'заголовок + 4 товара');
  assert.deepEqual(rows.slice(1).map((r) => r[0]), ['AD-S-40', 'EMPTY-1', 'NB-373-43', 'NK-AF1-42'], 'по артикулу');
  const nk = rowOf(rows, 'NK-AF1-42');
  assert.deepEqual(nk.slice(0, 12), ['NK-AF1-42', 'Nike AF1 42', 'Белые, кожа', '12990.5', 'MKT-NK', 'QR-1', '4006381333931; 4600702084030', '13', '3', '10', 'Склад А: A1-03', '2']);
  assert.match(nk[12], /^\d{4}-\d{2}-\d{2}$/);
  const ad = rowOf(rows, 'AD-S-40');
  assert.deepEqual(ad.slice(0, 12), ['AD-S-40', '=Adidas Samba 40', '', '9000', '', '', '036000291452', '6', '0', '6', '', '0']);
  assert.equal(rowOf(rows, 'NB-373-43')[2], 'строка 1\nстрока 2');
  assert.equal(rowOf(rows, 'NB-373-43')[3], '250.99');
  const empty = rowOf(rows, 'EMPTY-1');
  assert.deepEqual(empty.slice(3, 12), ['0', '', '', '', '0', '0', '0', '', '0'], 'товар без остатков и штрихкодов: нули, пустые ячейки');
});

test('числа выгружаются числами, а штрихкод и артикул - текстом', async () => {
  const res = await staff.get('/export/products.xlsx');
  const { unzipSync, strFromU8 } = require('fflate');
  const xml = strFromU8(unzipSync(new Uint8Array(res.raw))['xl/worksheets/sheet1.xml']);
  assert.match(xml, /<c r="D\d+"><v>12990\.5<\/v><\/c>/, 'цена - число');
  assert.match(xml, /<c r="H\d+"><v>13<\/v><\/c>/, 'всего - число');
  assert.match(xml, /<c r="G\d+" t="inlineStr"><is><t xml:space="preserve">4006381333931; 4600702084030<\/t>/, 'штрихкоды - текст (Excel не превратит в 4,0064E+12)');
  assert.match(xml, /<c r="A\d+" t="inlineStr"><is><t xml:space="preserve">=Adidas|<c r="B\d+" t="inlineStr"><is><t xml:space="preserve">=Adidas/, 'начало с «=» - просто текст, не формула');
});

test('поиск: выгружается то, что найдено (штрихкод, часть названия, артикул на Маркете)', async () => {
  const skus = async (query) => sheet(await staff.get('/export/products.xlsx?' + new URLSearchParams(query))).slice(1).map((r) => r[0]);
  assert.deepEqual(await skus({ q: '4600702084030' }), ['NK-AF1-42'], 'по любому из штрихкодов');
  assert.deepEqual(await skus({ q: 'samba' }), ['AD-S-40']);
  assert.deepEqual(await skus({ q: 'MKT-NK' }), ['NK-AF1-42']);
  assert.deepEqual(await skus({ q: 'нет-такого' }), [], 'ничего не найдено - файл только с заголовками');
  assert.deepEqual(await skus({ q: '%' }), [], '«%» ищется буквально');
  assert.equal((await skus({ q: '' })).length, 4);
});

test('CSV: BOM, «;», цена с запятой, защита от формул', async () => {
  const res = await staff.get('/export/products.csv');
  assert.equal(res.status, 200);
  assert.match(res.headers.get('content-type'), /text\/csv/);
  assert.ok(res.raw.subarray(0, 3).equals(Buffer.from([0xef, 0xbb, 0xbf])), 'BOM для Excel');
  const lines = res.text.slice(1).split('\r\n').filter(Boolean);
  assert.equal(lines.length, 5);
  assert.ok(lines[0].startsWith('Артикул;Название;Описание;Цена, ₽;'), lines[0]);
  const nk = lines.find((l) => l.startsWith('NK-AF1-42'));
  assert.ok(nk.includes(';12990,5;'), 'дробная часть через запятую: ' + nk);
  assert.ok(nk.includes(';"4006381333931; 4600702084030";'), 'несколько штрихкодов в одной ячейке - в кавычках: ' + nk);
  assert.ok(lines.find((l) => l.startsWith('AD-S-40')).includes(";'=Adidas Samba 40;"), 'формула обезврежена');
  assert.ok(res.text.includes('"строка 1\nстрока 2"'), 'перенос строки в описании - в кавычках');
});

test('отмеченные товары: POST со списком, повторяющимися ids и «всем найденным»', async () => {
  const token = await staff.csrf('/products');
  const post = (url, pairs) => staff.req('POST', url, { form: [['_csrf', token], ...pairs] });
  let r = await post('/export/products.xlsx', [['ids', String(ids['NB-373-43'])], ['ids', String(ids['AD-S-40'])]]);
  assert.equal(r.status, 200);
  assert.deepEqual(sheet(r).slice(1).map((x) => x[0]), ['AD-S-40', 'NB-373-43'], 'два отмеченных');
  r = await post('/export/products.csv', [['idlist', `${ids['NK-AF1-42']},${ids['EMPTY-1']},abc,0,${ids['NK-AF1-42']}`]]);
  assert.equal(r.text.slice(1).split('\r\n').filter(Boolean).length, 3, 'мусор и повторы отброшены');
  r = await post('/export/products.xlsx', [['all', '1'], ['q', 'ad-s']]);
  assert.deepEqual(sheet(r).slice(1).map((x) => x[0]), ['AD-S-40'], '«всем найденным» - по тексту поиска');
  r = await post('/export/products.xlsx', [['q', 'ad-s']]);
  assert.equal(r.status, 302); assert.equal(r.location, '/products', 'ничего не отмечено - вернуть к списку');
  assert.match((await staff.get('/products')).text, /Отметьте хотя бы один товар/);
  r = await post('/export/products.xlsx', [['ids', '999999']]);
  assert.deepEqual(sheet(r).slice(1), [], 'несуществующий id - пустой файл, не ошибка');
  const noCsrf = await staff.req('POST', '/export/products.xlsx', { form: [['ids', '1']] });
  assert.equal(noCsrf.status, 403, 'без CSRF-токена не принимается');
});

test('права: просмотр и вход', async () => {
  assert.equal((await site.client().get('/export/products.xlsx')).status, 302, 'без входа - на страницу входа');
  const r = await viewer.get('/export/products.xlsx');
  assert.equal(r.status, 200, 'роль «только просмотр» может выгружать');
  assert.equal(sheet(r).length, 5);
  assert.equal((await viewer.get('/export/products.csv')).status, 200);
});

test('кнопки: список товаров и страница экспорта', async () => {
  const list = (await staff.get('/products?q=nike')).text;
  assert.match(list, /href="\/export\/products\.xlsx\?q=nike"/);
  assert.match(list, /href="\/export\/products\.csv\?q=nike"/);
  assert.match(list, /formaction="\/export\/products\.xlsx"/, 'у отмеченных - своя кнопка');
  const v = (await viewer.get('/products')).text;
  assert.match(v, /href="\/export\/products\.xlsx\?q="/);
  assert.ok(!v.includes('formaction'), 'у просмотра нет панели отмеченных');
  const imp = (await staff.get('/import')).text;
  assert.match(imp, /Выгрузить все товары \(полные данные\)/); assert.match(imp, /href="\/export\/products\.xlsx"/);
  assert.match((await viewer.get('/import')).text, /href="\/export\/products\.csv"/);
  for (const t of [list, imp]) assert.ok(!/ style="|\son\w+="/.test(t), 'CSP: без inline-стилей и обработчиков');
});

test('много товаров: все строки на месте', async () => {
  await site.db.query(`INSERT INTO products (sku, name, price) SELECT 'BULK-' || lpad(g::text, 5, '0'), 'Товар ' || g, g FROM generate_series(1, 3000) g`);
  await site.db.query(`INSERT INTO stock (product_id, warehouse_id, quantity) SELECT id, $1, 2 FROM products WHERE sku LIKE 'BULK-%'`, [ids['Склад А']]);
  const rows = sheet(await staff.get('/export/products.xlsx?q=BULK-'));
  assert.equal(rows.length, 3001);
  assert.equal(rows[1][0], 'BULK-00001'); assert.equal(rows[3000][0], 'BULK-03000');
  assert.equal(rows[2999][7], '2'); assert.equal(rows[2999][9], '2');
});
