'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const xlsx = require('../src/xlsx');
const st = require('../src/stocktake');
const ops = require('../src/stock');
const { startSite } = require('./helpers');

// Инвентаризация по штрихкодам: те же правила, что в приложении (src/stocktake.cpp) - сканы складываются в общий подсчёт,
// при завершении остаток = посчитанному, а чего в подсчёте нет, у того 0; всё одним документом «Инвентаризация».
let site; let staff; let viewer;
const ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const stockOf = async (sku, wh) => (await q(`SELECT COALESCE((SELECT s.quantity FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id WHERE p.sku = $1 AND w.name = $2), -1)::int AS n`, [sku, wh]))[0].n;
const pid = async (sku) => (await q('SELECT id FROM products WHERE sku = $1', [sku]))[0].id;

async function reset() {
  await site.db.query('DELETE FROM stocktakes'); await site.db.query('DELETE FROM stock_movements'); await site.db.query('DELETE FROM stock_documents'); await site.db.query('DELETE FROM stock');
  await site.db.query(`INSERT INTO stock (product_id, warehouse_id, quantity) SELECT p.id, w.id, v.q FROM (VALUES
    ('P1','Склад А',10),('P2','Склад А',5),('P3','Склад А',7),('P4','Склад Б',3)) v(sku, wh, q)
    JOIN products p ON p.sku = v.sku JOIN warehouses w ON w.name = v.wh`);
}
const startCount = async (client, wh = 'Склад А') => {
  const r = await client.post('/stocktake/start', { warehouse: String(ids[wh]) }, { from: '/stocktake' });
  assert.equal(r.status, 302, r.text.slice(0, 200));
  assert.match(r.location, /^\/stocktake\/\d+$/);
  return Number(r.location.split('/').pop());
};
// скан и страница после него (там показывается сообщение о скане)
const scan = async (client, id, code, view = '') => {
  const r = await client.post(`/stocktake/${id}/scan`, { code, ...(view ? { view } : {}) }, { from: `/stocktake/${id}` });
  assert.equal(r.status, 302, `скан «${code}»: ${r.status}`);
  const page = await client.get(r.location);
  assert.equal(page.status, 200);
  return page.text;
};
const unesc = (t) => t.replace(/&quot;/g, '"').replace(/&amp;/g, '&').replace(/&#39;/g, "'");

test.before(async () => {
  site = await startSite();
  await site.addUser('boss', 'bosspass1', true); await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  staff = site.client(); viewer = site.client();
  await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад Б')");
  for (const w of await q('SELECT id, name FROM warehouses')) ids[w.name] = w.id;
  await site.db.query("INSERT INTO products (sku, name, market_sku) VALUES ('P1','Первый','MK-1'),('P2','Второй',NULL),('P3','Третий',NULL),('P4','Четвёртый',NULL),('NK-AF1-42','Nike AF1 42',NULL),('NK-AF1-43','Nike AF1 43',NULL)");
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) SELECT '4006381333931', id FROM products WHERE sku = 'P1'");
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) SELECT '4600702084030', id FROM products WHERE sku = 'P2'");
  await reset();
});
test.after(() => site.stop());

test('доступ: редактор видит раздел, просмотр - нет', async () => {
  const page = await staff.get('/stocktake');
  assert.equal(page.status, 200);
  assert.match(page.text, /Инвентаризация/); assert.match(page.text, /Начать подсчёт/);
  assert.match((await staff.get('/stock')).text, /href="\/stocktake"/, 'пункт меню');
  assert.doesNotMatch((await viewer.get('/stock')).text, /href="\/stocktake"/, 'у просмотра пункта нет');
  const v = await viewer.get('/stocktake');
  assert.equal(v.status, 302, 'страница закрыта для просмотра');
  assert.equal((await viewer.post('/stocktake/start', { warehouse: String(ids['Склад А']) }, { from: '/account' })).status, 302);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stocktakes'))[0].n, 0, 'просмотр не начал подсчёт');
  assert.equal((await site.client().get('/stocktake')).status, 302, 'без входа - нет');
});

