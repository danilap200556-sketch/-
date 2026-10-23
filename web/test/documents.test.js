'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const ops = require('../src/stock');
const docs = require('../src/documents');
const batch = require('../src/batch');
const imp = require('../src/importer');
const xlsx = require('../src/xlsx');
const { startSite } = require('./helpers');

let site; let staff; let viewer;
const ids = {};
const A = 'Склад А'; const B = 'Склад Б';
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const stockOf = async (sku, wh) => (await q('SELECT COALESCE((SELECT s.quantity FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id WHERE p.sku = $1 AND w.name = $2), 0)::int AS n', [sku, wh]))[0].n;
const operation = (client, f) => client.post('/operation', { product: 'NK-1', warehouse: String(ids[A]), quantity: '1', comment: '', ...f }, { from: '/operation' });
const lastDoc = async () => (await q('SELECT * FROM stock_documents ORDER BY id DESC LIMIT 1'))[0];
const docCount = async () => (await q('SELECT COUNT(*)::int AS n FROM stock_documents'))[0].n;
const cancelDoc = (client, d, extra = {}) => client.post('/movements/cancel', {
  idlist: extra.idlist, docno: String(d.id), back: `/documents/${d.id}`, reason: extra.reason ?? '',
}, { from: '/documents' });

test.before(async () => {
  site = await startSite();
  await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  staff = site.client(); viewer = site.client();
  await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад Б')");
  for (const w of await q('SELECT id, name FROM warehouses')) ids[w.name] = w.id;
  await site.db.query("INSERT INTO products (sku, name) VALUES ('NK-1','Nike 1'),('AD-1','Adidas 1'),('NB-1','NB 1'),('<b>X</b>','XSS')");
  for (const p of await q('SELECT id, sku FROM products')) ids[p.sku] = p.id;
});
test.after(() => site.stop());

test('каждая одиночная операция сайта - документ со своим номером, видом, автором и комментарием', async () => {
  let r = await operation(staff, { kind: 'receipt', quantity: '10', comment: 'поставка' });
  assert.equal(r.status, 302);
  assert.match(decodeURIComponent((await staff.follow(r)).text), /Приход: NK-1, 10 шт.*Документ №1/);
  r = await operation(staff, { kind: 'writeoff', quantity: '4', comment: 'продажа' });
  assert.match(decodeURIComponent((await staff.follow(r)).text), /Документ №2/);
  await operation(staff, { kind: 'transfer', quantity: '3', dest: String(ids[B]), comment: 'на другой склад' });
  await operation(staff, { kind: 'inventory', quantity: '9', comment: 'пересчёт' });

  const rows = await q('SELECT id, kind, comment, username FROM stock_documents ORDER BY id');
  assert.deepEqual(rows.map((d) => [d.id, d.kind, d.comment, d.username]), [
    [1, 'receipt', 'поставка', 'staff'], [2, 'writeoff', 'продажа', 'staff'],
    [3, 'transfer', 'на другой склад', 'staff'], [4, 'inventory', 'пересчёт', 'staff'],
  ]);
  const lines = await q('SELECT d.id, COUNT(*)::int AS n FROM stock_documents d JOIN stock_movements m ON m.doc_id = d.doc_key GROUP BY d.id ORDER BY d.id');
  assert.deepEqual(lines.map((x) => [x.id, x.n]), [[1, 1], [2, 1], [3, 2], [4, 1]], 'перемещение - один документ из двух записей');
  assert.equal(await stockOf('NK-1', A), 9); assert.equal(await stockOf('NK-1', B), 3);
});

test('неудавшаяся операция документа не оставляет и номер не теряется', async () => {
  const before = await docCount();
  const r = await operation(staff, { kind: 'writeoff', quantity: '999' });
  assert.equal(r.status, 409); assert.match(r.text, /Недостаточно остатка/);
  assert.equal(await docCount(), before);
  await operation(staff, { kind: 'receipt', quantity: '1', comment: 'следом' });
  assert.equal((await lastDoc()).id, before + 1, 'следующий номер идёт без пропуска');
  await operation(staff, { kind: 'writeoff', quantity: '1', comment: 'вернуть' });
  await operation(staff, { kind: 'inventory', quantity: String(await stockOf('NK-1', A)) }); // ничего не изменилось
  assert.equal((await lastDoc()).comment, 'вернуть', 'инвентаризация без разницы документа не создаёт');
});

