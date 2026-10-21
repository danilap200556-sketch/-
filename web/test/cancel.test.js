'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const ops = require('../src/stock');
const batch = require('../src/batch');
const imp = require('../src/importer');
const { startSite } = require('./helpers');

let site; let staff; let viewer;
const ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const stockOf = async (sku, wh) => (await q('SELECT COALESCE((SELECT s.quantity FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id WHERE p.sku = $1 AND w.name = $2), 0)::int AS n', [sku, wh]))[0].n;
const movesOf = (sku) => q('SELECT m.* FROM stock_movements m JOIN products p ON p.id = m.product_id WHERE p.sku = $1 ORDER BY m.id', [sku]);
const lastMove = async (sku, type) => (await movesOf(sku)).filter((m) => m.type === type).at(-1);
const tx = (fn) => site.db.tx(fn);
const A = 'Склад А'; const B = 'Склад Б';
const doCancel = (client, idlist, extra = {}) => client.post('/movements/cancel', { idlist: String(idlist), ...extra }, { from: '/movements' });

test.before(async () => {
  site = await startSite();
  await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  staff = site.client(); viewer = site.client();
  await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад Б')");
  for (const w of await q('SELECT id, name FROM warehouses')) ids[w.name] = w.id;
  await site.db.query("INSERT INTO products (sku, name) VALUES ('NK-AF1-42','Nike AF1 42'),('AD-S-40','Adidas Samba 40'),('NB-373-43','NB 373 43'),('TR-1','Перемещаемый'),('<b>X</b>','XSS-товар')");
  for (const p of await q('SELECT id, sku FROM products')) ids[p.sku] = p.id;
});
test.after(() => site.stop());

test('отмена списания: остаток возвращается, журнал дополняется, повторно отменить нельзя', async () => {
  await tx(async (c) => { await ops.receive(c, ids['NK-AF1-42'], ids[A], 10, 'приход 10'); await ops.takeOut(c, ids['NK-AF1-42'], ids[A], 4, 'продажа'); });
  const w = await lastMove('NK-AF1-42', 'writeoff');
  assert.equal(await stockOf('NK-AF1-42', A), 6);

  const page = await staff.get(`/movements/cancel?id=${w.id}`);
  assert.equal(page.status, 200);
  assert.match(page.text, /Отменить запись журнала/); assert.match(page.text, /NK-AF1-42/);
  assert.match(page.text, new RegExp(`name="idlist" value="${w.id}"`));
  assert.match(page.text, /<td class="num">6<\/td><td class="num ">10<\/td>/, 'сейчас 6 -> станет 10');

  const r = await doCancel(staff, w.id, { reason: 'ошиблись артикулом' });
  assert.equal(r.status, 302); assert.equal(r.location, '/movements');
  assert.match((await staff.get('/movements')).text, /Отменено записей: 1 \(4 шт\)/);
  assert.equal(await stockOf('NK-AF1-42', A), 10);

  const c = await lastMove('NK-AF1-42', 'cancel');
  assert.equal(c.delta, 4); assert.equal(c.reverts_id, w.id); assert.equal(c.comment, 'Отмена: ошиблись артикулом');
  assert.equal(c.warehouse_id, ids[A]);
  const orig = (await movesOf('NK-AF1-42')).find((m) => m.id === w.id);
  assert.equal(orig.delta, -4, 'исходная запись не меняется - журнал только дополняется');

  const again = await doCancel(staff, w.id);
  assert.equal(again.status, 409); assert.match(again.text, /уже отменена записью №/);
  assert.equal(await stockOf('NK-AF1-42', A), 10, 'повтор ничего не изменил');
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM stock_movements WHERE type = 'cancel' AND reverts_id = $1", [w.id]))[0].n, 1);

  const journal = (await staff.get('/movements')).text;
  assert.match(journal, new RegExp(`отменена записью №${c.id}`)); assert.match(journal, new RegExp(`отменяет запись №${w.id}`));
  assert.match(journal, /Отмена<\/td>/);
});

