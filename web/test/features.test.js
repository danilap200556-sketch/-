'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const sharp = require('sharp');
const { startSite } = require('./helpers');

let site; let admin; let staff; let viewer;
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const ids = {};
const png = (w, h, bg = { r: 255, g: 0, b: 0, alpha: 0.5 }) => sharp({ create: { width: w, height: h, channels: 4, background: bg } }).png().toBuffer();
const jpg = (w, h, c = '#00aa00') => sharp({ create: { width: w, height: h, channels: 3, background: c } }).jpeg().toBuffer();

test.before(async () => {
  site = await startSite();
  await site.addUser('boss', 'bosspass1', true);
  await site.addUser('staff', 'staffpass1', false);
  await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  admin = site.client(); staff = site.client(); viewer = site.client();
  await admin.login('boss', 'bosspass1'); await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO warehouses (name) VALUES ('Склад А'), ('Склад Б')");
  const w = await q('SELECT id, name FROM warehouses ORDER BY name');
  ids.A = w[0].id; ids.B = w[1].id;
  await site.db.query("INSERT INTO locations (warehouse_id, code) VALUES ($1, 'A1-03'), ($2, 'B1')", [ids.A, ids.B]);
  ids.locA = (await q("SELECT id FROM locations WHERE code = 'A1-03'"))[0].id;
  ids.locB = (await q("SELECT id FROM locations WHERE code = 'B1'"))[0].id;
  await site.db.query(`INSERT INTO products (sku, name, price, market_sku) VALUES
    ('NK-AF1-42', 'Nike AF1 42', 100, 'MKT-NK'), ('AD-S-40', 'Adidas Samba 40', 9000.5, ''), ('NB-373-43', 'NB 373 43', 250, '')`);
  for (const p of await q('SELECT id, sku FROM products')) ids[p.sku] = p.id;
  await site.db.query("INSERT INTO product_barcodes (barcode, product_id) VALUES ('4006381333931', $1)", [ids['NK-AF1-42']]);
  await site.db.query('INSERT INTO stock (product_id, warehouse_id, quantity) VALUES ($1, $2, 5)', [ids['NK-AF1-42'], ids.A]);
});
test.after(() => site.stop());

