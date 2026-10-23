'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const ps = require('../src/productsync');
const mi = require('../src/marketinfo');
const { productTable } = require('../src/export');
const { startSite } = require('./helpers');

let site; let staff; let viewer;
const ids = {};
const q = async (sql, p) => (await site.db.query(sql, p)).rows;
const product = async (sku) => (await q('SELECT * FROM products WHERE sku = $1', [sku]))[0];
const CATEGORY = { market_category_id: 101, market_category_name: 'Обувь › Кроссовки', market_params: '{"1":[{"value":"42","valueId":11}],"4":[{"value":"кожа"}]}' };

test.before(async () => {
  site = await startSite();
  await site.addUser('staff', 'staffpass1', false); await site.addUser('viewer', 'viewerpass1', false);
  await site.db.query("UPDATE users SET read_only = TRUE WHERE username = 'viewer'");
  staff = site.client(); viewer = site.client();
  await staff.login('staff', 'staffpass1'); await viewer.login('viewer', 'viewerpass1');
  await site.db.query("INSERT INTO products (sku, name) VALUES ('NK-1','Nike 1'),('AD-1','Adidas 1'),('XX-1','Без параметров')");
  for (const p of await q('SELECT id, sku FROM products')) ids[p.sku] = p.id;
  await site.db.query(`UPDATE products SET market_vendor = 'Nike', market_countries = 'Вьетнам;Китай', market_length_cm = 30.5, market_width_cm = 20,
    market_height_cm = 12, market_weight_kg = 1.25, market_commodity_code = '6404110000', market_tags = 'лето;спорт', market_adult = TRUE,
    market_category_id = $2, market_category_name = $3, market_params = $4 WHERE id = $1`,
  [ids['NK-1'], CATEGORY.market_category_id, CATEGORY.market_category_name, CATEGORY.market_params]);
});
test.after(() => site.stop());

const form = (over = {}) => ({
  sku: 'NK-1', name: 'Nike 1', price: '0', description: '', custom_code: '', market_sku: '', ym_present: '1',
  ym_vendor: 'Nike', ym_countries: 'Вьетнам; Китай', ym_length: '30,5', ym_width: '20', ym_height: '12', ym_weight: '1.25',
  ym_commodity: '6404110000', ym_tags: 'лето; спорт', ym_adult: '1', ...over,
});

test('страница товара показывает параметры для Маркета и категорию (только просмотр - без правки)', async () => {
  const page = (await staff.get(`/products/${ids['NK-1']}`)).text;
  assert.match(page, /Для карточки Яндекс Маркета/);
  assert.match(page, /name="ym_vendor" value="Nike"/); assert.match(page, /name="ym_countries" value="Вьетнам; Китай"/);
  assert.match(page, /name="ym_length" value="30.5"/); assert.match(page, /name="ym_weight" value="1.25"/);
  assert.match(page, /name="ym_commodity" value="6404110000"/); assert.match(page, /name="ym_tags" value="лето; спорт"/);
  assert.match(page, /name="ym_adult" value="1" checked/);
  assert.match(page, /Категория Маркета: <b>Обувь › Кроссовки<\/b>, заполнено характеристик: 2/);
  const empty = (await staff.get(`/products/${ids['XX-1']}`)).text;
  assert.match(empty, /name="ym_length" value=""/, 'нули не показываются');
  assert.doesNotMatch(empty, /name="ym_adult" value="1" checked/);
  assert.match(empty, /Категорию Маркета и её характеристики/);
  const v = (await viewer.get(`/products/${ids['NK-1']}`)).text;
  assert.match(v, /name="ym_vendor" value="Nike" maxlength="100" readonly/); assert.match(v, /name="ym_adult"[^>]*disabled/);
  assert.doesNotMatch(v, /<button class="primary">/);
});