test('отмена прихода: нельзя, пока товар уже расходован; все причины сразу; после возврата - можно', async () => {
  await tx(async (c) => { await ops.receive(c, ids['AD-S-40'], ids[A], 5, 'поставка'); await ops.takeOut(c, ids['AD-S-40'], ids[A], 3, 'продажа'); });
  const rcpt = await lastMove('AD-S-40', 'receipt');
  const r = await doCancel(staff, rcpt.id);
  assert.equal(r.status, 409);
  assert.match(r.text, /отмена уберёт 5 шт, а на складе сейчас 2 шт - товар уже расходован/);
  assert.equal(await stockOf('AD-S-40', A), 2); assert.equal((await q("SELECT COUNT(*)::int AS n FROM stock_movements WHERE type = 'cancel' AND reverts_id = $1", [rcpt.id]))[0].n, 0);
  assert.ok(!/<button[^>]*>Отменить запись<\/button>/.test((await staff.get(`/movements/cancel?id=${rcpt.id}`)).text), 'кнопки отмены нет, пока нельзя');

  // отменили продажу -> товар вернулся -> приход отменить можно
  await doCancel(staff, (await lastMove('AD-S-40', 'writeoff')).id);
  assert.equal(await stockOf('AD-S-40', A), 5);
  assert.equal((await doCancel(staff, rcpt.id)).status, 302);
  assert.equal(await stockOf('AD-S-40', A), 0);
});

test('инвентаризация: отмена возвращает прежнее количество, и вверх и вниз', async () => {
  await tx(async (c) => { await ops.receive(c, ids['NB-373-43'], ids[A], 8, 'старт'); await ops.inventory(c, ids['NB-373-43'], ids[A], 3, 'пересчёт'); });
  const down = await lastMove('NB-373-43', 'inventory_adjust');
  assert.equal(down.delta, -5);
  assert.equal((await doCancel(staff, down.id)).status, 302);
  assert.equal(await stockOf('NB-373-43', A), 8);
  await tx(async (c) => ops.inventory(c, ids['NB-373-43'], ids[A], 20, 'ещё'));
  const up = await lastMove('NB-373-43', 'inventory_adjust');
  assert.equal(up.delta, 12);
  assert.equal((await doCancel(staff, up.id)).status, 302);
  assert.equal(await stockOf('NB-373-43', A), 8);
  // после инвентаризации товар продали - отменить её в плюс нельзя
  await tx(async (c) => { await ops.inventory(c, ids['NB-373-43'], ids[A], 10, 'ещё раз'); await ops.takeOut(c, ids['NB-373-43'], ids[A], 9, 'продажа'); });
  const inv = await lastMove('NB-373-43', 'inventory_adjust');
  const bad = await doCancel(staff, inv.id);
  assert.equal(bad.status, 409); assert.match(bad.text, /отмена уберёт 2 шт, а на складе сейчас 1 шт/);
});

