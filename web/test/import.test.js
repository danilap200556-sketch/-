'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');
const xlsx = require('../src/xlsx');
const imp = require('../src/importer');
const { startSite } = require('./helpers');

const fixture = (n) => fs.readFileSync(path.join(__dirname, 'fixtures', n));

// ------------------------------------------------------------------ чтение/запись xlsx
test('xlsx: читает файл Excel (общие строки, числа, сущности, пустые строки, первый лист)', () => {
  const t = xlsx.readFirstSheet(fixture('report.xlsx'));
  assert.equal(t.ok, true);
  assert.deepEqual(t.rows[0], ['Отчёт по остаткам за сентябрь']);
  assert.deepEqual(t.rows[1], []);
  assert.deepEqual(t.rows[2], ['Артикул', 'Наименование', 'Остаток', 'Штрихкод', 'Склад']);
  assert.deepEqual(t.rows[3], ['NK-AF1-42', 'Nike & <Air> "AF1"', '12', '4006381333931', 'Склад А']);
  assert.deepEqual(t.rows[4], ['4600702084030', 'Число как артикул', '3', '4600702084030, 4006381333931', 'Склад А']);
  assert.deepEqual(t.rows[5], ['DEC', 'Дробное количество', '2.5', '', 'Склад Б']);
  assert.equal(t.rows.length, 7, 'второй лист не читается');
});

test('xlsx: запись -> чтение возвращает то же; спецсимволы и управляющие символы безопасны', () => {
  const rows = [['Артикул', 'Название', 'Остаток'], ['A&B', '<b>"x"</b>\u0001\u0008 ok', '5'], ['', 'пусто в первой', '0'], ['Ёлка', 'длинная строка '.repeat(30), '12']];
  const buf = xlsx.write([{ name: 'Остатки: [1]*', rows, numericColumns: [2], columnWidths: [20, 40, 10] }, { name: 'Второй', rows: [['a']] }]);
  assert.equal(buf.subarray(0, 2).toString(), 'PK');
  const back = xlsx.readFirstSheet(buf);
  assert.equal(back.ok, true);
  assert.deepEqual(back.rows[0], rows[0]);
  assert.deepEqual(back.rows[1], ['A&B', '<b>"x"</b> ok', '5']);
  assert.deepEqual(back.rows[2], ['', 'пусто в первой', '0']);
  assert.equal(back.rows[3][1], rows[3][1]);
});

test('xlsx: мусор и подделки не роняют разбор', () => {
  assert.equal(xlsx.readFirstSheet(Buffer.from('это не zip')).ok, false);
  assert.equal(xlsx.readFirstSheet(Buffer.alloc(0)).ok, false);
  const bomb = require('fflate').zipSync({ 'xl/worksheets/sheet1.xml': new Uint8Array(100 * 1024 * 1024) });
  assert.equal(xlsx.readFirstSheet(Buffer.from(bomb)).ok, false, 'файл, который распаковывается в 100 МБ, отвергается');
  const evil = require('fflate').zipSync({ 'xl/worksheets/sheet1.xml': require('fflate').strToU8('<worksheet><sheetData><row r="9999999999"><c r="XFD1048576"><v>1</v></c></row><row r="2"><c r="ZZZ2" t="s"><v>99</v></c></row></sheetData></worksheet>') });
  const r = xlsx.readFirstSheet(Buffer.from(evil));
  assert.equal(r.ok, true); assert.ok(r.rows.length < 10, 'огромные индексы строк/колонок игнорируются');
});

test('csv: UTF-8 с BOM и Windows-1251, разделители ; , табуляция, кавычки', () => {
  assert.deepEqual(imp.parseCsv(fixture('cp1251.csv'))[1], ['SKU-А1', 'Кроссовки Ёлка', '5']);
  assert.deepEqual(imp.parseCsv(fixture('utf8-bom.csv'))[0], ['Артикул', 'Название', 'Остаток']);
  assert.deepEqual(imp.parseCsv(Buffer.from('a;b;c\n"x;1";"он сказал ""да""";3\n')), [['a', 'b', 'c'], ['x;1', 'он сказал "да"', '3']]);
  assert.deepEqual(imp.parseCsv(Buffer.from('a\tb\n1\t2')), [['a', 'b'], ['1', '2']]);
  assert.equal(imp.parseFile(Buffer.from('x'), 'old.xls').error.includes('.xls'), true);
  assert.equal(imp.parseFile(Buffer.from('x'), 'a.pdf').error.includes('.xlsx'), true);
  assert.equal(imp.parseFile(Buffer.from(';;\n;;'), 'empty.csv').error.includes('ни одной'), true);
});

