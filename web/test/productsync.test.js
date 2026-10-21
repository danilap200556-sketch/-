'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const ps = require('../src/productsync');
const xlsx = require('../src/xlsx');
const { productTable } = require('../src/export');
const { startSite } = require('./helpers');

let site; let staff; let viewer;
const ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const prod = async (sku) => { const r = (await q('SELECT sku, name, description, price::float8 AS price, market_sku, custom_code FROM products WHERE sku = $1', [sku]))[0]; if (r) r.price = Math.round(r.price * 100) / 100; return r; };
const movements = async () => (await q('SELECT COUNT(*)::int AS n FROM stock_movements'))[0].n;
const stockSum = async () => (await q('SELECT COALESCE(SUM(quantity), 0)::int AS n FROM stock'))[0].n;
const R = ps.ROLES;
const HEAD = ['Артикул', 'Название', 'Описание', 'Цена, ₽', 'Артикул на Маркете', 'Свой код', 'Штрихкоды', 'Всего на складах', 'Остаток: Склад А'];
const ROLES = ps.guessRoles(HEAD);
const plan = (rows, o = {}) => ps.plan(site.db, { rows: [HEAD, ...rows], headerIdx: 0, roles: ROLES, ...o });
const byRow = (p, n) => p.items.find((i) => i.n === n);

test.before(async () => {
  site = await startSite();
  await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  staff = site.client(); viewer = site.client();
  await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад Б')");
  for (const w of await q('SELECT id, name FROM warehouses')) ids[w.name] = w.id;
  await site.db.query(`INSERT INTO products (sku, name, description, price, market_sku, custom_code) VALUES
    ('NK-AF1-42','Nike AF1 42','Белые',12990.5,'MKT-NK','QR-1'),('AD-S-40','Adidas Samba 40','',9000,'',''),
    ('NB-373-43','NB 373 43','Серые',250.99,'',''),('x-1','Нижний регистр','',1,'',''),('X-1','Верхний регистр','',2,'',''),('only-lower','Один','',3,'','')`);
  for (const p of await q('SELECT id, sku FROM products')) ids[p.sku] = p.id;
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) VALUES ('4006381333931', $1), ('036000291452', $2)", [ids['NK-AF1-42'], ids['AD-S-40']]);
  await site.db.query('INSERT INTO stock (product_id, warehouse_id, quantity) VALUES ($1,$2,10),($3,$2,6),($1,$4,3)', [ids['NK-AF1-42'], ids['Склад А'], ids['AD-S-40'], ids['Склад Б']]);
  await site.db.query("INSERT INTO stock_movements (product_id, warehouse_id, type, delta, comment) VALUES ($1,$2,'receipt',10,'старт')", [ids['NK-AF1-42'], ids['Склад А']]);
});
test.after(() => site.stop());

test('роли колонок по заголовкам; колонки остатков пропускаются; одна роль - одной колонке', () => {
  assert.deepEqual(ROLES, [R.key, R.name, R.description, R.price, R.market_sku, R.custom_code, R.barcode, R.ignore, R.ignore]);
  assert.deepEqual(ps.guessRoles(['Наименование', 'Артикул', 'Артикул', 'EAN', 'Стоимость']), [R.name, R.key, R.ignore, R.barcode, R.price], 'второй «Артикул» не получает ту же роль');
  assert.deepEqual(ps.guessRoles(['Остаток: Склад Артикул', 'Места хранения', 'Фото, шт', 'Добавлен', '', 'Что-то']), [0, 0, 0, 0, 0, 0], 'склад со словом «Артикул» не путается с колонкой артикула');
  assert.equal(ps.guessHeaderRow([['Отчёт'], [], ['Артикул', 'Название', 'Цена']]), 2);
});

test('цена из ячейки: пробелы, запятая, валюта, мусор', () => {
  const p = ps.parsePriceCell;
  assert.deepEqual(['12 990,5', '12990.5', '1,234.50', '₽ 500', '500 руб.', '0', '1.234,5', '12,5р.', '1 234,50'].map(p), [12990.5, 12990.5, 1234.5, 500, 500, 0, 1234.5, 12.5, 1234.5]);
  assert.deepEqual(['abc', '-5', '10000001', '1e3', '', '1,2,3', '12.345.678'].map(p), [null, null, null, null, null, null, null]);
  assert.equal(p('0.1'), 0.1); assert.equal(p('2.675'), 2.68, 'округление до копеек');
});