test('документ: пакет отменяется целиком или по одной позиции; всё или ничего', async () => {
  await tx(async (c) => { await ops.receive(c, ids['NK-AF1-42'], ids[B], 20, 'склад Б'); });
  const res = await batch.apply(site.db, 'NK-AF1-42 5\nAD-S-40 2\nNB-373-43 1', 'receipt', ids[B], 'приёмка №15');
  assert.equal(res.ok, true);
  const docRows = await q("SELECT m.id, p.sku, m.doc_id FROM stock_movements m JOIN products p ON p.id = m.product_id WHERE m.comment = 'приёмка №15' ORDER BY m.id");
  assert.equal(docRows.length, 3); assert.ok(docRows[0].doc_id && docRows.every((r) => r.doc_id === docRows[0].doc_id), 'у пакета общий документ');
  assert.deepEqual(await q('SELECT doc_id FROM stock_movements WHERE comment = $1', ['склад Б']), [{ doc_id: null }], 'у одиночной операции документа нет');

  // в журнале: «весь документ (3)»
  const journal = (await staff.get('/movements')).text;
  assert.match(journal, /весь документ \(3\)/);
  // страница документа показывает все три позиции
  const first = docRows[0].id;
  const page = await staff.get(`/movements/cancel?id=${first}&doc=1`);
  assert.match(page.text, /операцию: 3 записи/); for (const r of docRows) assert.match(page.text, new RegExp(r.sku));
  // страница одной позиции предлагает весь документ
  assert.match((await staff.get(`/movements/cancel?id=${first}`)).text, /часть документа из 3 записей/);

  // одну позицию (NB) тратим -> документ целиком не отменить, ни одна запись не изменится
  await tx(async (c) => ops.takeOut(c, ids['NB-373-43'], ids[B], 1, 'продали'));
  const before = { nk: await stockOf('NK-AF1-42', B), ad: await stockOf('AD-S-40', B), nb: await stockOf('NB-373-43', B) };
  const all = await doCancel(staff, docRows.map((r) => r.id).join(','));
  assert.equal(all.status, 409); assert.match(all.text, /NB-373-43, склад «Склад Б»: отмена уберёт 1 шт, а на складе сейчас 0 шт/);
  assert.deepEqual({ nk: await stockOf('NK-AF1-42', B), ad: await stockOf('AD-S-40', B), nb: await stockOf('NB-373-43', B) }, before, 'всё или ничего');
  // а две других позиции - можно, отдельно
  const two = docRows.filter((r) => r.sku !== 'NB-373-43').map((r) => r.id).join(',');
  assert.equal((await doCancel(staff, two, { reason: 'вернули поставщику' })).status, 302);
  assert.equal(await stockOf('NK-AF1-42', B), 20); assert.equal(await stockOf('AD-S-40', B), 0);
  // отмены одного документа - тоже один документ
  const cancels = await q("SELECT DISTINCT doc_id FROM stock_movements WHERE type = 'cancel' AND comment = 'Отмена: вернули поставщику'");
  assert.equal(cancels.length, 1); assert.ok(cancels[0].doc_id);
});

test('перемещение отменяется парой с любой стороны; старые записи без документа - тоже', async () => {
  await tx(async (c) => { await ops.receive(c, ids['TR-1'], ids[A], 10, 'старт'); await ops.transfer(c, ids['TR-1'], ids[A], ids[B], 4, 'на склад Б'); });
  assert.equal(await stockOf('TR-1', A), 6); assert.equal(await stockOf('TR-1', B), 4);
  const tin = await lastMove('TR-1', 'transfer_in');
  assert.ok(tin.doc_id);
  const page = await staff.get(`/movements/cancel?id=${tin.id}`);
  assert.match(page.text, /операцию: 2 записи/); assert.ok(!/отменить весь документ/.test(page.text), 'пара уже включена, предлагать документ не нужно');
  assert.equal((await doCancel(staff, tin.id)).status, 302);
  assert.equal(await stockOf('TR-1', A), 10); assert.equal(await stockOf('TR-1', B), 0);
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM stock_movements m JOIN products p ON p.id = m.product_id WHERE p.sku = 'TR-1' AND m.type = 'cancel'"))[0].n, 2);

  // старое перемещение (до появления документов): пара находится по времени и складам
  await tx(async (c) => { await ops.transfer(c, ids['TR-1'], ids[A], ids[B], 3, 'старое'); });
  await site.db.query("UPDATE stock_movements SET doc_id = NULL WHERE comment = 'старое'");
  const out = await lastMove('TR-1', 'transfer_out');
  assert.equal((await doCancel(staff, out.id)).status, 302);
  assert.equal(await stockOf('TR-1', A), 10); assert.equal(await stockOf('TR-1', B), 0);
  // в пару нельзя вмешаться: расход перемещения в одиночку не отменить, даже если его id один
  await tx(async (c) => { await ops.transfer(c, ids['TR-1'], ids[A], ids[B], 2, 'ещё'); });
  await tx(async (c) => ops.takeOut(c, ids['TR-1'], ids[B], 2, 'продали на Б'));
  const o2 = await lastMove('TR-1', 'transfer_out');
  const blocked = await doCancel(staff, o2.id);
  assert.equal(blocked.status, 409); assert.match(blocked.text, /TR-1, склад «Склад Б»: отмена уберёт 2 шт, а на складе сейчас 0 шт/);
  assert.equal(await stockOf('TR-1', A), 8, 'ничего не изменилось');
});

