'use strict';
const { html } = require('../html');
const { csrfField } = require('../layout');
const xlsx = require('../xlsx');
const batch = require('../batch');
const st = require('../stocktake');
const ops = require('../stock');
const { findExact, suggestProducts } = require('../finder');
const bc = require('../barcode');
const { parseId, fmtDate, str } = require('../util');

const SHOWN = 300;
const VIEWS = { counted: 'Посчитанное', diff: 'Расхождения', missing: 'Не посчитано (станет 0)' };
const STATUS = { done: 'проведён', discarded: 'сброшен', open: 'идёт' };
const pieces = (n) => `${n} шт`;
const signed = (n) => (n > 0 ? `+${n}` : String(n));

// Инвентаризация по штрихкодам (то же, что раздел «Инвентаризация» в приложении): выбираете склад, начинаете подсчёт и
// сканируете - каждый скан добавляет штуку товару. При завершении остатки становятся равны посчитанному, а у товаров
// склада, которых в подсчёте нет, - нулю. Подсчёт общий с приложением и другими устройствами.
module.exports = (app, { db, send, requireLogin, requireEditor }) => {
  const notReady = (req, res) => {
    res.flash('err', 'Инвентаризация заработает после обновления настольного приложения: запустите свежую версию один раз с этой базой.');
    return res.redirect('/stock');
  };
  const guard = async (req, res, next) => ((await st.supported(db)) ? next() : notReady(req, res));
  const stocktakeId = (req) => parseId(req.params.id);
  const viewOf = (v) => (VIEWS[v] ? v : 'counted');
  const here = (id, view) => `/stocktake/${id}${view && view !== 'counted' ? `?view=${view}` : ''}`;
  const noCount = (req, res) => { res.flash('err', 'Подсчёт не найден.'); return res.redirect('/stocktake'); };

  // ---- Список складов и подсчётов
  app.get('/stocktake', requireLogin, requireEditor, guard, async (req, res) => {
    const whs = (await db.query('SELECT id, name FROM warehouses ORDER BY name')).rows;
    const { open, last } = await st.overview(db);
    const openBy = new Map(open.map((o) => [o.warehouse_id, o]));
    send(req, res, {
      title: 'Инвентаризация', active: 'stocktake',
      body: html`
      <p class="muted">Начните подсчёт склада и сканируйте штрихкоды товаров, которые лежат на складе: каждый скан - ещё одна штука.
        Когда закончите, остатки станут равны посчитанному, а у товаров, которых в подсчёте нет, на этом складе будет 0.
        Подсчёт общий: считать можно с нескольких устройств (и из приложения) одновременно. Пока идёт подсчёт, остатки склада лучше не менять.</p>
      <div class="table-wrap"><table>
        <thead><tr><th>Склад</th><th>Подсчёт</th><th class="num">Позиций</th><th class="num">Штук</th><th></th></tr></thead>
        <tbody>${whs.map((w) => {
          const o = openBy.get(w.id);
          return o ? html`<tr><td>${w.name}</td><td>№${o.id}, начат ${fmtDate(o.started_at)}${o.started_by ? `, ${o.started_by}` : ''}</td>
              <td class="num">${o.positions}</td><td class="num">${Number(o.units)}</td><td><a class="button primary" href="/stocktake/${o.id}">Продолжить</a></td></tr>`
            : html`<tr><td>${w.name}</td><td class="muted">не начат</td><td class="num"></td><td class="num"></td><td>
              <form method="post" action="/stocktake/start" class="inline">${csrfField(req.ctx)}<input type="hidden" name="warehouse" value="${w.id}"><button class="primary">Начать подсчёт</button></form></td></tr>`;
        })}
        ${whs.length ? '' : html`<tr><td colspan="5" class="muted">Сначала добавьте склад на странице «Склады».</td></tr>`}</tbody>
      </table></div>
      ${last.length ? html`<h2>Последние подсчёты</h2><div class="table-wrap"><table>
        <thead><tr><th>№</th><th>Склад</th><th>Состояние</th><th>Закончен</th><th>Кто</th><th class="num">Позиций</th><th>Документ</th></tr></thead>
        <tbody>${last.map((l) => html`<tr><td>${l.id}</td><td>${l.warehouse}</td><td>${STATUS[l.status] ?? l.status}</td><td>${fmtDate(l.finished_at)}</td><td>${l.finished_by}</td>
          <td class="num">${l.positions}</td><td>${l.document_id ? html`<a href="/documents/${l.document_id}">№${l.document_id}</a>` : html`<span class="muted">${l.status === 'done' ? 'расхождений не было' : '—'}</span>`}</td></tr>`)}</tbody>
      </table></div>` : ''}`,
    });
  });

  app.post('/stocktake/start', requireLogin, requireEditor, guard, async (req, res) => {
    const wid = parseId(req.body.warehouse);
    const wh = wid ? (await db.query('SELECT id FROM warehouses WHERE id = $1', [wid])).rows[0] : null;
    if (!wh) { res.flash('err', 'Склад не найден'); return res.redirect('/stocktake'); }
    const id = await st.start(db, wid, req.ctx.user.username);
    res.redirect(`/stocktake/${id}`);
  });

  // ---- Страница подсчёта
  app.get('/stocktake/:id', requireLogin, requireEditor, guard, async (req, res) => {
    const id = stocktakeId(req);
    const info = id ? await st.get(db, id) : null;
    if (!info) return noCount(req, res);
    const view = viewOf(str(req.query.view, 10));
    if (info.status !== 'open') {
      res.flash('err', `Подсчёт №${id} уже ${STATUS[info.status] ?? 'закрыт'}${info.document_id ? ` (документ №${info.document_id})` : ''}.`);
      return res.redirect('/stocktake');
    }
    const sum = await st.summary(db, id);
    const list = view === 'missing' ? await st.missingRows(db, id, { limit: SHOWN }) : await st.countedRows(db, id, { onlyDiff: view === 'diff', limit: SHOWN });
    const csrf = csrfField(req.ctx);
    send(req, res, {
      title: `Инвентаризация: ${info.warehouse}`, active: 'stocktake',
      body: html`
      <p class="muted">Подсчёт №${id}, склад «${info.warehouse}», начат ${fmtDate(info.started_at)}${info.started_by ? `, ${info.started_by}` : ''}.
        Каждый скан - ещё одна штука. Можно ввести артикул или «артикул количество». Чего нет в подсчёте, у того после завершения станет 0.</p>
      <form method="post" action="/stocktake/${id}/scan" class="card scanbox" autocomplete="off">
        ${csrf}<input type="hidden" name="view" value="${view}">
        <input type="text" name="code" class="scaninput" placeholder="Отсканируйте штрихкод" aria-label="Штрихкод, артикул или «артикул количество»" maxlength="100"
          data-suggest="sku" autofocus autocomplete="off" autocapitalize="off" spellcheck="false" enterkeyhint="done">
      </form>
      <p><b>Посчитано:</b> ${sum.positions} поз., ${pieces(sum.units)}.
        ${sum.positions ? html`<b>Расхождений:</b> ${sum.diffPositions} (излишек ${pieces(sum.surplus)}, недостача ${pieces(sum.shortage)}).` : ''}
        <b>Не посчитано, но числится на складе:</b> ${sum.missingPositions} поз., ${pieces(sum.missingUnits)}.</p>
      <div class="toolbar">
        ${Object.entries(VIEWS).map(([k, label]) => html`<a class="button ${k === view ? 'primary' : ''}" href="${here(id, k)}">${label}</a>`)}
        <a class="button" href="/stocktake/${id}/export.xlsx">Выгрузить в Excel</a>
      </div>
      <div class="table-wrap"><table>
        <thead><tr><th>Артикул</th><th>Название</th><th class="num">В учёте</th><th class="num">Посчитано</th><th class="num">Разница</th><th></th></tr></thead>
        <tbody>${list.rows.map((r) => html`<tr>
          <td><a href="/products/${r.product_id}">${r.sku}</a></td><td>${r.name}</td><td class="num">${r.book}</td>
          <td class="num">${view === 'missing' ? html`<span class="muted">0</span>` : html`<form method="post" action="/stocktake/${id}/row" class="inline-row">
            ${csrf}<input type="hidden" name="view" value="${view}"><input type="hidden" name="product" value="${r.product_id}">
            <input type="text" name="counted" value="${r.counted}" inputmode="numeric" size="5" maxlength="9" class="qty" aria-label="Посчитано">
            <button name="do" value="set" title="Сохранить число">OK</button><button name="do" value="inc" title="Ещё одна штука">+1</button>
            <button name="do" value="dec" title="Убрать одну штуку (лишний скан)">−1</button><button name="do" value="remove" title="Убрать товар из подсчёта">✕</button></form>`}</td>
          <td class="num ${r.counted - r.book > 0 ? 'pos' : r.counted - r.book < 0 ? 'neg' : ''}">${signed(r.counted - r.book)}</td>
          <td>${view === 'missing' ? html`<form method="post" action="/stocktake/${id}/row" class="inline">${csrf}<input type="hidden" name="view" value="${view}"><input type="hidden" name="product" value="${r.product_id}"><button name="do" value="inc" title="Лежит на складе - посчитать одну штуку">+1</button></form>` : ''}</td></tr>`)}
          ${list.rows.length ? '' : html`<tr><td colspan="6" class="muted">${view === 'missing' ? 'Всё, что числится на складе, посчитано.' : view === 'diff' ? 'Расхождений нет.' : 'Пока ничего не посчитано - сканируйте.'}</td></tr>`}</tbody>
      </table></div>
      ${list.total > list.rows.length ? html`<p class="muted">Показаны ${list.rows.length} из ${list.total}: остальные - в выгрузке в Excel.</p>` : ''}
      <div class="card">
        <form method="post" action="/stocktake/${id}/finish" class="inline-row">
          ${csrf}<label class="check"><input type="checkbox" name="zero" value="1" checked> Не посчитанное считать нулевым (при завершении у товаров склада, которых нет в подсчёте, остаток станет 0)</label>
          <button class="primary">Завершить инвентаризацию…</button>
        </form>
        <form method="post" action="/stocktake/${id}/discard" class="inline">${csrf}
          <button class="danger" data-confirm="Сбросить подсчёт №${id} (посчитано ${sum.positions} поз.)? Остатки не изменятся, посчитанное будет потеряно.">Сбросить подсчёт</button></form>
      </div>`,
    });
  });

  // ---- Скан
  app.post('/stocktake/:id/scan', requireLogin, requireEditor, guard, async (req, res) => {
    const id = stocktakeId(req);
    const view = viewOf(str(req.body.view, 10));
    const back = () => res.redirect(here(id, view));
    const code = String(req.body.code ?? '').slice(0, 200);
    if (!code.trim()) return back();
    const lines = batch.parseLines(code);
    if (lines.length !== 1 || lines[0].error) { res.flash('err', lines[0]?.error ?? 'Пусто'); return back(); }
    const line = lines[0];
    let found = await findExact(db, line.key);
    if (found.error) {
      // Не нашли целиком: часть артикула или названия - если подходит один товар, берём его.
      const some = await suggestProducts(db, line.key, 4);
      if (some.length === 1) found = { id: some[0].id };
      else if (some.length > 1) {
        res.flash('err', `«${line.key}»: подходит несколько товаров (${some.map((s) => s.sku).join(', ')}) - введите артикул точнее.`);
        return back();
      } else if (/^\d{8,14}$/.test(bc.normalize(line.key))) {
        res.flash('err', `Штрихкод ${bc.normalize(line.key)} не привязан ни к одному товару. Привяжите его в карточке товара и отсканируйте снова.`);
        return back();
      } else { res.flash('err', found.error); return back(); }
    }
    try {
      const row = await st.add(db, id, found.id, line.qty, req.ctx.user.username);
      res.flash('ok', `${row.sku} — ${row.name}: посчитано ${row.counted} (в учёте ${row.book})`);
    } catch (e) {
      if (!(e instanceof ops.UserError)) throw e;
      res.flash('err', e.message);
    }
    back();
  });

  // ---- Правка строки: задать число, +1, −1, убрать
  app.post('/stocktake/:id/row', requireLogin, requireEditor, guard, async (req, res) => {
    const id = stocktakeId(req);
    const view = viewOf(str(req.body.view, 10));
    const pid = parseId(req.body.product);
    if (!id || !pid) return noCount(req, res);
    try {
      const action = str(req.body.do, 10);
      if (action === 'inc') await st.add(db, id, pid, 1, req.ctx.user.username);
      else if (action === 'dec') await st.add(db, id, pid, -1, req.ctx.user.username);
      else if (action === 'remove') await st.setCounted(db, id, pid, -1, req.ctx.user.username);
      else {
        const raw = String(req.body.counted ?? '').trim();
        if (!/^\d{1,9}$/.test(raw)) throw new ops.ValidationError('Посчитано - целое число от 0.');
        await st.setCounted(db, id, pid, Number(raw), req.ctx.user.username);
      }
    } catch (e) {
      if (!(e instanceof ops.UserError)) throw e;
      res.flash('err', e.message);
    }
    res.redirect(here(id, view));
  });

  // ---- Завершение: сначала «что изменится», потом проведение
  app.post('/stocktake/:id/finish', requireLogin, requireEditor, guard, async (req, res) => {
    const id = stocktakeId(req);
    const info = id ? await st.get(db, id) : null;
    if (!info) return noCount(req, res);
    const zero = req.body.zero === '1';
    if (info.status !== 'open') { res.flash('err', 'Подсчёт уже завершён или сброшен.'); return res.redirect('/stocktake'); }
    if (req.body.confirm === '1') {
      let result;
      try {
        result = await st.finish(db, id, { zeroMissing: zero, user: req.ctx.user.username });
      } catch (e) {
        if (!(e instanceof ops.UserError)) throw e;
        res.flash('err', e.message);
        return res.redirect('/stocktake');
      }
      if (result.changed && result.documentId) {
        res.flash('ok', `Инвентаризация проведена: остаток изменён у ${result.changed} товаров. Документ №${result.documentId} - здесь же его можно отменить целиком.`);
        return res.redirect(`/documents/${result.documentId}`);
      }
      res.flash('ok', result.changed ? `Инвентаризация проведена: остаток изменён у ${result.changed} товаров.` : 'Инвентаризация проведена: расхождений нет, остатки не менялись.');
      return res.redirect('/stocktake');
    }
    const sum = await st.summary(db, id);
    const missing = zero ? await st.missingRows(db, id, { limit: 50 }) : { rows: [], total: 0 };
    const wipes = zero && sum.positions === 0 && sum.missingPositions > 0;
    send(req, res, {
      title: 'Завершить инвентаризацию?', active: 'stocktake',
      body: html`
      <div class="card">
        <p>Склад «<b>${info.warehouse}</b>», подсчёт №${id}.</p>
        <p>Посчитано: <b>${sum.positions}</b> поз., ${pieces(sum.units)}. Расхождения: ${sum.diffPositions} поз. (излишек ${pieces(sum.surplus)}, недостача ${pieces(sum.shortage)}).</p>
        ${zero ? html`<p>Не посчитано, но числится на складе: <b>${sum.missingPositions}</b> поз., ${pieces(sum.missingUnits)} - <b>их остаток станет 0</b>.</p>`
          : html`<p>Не посчитанные товары (${sum.missingPositions} поз.) останутся как есть.</p>`}
        ${wipes ? html`<div class="flash err">Ничего не посчитано: если продолжить, весь склад «${info.warehouse}» будет обнулён.</div>` : ''}
        ${missing.rows.length ? html`<div class="table-wrap"><table>
          <thead><tr><th>Артикул</th><th>Название</th><th class="num">Сейчас</th></tr></thead>
          <tbody>${missing.rows.map((r) => html`<tr><td>${r.sku}</td><td>${r.name}</td><td class="num">${r.book}</td></tr>`)}</tbody></table></div>
          ${missing.total > missing.rows.length ? html`<p class="muted">… и ещё ${missing.total - missing.rows.length}.</p>` : ''}` : ''}
        <p class="muted">Остатки изменятся одним документом «Инвентаризация»; его можно отменить в разделе «Документы».</p>
        <form method="post" action="/stocktake/${id}/finish" class="inline-row">
          ${csrfField(req.ctx)}${zero ? html`<input type="hidden" name="zero" value="1">` : ''}<input type="hidden" name="confirm" value="1">
          <button class="primary">${wipes ? 'Обнулить весь склад' : 'Провести'}</button>
          <a class="button" href="/stocktake/${id}">Назад</a>
        </form>
      </div>`,
    });
  });

  app.post('/stocktake/:id/discard', requireLogin, requireEditor, guard, async (req, res) => {
    const id = stocktakeId(req);
    try {
      await st.discard(db, id, req.ctx.user.username);
      res.flash('ok', `Подсчёт №${id} сброшен, остатки не менялись.`);
    } catch (e) {
      if (!(e instanceof ops.UserError)) throw e;
      res.flash('err', e.message);
    }
    res.redirect('/stocktake');
  });

  // ---- Выгрузка: посчитанное и не посчитанное
  app.get('/stocktake/:id/export.xlsx', requireLogin, requireEditor, guard, async (req, res) => {
    const id = stocktakeId(req);
    const info = id ? await st.get(db, id) : null;
    if (!info) return noCount(req, res);
    const counted = await st.countedRows(db, id, { limit: 100000 });
    const missing = await st.missingRows(db, id, { limit: 100000 });
    const rows = [['Артикул', 'Название', 'В учёте', 'Посчитано', 'Разница', 'Примечание'],
      ...counted.rows.map((r) => [r.sku, r.name, String(r.book), String(r.counted), String(r.counted - r.book), '']),
      ...missing.rows.map((r) => [r.sku, r.name, String(r.book), '0', String(-r.book), 'не посчитано'])];
    const buf = xlsx.write([{ name: 'Подсчёт', rows, numericColumns: [2, 3, 4], columnWidths: [22, 45, 10, 11, 10, 28] }]);
    const stamp = new Date().toISOString().slice(0, 10);
    res.set({
      'Content-Type': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
      'Content-Disposition': `attachment; filename="stocktake_${id}_${stamp}.xlsx"; filename*=UTF-8''${encodeURIComponent(`Инвентаризация_${info.warehouse}_${stamp}.xlsx`)}`,
    });
    res.send(buf);
  });
};