test('что изменится: изменения, без изменений, пустые ячейки, регистр, итоги, пустые строки', async () => {
  const p = await plan([
    ['NK-AF1-42', 'Nike AF1 42 (новая модель)', 'Белые', '13 500', 'MKT-NK', 'QR-1', '', '999', '999'],   // 2: название и цена
    ['AD-S-40', 'Adidas Samba 40', '', '9000', '', '', '', '', ''],                                         // 3: всё то же
    ['nb-373-43', '', '', '', '', '', '', '', ''],                                                          // 4: пустые ячейки ничего не меняют
    ['', '', '', '', '', '', '', '', ''],                                                                   // 5: пустая строка
    ['Итого', '', '', '', '', '', '', '', ''],                                                              // 6: итоговая строка
    ['NO-SUCH', 'Новый', '', '', '', '', '', '', ''],                                                       // 7: не найден
  ]);
  assert.equal(p.fatal, null);
  assert.deepEqual(p.stats, { rows: 4, update: 1, same: 2, create: 0, missing: 1, error: 0 });
  const a = byRow(p, 2);
  assert.equal(a.status, 'update'); assert.deepEqual(a.changes.map((c) => [c.field, c.from, c.to]), [['name', 'Nike AF1 42', 'Nike AF1 42 (новая модель)'], ['price', '12990.5', '13500']]);
  assert.equal(byRow(p, 3).status, 'same');
  assert.equal(byRow(p, 4).status, 'same'); assert.equal(byRow(p, 4).sku, 'NB-373-43', 'артикул без учёта регистра');
  assert.equal(byRow(p, 7).status, 'missing'); assert.match(byRow(p, 7).message, /не найден.*включите «Создавать новые товары»/);
  assert.equal(await movements(), 1); assert.equal(await stockSum(), 19, 'просмотр ничего не пишет');
  assert.equal((await prod('NK-AF1-42')).price, 12990.5);
});

test('пустая ячейка очищает поле только по галочке, и только описание, артикул на Маркете и свой код', async () => {
  const rows = [['NK-AF1-42', '', '', '', '', '', '', '', '']];
  assert.equal(byRow(await plan(rows), 2).status, 'same');
  const p = await plan(rows, { clearEmpty: true });
  assert.deepEqual(byRow(p, 2).changes.map((c) => [c.field, c.from, c.to]), [['description', 'Белые', ''], ['market_sku', 'MKT-NK', ''], ['custom_code', 'QR-1', '']]);
});

test('штрихкоды: поиск товара без артикула, привязка нового кода, ошибки', async () => {
  const p = await plan([
    ['', 'Найден по штрихкоду', '', '', '', '', '4006381333931', '', ''],     // 2: артикула нет - ищем по коду
    ['AD-S-40', '', '', '', '', '', '4600702084030', '', ''],               // 3: новый код привязывается
    ['AD-S-40', '', '', '', '', '', '', '', ''],                            // 4: повтор товара
    ['NB-373-43', '', '', '', '', '', '4006381333931', '', ''],             // 5: код другого товара
    ['NB-373-43', '', '', '', '', '', '4006381333932', '', ''],             // 6: неверная контрольная цифра
    ['NB-373-43', '', '', '', '', '', '4600702084030', '', ''],             // 7: код уже занят строкой 3
    ['', '', '', '', '', '', '4006381333931 036000291452', '', ''],        // 8: коды двух разных товаров
  ]);
  assert.equal(byRow(p, 2).id, ids['NK-AF1-42']); assert.equal(byRow(p, 2).byCode, true); assert.equal(byRow(p, 2).status, 'update'); assert.deepEqual(byRow(p, 2).changes.map((c) => c.field), ['name']);
  assert.deepEqual(byRow(p, 3).newCodes, ['4600702084030']); assert.equal(byRow(p, 3).status, 'update');
  assert.equal(byRow(p, 4).status, 'error'); assert.match(byRow(p, 4).message, /уже был в строке 3/);
  assert.equal(byRow(p, 5).status, 'error'); assert.match(byRow(p, 5).message, /уже привязан к другому товару/);
  assert.equal(byRow(p, 6).status, 'error'); assert.match(byRow(p, 6).message, /4006381333932.*контрольная/);
  assert.equal(byRow(p, 7).status, 'error'); assert.match(byRow(p, 7).message, /уже указан в строке 3/);
  assert.equal(byRow(p, 8).status, 'error'); assert.match(byRow(p, 8).message, /принадлежат разным товарам/);
});