test('саму отмену отменить нельзя; неверные записи', async () => {
  const c = await lastMove('NK-AF1-42', 'cancel');
  const r = await doCancel(staff, c.id);
  assert.equal(r.status, 409); assert.match(r.text, /сама является отменой/);
  const miss = await doCancel(staff, '999999');
  assert.equal(miss.status, 409); assert.match(miss.text, /Запись №999999 не найдена/);
  assert.equal((await staff.get('/movements/cancel?id=abc')).location, '/movements');
  assert.equal((await staff.get('/movements/cancel')).location, '/movements');
  const empty = await doCancel(staff, '');
  assert.equal(empty.status, 302);
  assert.equal((await doCancel(staff, '0,-5,abc,1e3')).status, 302, 'мусор вместо id отбрасывается');
});

test('одновременные отмены одной записи: проходит ровно одна', async () => {
  await tx(async (c) => { await ops.receive(c, ids['AD-S-40'], ids[A], 10, 'гонка'); await ops.takeOut(c, ids['AD-S-40'], ids[A], 6, 'гонка продажа'); });
  const w = await lastMove('AD-S-40', 'writeoff');
  const settled = await Promise.allSettled([ops.cancel(site.db, [w.id], 'a'), ops.cancel(site.db, [w.id], 'b'), ops.cancel(site.db, [w.id], 'c')]);
  assert.equal(settled.filter((x) => x.status === 'fulfilled').length, 1);
  for (const x of settled.filter((s) => s.status === 'rejected')) assert.ok(x.reason instanceof ops.CancelRefused, String(x.reason));
  assert.equal(await stockOf('AD-S-40', A), 10 - 6 + 6, 'остаток вернулся один раз');
});

test('параллельные отмены разных записей не уводят остаток в минус', async () => {
  await tx(async (c) => { await ops.receive(c, ids['NB-373-43'], ids[B], 5, 'p1'); });
  const r1 = await lastMove('NB-373-43', 'receipt');
  await tx(async (c) => { await ops.receive(c, ids['NB-373-43'], ids[B], 5, 'p2'); });
  const r2 = await lastMove('NB-373-43', 'receipt');
  const start = await stockOf('NB-373-43', B);
  await tx(async (c) => ops.takeOut(c, ids['NB-373-43'], ids[B], start - 6, 'продали почти всё'));
  assert.equal(await stockOf('NB-373-43', B), 6);
  const settled = await Promise.allSettled([ops.cancel(site.db, [r1.id], ''), ops.cancel(site.db, [r2.id], '')]);
  assert.equal(settled.filter((x) => x.status === 'fulfilled').length, 1, 'вторая уже не помещается: 6 - 5 < 5');
  assert.equal(await stockOf('NB-373-43', B), 1);
});

test('импорт из Excel и правка карточки - тоже документы', async () => {
  const wid = ids[A];
  const base = { headerIdx: 0, roles: [imp.ROLES.key, imp.ROLES.stock], warehouseId: wid, mode: 'receipt', createMissing: false, fileName: 'прайс.xlsx' };
  const before = { nk: await stockOf('NK-AF1-42', A), ad: await stockOf('AD-S-40', A) };
  await imp.run(site.db, { ...base, rows: [['Артикул', 'Остаток'], ['NK-AF1-42', '7'], ['AD-S-40', '2']] });
  const rows = await q("SELECT id, doc_id FROM stock_movements WHERE comment = 'импорт из прайс.xlsx' ORDER BY id");
  assert.equal(rows.length, 2); assert.ok(rows[0].doc_id && rows[0].doc_id === rows[1].doc_id);
  assert.equal((await staff.post('/movements/cancel', { idlist: rows.map((r) => r.id).join(',') }, { from: '/movements' })).status, 302);
  assert.deepEqual({ nk: await stockOf('NK-AF1-42', A), ad: await stockOf('AD-S-40', A) }, before, 'импорт отменён целиком');

  // правка карточки: два пересчёта - один документ
  const csrf = await staff.csrf(`/products/${ids['NK-AF1-42']}/quick`);
  const page = (await staff.get(`/products/${ids['NK-AF1-42']}/quick`)).text;
  const form = { _csrf: csrf, sku: 'NK-AF1-42', name: 'Nike AF1 42', price: '0', market_sku: '', reason: 'проверка' };
  for (const m of page.matchAll(/name="was_(\d+)" value="(\d+)"/g)) { form[`was_${m[1]}`] = m[2]; form[`qty_${m[1]}`] = String(Number(m[2]) + 1); }
  const saved = await staff.req('POST', `/products/${ids['NK-AF1-42']}/quick`, { form });
  assert.equal(saved.status, 302);
  const inv = await q("SELECT doc_id FROM stock_movements m JOIN products p ON p.id = m.product_id WHERE p.sku = 'NK-AF1-42' AND m.comment LIKE 'проверка%'");
  assert.ok(inv.length >= 2 && inv.every((r) => r.doc_id === inv[0].doc_id), 'пересчёты карточки - один документ');
});