// ------------------------------------------------------------------ роли
test('роль «только просмотр»: читает всё, но на сервере не может ничего изменить', async () => {
  for (const url of ['/stock', '/products', '/products?view=cards', `/products/${ids['NK-AF1-42']}`, '/warehouses', `/warehouses/${ids.A}`, '/movements', '/stock.csv', '/import']) {
    assert.equal((await viewer.get(url)).status, 200, url);
  }
  // страницы с формами изменения закрыты
  for (const url of ['/operation', '/products/new', '/photos/bulk', '/orders']) {
    const r = await viewer.get(url);
    assert.equal(r.status, 302, url); assert.equal(r.location, '/', url);
  }
  // в интерфейсе нет ни форм, ни ссылок на изменение
  const page = (await viewer.get(`/products/${ids['NK-AF1-42']}`)).text;
  for (const bad of ['/barcodes', '/photos"', '/location', '/delete', 'name="_csrf" value="' + 'x']) assert.ok(!page.includes(bad), bad);
  assert.ok(page.includes('readonly'), 'поля заблокированы');
  const stock = (await viewer.get('/stock')).text;
  assert.ok(!stock.includes('/operation') && !stock.includes('href="/orders"') && !stock.includes('href="/users"'));
  assert.ok(stock.includes('href="/stock.csv'), 'выгрузка доступна');
  const products = (await viewer.get('/products')).text;
  assert.ok(!products.includes('/bulk') && !products.includes('/products/new') && !products.includes('class="sel"'));
  // любые изменения - запрещены (даже с верным CSRF-токеном)
  const before = JSON.stringify([await q('SELECT * FROM products ORDER BY id'), await q('SELECT * FROM stock ORDER BY product_id, warehouse_id'), await q('SELECT * FROM warehouses ORDER BY id'), await q('SELECT * FROM product_barcodes ORDER BY barcode')]);
  const attempts = [
    ['/products', { sku: 'X', name: 'Y', price: '1' }], [`/products/${ids['AD-S-40']}`, { sku: 'AD-S-40', name: 'Хак', price: '1' }],
    ['/operation', { kind: 'receipt', product: 'NK-AF1-42', warehouse: String(ids.A), quantity: '5' }],
    [`/products/${ids['NK-AF1-42']}/barcodes`, { barcode: '036000291452' }], [`/products/${ids['NK-AF1-42']}/barcodes/delete`, { barcode: '4006381333931' }],
    [`/products/${ids['NK-AF1-42']}/location`, { warehouse: String(ids.A), location: String(ids.locA) }],
    [`/products/${ids['NK-AF1-42']}/delete`, {}], ['/warehouses', { name: 'Новый' }], [`/warehouses/${ids.A}/delete`, {}],
    [`/warehouses/${ids.A}/locations`, { code: 'Z9' }], ['/bulk', { idlist: String(ids['AD-S-40']) }],
    ['/bulk/apply', { idlist: String(ids['AD-S-40']), price_on: '1', price_mode: 'set', price_value: '1' }],
    ['/bulk/delete', { idlist: String(ids['AD-S-40']) }], ['/users', { username: 'h', password: 'hackerpass1' }],
  ];
  for (const [url, form] of attempts) {
    const r = await viewer.post(url, form, { from: '/stock' });
    assert.ok([302, 403].includes(r.status), `${url} -> ${r.status}`);
    assert.ok(r.status === 403 || r.location === '/', `${url} -> ${r.location}`);
  }
  // multipart тоже закрыт: и с токеном в заголовке, и в поле
  const img = await jpg(200, 100);
  const m1 = await viewer.multipart(`/products/${ids['AD-S-40']}/photos`, {}, [{ name: 'a.jpg', data: img }], { csrf: 'header' });
  const m2 = await viewer.multipart('/photos/bulk', {}, [{ name: 'AD-S-40.jpg', data: img }], { csrf: 'header', headers: { accept: 'application/json' } });
  assert.ok([302, 403].includes(m1.status) && m2.status === 403, `${m1.status} ${m2.status}`);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos'))[0].n, 0);
  const after = JSON.stringify([await q('SELECT * FROM products ORDER BY id'), await q('SELECT * FROM stock ORDER BY product_id, warehouse_id'), await q('SELECT * FROM warehouses ORDER BY id'), await q('SELECT * FROM product_barcodes ORDER BY barcode')]);
  assert.equal(after, before, 'данные не изменились');
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM users WHERE username IN ('h','hacker')"))[0].n, 0);
  // свой пароль менять можно
  assert.equal((await viewer.post('/account/password', { current: 'viewerpass1', password: 'viewerpass2', confirm: 'viewerpass2' })).status, 302);
  assert.equal((await site.client().login('viewer', 'viewerpass2')).status, 302);
  await site.db.query("UPDATE users SET password_hash = (SELECT password_hash FROM users WHERE username = 'staff'), salt = (SELECT salt FROM users WHERE username = 'staff') WHERE username = 'viewer'");
  await viewer.login('viewer', 'staffpass1');
});

