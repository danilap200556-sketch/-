'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const batch = require('../src/batch');
const { startSite } = require('./helpers');

let site; let staff; let viewer;
const ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const stockOf = async (sku, wh) => (await q('SELECT COALESCE((SELECT s.quantity FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id WHERE p.sku = $1 AND w.name = $2), 0)::int AS n', [sku, wh]))[0].n;
const tokenOf = (html) => /name="token" value="([0-9a-f]+)"/.exec(html)[1];
const post = async (client, url, form) => client.post(url, form, { from: '/batch' });

test('разбор строк: сканы, количество, Excel, ошибки', () => {
  const r = batch.parseLines('4006381333931\n NK-AF1-42  5 \nSKU;3;лишнее\nA B C\t7\n4006 3813 33931\nX 0\nY 2,5\nZ 1.0\n\n;5\n12345 7\nFF abc');
  const by = (n) => r.find((x) => x.n === n);
  assert.deepEqual([by(1).key, by(1).qty], ['4006381333931', 1]);
  assert.deepEqual([by(2).key, by(2).qty], ['NK-AF1-42', 5]);
  assert.deepEqual([by(3).key, by(3).qty], ['SKU', 3]);
  assert.deepEqual([by(4).key, by(4).qty], ['A B C', 7]);
  assert.deepEqual([by(5).key, by(5).qty], ['4006381333931', 1], 'штрихкод с пробелами - целиком одна штука');
  assert.match(by(6).error, /больше нуля/); assert.match(by(7).error, /целое число/);
  assert.equal(by(8).qty, 1, '1.0 = 1');
  assert.match(by(10).error, /нет артикула/);
  assert.deepEqual([by(11).key, by(11).qty], ['12345', 7]);
  assert.equal(by(12).key, 'FF abc', 'не число на конце - часть артикула');
  assert.deepEqual(batch.parseLines(''), []); assert.deepEqual(batch.parseLines(null), []);
  assert.match(batch.parseLines('X 999999999999')[0].error, /слишком большое/);
});

test.before(async () => {
  site = await startSite();
  await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  staff = site.client(); viewer = site.client();
  await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад Б')");
  for (const w of await q('SELECT id, name FROM warehouses')) ids[w.name] = w.id;
  await site.db.query("INSERT INTO products (sku, name, market_sku) VALUES ('NK-AF1-42','Nike AF1 42',''),('NB-373-43','NB 373 43','MKT-NB'),('AD-S-40','Adidas Samba 40',''),('AD-S-41','Adidas Samba 41','')");
  for (const p of await q('SELECT id, sku FROM products')) ids[p.sku] = p.id;
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) VALUES ('4006381333931', $1), ('036000291452', $2)", [ids['NK-AF1-42'], ids['NB-373-43']]);
  await site.db.query('INSERT INTO stock (product_id, warehouse_id, quantity) VALUES ($1,$2,10),($3,$2,4),($4,$2,6)', [ids['NK-AF1-42'], ids['Склад А'], ids['NB-373-43'], ids['AD-S-40']]);
});
test.after(() => site.stop());

const form = (extra) => ({ kind: 'writeoff', warehouse: String(ids['Склад А']), comment: 'отгрузка №7', lines: '', ...extra });

test('страница: форма, ссылки, права', async () => {
  const page = await staff.get('/batch');
  assert.equal(page.status, 200); assert.match(page.text, /textarea name="lines"/); assert.match(page.text, /Сканируйте штрихкоды прямо сюда/);
  assert.match((await staff.get('/stock')).text, /href="\/batch"/);
  assert.match((await staff.get('/operation')).text, /href="\/batch"/);
  assert.equal((await viewer.get('/batch')).location, '/');
  assert.ok(!(await viewer.get('/stock')).text.includes('/batch'));
  assert.equal((await viewer.post('/batch/apply', form({ lines: 'NK-AF1-42' }), { from: '/stock' })).location, '/');
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 10);
  assert.equal((await site.client().get('/batch')).status, 302);
});

test('проверка показывает, что изменится, и ничего не пишет', async () => {
  const r = await post(staff, '/batch/preview', { ...form({ lines: '4006381333931\n4006381333931\nNK-AF1-42 2\nmkt-nb 3\n036000291452\n' }), token: tokenOf((await staff.get('/batch')).text) });
  assert.equal(r.status, 200);
  assert.match(r.text, /Списание: 2 поз\., 8 шт, склад «Склад А»/);
  assert.match(r.text, /Выполнить: списание 2 поз\., 8 шт/);
  const nk = /NK-AF1-42<\/a><\/td><td>Nike AF1 42<\/td>\s*<td class="num">10<\/td><td class="num neg"><b>−4<\/b><\/td>\s*<td class="num ">6<\/td><td class="muted">1, 2, 3<\/td>/.test(r.text);
  assert.ok(nk, 'NK: было 10, −4 (две скана + 2), станет 6, строки 1, 2, 3');
  assert.match(r.text, /<td class="num">4<\/td><td class="num neg"><b>−4<\/b><\/td>\s*<td class="num ">0<\/td><td class="muted">4, 5<\/td>/);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 10); assert.equal(await stockOf('NB-373-43', 'Склад А'), 4);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_movements'))[0].n, 0);
});