test('подсчёт: начать, сканировать штрихкоды, «артикул количество», правка, виды, итоги', async () => {
  await reset();
  const id = await startCount(staff);
  assert.equal(await startCount(staff), id, 'второй раз - тот же открытый подсчёт');
  assert.match((await staff.get('/stocktake')).text, new RegExp(`/stocktake/${id}`), 'в списке есть «Продолжить»');

  let page = unesc(await scan(staff, id, '4006381333931'));
  assert.match(page, /P1 — Первый: посчитано 1 \(в учёте 10\)/, 'сообщение о скане');
  await scan(staff, id, '4006381333931'); page = unesc(await scan(staff, id, '4006381333931'));
  assert.match(page, /посчитано 3/);
  page = unesc(await scan(staff, id, 'P2 4'));
  assert.match(page, /P2 — Второй: посчитано 4 \(в учёте 5\)/, '«артикул количество»');
  page = unesc(await scan(staff, id, 'mk-1'));
  assert.match(page, /P1 — Первый: посчитано 4/, 'артикул на Маркете, регистр не важен');
  page = unesc(await scan(staff, id, 'af1-43'));
  assert.match(page, /NK-AF1-43 — Nike AF1 43: посчитано 1 \(в учёте 0\)/, 'часть артикула: подходит один товар - берём его');
  page = unesc(await scan(staff, id, 'nike af1'));
  assert.match(page, /подходит несколько товаров \(NK-AF1-42, NK-AF1-43\)/, 'подходит несколько - ничего не считаем');
  page = unesc(await scan(staff, id, '5901234123457'));
  assert.match(page, /Штрихкод 5901234123457 не привязан ни к одному товару/, 'неизвестный штрихкод');
  page = unesc(await scan(staff, id, 'нет-такого'));
  assert.match(page, /не найден/, 'мусор');
  page = unesc(await scan(staff, id, 'P2 0'));
  assert.match(page, /количество должно быть больше нуля/);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stocktake_items'))[0].n, 3, 'посчитано только 3 товара: P1, P2, NK-AF1-43');
  assert.equal(await stockOf('P1', 'Склад А'), 10, 'остатки при подсчёте не меняются');

  const view = unesc((await staff.get(`/stocktake/${id}`)).text);
  assert.match(view, /<b>Посчитано:<\/b> 3 поз\., 9 шт/);
  assert.match(view, /Расхождений:<\/b> 3 \(излишек 1 шт, недостача 7 шт\)/);
  assert.match(view, /Не посчитано, но числится на складе:<\/b> 1 поз\., 7 шт/, 'P3 лежит на складе, но не посчитан');
  assert.ok(view.indexOf('>NK-AF1-43<') > 0 && view.indexOf('>NK-AF1-43<') < view.indexOf('>P1<') && view.indexOf('>P1<') < view.indexOf('>P2<'), 'последний скан сверху: NK-AF1-43, P1 (повторный скан), P2');

  // правка строки
  const p1 = await pid('P1'); const p3 = await pid('P3');
  const row = (body) => staff.post(`/stocktake/${id}/row`, body, { from: `/stocktake/${id}` });
  assert.equal((await row({ product: String(p1), do: 'set', counted: '9' })).status, 302);
  assert.equal((await q('SELECT counted FROM stocktake_items WHERE product_id = $1', [p1]))[0].counted, 9);
  await row({ product: String(p1), do: 'inc' }); await row({ product: String(p1), do: 'dec' }); await row({ product: String(p1), do: 'dec' });
  assert.equal((await q('SELECT counted FROM stocktake_items WHERE product_id = $1', [p1]))[0].counted, 8, '+1, −1, −1');
  const bad = await row({ product: String(p1), do: 'set', counted: '-3' });
  assert.match(unesc((await staff.get(bad.location)).text), /целое число/, 'неверное число');
  assert.equal((await q('SELECT counted FROM stocktake_items WHERE product_id = $1', [p1]))[0].counted, 8);
  await row({ product: String(await pid('NK-AF1-43')), do: 'remove' });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stocktake_items'))[0].n, 2, 'товар убран из подсчёта');

  // виды
  const diff = unesc((await staff.get(`/stocktake/${id}?view=diff`)).text);
  assert.match(diff, />P1</); assert.match(diff, />P2</);
  const missing = unesc((await staff.get(`/stocktake/${id}?view=missing`)).text);
  assert.match(missing, />P3</); assert.doesNotMatch(missing, />P1</, 'посчитанного в «не посчитано» нет');
  await row({ product: String(p3), do: 'inc', view: 'missing' });
  assert.equal((await q('SELECT counted FROM stocktake_items WHERE product_id = $1', [p3]))[0].counted, 1, '+1 из «не посчитано»');
  assert.equal((await staff.post(`/stocktake/${id}/scan`, { code: 'P1', view: 'diff' }, { from: `/stocktake/${id}` })).location, `/stocktake/${id}?view=diff`, 'после скана возвращаемся в тот же вид');
});

