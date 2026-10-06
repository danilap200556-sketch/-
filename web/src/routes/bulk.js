'use strict';
const { html } = require('../html');
const { csrfField } = require('../layout');
const { readMultipart } = require('../multipart');
const bulk = require('../bulk');
const { UserError } = require('../stock');
const { searchSql } = require('./products');
const { parseId, fmtMoney, str } = require('../util');

const MAX_SELECTION = 20000;
const MAX_PHOTO_TARGETS = 500;

// Выбор товаров: отмеченные галочками (ids / idlist) или "все найденные по поиску" (all + q).
module.exports = (app, { db, send, requireLogin, requireAdmin }) => {
  async function resolveSelection(body) {
    if (body.all === '1') {
      const s = searchSql(str(body.q, 100), 1);
      const { rows } = await db.query(`SELECT p.id FROM products p WHERE ${s.sql} ORDER BY p.sku LIMIT ${MAX_SELECTION + 1}`, s.params);
      if (rows.length > MAX_SELECTION) throw new UserError(`Слишком много товаров (больше ${MAX_SELECTION}) - уточните поиск`);
      return { ids: rows.map((r) => r.id), all: true, q: str(body.q, 100) };
    }
    const raw = [].concat(body.ids ?? [], String(body.idlist ?? '').split(','));
    const ids = [...new Set(raw.map((v) => parseId(String(v).trim())).filter(Boolean))].slice(0, 2000);
    return { ids, all: false, q: '' };
  }
  const selectionFields = (sel) => (sel.all
    ? html`<input type="hidden" name="all" value="1"><input type="hidden" name="q" value="${sel.q}">`
    : html`<input type="hidden" name="idlist" value="${sel.ids.join(',')}">`);

  const sampleOf = async (ids) =>
    (await db.query('SELECT id, sku, name, price FROM products WHERE id = ANY($1::int[]) ORDER BY sku LIMIT 8', [ids.slice(0, 200)])).rows;

  async function selected(req, res) {
    let sel;
    try {
      sel = await resolveSelection(req.body);
    } catch (e) {
      if (!(e instanceof UserError)) throw e;
      res.flash('err', e.message);
      res.redirect('/products');
      return null;
    }
    if (!sel.ids.length) {
      res.flash('err', 'Отметьте хотя бы один товар в списке');
      res.redirect('/products');
      return null;
    }
    return sel;
  }

  // ---- Массовое редактирование: форма -> предпросмотр -> применить
  const FIELDS = ['price_on', 'price_mode', 'price_value', 'price_round', 'name_on', 'name_mode', 'name_a', 'name_b',
    'desc_on', 'description', 'market_on', 'market_sku', 'loc_on', 'loc'];
  const specBody = (body) => {
    const b = { ...body };
    if (body.loc_on === '1') {
      const [w, l] = String(body.loc ?? '').split(':');
      b.warehouse = w; b.location = l === '0' ? '' : l;
    }
    return b;
  };

  async function editForm(req, res, sel, { error = '', form = {} } = {}, status = 200) {
    const ctx = req.ctx;
    const whs = (await db.query(
      `SELECT w.id, w.name, COALESCE(json_agg(json_build_object('id', l.id, 'code', l.code) ORDER BY l.code) FILTER (WHERE l.id IS NOT NULL), '[]') AS locs
         FROM warehouses w LEFT JOIN locations l ON l.warehouse_id = w.id GROUP BY w.id ORDER BY w.name`)).rows;
    const on = (k) => (form[k] === '1' ? html`checked` : '');
    const val = (k, d = '') => form[k] ?? d;
    send(req, res, {
      title: 'Массовое редактирование', active: 'products',
      body: html`
      <p>Выбрано товаров: <b>${sel.ids.length}</b>. Отметьте, что нужно изменить - остальное останется как есть.</p>
      ${error ? html`<div class="flash err">${error}</div>` : ''}
      <form method="post" action="/bulk/preview" class="card">
        ${csrfField(ctx)}${selectionFields(sel)}
        <fieldset><legend><label class="check"><input type="checkbox" name="price_on" value="1" ${on('price_on')}> Цена</label></legend>
          <div class="row">
            <label class="field"><span>Как менять</span><select name="price_mode">
              <option value="set" ${val('price_mode') === 'set' ? html`selected` : ''}>Задать цену, ₽</option>
              <option value="percent" ${val('price_mode') === 'percent' ? html`selected` : ''}>Изменить на, % (минус - скидка)</option>
              <option value="add" ${val('price_mode') === 'add' ? html`selected` : ''}>Прибавить, ₽ (минус - убавить)</option></select></label>
            <label class="field"><span>Значение</span><input name="price_value" value="${val('price_value')}" inputmode="decimal" maxlength="14"></label>
          </div>
          <label class="check"><input type="checkbox" name="price_round" value="1" ${on('price_round')}> округлять до целых рублей</label>
        </fieldset>
        <fieldset><legend><label class="check"><input type="checkbox" name="name_on" value="1" ${on('name_on')}> Название</label></legend>
          <div class="row">
            <label class="field"><span>Как менять</span><select name="name_mode">
              <option value="prefix" ${val('name_mode') === 'prefix' ? html`selected` : ''}>Добавить в начало</option>
              <option value="suffix" ${val('name_mode') === 'suffix' ? html`selected` : ''}>Добавить в конец</option>
              <option value="replace" ${val('name_mode') === 'replace' ? html`selected` : ''}>Найти и заменить</option></select></label>
            <label class="field"><span>Текст / что найти</span><input name="name_a" value="${val('name_a')}" maxlength="300"></label>
            <label class="field"><span>Заменить на (пусто - удалить найденное)</span><input name="name_b" value="${val('name_b')}" maxlength="300"></label>
          </div>
        </fieldset>
        <fieldset><legend><label class="check"><input type="checkbox" name="desc_on" value="1" ${on('desc_on')}> Описание (заменить у всех)</label></legend>
          <input name="description" value="${val('description')}" maxlength="2000"></fieldset>
        <fieldset><legend><label class="check"><input type="checkbox" name="market_on" value="1" ${on('market_on')}> Артикул на Маркете</label></legend>
          <input name="market_sku" value="${val('market_sku')}" maxlength="200" placeholder="пусто - очистить (на Маркете будет использоваться наш артикул)"></fieldset>
        <fieldset><legend><label class="check"><input type="checkbox" name="loc_on" value="1" ${on('loc_on')}> Место хранения</label></legend>
          <select name="loc">${whs.map((w) => html`<optgroup label="${w.name}">
            <option value="${w.id}:0" ${val('loc') === `${w.id}:0` ? html`selected` : ''}>— не указано —</option>
            ${w.locs.map((l) => html`<option value="${w.id}:${l.id}" ${val('loc') === `${w.id}:${l.id}` ? html`selected` : ''}>${l.code}</option>`)}</optgroup>`)}</select>
        </fieldset>
        <button class="primary">Дальше: проверить</button>
      </form>`,
    }, status);
  }

  app.post('/bulk', requireLogin, async (req, res) => {
    const sel = await selected(req, res);
    if (sel) await editForm(req, res, sel);
  });

  const hidden = (body) => FIELDS.filter((k) => body[k] !== undefined).map((k) => html`<input type="hidden" name="${k}" value="${body[k]}">`);

  app.post('/bulk/preview', requireLogin, async (req, res) => {
    const sel = await selected(req, res);
    if (!sel) return;
    let spec;
    try {
      spec = bulk.parseSpec(specBody(req.body));
    } catch (e) {
      if (!(e instanceof UserError)) throw e;
      return editForm(req, res, sel, { error: e.message, form: req.body }, 400);
    }
    const sample = await sampleOf(sel.ids);
    send(req, res, {
      title: 'Проверьте изменения', active: 'products',
      body: html`
      <p>Будет изменено товаров: <b>${sel.ids.length}</b>. Что меняем: <b>${bulk.describe(spec)}</b>.</p>
      <div class="table-wrap"><table>
        <thead><tr><th>Артикул</th><th>Название: было → станет</th><th class="num">Цена: было → станет</th></tr></thead>
        <tbody>${sample.map((p) => html`<tr><td>${p.sku}</td>
          <td>${p.name}${spec.name ? html` → <b>${bulk.newName(p.name, spec.name)}</b>` : ''}</td>
          <td class="num">${fmtMoney(p.price)}${spec.price ? html` → <b>${fmtMoney(bulk.newPrice(p.price, spec.price))}</b>` : ''}</td></tr>`)}</tbody>
      </table></div>
      ${sel.ids.length > sample.length ? html`<p class="muted">Показаны первые ${sample.length} из ${sel.ids.length}.</p>` : ''}
      <form method="post" action="/bulk/apply" class="inline-row">
        ${csrfField(req.ctx)}${selectionFields(sel)}${hidden(req.body)}
        <button class="primary">Применить ко всем ${sel.ids.length}</button>
        <button formaction="/bulk" type="submit">← Назад</button>
      </form>
      <p class="muted">Отменить изменение нельзя.</p>`,
    });
  });

  app.post('/bulk/apply', requireLogin, async (req, res) => {
    const sel = await selected(req, res);
    if (!sel) return;
    try {
      const spec = bulk.parseSpec(specBody(req.body));
      await bulk.apply(db, sel.ids, spec);
      res.flash('ok', `Изменено товаров: ${sel.ids.length} (${bulk.describe(spec)})`);
      res.redirect('/products');
    } catch (e) {
      if (!(e instanceof UserError)) throw e;
      await editForm(req, res, sel, { error: e.message, form: req.body }, 400);
    }
  });

  // ---- Одни и те же фото - всем выбранным товарам
  app.post('/bulk/photos', requireLogin, async (req, res) => {
    const sel = await selected(req, res);
    if (!sel) return;
    if (sel.ids.length > MAX_PHOTO_TARGETS) {
      res.flash('err', `Для фото выберите не больше ${MAX_PHOTO_TARGETS} товаров (выбрано ${sel.ids.length})`);
      return res.redirect('/products');
    }
    send(req, res, {
      title: 'Добавить фото выбранным товарам', active: 'products',
      body: html`<p>Выбрано товаров: <b>${sel.ids.length}</b>. Эти фото будут добавлены каждому из них (например, одна модель в разных размерах).
        Для раздачи по именам файлов используйте <a href="/photos/bulk">«Фото пачкой»</a>.</p>
        <form method="post" action="/bulk/photos/apply" enctype="multipart/form-data" class="card">
          ${csrfField(req.ctx)}${selectionFields(sel)}
          <input type="file" name="photos" accept="image/jpeg,image/png,image/webp" multiple required>
          <button class="primary">Загрузить</button>
        </form><p class="muted">До 12 файлов за раз, jpg, png или webp. У товара не может быть больше 12 фото.</p>`,
    });
  });

  app.post('/bulk/photos/apply', requireLogin, async (req, res) => {
    let form;
    try {
      form = await readMultipart(req, { maxFiles: 12 });
    } catch (e) {
      if (!e.status || e.status === 403) throw e;
      res.flash('err', e.message);
      return res.redirect('/products');
    }
    const sel = await resolveSelection(form.fields);
    if (!sel.ids.length || sel.ids.length > MAX_PHOTO_TARGETS) {
      res.flash('err', 'Выберите от 1 до 500 товаров');
      return res.redirect('/products');
    }
    const skus = (await db.query('SELECT id, sku FROM products WHERE id = ANY($1::int[])', [sel.ids])).rows
      .map((r) => ({ productId: r.id, sku: r.sku }));
    const report = await app.locals.ingestPhotos(form.files, () => skus);
    send(req, res, {
      title: 'Фото выбранным: результат', active: 'products',
      body: html`<div class="table-wrap"><table><thead><tr><th>Файл</th><th>Результат</th></tr></thead>
        <tbody>${report.map((r) => html`<tr><td>${r.file}</td><td class="${r.ok ? 'pos' : ''}">${r.status}</td></tr>`)}
          ${form.rejected.map((r) => html`<tr><td>${r.filename}</td><td>${r.reason}</td></tr>`)}</tbody></table></div>
        <p><a class="button" href="/products">К товарам</a></p>`,
    });
  });

  // ---- Удаление выбранных (только администратор)
  app.post('/bulk/delete', requireLogin, requireAdmin, async (req, res) => {
    const sel = await selected(req, res);
    if (!sel) return;
    await db.tx(async (c) => {
      for (let i = 0; i < sel.ids.length; i += 1000) await c.query('DELETE FROM products WHERE id = ANY($1::int[])', [sel.ids.slice(i, i + 1000)]);
    });
    res.flash('ok', `Удалено товаров: ${sel.ids.length}`);
    res.redirect('/products');
  });
};