test('роль меняется сразу: админ дал/забрал права - действует в уже открытой сессии', async () => {
  const viewerId = (await q("SELECT id FROM users WHERE username = 'viewer'"))[0].id;
  const attempt = () => viewer.post('/warehouses', { name: 'Тест роли' }, { from: '/stock' });
  assert.equal((await attempt()).status, 302);
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM warehouses WHERE name = 'Тест роли'"))[0].n, 0);
  await admin.post(`/users/${viewerId}/role`, { role: 'editor' }, { from: '/users' });
  assert.equal((await attempt()).location, `/warehouses/${(await q("SELECT id FROM warehouses WHERE name = 'Тест роли'"))[0].id}`);
  await admin.post(`/users/${viewerId}/role`, { role: 'viewer' }, { from: '/users' });
  await attempt();
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM warehouses WHERE name = 'Тест роли'"))[0].n, 1, 'второй раз уже не создалось');
  await site.db.query("DELETE FROM warehouses WHERE name = 'Тест роли'");
  // администратор с «залипшим» read_only всё равно редактирует
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'boss'");
  assert.equal((await admin.post('/warehouses', { name: 'Админ-склад' }, { from: '/stock' })).status, 302);
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM warehouses WHERE name = 'Админ-склад'"))[0].n, 1);
  await site.db.query("DELETE FROM warehouses WHERE name = 'Админ-склад'");
  await site.db.query("UPDATE users SET read_only = FALSE WHERE username = 'boss'");
});

test('роли на странице пользователей: создание с ролью, смена, защита последнего админа', async () => {
  let r = await admin.post('/users', { username: 'наблюдатель', password: 'watcherpass', role: 'viewer' }, { from: '/users' });
  assert.equal(r.status, 302);
  assert.deepEqual((await q("SELECT is_admin, read_only FROM users WHERE username = 'наблюдатель'"))[0], { is_admin: false, read_only: true });
  const w = (await q("SELECT id FROM users WHERE username = 'наблюдатель'"))[0].id;
  await admin.post(`/users/${w}/role`, { role: 'admin' }, { from: '/users' });
  assert.deepEqual((await q('SELECT is_admin, read_only FROM users WHERE id = $1', [w]))[0], { is_admin: true, read_only: false });
  await admin.post(`/users/${w}/role`, { role: 'viewer' }, { from: '/users' });
  assert.deepEqual((await q('SELECT is_admin, read_only FROM users WHERE id = $1', [w]))[0], { is_admin: false, read_only: true });
  const bossId = (await q("SELECT id FROM users WHERE username = 'boss'"))[0].id;
  for (const role of ['viewer', 'editor']) {
    const resp = await admin.post(`/users/${bossId}/role`, { role }, { from: '/users' });
    assert.match((await admin.follow(resp)).text, /без администратора/);
  }
  assert.equal((await q('SELECT is_admin FROM users WHERE id = $1', [bossId]))[0].is_admin, true);
  assert.equal((await admin.post(`/users/${w}/role`, { role: 'superuser' }, { from: '/users' })).status, 302);
  assert.equal((await q('SELECT read_only FROM users WHERE id = $1', [w]))[0].read_only, true, 'неизвестная роль игнорируется');
  assert.match((await admin.get('/users')).text, /Только просмотр/);
});

