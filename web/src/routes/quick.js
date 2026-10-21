'use strict';
const { html } = require('../html');
const { csrfField } = require('../layout');
const bc = require('../barcode');
const ops = require('../stock');
const { parseId, parsePrice, str, safeLocal, field } = require('../util');
const { thumbImg, COVER_SQL } = require('../views');

// Быстрая правка карточки: всё, что чаще всего приходится исправлять (название, цена, артикулы, штрихкоды,
// остатки по складам и место хранения), на одной странице и одним сохранением - либо всё, либо ничего.
module.exports = (app, { db, send, requireLogin, requireEditor }) => {
  async function load(id) {
    const p = (await db.query(`SELECT p.*, ${COVER_SQL} AS cover_id FROM products p WHERE p.id = $1`, [id])).rows[0];
    if (!p) return null;
    const codes = (await db.query('SELECT barcode FROM product_barcodes WHERE product_id = $1 ORDER BY created_at, barcode', [id])).rows.map((r) => r.barcode);
    const stock = (await db.query(
      `SELECT w.id AS wid, w.name, COALESCE(s.quantity, 0)::int AS qty, COALESCE(s.location_id, 0) AS loc,
              COALESCE((SELECT json_agg(json_build_object('id', l.id, 'code', l.code) ORDER BY l.code) FROM locations l WHERE l.warehouse_id = w.id), '[]') AS locs
         FROM warehouses w LEFT JOIN stock s ON s.warehouse_id = w.id AND s.product_id = $1 ORDER BY w.name`, [id])).rows;
    return { p, codes, stock };
  }

  async function view(req, res, id, { error = '', form = null } = {}, status = 200) {
    const data = await load(id);
    if (!data) { res.flash('err', 'Товар не найден'); return res.redirect('/products'); }
    const { p, codes, stock } = data;
    const back = safeLocal(req.query.back ?? req.body?.back, `/products/${id}`);
    const f = form || {};
    const val = (k, d) => (f[k] !== undefined ? f[k] : d);
    send(req, res, {
      title: `Быстрая правка: ${p.sku}`, active: 'products',
      body: html`
      ${error ? html`<div class="flash err">${error}</div>` : ''}
      <form method="post" action="/products/${id}/quick" class="card quick">
        ${csrfField(req.ctx)}<input type="hidden" name="back" value="${back}">
        <div class="row">
          <div>${thumbImg(id, p.cover_id, true)}</div>
          <div class="grow">
            <div class="row">
              ${field('Артикул', 'sku', val('sku', p.sku), { required: true, max: 100 })}
              ${field('Артикул на Маркете', 'market_sku', val('market_sku', p.market_sku), { max: 200 })}
            </div>
            ${field('Название', 'name', val('name', p.name), { required: true, max: 300 })}
            ${field('Цена, ₽', 'price', val('price', String(p.price).replace('.', ',')), { inputmode: 'decimal', max: 20 })}
          </div>
        </div>

        <h2>Штрихкоды</h2>
        <ul class="codes">${codes.map((c) => html`<li><code>${c}</code>
          <label class="check"><input type="checkbox" name="rm" value="${c}" ${[].concat(f.rm || []).includes(c) ? html`checked` : ''}> убрать</label></li>`)}
          ${codes.length ? '' : html`<li class="muted">Штрихкодов нет</li>`}</ul>
        <label class="field"><span>Добавить штрихкоды (отсканируйте или введите; несколько - через пробел или запятую)</span>
          <input name="add" value="${val('add', '')}" inputmode="numeric" maxlength="300" autocomplete="off"></label>
        <label class="check"><input type="checkbox" name="generate" value="1" ${f.generate === '1' ? html`checked` : ''}> создать свой внутренний EAN-13</label>

        <h2>Остатки по складам</h2>
        <p class="muted">Впишите, сколько на самом деле лежит - в журнал попадёт инвентаризация. Не меняли - не трогаем.</p>
        ${stock.length ? html`<div class="table-wrap"><table>
          <thead><tr><th>Склад</th><th class="num">Сейчас</th><th>Фактически</th><th>Место хранения</th></tr></thead>
          <tbody>${stock.map((s) => html`<tr><td>${s.name}</td><td class="num">${s.qty}</td>
            <td><input name="qty_${s.wid}" value="${val(`qty_${s.wid}`, String(s.qty))}" inputmode="numeric" size="7" maxlength="9" class="short">
              <input type="hidden" name="was_${s.wid}" value="${s.qty}"></td>
            <td><select name="loc_${s.wid}"><option value="0">— не указано —</option>
              ${s.locs.map((l) => html`<option value="${l.id}" ${String(val(`loc_${s.wid}`, s.loc)) === String(l.id) ? html`selected` : ''}>${l.code}</option>`)}</select>
              <input type="hidden" name="locwas_${s.wid}" value="${s.loc}"></td></tr>`)}</tbody></table></div>
          <label class="field"><span>Причина исправления (необязательно, попадёт в журнал движений)</span>
            <input name="reason" value="${val('reason', '')}" maxlength="200"></label>` : html`<p class="muted">Складов пока нет.</p>`}

        <div class="toolbar">
          <button class="primary">Сохранить</button>
          <a class="button" href="${back}">Отмена</a>
          <a class="button" href="/products/${id}">Полная карточка (фото, описание)</a>
        </div>
      </form>`,
    }, status);
  }

  app.get('/products/:id/quick', requireLogin, requireEditor, async (req, res) => {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/products');
    await view(req, res, id);
  });

  app.post('/products/:id/quick', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/products');
    const b = req.body;
    const fail = (msg, status = 400) => view(req, res, id, { error: msg, form: b }, status);

    // ---- проверка всего до первой записи
    const sku = str(b.sku, 100); const name = str(b.name, 300);
    if (!sku) return fail('Введите артикул');
    if (!name) return fail('Введите название');
    const price = parsePrice(b.price);
    if (price === null) return fail('Цена - число не больше 10 000 000, до двух знаков после запятой');

    const toAdd = [...new Set(String(b.add ?? '').split(/[\s,;]+/).filter(Boolean).map(bc.normalize))];
    for (const code of toAdd) {
      const bad = bc.validate(code);
      if (bad) return fail(`Штрихкод ${code}: ${bad}`);
    }
    const toRemove = [].concat(b.rm ?? []).map(bc.normalize);
    const data = await load(id);
    if (!data) { res.flash('err', 'Товар уже удалён (возможно, с другого устройства)'); return res.redirect('/products'); }

    const edits = [];
    for (const s of data.stock) {
      const was = Number(b[`was_${s.wid}`]);
      const qtyRaw = String(b[`qty_${s.wid}`] ?? '').trim();
      const edit = { wid: s.wid, name: s.name };
      if (qtyRaw !== '' && Number.isInteger(was) && qtyRaw !== String(was)) {
        try { edit.counted = ops.qty(qtyRaw, { allowZero: true }); } catch (e) {
          if (e instanceof ops.UserError) return fail(`${s.name}: ${e.message}`);
          throw e;
        }
      }
      const locWas = String(b[`locwas_${s.wid}`] ?? '0'); const loc = String(b[`loc_${s.wid}`] ?? locWas);
      if (loc !== locWas) {
        const lid = loc === '0' ? 0 : parseId(loc);
        if (lid === null || (lid && !s.locs.some((l) => l.id === lid))) return fail(`${s.name}: это место относится к другому складу`);
        edit.location = lid;
      }
      if (edit.counted !== undefined || edit.location !== undefined) edits.push(edit);
    }
    let generated = null;

    try {
      await db.tx(async (c) => {
        try {
          const r = await c.query('UPDATE products SET sku = $2, name = $3, price = $4, market_sku = $5 WHERE id = $1',
            [id, sku, name, price, str(b.market_sku, 200)]);
          if (!r.rowCount) throw new ops.UserError('Товар уже удалён (возможно, с другого устройства)');
        } catch (e) {
          if (e.code === '23505') throw new ops.UserError(`Товар с артикулом ${sku} уже есть`);
          throw e;
        }
        if (toRemove.length) await c.query('DELETE FROM product_barcodes WHERE product_id = $1 AND barcode = ANY($2::text[])', [id, toRemove]);
        const wanted = [...toAdd];
        if (b.generate === '1') {
          for (let i = 0; i < 5 && !generated; i++) {
            const code = bc.generateInternal();
            if ((await c.query('SELECT 1 FROM product_barcodes WHERE barcode = $1', [code])).rowCount === 0) generated = code;
          }
          if (!generated) throw new ops.UserError('Не удалось подобрать свободный код, попробуйте ещё раз');
          wanted.push(generated);
        }
        for (const code of wanted) {
          const r = await c.query('INSERT INTO product_barcodes (barcode, product_id) VALUES ($1, $2) ON CONFLICT (barcode) DO NOTHING', [code, id]);
          if (!r.rowCount) {
            const owner = (await c.query('SELECT p.id, p.sku FROM product_barcodes b JOIN products p ON p.id = b.product_id WHERE b.barcode = $1', [code])).rows[0];
            if (owner && owner.id !== id) throw new ops.UserError(`Штрихкод ${code} уже привязан к товару ${owner.sku}. Один штрихкод может быть только у одного товара.`);
          }
        }
        const reason = str(b.reason, 200) || 'правка карточки';
        const doc = ops.newDoc(); // все пересчёты карточки - один документ
        for (const e of edits) {
          if (e.counted !== undefined) await ops.inventory(c, id, e.wid, e.counted, reason, doc);
          if (e.location !== undefined) {
            await c.query(`INSERT INTO stock (product_id, warehouse_id, location_id, quantity) VALUES ($1, $2, $3, 0)
                           ON CONFLICT (product_id, warehouse_id) DO UPDATE SET location_id = EXCLUDED.location_id`, [id, e.wid, e.location || null]);
          }
        }
      });
    } catch (e) {
      if (e instanceof ops.UserError) return fail(e.message, 409);
      if (e.code === '23503') return fail('Склад или товар уже удалены (возможно, с другого устройства)', 409);
      if (e.code === '22003') return fail('Слишком большое число', 400);
      throw e;
    }
    res.flash('ok', `Сохранено: ${sku}${generated ? `. Новый штрихкод ${generated}` : ''}`);
    res.redirect(safeLocal(b.back, `/products/${id}`));
  });
};