test('импорт: подбор строки заголовков и ролей колонок', () => {
  const rows = xlsx.readFirstSheet(fixture('report.xlsx')).rows;
  assert.equal(imp.guessHeaderRow(rows), 2);
  assert.deepEqual(rows[2].map(imp.guessRole), [imp.ROLES.key, imp.ROLES.name, imp.ROLES.stock, imp.ROLES.barcode, imp.ROLES.warehouse]);
  assert.equal(imp.guessRole('EAN-13'), imp.ROLES.barcode);
});

// ------------------------------------------------------------------ импорт/экспорт по HTTP
let site; let staff; let viewer; let admin;
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const stockOf = async (sku, wh) => (await q(`SELECT COALESCE((SELECT s.quantity FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id WHERE p.sku = $1 AND w.name = $2), 0)::int AS n`, [sku, wh]))[0].n;
const ids = {};

test.before(async () => {
  site = await startSite();
  await site.addUser('boss', 'bosspass1', true); await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  admin = site.client(); staff = site.client(); viewer = site.client();
  await admin.login('boss', 'bosspass1'); await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад Б')");
  for (const w of await q('SELECT id, name FROM warehouses')) ids[w.name] = w.id;
  await site.db.query("INSERT INTO products (sku, name, price) VALUES ('NK-AF1-42', 'Nike AF1 42', 100), ('NB-373-43', 'NB 373 43', 250), ('NOSTOCK', 'Без остатка', 1)");
  await site.db.query("INSERT INTO locations (warehouse_id, code) VALUES ($1, 'A1-03')", [ids['Склад А']]);
  await site.db.query(`INSERT INTO stock (product_id, warehouse_id, quantity, location_id)
    SELECT p.id, $1, 5, (SELECT id FROM locations LIMIT 1) FROM products p WHERE p.sku = 'NK-AF1-42'`, [ids['Склад А']]);
  await site.db.query("INSERT INTO stock (product_id, warehouse_id, quantity) SELECT p.id, $1, 2 FROM products p WHERE p.sku = 'NK-AF1-42'", [ids['Склад Б']]);
  await site.db.query("INSERT INTO stock (product_id, warehouse_id, quantity) SELECT p.id, $1, 7 FROM products p WHERE p.sku = 'NB-373-43'", [ids['Склад А']]);
});
test.after(() => site.stop());

const upload = async (client, buf, name) => client.multipart('/import/upload', {}, [{ name, data: buf, field: 'file' }], { from: '/import' });
// Загружает файл, проходит сопоставление колонок и запускает импорт; roles: { индекс колонки: роль }
async function runImport(client, buf, name, { roles, header = 1, mode = 'inventory', warehouse = 'Склад А', createMissing = true }) {
  const up = await upload(client, buf, name);
  assert.equal(up.status, 302, up.text.slice(0, 200));
  assert.match(up.location, /^\/import\/map\/[0-9a-f]{32}$/);
  const id = up.location.split('/').pop();
  const form = { header: String(header), warehouse: String(ids[warehouse]), mode, ...(createMissing ? { create_missing: '1' } : {}) };
  for (const [i, r] of Object.entries(roles)) form[`role_${i}`] = String(imp.ROLES[r]);
  const r = await client.post(`/import/run/${id}`, form, { from: up.location });
  return { id, r };
}
const logOf = (r) => decodeURIComponent(r.text.match(/<pre class="log">([\s\S]*?)<\/pre>/)?.[1] ?? '').replace(/&quot;/g, '"').replace(/&amp;/g, '&');