// ------------------------------------------------------------------ фото
test('фото: загрузка приводит к JPEG ≤1280 + миниатюра ≤240, повторы и мусор отсекаются', async () => {
  const id = ids['NK-AF1-42'];
  const big = await png(3000, 2000);
  const small = await jpg(100, 100, '#ffee00');
  const webp = await sharp({ create: { width: 640, height: 480, channels: 3, background: '#0000ff' } }).webp().toBuffer();
  const svg = Buffer.from('<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><script>alert(1)</script></svg>');
  const html = Buffer.from('<html><script>alert(1)</script></html>');
  const r = await staff.multipart(`/products/${id}/photos`, {}, [
    { name: 'big.png', data: big }, { name: 'small.jpg', data: small }, { name: 'w.webp', data: webp },
    { name: 'evil.svg', data: svg }, { name: 'fake.jpg', data: html }, { name: 'big.png', data: big },
  ], { from: `/products/${id}` });
  assert.equal(r.status, 302);
  const rows = await q('SELECT id, data, thumb FROM product_photos WHERE product_id = $1 ORDER BY position, id', [id]);
  assert.equal(rows.length, 3, 'png + jpg + webp; svg/html отвергнуты, дубликат пропущен');
  for (const row of rows) {
    assert.deepEqual([...row.data.subarray(0, 3)], [0xff, 0xd8, 0xff], 'JPEG');
    const m = await sharp(row.data).metadata(); const t = await sharp(row.thumb).metadata();
    assert.equal(m.format, 'jpeg'); assert.ok(m.width <= 1280 && m.height <= 1280); assert.ok(t.width <= 240 && t.height <= 240);
  }
  const meta = await sharp(rows[0].data).metadata();
  assert.deepEqual([meta.width, meta.height], [1280, 853]);
  const px = await sharp(rows[0].data).raw().toBuffer({ resolveWithObject: true });
  assert.ok(px.data[0] > 150 && px.data[1] > 100, 'прозрачность залита светлым фоном (не чёрным)');
  const flash = decodeURIComponent((await staff.follow(r)).text.match(/class="flash[^>]*>([^<]*)/)?.[1] ?? '');
  assert.match(flash, /Добавлено фото: 3/); assert.match(flash, /evil\.svg/); assert.match(flash, /fake\.jpg/);
});

test('фото: отдача только вошедшим, нужные заголовки, миниатюры в списках и карточках', async () => {
  const id = ids['NK-AF1-42'];
  const [first] = await q('SELECT id FROM product_photos WHERE product_id = $1 ORDER BY position, id', [id]);
  const anon = site.client();
  assert.equal((await anon.get(`/photos/${first.id}`)).status, 302);
  assert.equal((await anon.get(`/photos/${first.id}/thumb`)).status, 302);
  const full = await staff.get(`/photos/${first.id}`);
  assert.equal(full.status, 200);
  assert.equal(full.headers.get('content-type'), 'image/jpeg');
  assert.match(full.headers.get('cache-control'), /private, max-age=\d+/);
  assert.match(full.headers.get('content-security-policy'), /sandbox/);
  assert.equal(full.headers.get('x-content-type-options'), 'nosniff');
  assert.ok(full.raw.length > 1000);
  const th = await viewer.get(`/photos/${first.id}/thumb`);
  assert.equal(th.status, 200); assert.ok(th.raw.length < full.raw.length);
  assert.equal((await staff.get('/photos/999999')).status, 404);
  assert.equal((await staff.get('/photos/abc')).status, 404);
  for (const [client, url] of [[staff, '/products'], [staff, '/products?view=cards'], [staff, '/stock'], [viewer, '/products'], [viewer, '/stock']]) {
    const t = (await client.get(url)).text;
    assert.ok(t.includes(`/photos/${first.id}/thumb`), `${url} показывает миниатюру`);
    if (url.startsWith('/products')) assert.ok(t.includes('нет фото'), `${url} показывает заглушку без фото`);
  }
  const list = JSON.parse((await staff.get(`/products/${id}/photos.json`)).text);
  assert.equal(list.ids.length, 3); assert.equal(list.ids[0], first.id);
  assert.deepEqual(JSON.parse((await staff.get('/products/99999/photos.json')).text).ids, []);
});

test('фото: главное, удаление, лимит 12, защита от подмены чужого товара', async () => {
  const id = ids['NK-AF1-42'];
  const before = (await q('SELECT id FROM product_photos WHERE product_id = $1 ORDER BY position, id', [id])).map((r) => r.id);
  await staff.post(`/products/${id}/photos/${before[2]}/cover`, {}, { from: `/products/${id}` });
  assert.deepEqual((await q('SELECT id FROM product_photos WHERE product_id = $1 ORDER BY position, id', [id])).map((r) => r.id), [before[2], before[0], before[1]]);
  // чужой товар: фото не удаляется и не становится обложкой
  const other = ids['AD-S-40'];
  await staff.post(`/products/${other}/photos/${before[0]}/delete`, {}, { from: `/products/${id}` });
  await staff.post(`/products/${other}/photos/${before[0]}/cover`, {}, { from: `/products/${id}` });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [id]))[0].n, 3);
  assert.deepEqual((await q('SELECT id FROM product_photos WHERE product_id = $1 ORDER BY position, id', [id]))[0].id, before[2]);
  await staff.post(`/products/${id}/photos/${before[2]}/delete`, {}, { from: `/products/${id}` });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [id]))[0].n, 2);
  // лимит: 14 разных картинок -> всего 12
  const many = [];
  for (let i = 0; i < 12; i++) many.push({ name: `p${i}.jpg`, data: await jpg(40 + i, 40, `#${(i * 20 + 10).toString(16).padStart(2, '0')}2040`) });
  const r = await staff.multipart(`/products/${id}/photos`, {}, many, { from: `/products/${id}` });
  assert.equal(r.status, 302);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [id]))[0].n, 12);
  assert.match(decodeURIComponent((await staff.follow(r)).text), /уже 12 фото/);
  // страница товара показывает 12 из 12
  assert.match((await staff.get(`/products/${id}`)).text, /\(12 из 12\)/);
});