test('ошибки значений: цена, длина; неоднозначный артикул; повтор товара', async () => {
  const p = await plan([
    ['NK-AF1-42', '', '', 'много', '', '', '', '', ''],
    ['AD-S-40', 'я'.repeat(301), '', '', '', '', '', '', ''],
    ['NB-373-43', '', 'ы'.repeat(2001), '', '', '', '', '', ''],
    ['X-1', 'Точное совпадение регистра', '', '', '', '', '', '', ''],
    ['x-1', 'Тоже точное', '', '', '', '', '', '', ''],
    ['only-LOWER', 'Единственный без учёта регистра', '', '', '', '', '', '', ''],
  ]);
  assert.match(byRow(p, 2).message, /цена «много»/); assert.match(byRow(p, 3).message, /«Название» длиннее 300/); assert.match(byRow(p, 4).message, /«Описание» длиннее 2000/);
  assert.equal(byRow(p, 5).id, ids['X-1'], 'точный регистр побеждает'); assert.equal(byRow(p, 6).id, ids['x-1']);
  assert.equal(byRow(p, 7).id, ids['only-lower']);
  const amb = await plan([['x-1', 'a', '', '', '', '', '', '', ''], ['X-1', 'b', '', '', '', '', '', '', '']]);
  assert.deepEqual(amb.items.map((i) => i.status), ['update', 'update']);
  const amb2 = await ps.plan(site.db, { rows: [['Артикул', 'Название'], ['X-1', 'z']], headerIdx: 0, roles: [R.key, R.name] });
  assert.equal(amb2.items[0].status, 'update');
});

test('общие ошибки файла: нет артикула, нечего обновлять, роль назначена дважды', async () => {
  const base = { rows: [['a', 'b'], ['x', 'y']], headerIdx: 0 };
  assert.match((await ps.plan(site.db, { ...base, roles: [R.name, R.price] })).fatal, /Укажите колонку «Артикул»/);
  assert.match((await ps.plan(site.db, { ...base, roles: [R.key, R.ignore] })).fatal, /Не выбрано, что обновлять/);
  assert.match((await ps.plan(site.db, { ...base, roles: [R.key, R.key] })).fatal, /назначена нескольким колонкам/);
});

test('применить: меняются только выбранные поля; остатки и журнал не тронуты; есть файл «как было»', async () => {
  const stockBefore = await q('SELECT product_id, warehouse_id, quantity FROM stock ORDER BY 1, 2');
  const m0 = await movements();
  const r = await ps.apply(site.db, { rows: [HEAD,
    ['NK-AF1-42', 'Nike AF1 42 NEW', 'Белые, кожа', '13 500,50', '', '', '4600702084030', '', ''],
    ['AD-S-40', '', 'Самба', '', 'MKT-AD', 'QR-2', '', '', ''],
    ['NB-373-43', 'NB 373 43', 'Серые', '250,99', '', '', '', '', ''],
    ['NO-SUCH', 'Нет', '', '', '', '', '', '', '']], headerIdx: 0, roles: ROLES });
  assert.deepEqual([r.updated, r.created, r.stats.same, r.stats.missing], [2, 0, 1, 1]);
  assert.deepEqual(await prod('NK-AF1-42'), { sku: 'NK-AF1-42', name: 'Nike AF1 42 NEW', description: 'Белые, кожа', price: 13500.5, market_sku: 'MKT-NK', custom_code: 'QR-1' }, 'пустые ячейки не стёрли артикул на Маркете и свой код');
  assert.deepEqual(await prod('AD-S-40'), { sku: 'AD-S-40', name: 'Adidas Samba 40', description: 'Самба', price: 9000, market_sku: 'MKT-AD', custom_code: 'QR-2' });
  assert.deepEqual(await prod('NB-373-43'), { sku: 'NB-373-43', name: 'NB 373 43', description: 'Серые', price: 250.99, market_sku: '', custom_code: '' }, 'без изменений');
  assert.deepEqual((await q("SELECT barcode FROM product_barcodes WHERE product_id = $1 ORDER BY barcode", [ids['NK-AF1-42']])).map((x) => x.barcode), ['4006381333931', '4600702084030']);
  assert.deepEqual(await q('SELECT product_id, warehouse_id, quantity FROM stock ORDER BY 1, 2'), stockBefore, 'остатки не изменились');
  assert.equal(await movements(), m0, 'журнал движений не тронут');
  // «как было»
  const old = r.backup.rows;
  assert.deepEqual(old.slice(1).map((x) => x[0]).sort(), ['AD-S-40', 'NK-AF1-42']);
  const nk = old.find((x) => x[0] === 'NK-AF1-42');
  assert.deepEqual(nk.slice(0, 7), ['NK-AF1-42', 'Nike AF1 42', 'Белые', '12990.5', 'MKT-NK', 'QR-1', '4006381333931']);
  // и обратно: загрузили «как было» - вернулось (штрихкод остаётся привязанным: коды только добавляются)
  const back = await ps.apply(site.db, { rows: old, headerIdx: 0, roles: ps.guessRoles(old[0]), clearEmpty: true });
  assert.equal(back.updated, 2);
  assert.deepEqual(await prod('NK-AF1-42'), { sku: 'NK-AF1-42', name: 'Nike AF1 42', description: 'Белые', price: 12990.5, market_sku: 'MKT-NK', custom_code: 'QR-1' });
  assert.deepEqual(await prod('AD-S-40'), { sku: 'AD-S-40', name: 'Adidas Samba 40', description: '', price: 9000, market_sku: '', custom_code: '' }, 'с «очищать пустые» вернулись и пустые поля');
});