test('права: просмотр и вход; CSRF', async () => {
  const w = (await q("SELECT id FROM stock_movements WHERE type = 'receipt' AND reverts_id IS NULL AND id NOT IN (SELECT reverts_id FROM stock_movements WHERE reverts_id IS NOT NULL) ORDER BY id LIMIT 1"))[0];
  assert.ok(w, 'есть неотменённая запись для проверки');
  assert.equal((await viewer.get(`/movements/cancel?id=${w.id}`)).location, '/');
  assert.equal((await viewer.post('/movements/cancel', { idlist: String(w.id) }, { from: '/movements' })).location, '/');
  assert.equal((await site.client().get(`/movements/cancel?id=${w.id}`)).status, 302);
  const anon = await site.client().post('/movements/cancel', { idlist: String(w.id) }, { csrf: false });
  assert.equal(anon.status, 302); assert.match(anon.location, /^\/login/, 'без входа - на страницу входа');
  const noToken = await staff.req('POST', '/movements/cancel', { form: { idlist: String(w.id) } });
  assert.equal(noToken.status, 403);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_movements WHERE reverts_id = $1', [w.id]))[0].n, 0, 'ничего не отменилось');
});

test('интерфейс: кнопки у редактора, статус у «только просмотр», экранирование, безопасный возврат', async () => {
  await tx(async (c) => { await ops.receive(c, ids['<b>X</b>'], ids[A], 3, '<script>alert(1)</script>'); });
  const m = await lastMove('<b>X</b>', 'receipt');
  const ed = (await staff.get('/movements?q=X')).text;
  assert.match(ed, new RegExp(`href="/movements/cancel\\?id=${m.id}&amp;back=`)); assert.match(ed, />Отменить</);
  assert.ok(!ed.includes('<script>alert(1)</script>') && ed.includes('&lt;script&gt;'), 'комментарий экранирован');
  const v = (await viewer.get('/movements')).text;
  assert.ok(!v.includes('/movements/cancel?'), 'у «только просмотр» кнопок нет');
  assert.match(v, /отменена записью №/, 'но статус виден');
  // карточка товара: тот же журнал с кнопками отмены
  assert.match((await staff.get(`/products/${ids['<b>X</b>']}`)).text, new RegExp(`/movements/cancel\\?id=${m.id}`));
  assert.ok(!(await viewer.get(`/products/${ids['<b>X</b>']}`)).text.includes('/movements/cancel?'));
  // причина с HTML экранируется; возврат только на свои страницы
  const page = await staff.get(`/movements/cancel?id=${m.id}&back=${encodeURIComponent('https://evil.example/')}`);
  assert.match(page.text, /name="back" value="\/movements"/, 'чужой адрес заменён');
  const r = await doCancel(staff, m.id, { reason: '<img src=x onerror=alert(1)>', back: '//evil.example' });
  assert.equal(r.location, '/movements');
  assert.equal((await lastMove('<b>X</b>', 'cancel')).comment, 'Отмена: <img src=x onerror=alert(1)>');
  const shown = (await staff.get('/movements?q=X')).text;
  assert.ok(!shown.includes('<img src=x'), 'причина в журнале экранирована');
  for (const t of [ed, v, page.text]) assert.ok(!/ style="|\son\w+="/.test(t), 'CSP: без inline-стилей и обработчиков');
});