test('фото: CSRF и лимиты размера для multipart', async () => {
  const id = ids['NB-373-43'];
  const img = await jpg(60, 60);
  assert.equal((await staff.multipart(`/products/${id}/photos`, {}, [{ name: 'a.jpg', data: img }], { csrf: false })).status, 403);
  const wrong = await staff.multipart(`/products/${id}/photos`, { _csrf: 'wrong' }, [{ name: 'a.jpg', data: img }], { csrf: false });
  assert.equal(wrong.status, 403);
  assert.equal((await staff.multipart(`/products/${id}/photos`, {}, [{ name: 'a.jpg', data: img }], { csrf: 'header', from: `/products/${id}` })).status, 302);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [id]))[0].n, 1);
  const cross = await staff.multipart(`/products/${id}/photos`, {}, [{ name: 'b.jpg', data: await jpg(61, 60) }], { headers: { 'sec-fetch-site': 'cross-site' }, from: `/products/${id}` });
  assert.equal(cross.status, 403);
  // файл больше 12 МБ отклоняется, остальные принимаются
  const huge = Buffer.alloc(13 * 1024 * 1024, 7);
  const r = await staff.multipart(`/products/${id}/photos`, {}, [{ name: 'huge.jpg', data: huge }, { name: 'ok.jpg', data: await jpg(62, 60) }], { from: `/products/${id}` });
  assert.equal(r.status, 302);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [id]))[0].n, 2);
  assert.match(decodeURIComponent((await staff.follow(r)).text), /huge\.jpg: файл больше 12 МБ/);
  // "бомба" из пикселей не обрабатывается
  const bomb = await sharp({ create: { width: 12000, height: 12000, channels: 3, background: '#fff' } }).png({ compressionLevel: 9 }).toBuffer();
  const rb = await staff.multipart(`/products/${id}/photos`, {}, [{ name: 'bomb.png', data: bomb }], { from: `/products/${id}` });
  assert.equal(rb.status, 302);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [id]))[0].n, 2, 'огромная картинка отвергнута');
  await site.db.query('DELETE FROM product_photos WHERE product_id = $1', [id]);
});

