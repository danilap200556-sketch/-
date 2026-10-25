'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const xlsx = require('../src/xlsx');
const imp = require('../src/importer');
const ops = require('../src/stock');
const { startSite } = require('./helpers');

// «Обнулить то, чего нет в файле» при загрузке остатков (режим «фактический остаток»): те же правила, что в приложении
// (src/importtab.cpp): склады берутся из файла, товар со строкой в файле не трогается, обнуление - отдельный документ.
let site; let staff; let viewer;
const ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const stockOf = async (sku, wh) => (await q(`SELECT COALESCE((SELECT s.quantity FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id WHERE p.sku = $1 AND w.name = $2), -1)::int AS n`, [sku, wh]))[0].n;
const R = imp.ROLES;
const HEAD = ['Артикул', 'Название', 'Склад', 'Остаток'];
const ROLES4 = [R.key, R.name, R.warehouse, R.stock];

async function reset() {
  await site.db.query('DELETE FROM stock_movements'); await site.db.query('DELETE FROM stock_documents'); await site.db.query('DELETE FROM stock');
  await site.db.query(`INSERT INTO stock (product_id, warehouse_id, quantity) SELECT p.id, w.id, v.q FROM (VALUES
    ('A1','Склад А',10),('A2','Склад А',5),('A3','Склад А',7),('A2','Склад Б',3),('A3','Склад Б',4),('A5','Склад А',9),('A6','Склад В',6)) v(sku, wh, q)
    JOIN products p ON p.sku = v.sku JOIN warehouses w ON w.name = v.wh`);
}
const run = (rows, extra = {}) => imp.run(site.db, {
  rows: [HEAD, ...rows], headerIdx: 0, roles: ROLES4, warehouseId: ids['Склад А'], mode: 'inventory', createMissing: true,
  zeroMissing: true, fileName: 'f.xlsx', user: 'staff', ...extra,
});

test.before(async () => {
  site = await startSite();
  await site.addUser('boss', 'bosspass1', true); await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  staff = site.client(); viewer = site.client();
  await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад Б'), ('Склад В')");
  for (const w of await q('SELECT id, name FROM warehouses')) ids[w.name] = w.id;
  await site.db.query("INSERT INTO products (sku, name) VALUES ('A1','Один'),('A2','Два'),('A3','Три'),('A4','Четыре'),('A5','Пять'),('A6','Шесть')");
});
test.after(() => site.stop());

test('обнуление: склады берутся из файла, строка с ошибкой и склад вне файла не затрагиваются, отдельный документ', async () => {
  await reset();
  const r = await run([['A1', 'Один', 'Склад А', '8'], ['A2', 'Два', 'Склад Б', '3'], ['A5', 'Пять', 'Склад А', 'abc'], ['A1', 'Один', 'Склад Нет', '1']]);
  assert.equal(r.stats.zeroed, 3);
  assert.equal(await stockOf('A1', 'Склад А'), 8, 'значение из файла');
  assert.equal(await stockOf('A2', 'Склад А'), 0, 'A2 на складе А в файле нет -> 0');
  assert.equal(await stockOf('A3', 'Склад А'), 0); assert.equal(await stockOf('A3', 'Склад Б'), 0, 'A3 в файле нет -> 0 на обоих складах из файла');
  assert.equal(await stockOf('A2', 'Склад Б'), 3, 'A2 на Б есть в файле');
  assert.equal(await stockOf('A5', 'Склад А'), 9, 'у A5 в файле нечитаемое число: остаток остаётся (ошибка - не повод обнулять)');
  assert.equal(await stockOf('A6', 'Склад В'), 6, 'склад В в файле не упомянут');
  const log = r.log.join('\n');
  assert.match(log, /Обнулено: «A3», склад «Склад А» \(было 7\)/);
  assert.match(log, /обнулено позиций 3/);
  assert.match(log, /Обнуление записано отдельным документом №\d+/);
  assert.ok(r.doc && r.zeroDoc && r.zeroDoc !== r.doc, 'два разных документа');
  const docs = await q('SELECT id, kind, comment, username FROM stock_documents ORDER BY id');
  assert.deepEqual(docs.map((d) => [d.kind, d.comment, d.username]), [
    ['inventory', 'импорт из f.xlsx', 'staff'], ['inventory', 'импорт из f.xlsx: нет в файле', 'staff']]);
  const zeroLines = await q(`SELECT m.id FROM stock_movements m WHERE m.doc_id = (SELECT doc_key FROM stock_documents WHERE id = $1) ORDER BY m.id`, [r.zeroDoc]);
  assert.equal(zeroLines.length, 3);
  // отмена документа обнуления возвращает остатки, сама загрузка остаётся
  await ops.cancel(site.db, zeroLines.map((x) => x.id), 'проверка', 'staff');
  assert.equal(await stockOf('A3', 'Склад А'), 7); assert.equal(await stockOf('A3', 'Склад Б'), 4); assert.equal(await stockOf('A2', 'Склад А'), 5);
  assert.equal(await stockOf('A1', 'Склад А'), 8);
});