test('сохранение: значения читаются как в приложении, категория и характеристики не трогаются', async () => {
  const r = await staff.post(`/products/${ids['NK-1']}`, form({ ym_vendor: ' Nike Jordan ', ym_countries: 'Китай, Вьетнам; Китай', ym_length: '31,5', ym_weight: '1,3', ym_commodity: '6404 19 000 0', ym_tags: 'лето; новинка', ym_adult: undefined }), { from: `/products/${ids['NK-1']}` });
  assert.equal(r.status, 302);
  const p = await product('NK-1');
  assert.equal(p.market_vendor, 'Nike Jordan'); assert.equal(p.market_countries, 'Китай;Вьетнам'); assert.equal(Number(p.market_length_cm), 31.5);
  assert.equal(Number(p.market_width_cm), 20); assert.equal(Number(p.market_weight_kg), 1.3); assert.equal(p.market_commodity_code, '6404190000');
  assert.equal(p.market_tags, 'лето;новинка'); assert.equal(p.market_adult, false);
  assert.equal(Number(p.market_category_id), 101); assert.equal(p.market_category_name, 'Обувь › Кроссовки'); assert.equal(p.market_params, CATEGORY.market_params, 'характеристики остались прежними');
  // пустое = 0 / пусто; без ym_present (старая форма) параметры не трогаются
  await staff.post(`/products/${ids['NK-1']}`, form({ ym_vendor: '', ym_countries: '', ym_length: '', ym_width: '', ym_height: '', ym_weight: '', ym_commodity: '', ym_tags: '' }), { from: `/products/${ids['NK-1']}` });
  const cleared = await product('NK-1');
  assert.deepEqual([cleared.market_vendor, cleared.market_countries, Number(cleared.market_length_cm), cleared.market_commodity_code, cleared.market_tags, cleared.market_adult], ['', '', 0, '', '', true]);
  const old = form(); delete old.ym_present; old.ym_vendor = 'ПОДМЕНА';
  await staff.post(`/products/${ids['NK-1']}`, old, { from: `/products/${ids['NK-1']}` });
  assert.equal((await product('NK-1')).market_vendor, '', 'форма без блока Маркета ничего в нём не меняет');
  // вернуть как было
  await staff.post(`/products/${ids['NK-1']}`, form(), { from: `/products/${ids['NK-1']}` });
  assert.equal((await product('NK-1')).market_countries, 'Вьетнам;Китай');
});

test('неверные значения: понятная ошибка, введённое не теряется, ничего не записано', async () => {
  const before = await product('NK-1');
  for (const [field, value, why] of [['ym_length', 'abc', /«Длина, см»: нужно число от 0 до 1000 \(сантиметры\)/], ['ym_weight', '2000', /«Вес, кг»: нужно число от 0 до 1000 \(килограммы\)/],
    ['ym_commodity', '12', /«ТН ВЭД»: код ТН ВЭД - от 4 до 14 цифр/], ['ym_vendor', 'x'.repeat(101), /«Бренд»: длиннее 100 знаков/]]) {
    const r = await staff.post(`/products/${ids['NK-1']}`, form({ [field]: value }), { from: `/products/${ids['NK-1']}` });
    assert.equal(r.status, 400, field); assert.match(r.text, why);
    if (field === 'ym_length') assert.match(r.text, /name="ym_length" value="abc"/, 'введённое осталось в поле');
  }
  const after = await product('NK-1');
  assert.deepEqual([after.market_vendor, after.market_length_cm, after.market_weight_kg], [before.market_vendor, before.market_length_cm, before.market_weight_kg]);
});

test('новый товар с параметрами для Маркета', async () => {
  const page = (await staff.get('/products/new')).text;
  assert.match(page, /Для карточки Яндекс Маркета/);
  const r = await staff.post('/products', form({ sku: 'NEW-1', name: 'Puma Suede', ym_vendor: 'Puma', ym_length: '25', ym_width: '', ym_height: '', ym_weight: '', ym_commodity: '', ym_countries: 'Индонезия', ym_adult: undefined }), { from: '/products/new' });
  assert.equal(r.status, 302);
  const p = await product('NEW-1');
  assert.deepEqual([p.market_vendor, p.market_countries, Number(p.market_length_cm), Number(p.market_width_cm), p.market_adult, p.market_category_id], ['Puma', 'Индонезия', 25, 0, false, null]);
  const bad = await staff.post('/products', form({ sku: 'NEW-2', name: 'x', ym_length: 'zzz' }), { from: '/products/new' });
  assert.equal(bad.status, 400); assert.equal((await product('NEW-2')), undefined);
});