test('фото пачкой: товар по артикулу / артикулу Маркета / штрихкоду в имени файла, порядок по номеру', async () => {
  await site.db.query('DELETE FROM product_photos');
  const files = [
    { name: 'NK-AF1-42_2.jpg', data: await jpg(300, 200, '#112233') },
    { name: 'NK-AF1-42.png', data: await png(300, 200, { r: 9, g: 9, b: 200, alpha: 1 }) },
    { name: 'nk-af1-42 (3).jpg', data: await jpg(300, 201, '#223344') },
    { name: 'MKT-NK-4.jpg', data: await jpg(301, 200, '#334455') },
    { name: '4006381333931_5.jpg', data: await jpg(302, 200, '#445566') },
    { name: 'AD-S-40.jpg', data: await jpg(303, 200, '#556677') },
    { name: 'NOPE-999.jpg', data: await jpg(304, 200, '#667788') },
    { name: 'AD-S-40_2.jpg', data: Buffer.from('это не картинка') },
  ];
  const r = await staff.multipart('/photos/bulk', {}, files, { csrf: 'header', headers: { accept: 'application/json' }, from: '/products' });
  assert.equal(r.status, 200);
  const res = JSON.parse(r.text).results;
  const byFile = Object.fromEntries(res.map((x) => [x.file, x]));
  assert.equal(byFile['NOPE-999.jpg'].status, 'нет товара');
  assert.match(byFile['AD-S-40_2.jpg'].status, /не удалось прочитать/);
  assert.equal(byFile['AD-S-40.jpg'].ok, true);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [ids['NK-AF1-42']]))[0].n, 5, 'NK: .png, _2, (3), MKT-NK-4, штрихкод');
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [ids['AD-S-40']]))[0].n, 1);
  // порядок: без номера (0) -> 2 -> 3 -> 4(Маркет) ... обложка - файл без номера (синий PNG)
  const order = await q('SELECT data FROM product_photos WHERE product_id = $1 ORDER BY position, id', [ids['NK-AF1-42']]);
  const widths = [];
  for (const row of order) widths.push((await sharp(row.data).stats()).channels[2].mean > 150 ? 'blue' : 'other');
  assert.equal(widths[0], 'blue', 'обложка - NK-AF1-42.png');
  // повтор тех же файлов - дубликаты
  const again = await staff.multipart('/photos/bulk', {}, files.slice(0, 2), { csrf: 'header', headers: { accept: 'application/json' }, from: '/products' });
  assert.deepEqual(JSON.parse(again.text).results.map((x) => x.status), ['уже было', 'уже было']);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [ids['NK-AF1-42']]))[0].n, 5);
  // версия без JS: форма, страница результата
  const html = await staff.multipart('/photos/bulk', {}, [{ name: 'NB-373-43.jpg', data: await jpg(305, 200) }], { from: '/photos/bulk' });
  assert.equal(html.status, 200); assert.match(html.text, /NB-373-43\.jpg/); assert.match(html.text, /добавлено/);
  // без токена
  assert.equal((await staff.multipart('/photos/bulk', {}, files.slice(0, 1), { csrf: false })).status, 403);
  assert.equal((await staff.get('/photos/bulk')).status, 200);
  assert.match((await staff.get('/photos/bulk')).text, /bulk-photos\.js/);
  assert.equal((await staff.get('/static/bulk-photos.js')).status, 200);
});

