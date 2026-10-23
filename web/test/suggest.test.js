'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { suggestProducts } = require('../src/finder');
const { startSite } = require('./helpers');

let site; let staff; let viewer;
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const suggest = async (client, text) => JSON.parse((await client.get(`/find/suggest?q=${encodeURIComponent(text)}`)).text).items;
const skus = (items) => items.map((i) => i.sku);

test.before(async () => {
  site = await startSite();
  await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  staff = site.client(); viewer = site.client();
  await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query(`INSERT INTO products (sku, name, market_sku) VALUES ('NK-AF1-42','Nike Air Force 1 42','MKT-NK'),('NK-AF1-43','Nike Air Force 1 43',''),
    ('AD-S-40','Adidas Samba 40',''),('NB-373-43','New Balance 373 43',''),('PCT-50%','Скидка 50% _тест',''),('AF1','Короткий артикул','')`);
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) SELECT '4006381333931', id FROM products WHERE sku = 'NK-AF1-42'");
});
test.after(() => site.stop());

test('подсказки: часть артикула, названия, артикула на Маркете и штрихкода', async () => {
  assert.deepEqual(skus(await suggest(staff, 'af1')), ['AF1', 'NK-AF1-42', 'NK-AF1-43'], 'артикул целиком - первым, потом остальные по артикулу');
  assert.deepEqual(skus(await suggest(staff, 'nk-af')), ['NK-AF1-42', 'NK-AF1-43']);
  assert.deepEqual(skus(await suggest(staff, 'nike 43')), ['NK-AF1-43'], 'несколько слов: каждое должно встретиться');
  assert.deepEqual(skus(await suggest(staff, 'samba')), ['AD-S-40'], 'по названию');
  assert.deepEqual(skus(await suggest(staff, 'mkt-n')), ['NK-AF1-42'], 'по артикулу на Маркете');
  assert.deepEqual(await suggest(staff, '63813339'), [], 'цифры от сканера (8 и больше) - без подсказок');
  assert.deepEqual(skus(await suggest(staff, '3813')), ['NK-AF1-42'], 'по части штрихкода (короткий кусок)');
  assert.deepEqual(skus(await suggest(staff, '  NIKE   air ')), ['NK-AF1-42', 'NK-AF1-43'], 'лишние пробелы и любой регистр');
  const first = (await suggest(staff, 'nk-af1-42'))[0];
  assert.deepEqual([first.id > 0, first.sku, first.name, first.market_sku], [true, 'NK-AF1-42', 'Nike Air Force 1 42', 'MKT-NK']);
});

test('подсказки: спецсимволы ищутся буквально, пустой и короткий запрос - пусто, сканер - пусто', async () => {
  assert.deepEqual(skus(await suggest(staff, '50%')), ['PCT-50%']);
  assert.deepEqual(skus(await suggest(staff, '_т')), ['PCT-50%']);
  assert.deepEqual(await suggest(staff, ''), []); assert.deepEqual(await suggest(staff, 'a'), []);
  assert.deepEqual(await suggest(staff, '4006381333931'), [], 'штрихкод целиком - это работа сканера, а не подсказок');
  assert.deepEqual(await suggest(staff, "'; DROP TABLE products; --"), []);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM products'))[0].n, 6);
});

test('подсказки: не больше 12, лимит в функции', async () => {
  await site.db.query("INSERT INTO products (sku, name) SELECT 'BULK-' || g, 'Партия ' || g FROM generate_series(1, 30) g");
  assert.equal((await suggest(staff, 'bulk')).length, 12);
  assert.equal((await suggestProducts(site.db, 'bulk', 5)).length, 5);
  assert.equal((await suggestProducts(site.db, 'bulk', 5000)).length, 30, 'верхняя граница лимита - 50');
});

test('подсказки: нужен вход; «только просмотр» тоже пользуется; ответ не кэшируется', async () => {
  const anon = await site.client().get('/find/suggest?q=nike');
  assert.equal(anon.status, 302); assert.match(anon.location, /^\/login/);
  assert.equal((await suggest(viewer, 'samba')).length, 1);
  const r = await staff.get('/find/suggest?q=nike');
  assert.match(r.headers.get('content-type'), /application\/json/); assert.equal(r.headers.get('cache-control'), 'no-store');
});

test('страницы: подсказки подключены к полю товара и к списку прихода', async () => {
  const op = (await staff.get('/operation')).text;
  assert.match(op, /name="product"[^>]*data-suggest="sku"/); assert.match(op, /Артикул, штрихкод или часть названия/); assert.match(op, /id="quantity-input"/);
  const batch = (await staff.get('/batch')).text;
  assert.match(batch, /data-suggest="line" data-target="batch-lines"/); assert.match(batch, /<textarea name="lines" id="batch-lines"/);
  const js = (await staff.get('/static/app.js')).text;
  assert.match(js, /\/find\/suggest\?q=/);
});