test('удаление товара или склада вместе с отменёнными записями не упирается в связи', async () => {
  await site.db.query("INSERT INTO products (sku, name) VALUES ('DEL-1','Удаляемый'), ('DEL-2','Тоже удаляемый')");
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад В')");
  const p1 = (await q("SELECT id FROM products WHERE sku = 'DEL-1'"))[0].id; const p2 = (await q("SELECT id FROM products WHERE sku = 'DEL-2'"))[0].id;
  const wv = (await q("SELECT id FROM warehouses WHERE name = 'Склад В'"))[0].id;
  // товар: приход, перемещение между складами, отмена пары
  await tx(async (c) => { await ops.receive(c, p1, ids[A], 5, 'д'); await ops.transfer(c, p1, ids[A], ids[B], 2, 'пара'); });
  await ops.cancel(site.db, [(await lastMove('DEL-1', 'transfer_out')).id], '');
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM stock_movements m JOIN products p ON p.id = m.product_id WHERE p.sku = 'DEL-1' AND m.type = 'cancel'"))[0].n, 2);
  await site.db.query('DELETE FROM products WHERE id = $1', [p1]);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_movements WHERE product_id = $1', [p1]))[0].n, 0, 'записи товара ушли вместе с отменами');
  // склад: приход и его отмена
  await tx(async (c) => ops.receive(c, p2, wv, 3, 'на В'));
  await ops.cancel(site.db, [(await lastMove('DEL-2', 'receipt')).id], '');
  await site.db.query('DELETE FROM warehouses WHERE id = $1', [wv]);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock_movements WHERE warehouse_id = $1', [wv]))[0].n, 0, 'записи склада ушли вместе с ним');
});

test('старая схема без doc_id/reverts_id: журнал и операции работают, отмена вежливо недоступна', async () => {
  await site.sys('DROP INDEX stock_movements_reverts_uq; DROP INDEX stock_movements_doc_idx');
  await site.sys('ALTER TABLE stock_movements DROP COLUMN doc_id, DROP COLUMN reverts_id');
  ops.resetFeatureCache();
  assert.equal(await ops.cancelSupported(site.db), false);

  await tx(async (c) => { await ops.receive(c, ids['TR-1'], ids[A], 1, 'без новых столбцов'); await ops.transfer(c, ids['TR-1'], ids[A], ids[B], 1, 'пара'); });
  assert.equal((await staff.post('/batch/apply', { kind: 'receipt', warehouse: String(ids[A]), lines: 'TR-1 2', token: 'x' }, { from: '/batch' })).status, 302, 'форма пакета отвечает (метка устарела)');
  const journal = await staff.get('/movements');
  assert.equal(journal.status, 200);
  assert.match(journal.text, /Отмена операций появится после обновления настольного приложения/);
  assert.ok(!journal.text.includes('/movements/cancel?'));
  assert.equal((await viewer.get('/movements')).status, 200);
  assert.equal((await staff.get('/products/' + ids['TR-1'])).status, 200);
  const g = await staff.get('/movements/cancel?id=1');
  assert.equal(g.status, 302); assert.equal(g.location, '/movements');
  assert.match((await staff.get('/movements')).text, /заработает после обновления настольного приложения/);
  assert.equal((await doCancel(staff, 1)).status, 302);
  assert.equal(await stockOf('TR-1', B) >= 1, true, 'остатки не тронуты');
  const res = await batch.apply(site.db, 'TR-1 1', 'receipt', ids[A], 'старая схема');
  assert.equal(res.ok, true, 'пакет работает без документа');

  // приложение обновили - сайт сам подхватывает новые столбцы (без перезапуска)
  await site.sys('ALTER TABLE stock_movements ADD COLUMN doc_id TEXT, ADD COLUMN reverts_id INTEGER REFERENCES stock_movements(id)');
  await site.sys('CREATE UNIQUE INDEX stock_movements_reverts_uq ON stock_movements (reverts_id) WHERE reverts_id IS NOT NULL');
  ops.resetFeatureCache();
  assert.equal(await ops.cancelSupported(site.db), true);
  assert.match((await staff.get('/movements')).text, /Отменить/);
});