test('обнуление: без колонки «Склад» - только выбранный склад; без галочки и в других режимах - ничего', async () => {
  await reset();
  const rows = [['Артикул', 'Название', 'Остаток'], ['A2', 'Два', '2']];
  const base = { rows, headerIdx: 0, roles: [R.key, R.name, R.stock], warehouseId: ids['Склад Б'], mode: 'inventory', createMissing: true, fileName: 'g.xlsx', user: 'staff' };
  const r = await imp.run(site.db, { ...base, zeroMissing: true });
  assert.equal(await stockOf('A3', 'Склад Б'), 0); assert.equal(await stockOf('A2', 'Склад Б'), 2);
  assert.equal(await stockOf('A1', 'Склад А'), 10); assert.equal(await stockOf('A3', 'Склад А'), 7); assert.equal(await stockOf('A5', 'Склад А'), 9);
  assert.equal(r.stats.zeroed, 1);

  await reset();
  const off = await imp.run(site.db, { ...base, zeroMissing: false });
  assert.equal(off.stats.zeroed, 0); assert.equal(await stockOf('A3', 'Склад Б'), 4); assert.equal(off.zeroDoc, null);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_documents'))[0].n, 1, 'без галочки - один документ, как раньше');

  await reset();
  const rec = await imp.run(site.db, { ...base, mode: 'receipt', zeroMissing: true });
  assert.equal(rec.stats.zeroed, 0); assert.equal(await stockOf('A3', 'Склад Б'), 4, 'приход: галочка не действует');
  await reset();
  const wo = await imp.run(site.db, { ...base, mode: 'writeoff', zeroMissing: true });
  assert.equal(wo.stats.zeroed, 0); assert.equal(await stockOf('A3', 'Склад Б'), 4, 'списание: галочка не действует');
});

test('обнуление: если ни один товар файла не найден - не обнуляем (скорее всего не те колонки); если всё есть - нечего обнулять', async () => {
  await reset();
  const bad = await run([['ZZ-1', 'Чужой', 'Склад А', '5'], ['ZZ-2', 'Чужой 2', 'Склад А', '6']]);
  assert.equal(bad.stats.zeroed, 0);
  assert.equal(await stockOf('A1', 'Склад А'), 10); assert.equal(await stockOf('A3', 'Склад А'), 7);
  assert.match(bad.log.join('\n'), /Обнуление пропущено: ни один товар из файла не найден/);
  await reset();
  const all = await run([['A1', '', 'Склад А', '1'], ['A2', '', 'Склад А', '1'], ['A3', '', 'Склад А', '1'], ['A5', '', 'Склад А', '1']]);
  assert.equal(all.stats.zeroed, 0); assert.equal(all.zeroDoc, null);
  assert.match(all.log.join('\n'), /Обнулять нечего/);
});

test('обнуление: товар находится и по штрихкоду; строка «Итого» и пустые строки не мешают', async () => {
  await reset();
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) SELECT '4006381333931', id FROM products WHERE sku = 'A2' ON CONFLICT DO NOTHING");
  const rows = [HEAD.concat('Штрихкод'), ['', '', 'Склад А', '4', '4006381333931'], ['A1', '', 'Склад А', '10', ''], ['', '', '', '', ''], ['Итого', '', 'Склад А', '999', '']];
  const r = await imp.run(site.db, { rows, headerIdx: 0, roles: [...ROLES4, R.barcode], warehouseId: ids['Склад А'], mode: 'inventory', createMissing: true, zeroMissing: true, fileName: 'h.xlsx', user: 'staff' });
  assert.equal(await stockOf('A2', 'Склад А'), 4, 'A2 найден по штрихкоду');
  assert.equal(await stockOf('A3', 'Склад А'), 0, 'A3 нет в файле');
  assert.equal(await stockOf('A5', 'Склад А'), 0);
  assert.equal(r.stats.zeroed, 2);
});

// ------------------------------------------------------------------ по HTTP: подтверждение
const upload = (client, buf, name) => client.multipart('/import/upload', {}, [{ name, data: buf, field: 'file' }], { from: '/import' });
const file = (rows) => xlsx.write([{ name: 'Остатки', rows: [HEAD, ...rows] }]);
const formFor = (extra = {}) => ({ header: '1', warehouse: String(ids['Склад А']), mode: 'inventory', create_missing: '1', zero: '1', role_0: String(R.key), role_1: String(R.name), role_2: String(R.warehouse), role_3: String(R.stock), ...extra });