test('завершение: сначала «что изменится», потом одним документом; чего нет в подсчёте - 0; повторно нельзя', async () => {
  await reset();
  const id = await startCount(staff);
  await scan(staff, id, 'P1 8'); await scan(staff, id, '4600702084030 5');
  const ask = await staff.post(`/stocktake/${id}/finish`, { zero: '1' }, { from: `/stocktake/${id}` });
  assert.equal(ask.status, 200);
  const t = unesc(ask.text);
  assert.match(t, /Посчитано: <b>2<\/b> поз\., 13 шт/); assert.match(t, /Не посчитано, но числится на складе: <b>1<\/b> поз\., 7 шт - <b>их остаток станет 0<\/b>/);
  assert.match(t, /<td>P3<\/td>/); assert.match(t, /name="confirm" value="1"/);
  assert.equal(await stockOf('P1', 'Склад А'), 10, 'до подтверждения ничего не изменено');
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_documents'))[0].n, 0);

  const done = await staff.post(`/stocktake/${id}/finish`, { zero: '1', confirm: '1' }, { from: `/stocktake/${id}` });
  assert.equal(done.status, 302);
  assert.match(done.location, /^\/documents\/\d+$/, 'после проведения - документ');
  const docNo = Number(done.location.split('/').pop());
  assert.equal(await stockOf('P1', 'Склад А'), 8); assert.equal(await stockOf('P2', 'Склад А'), 5); assert.equal(await stockOf('P3', 'Склад А'), 0, 'P3 не посчитан -> 0');
  assert.equal(await stockOf('P4', 'Склад Б'), 3, 'другой склад не тронут');
  const doc = (await q('SELECT id, kind, comment, username FROM stock_documents'))[0];
  assert.deepEqual([doc.id, doc.kind, doc.username], [docNo, 'inventory', 'staff']);
  assert.match(doc.comment, /^инвентаризация склада «Склад А» №\d+$/);
  const lines = await q("SELECT p.sku, m.delta, m.type, m.comment FROM stock_movements m JOIN products p ON p.id = m.product_id WHERE m.doc_id = (SELECT doc_key FROM stock_documents) ORDER BY p.sku");
  assert.deepEqual(lines.map((l) => [l.sku, l.delta, l.type]), [['P1', -2, 'inventory_adjust'], ['P3', -7, 'inventory_adjust']], 'в журнале только расхождения');
  assert.match(lines[0].comment, /\(инвентаризация: было 10, стало 8\)$/);
  const info = await st.get(site.db, id);
  assert.equal(info.status, 'done'); assert.equal(info.document_id, docNo);
  assert.match(unesc((await staff.get(done.location)).text), /Инвентаризация проведена: остаток изменён у 2 товаров/);

  // повторно и в закрытый подсчёт
  const again = await staff.post(`/stocktake/${id}/finish`, { zero: '1', confirm: '1' }, { from: '/stocktake' });
  assert.equal(again.status, 302); assert.equal(again.location, '/stocktake');
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_documents'))[0].n, 1, 'второго документа нет');
  const closed = await staff.post(`/stocktake/${id}/scan`, { code: 'P1' }, { from: '/stocktake' });
  const bounced = await staff.get(closed.location);
  assert.equal(bounced.status, 302); assert.equal(bounced.location, '/stocktake', 'закрытый подсчёт не открывается');
  assert.match(unesc((await staff.get('/stocktake')).text), /Подсчёт №\d+ уже проведён \(документ №\d+\)/, 'в закрытый подсчёт не считаем');
  assert.equal((await q('SELECT counted FROM stocktake_items WHERE product_id = $1', [await pid('P1')]))[0].counted, 8);

  // документ можно отменить целиком
  const ids2 = (await q('SELECT id FROM stock_movements ORDER BY id')).map((r) => r.id);
  await ops.cancel(site.db, ids2, 'проверка', 'staff');
  assert.equal(await stockOf('P1', 'Склад А'), 10); assert.equal(await stockOf('P3', 'Склад А'), 7);
});