test('пакет - один документ: список, состав, фильтр и поиск', async () => {
  const res = await batch.apply(site.db, 'NK-1 5\nAD-1 7\nNB-1 2', 'receipt', ids[A], 'приёмка №15', 'staff');
  assert.equal(res.ok, true);
  const d = await lastDoc();
  assert.equal(d.kind, 'receipt'); assert.equal(d.username, 'staff');
  const info = await docs.get(site.db, d.id);
  assert.deepEqual([info.lines, info.positions, info.units, info.warehouses], [3, 3, 14, A]);
  assert.equal(docs.statusText(info), 'действует');

  const list = (await staff.get('/documents')).text;
  assert.match(list, new RegExp(`<a href="/documents/${d.id}">${d.id}</a>`));
  assert.match(list, /приёмка №15/); assert.match(list, /Отменить последний документ/);
  const onlyWriteoff = (await staff.get('/documents?kind=writeoff')).text;
  assert.match(onlyWriteoff, /продажа/); assert.doesNotMatch(onlyWriteoff, /приёмка №15/);
  assert.match((await staff.get('/documents?q=AD-1')).text, /приёмка №15/, 'поиск по артикулу товара внутри документа');
  assert.doesNotMatch((await staff.get('/documents?q=AD-1')).text, /продажа/);
  assert.match((await staff.get(`/documents?q=${d.id}`)).text, /приёмка №15/, 'поиск по номеру');
  assert.match((await staff.get('/documents?q=%25')).text, /Документов не найдено/, 'спецсимволы ищутся буквально');

  const page = (await staff.get(`/documents/${d.id}`)).text;
  assert.match(page, /Приход №\d+/); assert.match(page, /NK-1/); assert.match(page, /AD-1/); assert.match(page, /NB-1/);
  assert.match(page, /Отменить документ целиком/); assert.match(page, /действует/);
});

test('перемещение: «склад А → склад Б», штуки считаются один раз', async () => {
  const d = (await q("SELECT * FROM stock_documents WHERE kind = 'transfer'"))[0];
  const info = await docs.get(site.db, d.id);
  assert.equal(info.warehouses, `${A} → ${B}`); assert.equal(info.units, 3); assert.equal(info.lines, 2);
  assert.match((await staff.get('/documents?kind=transfer')).text, /Склад А → Склад Б/);
});

test('выгрузка документа в Excel и CSV', async () => {
  const d = await lastDoc();
  const x = await staff.get(`/documents/${d.id}/export.xlsx`);
  assert.equal(x.status, 200); assert.match(x.headers.get('content-type'), /spreadsheetml/);
  const t = xlsx.readFirstSheet(x.raw); assert.ok(t.ok, t.error);
  assert.equal(t.rows[0][0], 'Документ'); assert.match(t.rows[0][1], /Приход №/);
  const head = t.rows.findIndex((r) => r[0] === 'Артикул');
  assert.deepEqual(t.rows[head], ['Артикул', 'Название', 'Склад', 'Изменение, шт', 'Запись журнала', 'Отмена']);
  assert.equal(t.rows.length - head - 1, 3);
  const c = await staff.get(`/documents/${d.id}/export.csv`);
  assert.match(c.headers.get('content-type'), /text\/csv/); assert.ok(c.text.startsWith('﻿'));
  assert.match(c.text, /NK-1;Nike 1;Склад А;5;/);
  assert.equal((await staff.get('/documents/99999/export.xlsx')).location, '/documents');
});

test('отмена документа целиком: страница «что изменится», документ «Отмена», состояние', async () => {
  const d = await lastDoc();
  const before = { nk: await stockOf('NK-1', A), ad: await stockOf('AD-1', A) };
  const confirm = await staff.get(`/documents/${d.id}/cancel`);
  assert.equal(confirm.status, 200);
  assert.match(confirm.text, new RegExp(`Отменить Приход №${d.id} целиком: 3 записи`));
  assert.match(confirm.text, new RegExp(`name="docno" value="${d.id}"`));
  assert.match(confirm.text, new RegExp(`value="документ №${d.id}"`), 'причина по умолчанию - номер документа');
  const idlist = /name="idlist" value="([\d,]+)"/.exec(confirm.text)[1];
  assert.equal(idlist.split(',').length, 3);

  const r = await cancelDoc(staff, d, { idlist, reason: `документ №${d.id}` });
  assert.equal(r.status, 302); assert.equal(r.location, `/documents/${d.id}`);
  const after = decodeURIComponent((await staff.follow(r)).text);
  assert.match(after, /Отменено записей: 3 \(14 шт\)/); assert.match(after, /создан документ «Отмена» №\d+/);
  assert.equal(await stockOf('NK-1', A), before.nk - 5); assert.equal(await stockOf('AD-1', A), before.ad - 7);

  const cancelDocRow = await lastDoc();
  assert.equal(cancelDocRow.kind, 'cancel'); assert.equal(cancelDocRow.username, 'staff');
  assert.equal(cancelDocRow.comment, `Отмена: документ №${d.id}`);
  const info = await docs.get(site.db, d.id);
  assert.equal(docs.statusText(info), 'отменён'); assert.ok(docs.isCancelled(info));
  const cinfo = await docs.get(site.db, cancelDocRow.id);
  assert.equal(docs.statusText(cinfo), `отмена документа №${d.id}`);
  const page = (await staff.get(`/documents/${d.id}`)).text;
  assert.doesNotMatch(page, /Отменить документ целиком/); assert.match(page, /отменена записью №/);
  // повторно отменять нечего
  const again = await staff.get(`/documents/${d.id}/cancel`);
  assert.equal(again.location, `/documents/${d.id}`);
  assert.match(decodeURIComponent((await staff.follow(again)).text), /отменять уже нечего/);
  // документ «Отмена» отменить нельзя
  const c = await staff.get(`/documents/${cancelDocRow.id}/cancel`);
  assert.match(decodeURIComponent((await staff.follow(c)).text), /Документ «Отмена» отменить нельзя/);
});