test('сайт: на странице сопоставления есть галочка; сначала показываем, что обнулится, и только потом загружаем', async () => {
  await reset();
  const up = await upload(staff, file([['A1', 'Один', 'Склад А', '8'], ['A2', 'Два', 'Склад Б', '3']]), 'z.xlsx');
  const id = up.location.split('/').pop();
  const mapPage = (await staff.get(up.location)).text;
  assert.match(mapPage, /name="zero" value="1"/); assert.match(mapPage, /Обнулить остатки товаров, которых нет в файле/);

  const ask = await staff.post(`/import/run/${id}`, formFor(), { from: up.location });
  assert.equal(ask.status, 200);
  assert.match(ask.text, /Товаров, которых нет в файле, но которые сейчас на складе: <b>3<\/b> \(позиций 4, всего 25 шт\.\)/);
  assert.match(ask.text, /Склад А, Склад Б/); assert.match(ask.text, /<td>A3<\/td><td>Склад А<\/td><td class="num">7<\/td>/); assert.match(ask.text, /<td>A5<\/td><td>Склад А<\/td><td class="num">9<\/td>/);
  assert.match(ask.text, /name="confirm" value="1"/); assert.match(ask.text, /name="role_3" value="\d+"/);
  assert.equal(await stockOf('A1', 'Склад А'), 10, 'до подтверждения ничего не изменено');
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_documents'))[0].n, 0);

  // подтверждение: те же поля + confirm=1
  const hidden = Object.fromEntries([...ask.text.matchAll(/<input type="hidden" name="([^"]+)" value="([^"]*)">/g)].map((m) => [m[1], m[2]]));
  assert.equal(hidden.zero, '1'); assert.equal(hidden.confirm, '1'); assert.equal(hidden.mode, 'inventory');
  const done = await staff.post(`/import/run/${id}`, hidden, { from: up.location });
  assert.equal(done.status, 200);
  const log = decodeURIComponent(done.text.match(/<pre class="log">([\s\S]*?)<\/pre>/)?.[1] ?? '').replace(/&quot;/g, '"').replace(/&amp;/g, '&');
  assert.match(log, /обнулено позиций 4/); assert.match(log, /Обнуление записано отдельным документом №\d+/);
  assert.equal(await stockOf('A1', 'Склад А'), 8); assert.equal(await stockOf('A3', 'Склад А'), 0); assert.equal(await stockOf('A5', 'Склад А'), 0); assert.equal(await stockOf('A2', 'Склад Б'), 3);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_documents'))[0].n, 2);
  const repeat = await staff.post(`/import/run/${id}`, hidden, { from: '/import' });
  assert.equal(repeat.status, 302, 'повторная отправка не задваивает');
});

test('сайт: «назад» оставляет всё как есть; нечего обнулять - загрузка сразу; другие режимы без вопроса; просмотр запрещён', async () => {
  await reset();
  const up = await upload(staff, file([['A1', 'Один', 'Склад А', '8']]), 'y.xlsx');
  const id = up.location.split('/').pop();
  const ask = await staff.post(`/import/run/${id}`, formFor(), { from: up.location });
  assert.match(ask.text, /name="confirm"/);
  assert.equal(await stockOf('A3', 'Склад А'), 7);
  const back = await staff.get(`/import/map/${id}`);
  assert.equal(back.status, 200, 'файл ещё ждёт: можно вернуться и передумать');

  // нечего обнулять - без вопроса
  await reset();
  const up2 = await upload(staff, file([['A1', '', 'Склад А', '1'], ['A2', '', 'Склад А', '1'], ['A3', '', 'Склад А', '1'], ['A5', '', 'Склад А', '1']]), 'x.xlsx');
  const r2 = await staff.post(`/import/run/${up2.location.split('/').pop()}`, formFor(), { from: up2.location });
  assert.equal(r2.status, 200); assert.doesNotMatch(r2.text, /name="confirm"/); assert.match(r2.text, /Обнулять нечего/);

  // приход - без вопроса и без обнуления
  await reset();
  const up3 = await upload(staff, file([['A1', '', 'Склад А', '1']]), 'w.xlsx');
  const r3 = await staff.post(`/import/run/${up3.location.split('/').pop()}`, formFor({ mode: 'receipt' }), { from: up3.location });
  assert.equal(r3.status, 200); assert.doesNotMatch(r3.text, /name="confirm"/);
  assert.equal(await stockOf('A3', 'Склад А'), 7); assert.equal(await stockOf('A1', 'Склад А'), 11);

  // неверные колонки: ошибка как раньше, остатки целы
  await reset();
  const up4 = await upload(staff, file([['A1', '', 'Склад А', '1']]), 'v.xlsx');
  const r4 = await staff.post(`/import/run/${up4.location.split('/').pop()}`, formFor({ role_0: '0', role_3: '0' }), { from: up4.location });
  assert.equal(r4.status, 400); assert.equal(await stockOf('A3', 'Склад А'), 7);

  assert.equal((await viewer.get('/import')).status, 200);
  const upv = await upload(viewer, file([['A1', '', 'Склад А', '1']]), 'u.xlsx');
  assert.notEqual(upv.status, 302 && /\/import\/map\//.test(upv.location || '') ? 302 : 0, 'просмотр не может загрузить файл');
});