test('завершение без обнуления остального; пустой подсчёт с обнулением - отдельное предупреждение; сброс', async () => {
  await reset();
  let id = await startCount(staff);
  await scan(staff, id, 'P1 9');
  const ask = unesc((await staff.post(`/stocktake/${id}/finish`, {}, { from: `/stocktake/${id}` })).text);
  assert.match(ask, /Не посчитанные товары \(2 поз\.\) останутся как есть/);
  const done = await staff.post(`/stocktake/${id}/finish`, { confirm: '1' }, { from: `/stocktake/${id}` });
  assert.equal(done.status, 302);
  assert.equal(await stockOf('P1', 'Склад А'), 9); assert.equal(await stockOf('P2', 'Склад А'), 5); assert.equal(await stockOf('P3', 'Склад А'), 7);

  id = await startCount(staff);
  const wipe = unesc((await staff.post(`/stocktake/${id}/finish`, { zero: '1' }, { from: `/stocktake/${id}` })).text);
  assert.match(wipe, /Ничего не посчитано: если продолжить, весь склад «Склад А» будет обнулён/);
  assert.match(wipe, /Обнулить весь склад/);
  assert.equal(await stockOf('P2', 'Склад А'), 5, 'пока не подтвердили - цело');

  await scan(staff, id, 'P2 1');
  const dis = await staff.post(`/stocktake/${id}/discard`, {}, { from: `/stocktake/${id}` });
  assert.equal(dis.status, 302); assert.equal(dis.location, '/stocktake');
  assert.equal((await st.get(site.db, id)).status, 'discarded');
  assert.equal(await stockOf('P2', 'Склад А'), 5, 'сброс не меняет остатки');
  assert.equal(await st.current(site.db, ids['Склад А']), null);
  assert.equal((await staff.get(`/stocktake/${id}`)).status, 302, 'сброшенный подсчёт не открывается');
  // подсчёт, в котором всё совпало: документа нет
  await reset();
  id = await startCount(staff);
  await scan(staff, id, 'P1 10'); await scan(staff, id, 'P2 5'); await scan(staff, id, 'P3 7');
  const same = await staff.post(`/stocktake/${id}/finish`, { zero: '1', confirm: '1' }, { from: `/stocktake/${id}` });
  assert.equal(same.location, '/stocktake');
  assert.match(unesc((await staff.get('/stocktake')).text), /расхождений не было/);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_documents'))[0].n, 0);
});