test('списание списком: всё одним действием, журнал по каждой позиции, повтор той же формы не выполняется дважды', async () => {
  const page = await staff.get('/batch');
  const token = tokenOf(page.text);
  const f = { ...form({ lines: '4006381333931\nNK-AF1-42 2\nmkt-nb 3\nAD-S-40 6' }), token };
  const r = await post(staff, '/batch/apply', f);
  assert.equal(r.status, 302); assert.match(r.location, /^\/documents\/\d+$/, 'после пакета открывается созданный документ');
  const done = decodeURIComponent((await staff.follow(r)).text);
  assert.match(done, /Списание: 3 поз\., 12 шт, склад «Склад А»/); assert.match(done, /Создан документ №1/);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 7); assert.equal(await stockOf('NB-373-43', 'Склад А'), 1); assert.equal(await stockOf('AD-S-40', 'Склад А'), 0);
  const mv = await q("SELECT p.sku, m.type, m.delta, m.comment FROM stock_movements m JOIN products p ON p.id = m.product_id ORDER BY p.sku");
  assert.deepEqual(mv, [{ sku: 'AD-S-40', type: 'writeoff', delta: -6, comment: 'отгрузка №7' }, { sku: 'NB-373-43', type: 'writeoff', delta: -3, comment: 'отгрузка №7' }, { sku: 'NK-AF1-42', type: 'writeoff', delta: -3, comment: 'отгрузка №7' }]);
  // повторная отправка той же формы (двойной щелчок / "назад" и ещё раз)
  const again = await post(staff, '/batch/apply', f);
  assert.equal(again.status, 302); assert.equal(again.location, '/movements');
  assert.match(decodeURIComponent((await staff.follow(again)).text), /уже выполнена/);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 7, 'второй раз не списалось');
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_movements'))[0].n, 3);
});

test('одновременная двойная отправка: выполняется один раз', async () => {
  await site.db.query('UPDATE stock SET quantity = 50 WHERE product_id = $1', [ids['NK-AF1-42']]);
  const token = tokenOf((await staff.get('/batch')).text);
  const csrf = await staff.csrf('/batch');
  const f = { ...form({ lines: 'NK-AF1-42 5', comment: 'двойной' }), token, _csrf: csrf };
  const res = await Promise.all([1, 2, 3].map(() => staff.req('POST', '/batch/apply', { form: f })));
  assert.ok(res.every((r) => r.status === 302));
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 45, 'только одно списание');
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM stock_movements WHERE comment = 'двойной'"))[0].n, 1);
});

test('ошибки: ничего не меняется, показаны все проблемные строки', async () => {
  await site.db.query('UPDATE stock SET quantity = 5 WHERE product_id = $1', [ids['NK-AF1-42']]);
  await site.db.query('UPDATE stock SET quantity = 2 WHERE product_id = $1', [ids['NB-373-43']]);
  const snap = async () => JSON.stringify([await q('SELECT product_id, warehouse_id, quantity FROM stock ORDER BY 1, 2'), (await q('SELECT COUNT(*)::int AS n FROM stock_movements'))[0].n]);
  const before = await snap();
  const token = tokenOf((await staff.get('/batch')).text);
  const bad = await post(staff, '/batch/apply', { ...form({ lines: 'NK-AF1-42 1\nNB-373-43 9\nNOPE-1 2\nAD-S 1\nX abc\n' }), token });
  assert.equal(bad.status, 409);
  assert.match(bad.text, /Ничего не выполнено/);
  assert.match(bad.text, /строка 2:<\/b> NB-373-43: на складе 2 шт, а списать нужно 9 шт/);
  assert.match(bad.text, /строка 3:<\/b> Товар «NOPE-1» не найден/);
  assert.match(bad.text, /строка 4:<\/b> Товар «AD-S» не найден: нужен артикул или штрихкод целиком/);
  assert.match(bad.text, /строка 5:<\/b> Товар «X abc» не найден/);
  assert.ok(!bad.text.includes('Выполнить: списание'), 'кнопки выполнения нет, пока есть ошибки');
  assert.equal(await snap(), before, 'ни одна строка не применилась, даже правильная');
  // после исправления та же форма (та же метка) срабатывает
  const ok = await post(staff, '/batch/apply', { ...form({ lines: 'NK-AF1-42 1\nNB-373-43 2' }), token: tokenOf(bad.text) });
  assert.equal(ok.status, 302); assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 4); assert.equal(await stockOf('NB-373-43', 'Склад А'), 0);
  // пустой список, неверный склад/действие
  assert.match((await post(staff, '/batch/preview', { ...form({ lines: '  \n' }), token: 'x' })).text, /Список пуст/);
  assert.equal((await post(staff, '/batch/apply', { ...form({ lines: 'NK-AF1-42', warehouse: '999' }), token: 'x' })).status, 400);
  assert.equal((await post(staff, '/batch/apply', { ...form({ lines: 'NK-AF1-42', kind: 'transfer' }), token: 'x' })).status, 400);
  assert.equal((await post(staff, '/batch/preview', { ...form({ lines: 'NK-AF1-42', warehouse: '' }), token: 'x' })).status, 400);
  // чужая метка не подходит
  const foreign = tokenOf((await viewer.get('/stock')).text.includes('navsearch') ? (await staff.get('/batch')).text : '');
  assert.ok(foreign);
  const admin = site.client(); await site.addUser('boss2', 'bosspass2', true); await admin.login('boss2', 'bosspass2');
  const stolen = await post(admin, '/batch/apply', { ...form({ lines: 'NK-AF1-42' }), token: foreign });
  assert.match(decodeURIComponent((await admin.follow(stolen)).text), /уже выполнена или форма устарела/);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 4);
});