test('обновление из Excel: те же роли и тексты, что в приложении', async () => {
  const head = ['Артикул', 'Название', 'Описание', 'Цена, ₽', 'Артикул на Маркете', 'Свой код', 'Штрихкоды', 'Всего на складах', 'Остаток: Склад А', 'Места хранения', 'Фото, шт', 'Добавлен',
    'Бренд', 'Страна производства', 'Длина, см', 'Ширина, см', 'Высота, см', 'Вес, кг', 'ТН ВЭД', 'Теги', '18+', 'Категория Маркета (ID)', 'Категория Маркета'];
  assert.equal(ps.ROLE_LABELS.length, 17);
  assert.deepEqual(ps.guessRoles(head), [1, 3, 4, 5, 6, 7, 2, 0, 0, 0, 0, 0, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 0]);
  assert.deepEqual(ps.guessRoles(['Теги', 'Категория', 'Вес']), [15, 0, 13], '«Категория» не путается с «Теги»');
  const roles = ps.guessRoles(head);
  const nk = (over = {}) => ['NK-1', 'Nike 1', '', '0', '', '', '', '0', '0', '', '0', '', 'Nike', 'Вьетнам; Китай', '30.5', '20', '12', '1.25', '6404110000', 'лето; спорт', 'да', '101', 'Обувь › Кроссовки'].map((v, i) => over[i] ?? v);
  const same = await ps.plan(site.db, { rows: [head, nk()], headerIdx: 0, roles });
  assert.equal(same.fatal, null); assert.equal(same.stats.same, 1, 'выгрузка без правок - менять нечего'); assert.deepEqual(same.items[0].changes, []);

  const edited = nk({ 12: 'Nike Jordan', 13: 'Китай, Вьетнам; Китай', 14: '31,5', 17: '1.3', 18: '6404 19 000 0', 19: 'лето; спорт; новинка', 20: 'нет' });
  const bad1 = nk({ 14: 'abc' }); bad1[0] = 'AD-1';
  const bad2 = nk({ 20: 'может быть' }); bad2[0] = 'XX-1';
  const plan = await ps.plan(site.db, { rows: [head, edited, bad1, bad2], headerIdx: 0, roles });
  assert.deepEqual(plan.items[0].changes.map((c) => `${c.field}=${c.to}`), ['ym_vendor=Nike Jordan', 'ym_countries=Китай; Вьетнам', 'ym_length=31.5', 'ym_weight=1.3', 'ym_commodity=6404190000', 'ym_tags=лето; спорт; новинка', 'ym_adult=нет']);
  assert.deepEqual(plan.items[0].changes.map((c) => c.label), ['Бренд', 'Страна производства', 'Длина, см', 'Вес, кг', 'ТН ВЭД', 'Теги', '18+']);
  assert.equal(plan.stats.error, 2);
  assert.match(plan.problems.map((p) => p.text).join('|'), /«Длина, см» - нужно число от 0 до 1000 \(сантиметры\)/);
  assert.match(plan.problems.map((p) => p.text).join('|'), /«18\+» - нужно «да» или «нет»/);

  const done = await ps.apply(site.db, { rows: [head, edited, bad1, bad2], headerIdx: 0, roles });
  assert.equal(done.updated, 1);
  const p = await product('NK-1');
  assert.deepEqual([p.market_vendor, p.market_countries, Number(p.market_length_cm), Number(p.market_width_cm), Number(p.market_weight_kg), p.market_commodity_code, p.market_tags, p.market_adult],
    ['Nike Jordan', 'Китай;Вьетнам', 31.5, 20, 1.3, '6404190000', 'лето;спорт;новинка', false]);
  assert.equal(p.market_params, CATEGORY.market_params, 'характеристики не затронуты');
  assert.equal(done.backup.rows[0].length, head.length - 1, 'файл «как было» - те же колонки (в этой базе нет склада, значит нет и колонки остатка)');
  assert.equal(done.backup.rows[1][11], 'Nike', 'в нём прежние значения');
  const again = await ps.plan(site.db, { rows: [head, edited], headerIdx: 0, roles });
  assert.deepEqual(again.items[0].changes, [], 'повторное применение ничего не меняет');

  // новые товары
  const fresh = [['Артикул', 'Название', 'Бренд', 'Длина, см', 'ТН ВЭД', '18+'], ['FRESH-1', 'Свежий', 'Reebok', '25', '6403', 'да']];
  const res = await ps.apply(site.db, { rows: fresh, headerIdx: 0, roles: ps.guessRoles(fresh[0]), createMissing: true });
  assert.equal(res.created, 1);
  const f = await product('FRESH-1');
  assert.deepEqual([f.market_vendor, Number(f.market_length_cm), Number(f.market_width_cm), f.market_commodity_code, f.market_adult], ['Reebok', 25, 0, '6403', true]);
  // файл только с брендом - допустимое обновление
  const only = [['Артикул', 'Бренд'], ['FRESH-1', 'Puma']];
  const po = await ps.plan(site.db, { rows: only, headerIdx: 0, roles: ps.guessRoles(only[0]) });
  assert.equal(po.fatal, null); assert.equal(po.stats.update, 1);
});