test('выгрузка в Excel: посчитанное и не посчитанное', async () => {
  await reset();
  const id = await startCount(staff);
  await scan(staff, id, 'P1 8');
  const r = await staff.get(`/stocktake/${id}/export.xlsx`);
  assert.equal(r.status, 200);
  assert.equal(r.headers.get('content-type'), 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet');
  const rows = xlsx.readFirstSheet(r.raw).rows;
  assert.deepEqual(rows[0], ['Артикул', 'Название', 'В учёте', 'Посчитано', 'Разница', 'Примечание']);
  assert.deepEqual(rows.slice(1).map((x) => x.join('|')), ['P1|Первый|10|8|-2', 'P2|Второй|5|0|-5|не посчитано', 'P3|Третий|7|0|-7|не посчитано']);
  assert.equal((await viewer.get(`/stocktake/${id}/export.xlsx`)).status, 302);
});

test('ядро: общий подсчёт (несколько пользователей), параллельное завершение проводится один раз, большой подсчёт', async () => {
  await reset();
  const id = await st.start(site.db, ids['Склад А'], 'anna');
  const p1 = await pid('P1');
  await Promise.all(Array.from({ length: 20 }, (_, i) => st.add(site.db, id, p1, 1, i % 2 ? 'anna' : 'boris')));
  assert.equal((await q('SELECT counted FROM stocktake_items WHERE product_id = $1', [p1]))[0].counted, 20, '20 одновременных сканов не потерялись');
  assert.equal((await st.current(site.db, ids['Склад А'])).started_by, 'anna');
  assert.equal((await q('SELECT updated_by FROM stocktake_items WHERE product_id = $1', [p1]))[0].updated_by.length > 0, true);

  const results = await Promise.allSettled([st.finish(site.db, id, { zeroMissing: true, user: 'a' }), st.finish(site.db, id, { zeroMissing: true, user: 'b' })]);
  assert.deepEqual(results.map((x) => x.status).sort(), ['fulfilled', 'rejected'], 'проведён один раз');
  assert.ok(results.find((x) => x.status === 'rejected').reason instanceof ops.UserError);
  assert.equal(await stockOf('P1', 'Склад А'), 20);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_documents'))[0].n, 1);

  // большой подсчёт: 3000 товаров одним документом
  await reset();
  await site.db.query("INSERT INTO products (sku, name) SELECT 'M' || g, 'Массовый ' || g FROM generate_series(1, 3000) g");
  await site.db.query(`INSERT INTO stock (product_id, warehouse_id, quantity) SELECT id, ${ids['Склад А']}, 4 FROM products WHERE sku LIKE 'M%'`);
  const big = await st.start(site.db, ids['Склад А'], 'anna');
  await site.db.query(`INSERT INTO stocktake_items (stocktake_id, product_id, counted) SELECT ${big}, id, 5 FROM products WHERE sku LIKE 'M%' AND sku <> 'M1'`);
  const t0 = Date.now();
  const res = await st.finish(site.db, big, { zeroMissing: true, user: 'anna' });
  assert.equal(res.changed, 2999 + 1 + 3, 'подняты 2999, M1 и P1..P3 обнулены');
  assert.ok(Date.now() - t0 < 15000, `проведён за ${Date.now() - t0} мс`);
  assert.equal((await q(`SELECT COUNT(*)::int AS n FROM stock WHERE warehouse_id = ${ids['Склад А']} AND quantity = 5`))[0].n, 2999);
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM stock_movements WHERE type = 'inventory_adjust'"))[0].n, 3003);
  assert.equal((await q('SELECT COUNT(DISTINCT doc_id)::int AS n FROM stock_movements'))[0].n, 1, 'все записи - один документ');
  await site.db.query("DELETE FROM products WHERE sku LIKE 'M%'");
});

test('раздел без таблиц (приложение ещё не обновлено): подсказка вместо ошибки', async () => {
  const old = await startSite({ schema: false });
  try {
    // схема без таблиц подсчёта - как в базе, которую приложение ещё не обновило
    const fs = require('fs'); const path = require('path');
    const sql = fs.readFileSync(path.join(__dirname, 'schema.sql'), 'utf8').replace(/CREATE TABLE IF NOT EXISTS stocktake[\s\S]*?\)\s*;\s*/g, '').replace(/CREATE UNIQUE INDEX IF NOT EXISTS stocktakes_one_open[^;]*;/g, '');
    await old.db.query(sql);
    assert.equal((await old.db.query("SELECT to_regclass('stocktakes') AS t")).rows[0].t, null, 'в этой базе таблиц подсчёта нет');
    await old.addUser('boss', 'bosspass1', true);
    const c = old.client(); await c.login('boss', 'bosspass1');
    st.resetFeatureCache();
    const r = await c.get('/stocktake');
    assert.equal(r.status, 302);
    assert.match(unesc((await c.get(r.location)).text), /Инвентаризация заработает после обновления настольного приложения/);
  } finally { st.resetFeatureCache(); await old.stop(); }
});