test('создание новых товаров по галочке, со штрихкодами; один файл - одна транзакция', async () => {
  const rows = [HEAD, ['NEW-1', 'Новый товар', 'Описание', '1 500', 'M-1', '', '4607177771236', '', ''], ['NEW-2', '', '', '', '', '', '', '', ''], ['AD-S-40', 'Adidas Samba 40', '', '', '', '', '', '', '']];
  const none = await ps.apply(site.db, { rows, headerIdx: 0, roles: ROLES });
  assert.equal(none.created, 0); assert.equal(await prod('NEW-1'), undefined);
  const r = await ps.apply(site.db, { rows, headerIdx: 0, roles: ROLES, createMissing: true });
  assert.equal(r.created, 2);
  assert.deepEqual(await prod('NEW-1'), { sku: 'NEW-1', name: 'Новый товар', description: 'Описание', price: 1500, market_sku: 'M-1', custom_code: '' });
  assert.equal((await prod('NEW-2')).name, 'NEW-2', 'без названия берётся артикул');
  assert.equal((await q("SELECT product_id FROM product_barcodes WHERE barcode = '4607177771236'"))[0].product_id, (await q("SELECT id FROM products WHERE sku = 'NEW-1'"))[0].id);
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM stock s JOIN products p ON p.id = s.product_id WHERE p.sku LIKE 'NEW-%'"))[0].n, 0, 'остатков у новых нет');
});

test('страницы: загрузка, предпросмотр, применение, «как было»; доступ; повтор', async () => {
  const mk = (rows) => Buffer.from(xlsx.write([{ name: 'Товары', rows }]));
  const file = mk([HEAD, ['NK-AF1-42', 'Nike <b>AF1</b> & Ко', 'Белые', '14000', 'MKT-NK', 'QR-1', '', '', ''], ['NB-373-43', 'NB 373 43 v2', '', '', '', '', '', '', ''], ['NO-SUCH', 'x', '', '', '', '', '', '', '']]);
  const up = async (client, name = 'товары.xlsx', data = file) => client.multipart('/import/products/upload', {}, [{ name, data, field: 'file' }], { from: '/import' });
  assert.equal((await up(viewer)).location, '/', 'только просмотр не загружает');
  assert.equal((await site.client().multipart('/import/products/upload', {}, [{ name: 'a.xlsx', data: file, field: 'file' }], { csrf: false })).status, 302);
  const bad = await up(staff, 'a.txt', Buffer.from('x'));
  assert.equal(bad.location, '/import'); assert.match((await staff.get('/import')).text, /Нужен файл \.xlsx или \.csv/);

  const r = await up(staff);
  assert.match(r.location, /^\/import\/products\/[0-9a-f]{32}$/);
  const pg = await staff.get(r.location);
  assert.equal(pg.status, 200);
  assert.match(pg.text, /Остатки не меняются/); assert.match(pg.text, /Будет обновлено: <b>2<\/b>/); assert.match(pg.text, /не найдено: <b>1<\/b>/);
  assert.ok(pg.text.includes('Nike &lt;b&gt;AF1&lt;/b&gt; &amp; Ко'), 'значения из файла экранированы'); assert.ok(!pg.text.includes('<b>AF1</b>'));
  assert.match(pg.text, /<option value="1" selected>Артикул/); assert.match(pg.text, /formaction="\/import\/products\/[0-9a-f]+\/apply"/);
  assert.ok(!/ style="|\son\w+="/.test(pg.text), 'CSP');
  assert.equal((await viewer.get(r.location)).location, '/');
  assert.equal((await staff.get('/import/products/' + 'a'.repeat(32))).location, '/import', 'неизвестный файл');
  // другой пользователь чужой файл не видит
  await site.addUser('other', 'otherpass1', false); const other = site.client(); await other.login('other', 'otherpass1');
  assert.equal((await other.get(r.location)).location, '/import');

  // предпросмотр с другими колонками: цена не нужна
  const form = { header: '1', create_missing: '', clear_empty: '' };
  ps.guessRoles(HEAD).forEach((role, i) => { form[`role_${i}`] = String(role === R.price ? 0 : role); });
  const re = await staff.post(r.location, form, { from: '/import' });
  assert.equal(re.status, 200); assert.ok(!/<span class="muted">Цена:<\/span>/.test(re.text), 'роль «цена» убрали - цены в изменениях нет');
  // применить
  const id = r.location.split('/').pop();
  const m0 = await movements(); const s0 = await stockSum();
  const done = await staff.post(`/import/products/${id}/apply`, { ...form, role_3: String(R.price) }, { from: '/import' });
  assert.equal(done.status, 200); assert.match(done.text, /Обновлено товаров: <b>2<\/b>/); assert.match(done.text, /Остатки не менялись/);
  assert.equal((await prod('NK-AF1-42')).name, 'Nike <b>AF1</b> & Ко'); assert.equal((await prod('NK-AF1-42')).price, 14000);
  assert.equal(await movements(), m0); assert.equal(await stockSum(), s0);
  // повторная отправка формы не применяет второй раз
  const again = await staff.post(`/import/products/${id}/apply`, form, { from: '/import' });
  assert.equal(again.status, 302); assert.equal(again.location, '/import');
  // файл «как было»
  const link = /href="(\/import\/products\/backup\/[0-9a-f]+)"/.exec(done.text)[1];
  const dl = await staff.get(link);
  assert.equal(dl.status, 200); assert.match(dl.headers.get('content-type'), /spreadsheetml/);
  const back = xlsx.readFirstSheet(dl.raw).rows;
  assert.equal(back.find((x) => x[0] === 'NK-AF1-42')[1], 'Nike AF1 42', 'в файле прежнее название');
  assert.equal((await other.get(link)).location, '/import', 'чужой файл не отдаётся');
  assert.equal((await viewer.get(link)).location, '/');
});