test('экспорт: все остатки + товары без остатка, фильтры по складу и поиску', async () => {
  const r = await staff.get('/export/stock.xlsx');
  assert.equal(r.status, 200);
  assert.equal(r.headers.get('content-type'), 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet');
  assert.match(r.headers.get('content-disposition'), /attachment; filename="stock_\d{4}-\d{2}-\d{2}\.xlsx"; filename\*=UTF-8''/);
  const t = xlsx.readFirstSheet(r.raw);
  assert.deepEqual(t.rows[0], ['Артикул', 'Название', 'Склад', 'Остаток', 'Место хранения']);
  const body = t.rows.slice(1).map((x) => x.join('|'));
  assert.deepEqual(body, ['NB-373-43|NB 373 43|Склад А|7', 'NK-AF1-42|Nike AF1 42|Склад А|5|A1-03', 'NK-AF1-42|Nike AF1 42|Склад Б|2', 'NOSTOCK|Без остатка|Склад А|0']);
  const wB = xlsx.readFirstSheet((await staff.get(`/export/stock.xlsx?w=${ids['Склад Б']}`)).raw).rows.slice(1).map((x) => x.join('|'));
  assert.deepEqual(wB, ['NB-373-43|NB 373 43|Склад Б|0', 'NK-AF1-42|Nike AF1 42|Склад Б|2', 'NOSTOCK|Без остатка|Склад Б|0']);
  const qq = xlsx.readFirstSheet((await staff.get('/export/stock.xlsx?q=nostock')).raw).rows.slice(1);
  assert.equal(qq.length, 1);
  assert.equal((await viewer.get('/export/stock.xlsx')).status, 200, 'просмотр: экспорт доступен');
  assert.equal((await site.client().get('/export/stock.xlsx')).status, 302, 'без входа - нет');
});

test('импорт: файл Excel с шапкой отчёта, режим «фактический остаток», создание нового товара и штрихкоды', async () => {
  const { r } = await runImport(staff, fixture('report.xlsx'), 'report.xlsx', { header: 3, roles: { 0: 'key', 1: 'name', 2: 'stock', 3: 'barcode', 4: 'warehouse' } });
  assert.equal(r.status, 200);
  const log = logOf(r);
  // Строка 5: артикула 4600702084030 нет, но второй её штрихкод уже у NK-AF1-42 (привязан строкой выше) - как в приложении,
  // товар ищется по артикулу, а если не найден - по штрихкоду.
  assert.match(log, /обработано 3, создано товаров 1, обновлено остатков 3, привязано штрихкодов 2, пропущено 0, ошибок 0/);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 3, 'строка 5 перезаписала остаток NK-AF1-42 (12 -> 3)');
  assert.equal(await stockOf('NK-AF1-42', 'Склад Б'), 2, 'другой склад не тронут');
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM products WHERE sku = '4600702084030'"))[0].n, 0);
  assert.equal(await stockOf('DEC', 'Склад Б'), 3, '2.5 округлено как в приложении');
  assert.deepEqual((await q("SELECT barcode FROM product_barcodes ORDER BY barcode")).map((x) => x.barcode), ['4006381333931', '4600702084030']);
  assert.match(log, /создан новый товар «DEC»/);
  const moves = await q("SELECT type, delta, comment FROM stock_movements WHERE comment LIKE 'импорт из %' ORDER BY id");
  assert.equal(moves.length, 3); assert.ok(moves.every((m) => m.comment.startsWith('импорт из report.xlsx (инвентаризация: было')));
  assert.deepEqual(moves.map((m) => m.delta), [7, -9, 3], '5 -> 12 -> 3 и новый товар 0 -> 3');
});