// ------------------------------------------------------------------ массовое редактирование
test('массовое редактирование: форма -> предпросмотр -> применить (выбранные товары)', async () => {
  const sel = { idlist: `${ids['AD-S-40']},${ids['NB-373-43']}` };
  const form = await staff.post('/bulk', sel, { from: '/products' });
  assert.equal(form.status, 200); assert.match(form.text, /Выбрано товаров: <b>2<\/b>/); assert.match(form.text, /Склад А/);
  // предпросмотр: было -> станет
  const spec = { ...sel, price_on: '1', price_mode: 'percent', price_value: '-10', name_on: '1', name_mode: 'prefix', name_a: 'Кроссовки ' };
  const pv = await staff.post('/bulk/preview', spec, { from: '/products' });
  assert.equal(pv.status, 200);
  assert.match(pv.text, /8\s?100,45/); assert.match(pv.text, /Кроссовки Adidas Samba 40/);
  assert.equal(Number((await q("SELECT price FROM products WHERE sku = 'AD-S-40'"))[0].price), 9000.5, 'предпросмотр ничего не меняет');
  const ap = await staff.post('/bulk/apply', spec, { from: '/products' });
  assert.equal(ap.status, 302);
  const rows = Object.fromEntries((await q("SELECT sku, name, price FROM products WHERE sku IN ('AD-S-40','NB-373-43','NK-AF1-42')")).map((r) => [r.sku, r]));
  assert.ok(Math.abs(rows['AD-S-40'].price - 8100.45) < 0.01 && Math.abs(rows['NB-373-43'].price - 225) < 0.01);
  assert.equal(rows['AD-S-40'].name, 'Кроссовки Adidas Samba 40');
  assert.deepEqual([rows['NK-AF1-42'].name, Number(rows['NK-AF1-42'].price)], ['Nike AF1 42', 100], 'остальные товары не тронуты');
  // прибавить + округлить; задать; ниже нуля не уходит
  await staff.post('/bulk/apply', { ...sel, price_on: '1', price_mode: 'add', price_value: '49,5', price_round: '1' }, { from: '/products' });
  assert.deepEqual((await q("SELECT price FROM products WHERE sku IN ('AD-S-40','NB-373-43') ORDER BY sku")).map((r) => Number(r.price)), [8150, 275]);
  await staff.post('/bulk/apply', { ...sel, price_on: '1', price_mode: 'add', price_value: '-99999999' }, { from: '/products' });
  assert.deepEqual((await q("SELECT price FROM products WHERE sku IN ('AD-S-40','NB-373-43')")).map((r) => Number(r.price)), [0, 0]);
  // название: суффикс, найти/заменить
  await staff.post('/bulk/apply', { ...sel, name_on: '1', name_mode: 'suffix', name_a: ' (опт)' }, { from: '/products' });
  await staff.post('/bulk/apply', { ...sel, name_on: '1', name_mode: 'replace', name_a: 'Кроссовки ', name_b: '' }, { from: '/products' });
  assert.deepEqual((await q("SELECT name FROM products WHERE sku IN ('AD-S-40','NB-373-43') ORDER BY sku")).map((r) => r.name), ['Adidas Samba 40 (опт)', 'NB 373 43 (опт)']);
  // описание с кавычками и очистка артикула на Маркете
  await staff.post('/bulk/apply', { idlist: `${ids['NK-AF1-42']},${ids['AD-S-40']}`, desc_on: '1', description: "О'пи\"сание; DROP TABLE products;--", market_on: '1', market_sku: '' }, { from: '/products' });
  assert.equal((await q("SELECT description FROM products WHERE sku = 'AD-S-40'"))[0].description, "О'пи\"сание; DROP TABLE products;--");
  assert.equal((await q("SELECT market_sku FROM products WHERE sku = 'NK-AF1-42'"))[0].market_sku, '');
  // ошибки валидации не меняют данные и возвращают форму
  for (const bad of [{ price_on: '1', price_value: 'abc' }, { name_on: '1', name_mode: 'replace', name_a: '' }, {}]) {
    const r = await staff.post('/bulk/apply', { ...sel, ...bad }, { from: '/products' });
    assert.equal(r.status, 400, JSON.stringify(bad));
  }
  assert.equal((await staff.post('/bulk', { idlist: '' }, { from: '/products' })).status, 302);
});