test('выгрузка товаров -> правка в Excel -> загрузка обратно: меняется только то, что правили', async () => {
  const table = await productTable(site.db, {});
  const rows = table.rows.map((r) => r.slice());
  const col = (name) => rows[0].indexOf(name);
  const idx = rows.findIndex((r) => r[0] === 'AD-S-40');
  rows[idx][col('Описание')] = 'Правка из выгрузки'; rows[idx][col('Всего на складах')] = '777'; rows[idx][col('Остаток: Склад А')] = '555';
  const before = await stockSum();
  const p = await ps.plan(site.db, { rows, headerIdx: 0, roles: ps.guessRoles(rows[0]) });
  assert.equal(p.fatal, null);
  const changed = p.items.filter((i) => i.status === 'update');
  assert.equal(changed.length, 1, 'изменена одна ячейка - обновится один товар: ' + JSON.stringify(changed.map((c) => c.sku)));
  assert.deepEqual(changed[0].changes.map((c) => c.field), ['description']);
  await ps.apply(site.db, { rows, headerIdx: 0, roles: ps.guessRoles(rows[0]) });
  assert.equal((await prod('AD-S-40')).description, 'Правка из выгрузки');
  assert.equal(await stockSum(), before, 'числа остатков в файле проигнорированы');
});

test('много строк: 3000 товаров одним файлом', async () => {
  await site.db.query(`INSERT INTO products (sku, name, price) SELECT 'BULK-' || lpad(g::text, 5, '0'), 'Товар ' || g, g FROM generate_series(1, 3000) g`);
  const rows = [['Артикул', 'Название', 'Цена']];
  for (let g = 1; g <= 3000; g++) rows.push([`BULK-${String(g).padStart(5, '0')}`, `Новое ${g}`, String(g % 2 ? g + 1 : g)]);
  const t = Date.now();
  const r = await ps.apply(site.db, { rows, headerIdx: 0, roles: [R.key, R.name, R.price] });
  assert.equal(r.updated, 3000);
  assert.ok(Date.now() - t < 20_000, `быстро: ${Date.now() - t} мс`);
  assert.equal((await prod('BULK-00007')).name, 'Новое 7'); assert.equal((await prod('BULK-00007')).price, 8); assert.equal((await prod('BULK-00008')).price, 8);
  assert.match((await ps.plan(site.db, { rows: Array.from({ length: ps.MAX_ROWS + 10 }, (_, i) => (i ? [`S${i}`, 'x'] : ['Артикул', 'Название'])), headerIdx: 0, roles: [R.key, R.name] })).fatal, /Слишком много строк/);
});