test('импорт: повторная отправка не задваивает, чужой файл недоступен, просмотр запрещён', async () => {
  const buf = xlsx.write([{ name: 'x', rows: [['Артикул', 'Остаток'], ['NB-373-43', '1']] }]);
  const up = await upload(staff, buf, 'x.xlsx');
  const id = up.location.split('/').pop();
  const mapHtml = (await staff.get(up.location)).text;
  assert.match(mapHtml, /name="create_missing" value="1" checked/, 'создание новых товаров включено по умолчанию');
  assert.match(mapHtml, /name="mode" value="inventory" checked/);
  assert.equal((await admin.get(up.location)).status, 302, 'другой пользователь не видит файл');
  assert.equal((await viewer.get(up.location)).status, 302);
  const form = { header: '1', warehouse: String(ids['Склад А']), mode: 'receipt', role_0: '1', role_1: '3' };
  assert.equal((await staff.post(`/import/run/${id}`, form, { from: up.location })).status, 200);
  assert.equal(await stockOf('NB-373-43', 'Склад А'), 8);
  const again = await staff.post(`/import/run/${id}`, form, { from: '/import' });
  assert.equal(again.status, 302);
  assert.equal(await stockOf('NB-373-43', 'Склад А'), 8, 'повтор не добавил ещё раз');
  assert.equal((await staff.get('/import/map/' + 'f'.repeat(32))).status, 302);
  const v = await viewer.multipart('/import/upload', {}, [{ name: 'x.xlsx', data: buf, field: 'file' }], { from: '/import' });
  assert.equal(v.status, 302); assert.equal(v.location, '/');
  assert.ok(!(await viewer.get('/import')).text.includes('/import/upload'), 'у просмотра нет формы загрузки');
  assert.ok((await staff.get('/import')).text.includes('/import/upload'));
});

test('импорт: приход, списание (с проверкой остатка), отрицательные, нули, склады из колонки, итого', async () => {
  const rows = [['Артикул', 'Остаток', 'Склад'], ['NK-AF1-42', '4', 'Склад А'], ['NB-373-43', '0', 'Склад А'], ['Итого', '99', ''], ['NK-AF1-42', '-1', 'Склад А'], ['NK-AF1-42', 'abc', 'Склад А'], ['NK-AF1-42', '1', 'Нет такого'], ['NEW-ONE', '3', 'Склад А']];
  const buf = xlsx.write([{ name: 'x', rows }]);
  const roles = { 0: 'key', 1: 'stock', 2: 'warehouse' };
  const before = await stockOf('NK-AF1-42', 'Склад А');
  let { r } = await runImport(staff, buf, 'p.xlsx', { roles, mode: 'receipt', createMissing: false });
  let log = logOf(r);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), before + 4);
  assert.match(log, /отрицательное количество -1/); assert.match(log, /не удалось прочитать остаток «abc»/); assert.match(log, /склад «Нет такого» не найден/);
  assert.match(log, /пропущено 1, ошибок 3/, 'NEW-ONE пропущен без создания');
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM products WHERE sku = 'NEW-ONE'"))[0].n, 0);
  ({ r } = await runImport(staff, xlsx.write([{ name: 'x', rows: [['Артикул', 'Остаток'], ['NK-AF1-42', '6'], ['NB-373-43', '100']] }]), 'w.xlsx', { roles: { 0: 'key', 1: 'stock' }, mode: 'writeoff' }));
  log = logOf(r);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), before + 4 - 6);
  assert.equal(await stockOf('NB-373-43', 'Склад А'), 8, 'списать больше остатка нельзя - строка пропущена');
  assert.match(log, /Недостаточно остатка: на складе 8 шт, требуется списать 100 шт/);
  assert.match(log, /обновлено остатков 1.*ошибок 1/);
  assert.deepEqual((await q("SELECT COUNT(*)::int AS n FROM stock WHERE quantity < 0"))[0], { n: 0 });
  // списание не создаёт товары
  ({ r } = await runImport(staff, xlsx.write([{ name: 'x', rows: [['Артикул', 'Остаток'], ['GHOST', '1']] }]), 'g.xlsx', { roles: { 0: 'key', 1: 'stock' }, mode: 'writeoff', createMissing: true }));
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM products WHERE sku = 'GHOST'"))[0].n, 0);
  // инвентаризация: отрицательное - ошибка
  ({ r } = await runImport(staff, xlsx.write([{ name: 'x', rows: [['Артикул', 'Остаток'], ['NB-373-43', '-5']] }]), 'n.xlsx', { roles: { 0: 'key', 1: 'stock' } }));
  assert.match(logOf(r), /не может быть отрицательным/);
  assert.equal(await stockOf('NB-373-43', 'Склад А'), 8);
});