test('массовое редактирование: место хранения, все найденные по поиску, атомарность', async () => {
  const n0 = (await q('SELECT COUNT(*)::int AS n FROM stock WHERE location_id IS NOT NULL'))[0].n;
  await staff.post('/bulk/apply', { idlist: `${ids['AD-S-40']},${ids['NB-373-43']},${ids['NK-AF1-42']}`, loc_on: '1', loc: `${ids.A}:${ids.locA}` }, { from: '/products' });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock WHERE location_id = $1', [ids.locA]))[0].n, 3);
  assert.equal((await q('SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2', [ids['NK-AF1-42'], ids.A]))[0].quantity, 5, 'остаток не тронут');
  assert.equal((await q('SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2', [ids['AD-S-40'], ids.A]))[0].quantity, 0, 'создана строка с нулём');
  await staff.post('/bulk/apply', { idlist: `${ids['AD-S-40']}`, loc_on: '1', loc: `${ids.A}:0` }, { from: '/products' });
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM stock WHERE location_id = $1', [ids.locA]))[0].n, 2);
  // место чужого склада - отказ, ничего не меняется (в т.ч. цена из той же операции)
  const priceBefore = (await q("SELECT price FROM products WHERE sku = 'NB-373-43'"))[0].price;
  const bad = await staff.post('/bulk/apply', { idlist: `${ids['NB-373-43']}`, price_on: '1', price_mode: 'set', price_value: '777', loc_on: '1', loc: `${ids.B}:${ids.locA}` }, { from: '/products' });
  assert.equal(bad.status, 400);
  assert.equal((await q("SELECT price FROM products WHERE sku = 'NB-373-43'"))[0].price, priceBefore, 'откат всей операции');
  const noWh = await staff.post('/bulk/apply', { idlist: `${ids['NB-373-43']}`, price_on: '1', price_mode: 'set', price_value: '777', loc_on: '1', loc: '999999:0' }, { from: '/products' });
  assert.equal(noWh.status, 400);
  assert.equal((await q("SELECT price FROM products WHERE sku = 'NB-373-43'"))[0].price, priceBefore, 'нет склада - откат');
  assert.ok(n0 >= 0);
  // "все найденные": по поиску samba
  await staff.post('/bulk/apply', { all: '1', q: 'samba', price_on: '1', price_mode: 'set', price_value: '1234,5' }, { from: '/products' });
  assert.deepEqual((await q('SELECT sku, price FROM products ORDER BY sku')).map((r) => [r.sku, Number(r.price)]),
    [['AD-S-40', 1234.5], ['NB-373-43', priceBefore], ['NK-AF1-42', 100]].map(([s, p]) => [s, Number(p)]));
  // страница списка: чекбоксы, кнопки и счётчик "всех найденных"
  const list = (await staff.get('/products?q=samba')).text;
  assert.match(list, /name="ids" value="\d+"/); assert.match(list, /ко <b>всем 1<\/b> найденным/); assert.match(list, /formaction="\/bulk\/photos"/);
  assert.ok(!list.includes('/bulk/delete'), 'кнопка удаления - только админу');
  assert.ok((await admin.get('/products')).text.includes('/bulk/delete'));
});

test('массовое: одни и те же фото всем выбранным, удаление выбранных (админ)', async () => {
  await site.db.query('DELETE FROM product_photos');
  const page = await staff.post('/bulk/photos', { idlist: `${ids['AD-S-40']},${ids['NB-373-43']}` }, { from: '/products' });
  assert.equal(page.status, 200); assert.match(page.text, /Выбрано товаров: <b>2<\/b>/);
  const files = [{ name: 'model.jpg', data: await jpg(400, 300, '#aa5500') }, { name: 'model2.png', data: await png(200, 200, { r: 0, g: 100, b: 0, alpha: 1 }) }];
  const r = await staff.multipart('/bulk/photos/apply', { idlist: `${ids['AD-S-40']},${ids['NB-373-43']}` }, files, { from: '/products' });
  assert.equal(r.status, 200); assert.match(r.text, /model\.jpg/);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = ANY($1::int[])', [[ids['AD-S-40'], ids['NB-373-43']]]))[0].n, 4);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [ids['NK-AF1-42']]))[0].n, 0);
  assert.equal((await staff.multipart('/bulk/photos/apply', { idlist: String(ids['AD-S-40']) }, files, { csrf: false })).status, 403);
  // удалить может только админ
  await staff.post('/bulk/delete', { idlist: String(ids['NB-373-43']) }, { from: '/products' });
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM products WHERE sku = 'NB-373-43'"))[0].n, 1);
  await admin.post('/bulk/delete', { idlist: String(ids['NB-373-43']) }, { from: '/products' });
  assert.equal((await q("SELECT COUNT(*)::int AS n FROM products WHERE sku = 'NB-373-43'"))[0].n, 0);
  assert.equal((await q('SELECT COUNT(*)::int AS n FROM product_photos WHERE product_id = $1', [ids['AD-S-40']]))[0].n, 2);
});
