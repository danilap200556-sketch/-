'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const xlsx = require('../src/xlsx');
const bc = require('../src/barcode');
const imp = require('../src/importer');
const { startSite } = require('./helpers');

// Новые товары без своего штрихкода получают свой EAN-13 из последовательности в базе (функция next_internal_ean13):
// коды уникальны, у всех верная контрольная цифра и префикс 2, их количество не ограничено.
let site; let admin;
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const codesOf = async (sku) => (await q('SELECT b.barcode FROM product_barcodes b JOIN products p ON p.id = b.product_id WHERE p.sku = $1 ORDER BY b.barcode', [sku])).map((r) => r.barcode);
const unesc = (t) => t.replace(/&quot;/g, '"').replace(/&amp;/g, '&');

test.before(async () => {
  site = await startSite();
  await site.addUser('boss', 'bosspass1', true);
  admin = site.client(); await admin.login('boss', 'bosspass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А')");
  bc.resetOwnCache();
});
test.after(() => site.stop());

const valid = (code) => /^2\d{12}$/.test(code) && bc.validate(code) === null;

test('новый товар через форму получает свой EAN-13', async () => {
  const r = await admin.post('/products', { sku: 'NEW-1', name: 'Новый один', price: '100' });
  assert.equal(r.status, 302);
  const codes = await codesOf('NEW-1');
  assert.equal(codes.length, 1); assert.ok(valid(codes[0]), `код ${codes[0]}`);
  assert.match(unesc((await admin.get(r.location)).text), /присвоен свой штрихкод EAN-13/);
  await admin.post('/products', { sku: 'NEW-2', name: 'Новый два', price: '100' });
  const second = await codesOf('NEW-2');
  assert.equal(second.length, 1); assert.notEqual(second[0], codes[0], 'коды разные');
  assert.equal(Number(second[0].slice(1, 12)), Number(codes[0].slice(1, 12)) + 1, 'идут по порядку');
});

test('загрузка остатков: новый товар без штрихкода в файле получает свой, а с кодом - нет', async () => {
  const rows = [['Артикул', 'Название', 'Остаток', 'Штрихкод'], ['IMP-A', 'Без кода', '3', ''], ['IMP-B', 'С кодом', '2', '4006381333931'], ['IMP-C', 'Тоже без', '1', '']];
  const r = await imp.run(site.db, { rows, headerIdx: 0, roles: [imp.ROLES.key, imp.ROLES.name, imp.ROLES.stock, imp.ROLES.barcode], warehouseId: 1, mode: 'inventory', createMissing: true, fileName: 'f.xlsx', user: 'boss' });
  assert.equal(r.stats.created, 3); assert.equal(r.stats.own, 2);
  assert.ok(valid((await codesOf('IMP-A'))[0]) && valid((await codesOf('IMP-C'))[0]));
  assert.deepEqual(await codesOf('IMP-B'), ['4006381333931'], 'свой код файла остаётся единственным');
  assert.match(r.log.join('\n'), /присвоен свой EAN-13: 2/);
  // существующий товар без штрихкода импортом не затрагивается
  await site.db.query("INSERT INTO products (sku, name) VALUES ('OLD-1', 'Старый')");
  await imp.run(site.db, { rows: [['Артикул', 'Остаток'], ['OLD-1', '5']], headerIdx: 0, roles: [imp.ROLES.key, imp.ROLES.stock], warehouseId: 1, mode: 'inventory', createMissing: true, fileName: 'g.xlsx', user: 'boss' });
  assert.deepEqual(await codesOf('OLD-1'), [], 'только новым товарам');
});

test('ensureOwn: тысячи товаров, уникальность, повтор ничего не меняет, занятый вручную код пропускается', async () => {
  await site.db.query("INSERT INTO products (sku, name) SELECT 'BULK-' || g, 'Массовый ' || g FROM generate_series(1, 3000) g");
  const ids = (await q("SELECT id FROM products WHERE sku LIKE 'BULK-%'")).map((r) => r.id);
  // занимаем вручную тот код, который выдала бы последовательность следующим
  const seq = (await site.sys('SELECT last_value, is_called FROM internal_barcode_seq')).rows[0]; // читает владелец: у пользователя сайта прав на саму последовательность нет, он работает через функцию
  const nextN = Number(seq.last_value) + (seq.is_called ? 1 : 0);
  const body = `2${String(nextN).padStart(11, '0')}`;
  const taken = body + bc.checkDigit(body);
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) SELECT $1, id FROM products WHERE sku = 'OLD-1'", [taken]);
  const n = await bc.ensureOwn(site.db, ids);
  assert.equal(n, 3000);
  const all = (await q("SELECT b.barcode FROM product_barcodes b JOIN products p ON p.id = b.product_id WHERE p.sku LIKE 'BULK-%'")).map((r) => r.barcode);
  assert.equal(new Set(all).size, 3000, 'все разные');
  assert.ok(all.every(valid), 'у всех верная контрольная цифра и префикс 2');
  assert.ok(!all.includes(taken), 'занятый вручную код пропущен');
  assert.equal(await bc.ensureOwn(site.db, ids), 0, 'повторно - ничего');
  // параллельно из нескольких мест
  await site.db.query("INSERT INTO products (sku, name) SELECT 'PAR-' || g, 'Параллельный ' || g FROM generate_series(1, 400) g");
  const pids = (await q("SELECT id FROM products WHERE sku LIKE 'PAR-%' ORDER BY id")).map((r) => r.id);
  await Promise.all([0, 1, 2, 3].map((k) => bc.ensureOwn(site.db, pids.filter((_, i) => i % 4 === k))));
  const par = (await q("SELECT b.barcode FROM product_barcodes b JOIN products p ON p.id = b.product_id WHERE p.sku LIKE 'PAR-%'")).map((r) => r.barcode);
  assert.equal(par.length, 400); assert.equal(new Set(par).size, 400);
});

test('функция в базе: коды идут подряд, а не случайно; запас - десятки миллиардов', async () => {
  const a = (await q('SELECT next_internal_ean13() AS c'))[0].c; const b = (await q('SELECT next_internal_ean13() AS c'))[0].c;
  assert.ok(valid(a) && valid(b)); assert.equal(Number(b.slice(1, 12)) - Number(a.slice(1, 12)), 1);
  const max = (await q("SELECT max_value::bigint AS m FROM pg_sequences WHERE sequencename = 'internal_barcode_seq'"))[0].m;
  assert.equal(String(max), '99999999999', '11 цифр: 99 999 999 999 кодов');
});

test('старая база без функции: товар создаётся без штрихкода, как раньше', async () => {
  await site.sys('DROP FUNCTION next_internal_ean13()');
  bc.resetOwnCache();
  try {
    const r = await admin.post('/products', { sku: 'NOFUNC', name: 'Без функции', price: '1' });
    assert.equal(r.status, 302);
    assert.deepEqual(await codesOf('NOFUNC'), []);
    assert.match(unesc((await admin.get(r.location)).text), /Товар создан\. Теперь можно добавить фото и штрихкоды/);
  } finally { bc.resetOwnCache(); }
});
