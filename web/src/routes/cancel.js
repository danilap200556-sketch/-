'use strict';
const { html } = require('../html');
const { csrfField } = require('../layout');
const ops = require('../stock');
const docs = require('../documents');
const { parseId, fmtDate, str, safeLocal } = require('../util');

// Отмена операций из журнала: страница «что изменится» и применение. Журнал только дополняется:
// отмена - новая запись с противоположным изменением (см. stock.js).
module.exports = (app, { db, send, requireLogin, requireEditor }) => {
  // 1 запись, 2 записи, 5 записей
  const records = (n) => `${n} ${n % 10 === 1 && n % 100 !== 11 ? 'запись' : (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14) ? 'записи' : 'записей')}`;
  const idsOf = (v) => [...new Set(String(v ?? '').split(',').map((x) => parseId(x.trim())).filter(Boolean))].slice(0, 2000);

  async function confirmPage(req, res, ids, { reason = '', back, error = '', status = 200, whole = false, doc = null } = {}) {
    const plan = await ops.planCancel(db, ids);
    const first = ids[0];
    const docIds = first && !whole ? await ops.documentIds(db, first) : [];
    const planned = new Set([...ids, ...plan.rows.map((r) => r.id)]);
    const moreInDoc = docIds.some((d) => !planned.has(d)); // перемещение - пара, её отдельно предлагать не нужно
    const bad = plan.problems.length > 0 || !plan.rows.length;
    send(req, res, {
      title: 'Отмена операции', active: doc ? 'documents' : 'movements',
      body: html`
      ${error ? html`<div class="flash err">${error}</div>` : ''}
      <h2>${doc ? html`Отменить ${docs.title(doc)} целиком: ${records(plan.rows.length)}` : html`Отменить ${plan.rows.length > 1 ? `операцию: ${records(plan.rows.length)}` : 'запись журнала'}`}</h2>
      <p class="muted">Журнал не стирается: в него добавится документ «Отмена» с записями с противоположным изменением, остатки вернутся к прежним значениям.
        Каждую запись можно отменить один раз.</p>
      ${plan.problems.length ? html`<div class="card"><h2>Отменить нельзя</h2>
        <ul class="problems">${plan.problems.map((p) => html`<li class="neg">${p}</li>`)}</ul></div>` : ''}
      ${plan.rows.length ? html`<div class="table-wrap"><table>
        <thead><tr><th>№</th><th>Дата</th><th>Артикул</th><th>Склад</th><th>Запись</th><th class="num">Было</th><th class="num">Отмена даст</th><th class="num">Сейчас</th><th class="num">Станет</th></tr></thead>
        <tbody>${plan.rows.map((r) => html`<tr class="${r.after < 0 ? 'bad' : ''}"><td class="muted">${r.id}</td><td>${fmtDate(r.created_at)}</td>
          <td><a href="/products/${r.product_id}">${r.sku}</a></td><td>${r.wname}</td><td>${ops.TYPE_LABELS[r.type] || r.type}</td>
          <td class="num">${r.delta > 0 ? '+' : ''}${r.delta}</td><td class="num ${r.delta > 0 ? 'neg' : 'pos'}"><b>${-r.delta > 0 ? '+' : ''}${-r.delta}</b></td>
          <td class="num">${r.current}</td><td class="num ${r.after < 0 ? 'neg' : ''}">${r.after}</td></tr>`)}</tbody>
      </table></div>` : ''}
      ${bad ? html`<p><a class="button" href="${back}">Назад</a></p>` : html`
      <form method="post" action="/movements/cancel" class="card">
        ${csrfField(req.ctx)}<input type="hidden" name="idlist" value="${ids.join(',')}"><input type="hidden" name="back" value="${back}">
        ${doc ? html`<input type="hidden" name="docno" value="${doc.id}">` : ''}
        <label class="field"><span>Причина (необязательно, попадёт в журнал)</span>
          <input name="reason" value="${reason || (doc ? `документ №${doc.id}` : '')}" maxlength="200" placeholder="например: ошиблись артикулом" autofocus></label>
        <div class="toolbar">
          <button class="danger-ok">Отменить ${plan.rows.length > 1 ? records(plan.rows.length) : 'запись'}</button>
          <a class="button" href="${back}">Назад</a>
          ${moreInDoc ? html`<a class="button" href="/movements/cancel?${new URLSearchParams({ id: first, doc: '1', back }).toString()}">Это часть документа из ${docIds.length} записей - отменить весь документ</a>` : ''}
        </div>
      </form>`}`,
    }, status);
  }

  const notReady = (req, res) => {
    res.flash('err', 'Отмена операций заработает после обновления настольного приложения: запустите свежую версию один раз с этой базой.');
    return res.redirect('/movements');
  };

  app.get('/movements/cancel', requireLogin, requireEditor, async (req, res) => {
    if (!(await ops.cancelSupported(db))) return notReady(req, res);
    const id = parseId(req.query.id);
    const back = safeLocal(req.query.back, '/movements');
    if (!id) { res.flash('err', 'Не выбрана запись журнала'); return res.redirect(back); }
    const whole = req.query.doc === '1';
    const ids = whole ? await ops.documentIds(db, id) : [id];
    return confirmPage(req, res, ids, { back, whole });
  });

  app.post('/movements/cancel', requireLogin, requireEditor, async (req, res) => {
    if (!(await ops.cancelSupported(db))) return notReady(req, res);
    const ids = idsOf(req.body.idlist);
    const back = safeLocal(req.body.back, '/movements');
    const reason = str(req.body.reason, 200);
    const docno = parseId(req.body.docno);
    const doc = docno && (await ops.docsSupported(db)) ? await docs.get(db, docno) : null;
    if (!ids.length) { res.flash('err', 'Не выбрана запись журнала'); return res.redirect(back); }
    try {
      const { rows, doc: cancelKey } = await ops.cancel(db, ids, reason, req.ctx.user.username);
      const units = rows.reduce((n, r) => n + Math.abs(r.delta), 0);
      const no = (await ops.docsSupported(db)) ? await docs.idByKey(db, cancelKey) : 0;
      res.flash('ok', `Отменено записей: ${rows.length} (${units} шт). Остатки возвращены, ` +
        (no ? `создан документ «Отмена» №${no}.` : 'в журнале появились записи «Отмена».'));
      return res.redirect(back);
    } catch (e) {
      if (e instanceof ops.UserError) return confirmPage(req, res, ids, { reason, back, status: 409, error: 'Ничего не отменено.', doc });
      if (e.code === '22003') return confirmPage(req, res, ids, { reason, back, status: 409, error: 'Слишком большое число в остатке.', doc });
      throw e;
    }
  });

  // ---- Отмена документа целиком (из раздела «Документы»)
  const docNotReady = (req, res) => {
    res.flash('err', 'Документы заработают после обновления настольного приложения: запустите свежую версию один раз с этой базой.');
    return res.redirect('/movements');
  };
  const cancelDocument = async (req, res, d) => {
    const ids = await docs.cancellableIds(db, d.key);
    if (!ids.length) {
      res.flash('err', d.kind === 'cancel' ? 'Документ «Отмена» отменить нельзя - проведите операцию заново' : `${docs.title(d)}: отменять уже нечего`);
      return res.redirect(`/documents/${d.id}`);
    }
    return confirmPage(req, res, ids, { back: `/documents/${d.id}`, whole: true, doc: d });
  };
  const ready = async () => (await ops.cancelSupported(db)) && (await ops.docsSupported(db));

  // Последний документ, в котором ещё есть что отменять: ошиблись только что - одна кнопка.
  app.get('/documents/last/cancel', requireLogin, requireEditor, async (req, res) => {
    if (!(await ready())) return docNotReady(req, res);
    const id = await docs.lastCancellable(db);
    const d = id ? await docs.get(db, id) : null;
    if (!d) { res.flash('err', 'Нет документов, которые можно отменить'); return res.redirect('/documents'); }
    return cancelDocument(req, res, d);
  });

  app.get('/documents/:id/cancel', requireLogin, requireEditor, async (req, res) => {
    if (!(await ready())) return docNotReady(req, res);
    const id = parseId(req.params.id);
    const d = id ? await docs.get(db, id) : null;
    if (!d) { res.flash('err', 'Документ не найден'); return res.redirect('/documents'); }
    return cancelDocument(req, res, d);
  });
};