test('приход списком: новые строки остатка, складывает повторы, другой склад, большие списки', async () => {
  const b = ids['Склад Б'];
  const r = await post(staff, '/batch/apply', { ...form({ kind: 'receipt', warehouse: String(b), comment: 'поставка', lines: 'NK-AF1-42 10\n4006381333931 5\nAD-S-41\nAD-S-41\nAD-S-41 3\n' }), token: tokenOf((await staff.get('/batch')).text) });
  assert.equal(r.status, 302);
  assert.match(decodeURIComponent((await staff.follow(r)).text), /Приход: 2 поз\., 20 шт, склад «Склад Б»/);
  assert.equal(await stockOf('NK-AF1-42', 'Склад Б'), 15); assert.equal(await stockOf('AD-S-41', 'Склад Б'), 5);
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 4, 'другой склад не тронут');
  assert.deepEqual((await q("SELECT type, delta FROM stock_movements WHERE comment = 'поставка' AND product_id = $1", [ids['AD-S-41']])), [{ type: 'receipt', delta: 5 }]);
  // 1500 строк (по скану на строку) одним документом
  const lines = Array.from({ length: 1500 }, (_, i) => (i % 3 === 0 ? '4006381333931' : i % 3 === 1 ? 'NK-AF1-42 2' : 'AD-S-40')).join('\n');
  const t0 = Date.now();
  const big = await post(staff, '/batch/apply', { ...form({ kind: 'receipt', warehouse: String(b), comment: 'много', lines }), token: tokenOf((await staff.get('/batch')).text) });
  assert.equal(big.status, 302); assert.ok(Date.now() - t0 < 20000);
  assert.equal(await stockOf('NK-AF1-42', 'Склад Б'), 15 + 500 + 1000); assert.equal(await stockOf('AD-S-40', 'Склад Б'), 500);
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM stock_movements WHERE comment = 'много'"))[0].n, 2, 'повторы сложены: 2 движения вместо 1500');
  // слишком много строк
  const huge = await post(staff, '/batch/preview', { ...form({ kind: 'receipt', lines: 'NK-AF1-42\n'.repeat(2100) }), token: 'x' });
  assert.match(huge.text, /Слишком много строк/);
  // XSS в списке и комментарии
  const xss = await post(staff, '/batch/preview', { ...form({ comment: '<script>alert(1)</script>', lines: '<img src=x onerror=alert(1)> 2' }), token: 'x' });
  assert.ok(!xss.text.includes('<script>alert(1)') && !xss.text.includes('<img src=x'));
  assert.match(xss.text, /&lt;img src=x onerror=alert\(1\)&gt;/);
});

test('параллельные списания: сумма не уходит ниже нуля', async () => {
  await site.db.query('UPDATE stock SET quantity = 10 WHERE product_id = $1 AND warehouse_id = $2', [ids['NK-AF1-42'], ids['Склад А']]);
  const csrf = await staff.csrf('/batch');
  const tokens = [];
  for (let i = 0; i < 6; i++) tokens.push(tokenOf((await staff.get('/batch')).text));
  const res = await Promise.all(tokens.map((token, i) => staff.req('POST', '/batch/apply', { form: { _csrf: csrf, ...form({ lines: 'NK-AF1-42 3', comment: `p${i}` }), token } })));
  const okCount = res.filter((r) => r.status === 302).length;
  assert.equal(okCount, 3, 'из шести списаний по 3 при остатке 10 проходят ровно три');
  assert.equal(await stockOf('NK-AF1-42', 'Склад А'), 1);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock WHERE quantity < 0'))[0].n, 0);
});