test('страница обновления из Excel: роли предлагаются и применяются', async () => {
  const xlsx = require('../src/xlsx');
  const rows = [['Артикул', 'Бренд', 'Длина, см'], ['AD-1', 'Adidas', '33'], ['XX-1', 'Свой', '']];
  const file = xlsx.write([{ name: 'Товары', rows }]);
  const up = await staff.multipart('/import/products/upload', {}, [{ name: 'p.xlsx', data: Buffer.from(file), field: 'file' }], { from: '/import' });
  assert.equal(up.status, 302, up.text.slice(0, 200));
  const page = await staff.get(up.location);
  assert.match(page.text, /Бренд \(для карточки Маркета\)/);
  assert.match(page.text, /Бренд: <b>|Бренд/);
  const id = up.location.split('/').pop();
  const ok = await staff.post(`/import/products/${id}/apply`, { header: '1', role_0: '1', role_1: '8', role_2: '10' }, { from: '/import' });
  assert.equal(ok.status, 200, ok.text.slice(0, 300));
  assert.equal((await product('AD-1')).market_vendor, 'Adidas'); assert.equal(Number((await product('AD-1')).market_length_cm), 33);
  assert.equal((await product('XX-1')).market_vendor, 'Свой');
});

test('CSV: размеры с запятой для русского Excel', async () => {
  const res = await staff.get('/export/products.csv?q=NK-1');
  const line = res.text.split('\r\n').find((l) => l.startsWith('NK-1'));
  assert.match(line, /;Nike Jordan;"Китай; Вьетнам";31,5;20;12;1,3;6404190000;/);
});

test('старая база без столбцов market_*: товар, выгрузка и обновление из Excel работают по-старому', async () => {
  await site.sys(`ALTER TABLE products DROP COLUMN market_vendor, DROP COLUMN market_countries, DROP COLUMN market_length_cm, DROP COLUMN market_width_cm,
    DROP COLUMN market_height_cm, DROP COLUMN market_weight_kg, DROP COLUMN market_commodity_code, DROP COLUMN market_tags, DROP COLUMN market_adult,
    DROP COLUMN market_category_id, DROP COLUMN market_category_name, DROP COLUMN market_params`);
  mi.resetFeatureCache();
  const page = await staff.get(`/products/${ids['NK-1']}`);
  assert.equal(page.status, 200); assert.doesNotMatch(page.text, /Для карточки Яндекс Маркета/);
  const saved = await staff.post(`/products/${ids['NK-1']}`, form({ name: 'Nike 1 новый' }), { from: `/products/${ids['NK-1']}` });
  assert.equal(saved.status, 302); assert.equal((await product('NK-1')).name, 'Nike 1 новый');
  const t = await productTable(site.db, {});
  assert.equal(t.rows[0].at(-1), 'Добавлен', 'в выгрузке нет колонок Маркета');
  const head = ['Артикул', 'Бренд', 'Название'];
  const plan = await ps.plan(site.db, { rows: [head, ['NK-1', 'Nike', 'Nike 1 ещё']], headerIdx: 0, roles: ps.guessRoles(head) });
  assert.equal(plan.fatal, null); assert.deepEqual(plan.items[0].changes.map((c) => c.field), ['name'], 'колонка бренда пропущена');
  assert.equal((await staff.post('/products', form({ sku: 'OLD-1', name: 'Старый' }), { from: '/products/new' })).status, 302);
});