test('импорт: только штрихкоды (без остатка), поиск товара по штрихкоду, CSV в cp1251', async () => {
  const rows = [['Артикул', 'Штрихкод'], ['NB-373-43', '036000291452'], ['NOSTOCK', '4006381333932'], ['NOPE', '96385074'], ['', '036000291452']];
  let { r } = await runImport(staff, xlsx.write([{ name: 'x', rows }]), 'codes.xlsx', { roles: { 0: 'key', 1: 'barcode' } });
  let log = logOf(r);
  assert.match(log, /привязано штрихкодов 1/);
  assert.match(log, /штрихкод «4006381333932» не привязан - неверная контрольная/);
  assert.match(log, /товар «NOPE» не найден/);
  assert.equal((await q("SELECT p.sku FROM product_barcodes b JOIN products p ON p.id = b.product_id WHERE b.barcode = '036000291452'"))[0].sku, 'NB-373-43');
  // товар по штрихкоду, когда артикула в файле нет
  ({ r } = await runImport(staff, xlsx.write([{ name: 'x', rows: [['Штрихкод', 'Остаток'], ['036000291452', '4']] }]), 'bc.xlsx', { roles: { 0: 'barcode', 1: 'stock' }, mode: 'receipt' }));
  assert.equal(await stockOf('NB-373-43', 'Склад А'), 12);
  // CSV из старой программы (Windows-1251, ;)
  ({ r } = await runImport(staff, fixture('cp1251.csv'), 'old.csv', { roles: { 0: 'key', 1: 'name', 2: 'stock' } }));
  log = logOf(r);
  assert.match(log, /создано товаров 2/);
  assert.equal((await q("SELECT name FROM products WHERE sku = 'SKU-А1'"))[0].name, 'Кроссовки Ёлка');
  assert.equal(await stockOf('SKU-А1', 'Склад А'), 5);
  // нет нужных колонок -> форма сопоставления с ошибкой, ничего не сделано
  const bad = await runImport(staff, xlsx.write([{ name: 'x', rows: [['Артикул', 'Остаток'], ['A', '1']] }]), 'b.xlsx', { roles: { 0: 'name' } });
  assert.equal(bad.r.status, 400); assert.match(bad.r.text, /Укажите, какая колонка/);
});

test('импорт: круг «скачал -> поправил в Excel -> загрузил»', async () => {
  const t = xlsx.readFirstSheet((await staff.get(`/export/stock.xlsx?w=${ids['Склад А']}`)).raw);
  const edited = t.rows.map((row, i) => (i === 0 ? row : [row[0], row[1], row[2], row[0] === 'NK-AF1-42' ? '77' : row[3], row[4]]));
  const { r } = await runImport(staff, xlsx.write([{ name: 'Остатки', rows: edited, numericColumns: [3] }]), 'edited.xlsx', { roles: { 0: 'key', 1: 'name', 2: 'warehouse', 3: 'stock' } });
  assert.match(logOf(r), /ошибок 0/);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 77);
  assert.equal(await stockOf('NK-AF1-42', 'Склад Б'), 2);
});

test('импорт: плохие файлы отклоняются понятно, большой файл на 3000 строк проходит', async () => {
  for (const [name, data, re] of [['old.xls', Buffer.from('x'), /xls/], ['a.txt', Buffer.from('x'), /xlsx/], ['broken.xlsx', Buffer.from('не zip'), /Не удалось прочитать/], ['empty.csv', Buffer.from(''), /ни одной/]]) {
    const up = await upload(staff, data, name);
    assert.equal(up.status, 302); assert.equal(up.location, '/import', name);
    assert.match(decodeURIComponent((await staff.follow(up)).text), re, name);
  }
  assert.equal((await staff.multipart('/import/upload', {}, [], { from: '/import' })).status, 302);
  const rows = [['Артикул', 'Название', 'Остаток']];
  for (let i = 0; i < 3000; i++) rows.push([`MASS-${i}`, `Массовый ${i}`, String(i % 50)]);
  const t0 = Date.now();
  const { r } = await runImport(staff, xlsx.write([{ name: 'x', rows }]), 'mass.xlsx', { roles: { 0: 'key', 1: 'name', 2: 'stock' } });
  assert.match(logOf(r), /обработано 3000, создано товаров 3000/);
  assert.equal(await stockOf('MASS-49', 'Склад А'), 49);
  assert.ok(Date.now() - t0 < 60_000, 'укладывается в минуту');
});
