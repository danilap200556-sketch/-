'use strict';
const crypto = require('crypto');
const { html } = require('../html');
const { csrfField } = require('../layout');
const { readMultipart } = require('../multipart');
const imp = require('../importer');
const ps = require('../productsync');
const xlsx = require('../xlsx');
const { UserError } = require('../stock');

const TTL_MS = 30 * 60_000;
const SHOWN = 200;
const clip = (s, n = 90) => { const t = String(s ?? ''); return t.length > n ? `${t.slice(0, n)}…` : t; };

// Обновление данных товаров из Excel/CSV: название, описание, цена, артикул на Маркете, свой код, штрихкоды.
// Остатки не затрагиваются. Шаги: загрузка -> колонки и «что изменится» -> применить (одна транзакция).
module.exports = (app, { db, send, requireLogin, requireEditor }) => {
  const pending = new Map(); // загруженные файлы
  const backups = new Map(); // файлы «как было» после применения
  const sweep = () => {
    const now = Date.now();
    for (const m of [pending, backups]) for (const [k, v] of m) if (now - v.at > TTL_MS) m.delete(k);
  };
  setInterval(sweep, 5 * 60_000).unref();
  const take = (req, id) => { sweep(); const e = pending.get(String(id)); return e && e.userId === req.ctx.user.id ? e : null; };

  const optionsOf = (b) => ({ createMissing: b.create_missing === '1', clearEmpty: b.clear_empty === '1' });
  const widthOf = (rows) => Math.min(Math.max(...rows.slice(0, 50).map((r) => r.length), 1), 40);
  const rolesOf = (b, rows, hIdx) => {
    const w = widthOf(rows);
    if (b && Object.keys(b).some((k) => k.startsWith('role_'))) {
      return Array.from({ length: w }, (_, i) => { const v = Number(b[`role_${i}`]); return Number.isInteger(v) && v >= 0 && v < ps.ROLE_LABELS.length ? v : 0; });
    }
    return ps.guessRoles(rows[hIdx] ?? [], w);
  };
  const headerOf = (v, rows) => Math.min(Math.max((parseInt(v, 10) || 1) - 1, 0), rows.length - 1);

  async function page(req, res, e, { error = '', status = 200 } = {}) {
    const id = req.params.id;
    const b = req.method === 'POST' ? req.body : {};
    const rows = e.rows;
    const given = (req.method === 'POST' ? b.header : req.query.header) !== undefined;
    const hIdx = given ? headerOf(req.method === 'POST' ? b.header : req.query.header, rows) : ps.guessHeaderRow(rows);
    const roles = rolesOf(b, rows, hIdx);
    const opts = optionsOf(b);
    const first = req.method !== 'POST';
    const plan = await ps.plan(db, { rows, headerIdx: hIdx, roles, ...opts });
    const cols = widthOf(rows);
    const st = plan.stats;
    const changed = plan.items.filter((i) => i.status === 'update' || i.status === 'create');
    const canApply = !plan.fatal && changed.length > 0;
    send(req, res, {
      title: 'Обновление товаров из Excel', active: 'import',
      body: html`
      <p>Файл <b>${e.fileName}</b>, строк: ${rows.length}. <b>Остатки не меняются</b> - только данные карточек.</p>
      ${error ? html`<div class="flash err">${error}</div>` : ''}
      <form method="get" action="/import/products/${id}" class="inline-row">
        <label>Строка с названиями колонок: <input name="header" value="${hIdx + 1}" inputmode="numeric" size="4" maxlength="6"></label>
        <button>Показать</button>
      </form>
      <div class="table-wrap"><table>
        <thead><tr><th>№</th>${Array.from({ length: cols }, (_, i) => html`<th>Колонка ${i + 1}</th>`)}</tr></thead>
        <tbody>${rows.slice(0, 8).map((r, ri) => html`<tr class="${ri === hIdx ? 'hdr' : ''}"><td class="muted">${ri + 1}</td>${Array.from({ length: cols }, (_, i) => html`<td>${clip(r[i], 60)}</td>`)}</tr>`)}</tbody>
      </table></div>
      <form method="post" action="/import/products/${id}" class="card">
        ${csrfField(req.ctx)}<input type="hidden" name="header" value="${hIdx + 1}">
        <h2>Что означает каждая колонка</h2>
        <p class="muted">Нужна колонка «Артикул» (или «Штрихкод») - по ней ищется товар. Пустые ячейки по умолчанию ничего не меняют.
          Файл, выгруженный кнопкой «Выгрузить в Excel» на странице «Товары», подойдёт как есть: колонки остатков будут пропущены.</p>
        ${Array.from({ length: cols }, (_, i) => html`<label class="field"><span>${(rows[hIdx]?.[i] ?? '').trim() || `Колонка ${i + 1}`}</span>
          <select name="role_${i}">${ps.ROLE_LABELS.map((l, v) => html`<option value="${v}" ${v === roles[i] ? html`selected` : ''}>${l}</option>`)}</select></label>`)}
        <h2>Параметры</h2>
        <label class="check"><input type="checkbox" name="create_missing" value="1" ${opts.createMissing ? html`checked` : ''}> Создавать новые товары, если артикула нет в базе</label>
        <label class="check"><input type="checkbox" name="clear_empty" value="1" ${opts.clearEmpty ? html`checked` : ''}> Пустая ячейка очищает поле (описание, артикул на Маркете, свой код)</label>
        <div class="toolbar">
          <button class="${canApply ? '' : 'primary'}">Показать, что изменится</button>
          ${canApply ? html`<button class="primary" formaction="/import/products/${id}/apply" data-confirm="Обновить товаров: ${st.update}${st.create ? `, создать: ${st.create}` : ''}. Остатки не изменятся. Продолжить?">Применить: обновить ${st.update}${st.create ? `, создать ${st.create}` : ''}</button>` : ''}
        </div>
      </form>
      ${plan.fatal ? html`<div class="flash err">${plan.fatal}</div>` : html`
        <h2>${first ? 'Предварительный просмотр' : 'Что изменится'}</h2>
        <p>Будет обновлено: <b>${st.update}</b> · создано: <b>${st.create}</b> · без изменений: <b>${st.same}</b> ·
          не найдено: <b>${st.missing}</b> · с ошибками: <b>${st.error}</b></p>
        ${plan.problems.length ? html`<div class="card"><h2>Не будет обработано (${plan.problems.length})</h2>
          <ul class="problems">${plan.problems.slice(0, 50).map((p) => html`<li class="neg"><b>строка ${p.n}:</b> ${p.text}</li>`)}</ul>
          ${plan.problems.length > 50 ? html`<p class="muted">…и ещё ${plan.problems.length - 50}</p>` : ''}</div>` : ''}
        ${changed.length ? html`<div class="table-wrap"><table>
          <thead><tr><th>Строка</th><th>Артикул</th><th>Название</th><th>Что изменится</th></tr></thead>
          <tbody>${changed.slice(0, SHOWN).map((i) => html`<tr><td class="muted">${i.n}</td><td>${i.sku}${i.byCode ? html` <span class="muted">(найден по штрихкоду)</span>` : ''}</td><td>${clip(i.name, 60)}</td>
            <td class="diff">${i.status === 'create' ? html`<div><b>новый товар</b>${i.newCodes.length ? html`, штрихкоды: ${i.newCodes.join(', ')}` : ''}</div>`
              : i.changes.map((c) => html`<div><span class="muted">${c.label}:</span> <span class="old">${clip(c.from) || '—'}</span> → <b>${clip(c.to) || '—'}</b></div>`)}</td></tr>`)}
          </tbody></table></div>
          ${changed.length > SHOWN ? html`<p class="muted">Показаны первые ${SHOWN} из ${changed.length}; применены будут все.</p>` : ''}` : ''}`}`,
    }, status);
  }

  app.post('/import/products/upload', requireLogin, requireEditor, async (req, res) => {
    let form;
    try {
      form = await readMultipart(req, { maxFiles: 1, maxFileBytes: 15 * 1024 * 1024, maxTotalBytes: 16 * 1024 * 1024 });
    } catch (e) {
      if (!e.status || e.status === 403) throw e;
      res.flash('err', e.message);
      return res.redirect('/import');
    }
    const file = form.files[0];
    if (!file) {
      res.flash('err', form.rejected[0] ? `${form.rejected[0].filename}: ${form.rejected[0].reason}` : 'Выберите файл');
      return res.redirect('/import');
    }
    const parsed = imp.parseFile(file.buffer, file.filename);
    if (parsed.error) { res.flash('err', parsed.error); return res.redirect('/import'); }
    if (pending.size > 100) sweep();
    const id = crypto.randomBytes(16).toString('hex');
    pending.set(id, { userId: req.ctx.user.id, rows: parsed.rows, fileName: file.filename, at: Date.now() });
    res.redirect(`/import/products/${id}`);
  });

  const gone = (req, res) => { res.flash('err', 'Файл не найден или устарел - загрузите его снова'); return res.redirect('/import'); };

  app.get('/import/products/:id', requireLogin, requireEditor, async (req, res) => {
    const e = take(req, req.params.id);
    if (!e) return gone(req, res);
    return page(req, res, e);
  });
  app.post('/import/products/:id', requireLogin, requireEditor, async (req, res) => {
    const e = take(req, req.params.id);
    if (!e) return gone(req, res);
    return page(req, res, e);
  });

  app.post('/import/products/:id/apply', requireLogin, requireEditor, async (req, res) => {
    const e = take(req, req.params.id);
    if (!e) return gone(req, res);
    const hIdx = headerOf(req.body.header, e.rows);
    const opts = { rows: e.rows, headerIdx: hIdx, roles: rolesOf(req.body, e.rows, hIdx), ...optionsOf(req.body) };
    pending.delete(req.params.id); // двойной щелчок не применит дважды
    let r;
    try {
      r = await ps.apply(db, opts);
    } catch (err) {
      pending.set(req.params.id, e);
      if (err instanceof UserError) return page(req, res, e, { error: err.message, status: 400 });
      if (err.code === '22001' || err.code === '22003') return page(req, res, e, { error: 'В файле есть слишком длинное или слишком большое значение', status: 400 });
      throw err;
    }
    let token = null;
    if (r.backup) {
      token = crypto.randomBytes(16).toString('hex');
      backups.set(token, { userId: req.ctx.user.id, at: Date.now(),
        buf: xlsx.write([{ name: 'Товары (как было)', rows: r.backup.rows, numericColumns: r.backup.numericColumns, columnWidths: r.backup.columnWidths }]) });
    }
    const s = r.stats;
    send(req, res, {
      title: 'Товары обновлены', active: 'import',
      body: html`<div class="card"><h2>Готово</h2>
        <p>Обновлено товаров: <b>${r.updated}</b> · создано: <b>${r.created}</b> · без изменений: <b>${s.same}</b> · не найдено: <b>${s.missing}</b> · с ошибками: <b>${s.error}</b>. Остатки не менялись.</p>
        ${r.problems.length ? html`<ul class="problems">${r.problems.slice(0, 50).map((p) => html`<li class="neg">${p.n ? html`<b>строка ${p.n}:</b> ` : ''}${p.text}</li>`)}</ul>` : ''}
        ${token ? html`<p>Ошиблись? Скачайте файл с прежними значениями и загрузите его так же, чтобы вернуть как было
          (если нужно вернуть и пустые поля - отметьте «Пустая ячейка очищает поле»). Файл хранится на сайте полчаса.</p>
          <p><a class="button primary" href="/import/products/backup/${token}">Скачать «как было» (.xlsx)</a></p>` : ''}</div>
        <p><a class="button" href="/products">К товарам</a> <a class="button" href="/import">Ещё файл</a></p>`,
    });
  });

  app.get('/import/products/backup/:token', requireLogin, requireEditor, (req, res) => {
    sweep();
    const b = backups.get(String(req.params.token));
    if (!b || b.userId !== req.ctx.user.id) { res.flash('err', 'Файл «как было» больше не хранится'); return res.redirect('/import'); }
    const stamp = new Date().toISOString().slice(0, 10);
    res.set({
      'Content-Type': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
      'Content-Disposition': `attachment; filename="products_before_${stamp}.xlsx"; filename*=UTF-8''${encodeURIComponent(`Товары_как_было_${stamp}.xlsx`)}`,
    });
    res.send(b.buf);
  });
};
