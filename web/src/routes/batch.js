'use strict';
const crypto = require('crypto');
const { html } = require('../html');
const { csrfField } = require('../layout');
const batch = require('../batch');
const docs = require('../documents');
const ops = require('../stock');
const { parseId, str } = require('../util');
const { thumbImg } = require('../views');

const KINDS = { receipt: 'Приход', writeoff: 'Списание' };
const TOKEN_TTL = 60 * 60_000;

// Приход / списание списком: много позиций одним документом. Шаги: ввод -> проверка (что и как изменится) -> выполнить.
module.exports = (app, { db, send, requireLogin, requireEditor }) => {
  // Одноразовые метки форм: двойной щелчок по «Выполнить» или повторная отправка не выполнит партию дважды.
  const tokens = new Map();
  setInterval(() => { const now = Date.now(); for (const [k, v] of tokens) if (now - v.at > TOKEN_TTL) tokens.delete(k); }, 5 * 60_000).unref();
  const issue = (userId) => {
    if (tokens.size > 5000) for (const k of [...tokens.keys()].slice(0, 1000)) tokens.delete(k); // защита от разрастания
    const t = crypto.randomBytes(16).toString('hex');
    tokens.set(t, { userId, at: Date.now() });
    return t;
  };
  // Метка из формы, если она ещё действует; иначе - новая (форма не должна "ломаться" из-за старой метки).
  const live = (token, userId) => (tokens.get(String(token ?? ''))?.userId === userId ? String(token) : issue(userId));

  const warehouses = async () => (await db.query('SELECT id, name FROM warehouses ORDER BY name')).rows;

  async function view(req, res, { form = {}, prep = null, error = '', status = 200, token } = {}) {
    const whs = await warehouses();
    const kind = KINDS[form.kind] ? form.kind : 'receipt';
    const wid = parseId(form.warehouse) || whs[0]?.id || 0;
    const whName = whs.find((w) => w.id === wid)?.name ?? '';
    const ok = prep && !prep.problems.length && prep.items.length > 0;
    send(req, res, {
      title: 'Приход / списание списком', active: 'stock',
      body: html`
      ${error ? html`<div class="flash err">${error}</div>` : ''}
      <form method="post" action="/batch/preview" class="card batch">
        ${csrfField(req.ctx)}<input type="hidden" name="token" value="${token || issue(req.ctx.user.id)}">
        <div class="row">
          <label class="field"><span>Что сделать</span><select name="kind">
            ${Object.entries(KINDS).map(([k, t]) => html`<option value="${k}" ${k === kind ? html`selected` : ''}>${t}</option>`)}</select></label>
          <label class="field"><span>Склад</span><select name="warehouse">
            ${whs.map((w) => html`<option value="${w.id}" ${w.id === wid ? html`selected` : ''}>${w.name}</option>`)}</select></label>
          <label class="field"><span>Комментарий (в журнал)</span><input name="comment" value="${form.comment ?? ''}" maxlength="300" placeholder="например: приёмка от поставщика №15"></label>
        </div>
        <label class="field"><span>Список позиций - по одной в строке</span>
          <textarea name="lines" rows="10" maxlength="200000" spellcheck="false" ${prep ? '' : html`autofocus`}
            placeholder="Сканируйте штрихкоды прямо сюда: каждый скан - 1 шт.&#10;Другое количество: 4006381333931 5&#10;Можно вставить два столбца из Excel: артикул и количество">${form.lines ?? ''}</textarea></label>
        <p class="muted">Строка - штрихкод, артикул или артикул на Маркете, затем (через пробел, «;» или Tab) количество; без количества - 1 шт.
          Одинаковые позиции складываются. Всё выполняется одним действием: если хоть одна строка не проходит, не меняется ничего.</p>
        <div class="toolbar">
          <button class="primary">Проверить</button>
          ${ok ? html`<button formaction="/batch/apply" class="danger-ok" data-confirm="${KINDS[kind]}: ${prep.items.length} поз., ${prep.totalQty} шт, склад «${whName}». Выполнить?">Выполнить: ${KINDS[kind].toLowerCase()} ${prep.items.length} поз., ${prep.totalQty} шт</button>` : ''}
        </div>
      </form>
      ${prep ? html`
        ${prep.problems.length ? html`<div class="card"><h2>Нужно исправить (${prep.problems.length})</h2>
          <ul class="problems">${prep.problems.map((p) => html`<li class="neg">${p.n ? html`<b>строка ${p.n}:</b> ` : ''}${p.text}</li>`)}</ul>
          <p class="muted">Поправьте список выше и нажмите «Проверить». Пока есть ошибки, выполнить нельзя.</p></div>` : ''}
        ${prep.items.length ? html`<h2>${KINDS[kind]}: ${prep.items.length} поз., ${prep.totalQty} шт, склад «${whName}»</h2>
          <div class="table-wrap"><table>
            <thead><tr><th></th><th>Артикул</th><th>Название</th><th class="num">Сейчас</th><th class="num">${kind === 'writeoff' ? 'Списать' : 'Добавить'}</th><th class="num">Станет</th><th>Строки</th></tr></thead>
            <tbody>${prep.items.map((i) => html`<tr class="${kind === 'writeoff' && i.after < 0 ? 'bad' : ''}">
              <td>${thumbImg(i.id, i.cover_id)}</td><td><a href="/products/${i.id}">${i.sku}</a></td><td>${i.name}</td>
              <td class="num">${i.current}</td><td class="num ${kind === 'writeoff' ? 'neg' : 'pos'}"><b>${kind === 'writeoff' ? '−' : '+'}${i.qty}</b></td>
              <td class="num ${i.after < 0 ? 'neg' : ''}">${i.after}</td><td class="muted">${i.lines.join(', ')}</td></tr>`)}</tbody>
          </table></div>` : ''}` : ''}`,
    }, status);
  }

  const readForm = (b) => ({ kind: str(b.kind, 20), warehouse: str(b.warehouse, 10), comment: str(b.comment, 300), lines: String(b.lines ?? '').slice(0, batch.MAX_TEXT) });

  app.get('/batch', requireLogin, requireEditor, (req, res) => view(req, res, { form: { kind: str(req.query.kind, 20) } }));

  app.post('/batch/preview', requireLogin, async (req, res) => {
    const form = readForm(req.body);
    const kind = KINDS[form.kind] ? form.kind : 'receipt';
    const wid = parseId(form.warehouse);
    if (!wid) return view(req, res, { form, error: 'Выберите склад', status: 400, token: live(req.body.token, req.ctx.user.id) });
    const prep = await batch.prepare(db, form.lines, kind, wid);
    return view(req, res, { form, prep, token: live(req.body.token, req.ctx.user.id) });
  });

  app.post('/batch/apply', requireLogin, async (req, res) => {
    const form = readForm(req.body);
    const kind = KINDS[form.kind] ? form.kind : null;
    const wid = parseId(form.warehouse);
    if (!kind || !wid) return view(req, res, { form, error: 'Выберите действие и склад', status: 400, token: live(req.body.token, req.ctx.user.id) });
    const warehouse = (await warehouses()).find((w) => w.id === wid);
    if (!warehouse) return view(req, res, { form, error: 'Склад не найден', status: 400, token: live(req.body.token, req.ctx.user.id) });

    // метка форм одноразовая: берём её до выполнения (JS однопоточный, второй такой же запрос уже не пройдёт)
    const token = String(req.body.token ?? '');
    const t = tokens.get(token);
    if (!t || t.userId !== req.ctx.user.id) {
      res.flash('err', 'Эта партия уже выполнена или форма устарела. Проверьте журнал движений, чтобы не сделать дважды.');
      return res.redirect('/movements');
    }
    tokens.delete(token);

    let result;
    try {
      result = await batch.apply(db, form.lines, kind, wid, form.comment, req.ctx.user.username);
    } catch (e) {
      tokens.set(token, t); // ничего не выполнено - можно повторить
      if (e.code === '22003') return view(req, res, { form, error: 'Слишком большое число в остатке', status: 400, token });
      throw e;
    }
    if (!result.ok) {
      tokens.set(token, t);
      const prep = await batch.prepare(db, form.lines, kind, wid);
      prep.problems = result.problems.length ? result.problems : prep.problems;
      return view(req, res, { form, prep, error: 'Ничего не выполнено: в списке есть ошибки.', status: 409, token });
    }
    const docNo = (await ops.docsSupported(db)) ? await docs.idByKey(db, result.doc) : 0;
    if (docNo) {
      res.flash('ok', `${KINDS[kind]}: ${result.items.length} поз., ${result.totalQty} шт, склад «${warehouse.name}». Создан документ №${docNo}: открыть и при ошибке отменить целиком его можно в разделе «Документы».`);
      return res.redirect(`/documents/${docNo}`);
    }
    res.flash('ok', `${KINDS[kind]}: ${result.items.length} поз., ${result.totalQty} шт, склад «${warehouse.name}». Подробности - в журнале.`);
    res.redirect('/movements');
  });
};