test('частичная отмена и «отменить последний документ»', async () => {
  const res = await batch.apply(site.db, 'NK-1 2\nAD-1 3\nNB-1 4', 'receipt', ids[B], 'партия Б', 'staff');
  assert.equal(res.ok, true);
  const d = await lastDoc();
  const lineIds = (await docs.lines(site.db, d.doc_key)).map((l) => l.id);
  // одну позицию - отдельно, из журнала
  assert.equal((await staff.post('/movements/cancel', { idlist: String(lineIds[1]), back: '/movements' }, { from: '/movements' })).status, 302);
  let info = await docs.get(site.db, d.id);
  assert.equal(docs.statusText(info), 'отменён частично (1 из 3)');
  const page = (await staff.get(`/documents/${d.id}`)).text;
  assert.match(page, /Отменить документ целиком \(осталось 2 из 3\)/);
  assert.match((await staff.get('/documents')).text, /отменён частично \(1 из 3\)/);

  // последний документ, в котором есть что отменять - тот же (документ «Отмена» пропускается)
  assert.equal(await docs.lastCancellable(site.db), d.id);
  const last = await staff.get('/documents/last/cancel');
  assert.equal(last.status, 200); assert.match(last.text, new RegExp(`Отменить Приход №${d.id} целиком: 2 записи`));
  const idlist = /name="idlist" value="([\d,]+)"/.exec(last.text)[1];
  assert.deepEqual(idlist.split(',').map(Number), [lineIds[0], lineIds[2]], 'только то, что ещё не отменено');
  const r = await cancelDoc(staff, d, { idlist });
  assert.equal(r.status, 302);
  info = await docs.get(site.db, d.id);
  assert.equal(docs.statusText(info), 'отменён');
  assert.notEqual(await docs.lastCancellable(site.db), d.id, 'отменённый документ больше не «последний»');
});

test('нельзя отменить, если товар уже расходован: ничего не меняется', async () => {
  await operation(staff, { kind: 'receipt', product: 'NB-1', quantity: '10', comment: 'для проверки' });
  const receipt = await lastDoc();
  await operation(staff, { kind: 'writeoff', product: 'NB-1', quantity: '8', comment: 'расход' });
  const n = await docCount(); const stock = await stockOf('NB-1', A);
  const confirm = await staff.get(`/documents/${receipt.id}/cancel`);
  assert.match(confirm.text, /Отменить нельзя/); assert.match(confirm.text, /уже расходован/);
  assert.doesNotMatch(confirm.text, /<button class="danger-ok">/, 'кнопки подтверждения нет');
  const ids_ = (await docs.cancellableIds(site.db, receipt.doc_key)).join(',');
  const r = await cancelDoc(staff, receipt, { idlist: ids_ });
  assert.equal(r.status, 409);
  assert.equal(await docCount(), n, 'документ «Отмена» не появился'); assert.equal(await stockOf('NB-1', A), stock);
});

test('импорт - один документ, номер в отчёте; откат строки не ломает документ', async () => {
  const rows = [['Артикул', 'Остаток'], ['NK-1', '50'], ['AD-1', '60'], ['НЕТ-ТАКОГО', '5'], ['NB-1', '70']];
  const res = await imp.run(site.db, {
    rows, headerIdx: 0, roles: [1, 3], mode: 'inventory', warehouseId: ids[B], createMissing: false, fileName: 'прайс.xlsx', user: 'staff',
  });
  const d = await lastDoc();
  assert.equal(d.kind, 'inventory'); assert.equal(d.comment, 'импорт из прайс.xlsx'); assert.equal(d.username, 'staff');
  assert.match(res.log.join('\n'), new RegExp(`Создан документ №${d.id}`));
  assert.equal((await docs.get(site.db, d.id)).lines, 3);

  // строка внутри точки сохранения откатилась после создания документа - следующая запись всё равно попадает в документ
  const doc = ops.newDoc('receipt', 'после отката', 'staff');
  await site.db.tx(async (c) => {
    await c.query('SAVEPOINT s');
    await ops.receive(c, ids['NK-1'], ids[A], 1, 'откатим', 'receipt', null, doc);
    await c.query('ROLLBACK TO SAVEPOINT s');
    ops.docRolledBack(doc);
    await ops.receive(c, ids['NK-1'], ids[A], 2, 'оставим', 'receipt', null, doc);
  });
  const saved = await lastDoc();
  assert.equal(saved.comment, 'после отката');
  assert.deepEqual((await docs.lines(site.db, saved.doc_key)).map((l) => l.delta), [2]);
});

test('правка карточки (быстрая) - документ «Инвентаризация» с причиной', async () => {
  const page = (await staff.get(`/products/${ids['AD-1']}/quick`)).text;
  const was = Number(new RegExp(`name="was_${ids[A]}" value="(\\d+)"`).exec(page)[1]);
  const b = { sku: 'AD-1', name: 'Adidas 1', price: '0', market_sku: '', reason: 'ошиблись при приёмке', [`was_${ids[A]}`]: String(was), [`qty_${ids[A]}`]: String(was + 4) };
  const r = await staff.post(`/products/${ids['AD-1']}/quick`, b, { from: `/products/${ids['AD-1']}/quick` });
  assert.equal(r.status, 302);
  const d = await lastDoc();
  assert.equal(d.kind, 'inventory'); assert.equal(d.comment, 'ошиблись при приёмке'); assert.equal(d.username, 'staff');
});

test('журнал движений ссылается на документ', async () => {
  const d = (await q("SELECT * FROM stock_documents WHERE comment = 'продажа'"))[0];
  assert.match((await staff.get('/movements')).text, new RegExp(`href="/documents/${d.id}"[^>]*>документ №${d.id}<`));
  assert.match((await staff.get(`/products/${ids['NK-1']}`)).text, new RegExp(`документ №${d.id}`));
});

test('только просмотр: список и документ видны, отмены и выгрузка отмены нет', async () => {
  const d = await lastDoc();
  const list = (await viewer.get('/documents')).text;
  assert.match(list, /Документы/); assert.doesNotMatch(list, /Отменить последний документ/);
  const page = (await viewer.get(`/documents/${d.id}`)).text;
  assert.doesNotMatch(page, /Отменить документ целиком/); assert.match(page, /Выгрузить в Excel/);
  assert.equal((await viewer.get(`/documents/${d.id}/cancel`)).location, '/');
  assert.equal((await viewer.get('/documents/last/cancel')).location, '/');
  assert.equal((await staff.get('/documents/abc')).location, '/documents');
  assert.equal((await staff.get('/documents/999999')).location, '/documents');
  assert.equal((await site.client().get('/documents')).status, 302, 'без входа - на страницу входа');
});

test('комментарий документа экранируется', async () => {
  await operation(staff, { kind: 'receipt', product: '<b>X</b>', quantity: '1', comment: '<script>alert(1)</script>' });
  const page = (await staff.get('/documents')).text;
  assert.doesNotMatch(page, /<script>alert\(1\)<\/script>/); assert.match(page, /&lt;script&gt;/);
});

test('старая база без stock_documents: операции работают, раздел вежливо недоступен', async () => {
  await site.sys('DROP TABLE stock_documents');
  ops.resetFeatureCache();
  const before = (await q('SELECT COUNT(*)::int AS n FROM stock_movements'))[0].n;
  const r = await operation(staff, { kind: 'receipt', quantity: '1', comment: 'без документов' });
  assert.equal(r.status, 302);
  const m = (await q("SELECT doc_id FROM stock_movements WHERE comment = 'без документов'"))[0];
  assert.ok(m.doc_id, 'запись сгруппирована по doc_id, как раньше');
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_movements'))[0].n, before + 1);
  const docsPage = await staff.get('/documents');
  assert.equal(docsPage.location, '/movements');
  assert.match(decodeURIComponent((await staff.follow(docsPage)).text), /Документы заработают после обновления/);
  assert.equal((await staff.get('/movements')).status, 200);
  assert.equal((await staff.get(`/products/${ids['NK-1']}`)).status, 200);
  const done = await batch.apply(site.db, 'NK-1 1', 'receipt', ids[A], 'пакет без документов', 'staff');
  assert.equal(done.ok, true);
});
