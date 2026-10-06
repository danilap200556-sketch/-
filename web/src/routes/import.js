'use strict';
const crypto = require('crypto');
const { html } = require('../html');
const { csrfField } = require('../layout');
const { readMultipart } = require('../multipart');
const imp = require('../importer');
const xlsx = require('../xlsx');
const ops = require('../stock');
const { parseId, str } = require('../util');

const TTL_MS = 30 * 60_000;

module.exports = (app, { db, send, requireLogin, requireEditor }) => {
  // Загруженный файл ждёт сопоставления колонок на сервере (в памяти, 30 минут, только владельцу).
  const pending = new Map();
  const sweep = () => { const now = Date.now(); for (const [k, v] of pending) if (now - v.at > TTL_MS) pending.delete(k); };
  setInterval(sweep, 5 * 60_000).unref();
  const take = (req, id) => {
    sweep();
    const e = pending.get(String(id));
    return e && e.userId === req.ctx.user.id ? e : null;
  };

  const warehouses = async () => (await db.query('SELECT id, name FROM warehouses ORDER BY name')).rows;

  // ---- Страница «Импорт / экспорт»
  app.get('/import', requireLogin, async (req, res) => {
    const ctx = req.ctx;
    const whs = await warehouses();
    const editor = !ctx.user.read_only;
    send(req, res, {
      title: editor ? 'Импорт и экспорт остатков' : 'Экспорт остатков', active: 'import',
      body: html`
      <div class="card">
        <h2>Выгрузить остатки в Excel</h2>
        <p class="muted">Файл: артикул, название, склад, остаток, место хранения. ${editor ? 'Чтобы поправить остатки: скачайте, измените числа в колонке «Остаток», сохраните и загрузите файл обратно ниже в режиме «фактический остаток». ' : ''}
        Товары без остатка выгружаются с нулём.</p>
        <form method="get" action="/export/stock.xlsx" class="inline-row">
          <select name="w"><option value="">Все склады</option>${whs.map((w) => html`<option value="${w.id}">${w.name}</option>`)}</select>
          <button class="primary">Скачать .xlsx</button>
        </form>
      </div>
      ${editor ? html`<div class="card">
        <h2>Загрузить остатки из Excel или CSV</h2>
        <p class="muted">Подойдёт файл, выгруженный отсюда или из приложения, и отчёты вроде «Остатки» из МойСклад. На следующем шаге вы укажете, какая колонка что значит.
        Старый формат .xls не читается - сохраните его как .xlsx.</p>
        <form method="post" action="/import/upload" enctype="multipart/form-data" class="inline-row">
          ${csrfField(ctx)}<input type="file" name="file" accept=".xlsx,.csv" required>
          <button class="primary">Загрузить и продолжить</button></form>
      </div>` : ''}`,
    });
  });

  // ---- Выгрузка
  app.get('/export/stock.xlsx', requireLogin, async (req, res) => {
    const wid = parseId(req.query.w);
    const whs = await warehouses();
    const rows = await imp.exportRows(db, {
      q: str(req.query.q, 100), warehouseId: wid, defaultWarehouseName: whs[0]?.name ?? '',
    });
    const buf = xlsx.write([{ name: 'Остатки', rows, numericColumns: [3], columnWidths: [22, 45, 20, 10, 16] }]);
    const stamp = new Date().toISOString().slice(0, 10);
    res.set({
      'Content-Type': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
      'Content-Disposition': `attachment; filename="stock_${stamp}.xlsx"; filename*=UTF-8''${encodeURIComponent(`Остатки_${stamp}.xlsx`)}`,
    });
    res.send(buf);
  });

  // ---- Шаг 1: загрузка файла
  app.post('/import/upload', requireLogin, async (req, res) => {
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
    res.redirect(`/import/map/${id}`);
  });

  // ---- Шаг 2: сопоставление колонок
  const mapPage = async (req, res, e, error = '', status = 200) => {
    const id = req.params.id;
    const rows = e.rows;
    const header = Math.min(Math.max(parseInt(req.query.header ?? req.body?.header, 10) - 1 || 0, 0), rows.length - 1);
    const headerAuto = req.query.header === undefined && req.body?.header === undefined;
    const hIdx = headerAuto ? imp.guessHeaderRow(rows) : header;
    const cols = Math.min(Math.max(...rows.slice(0, 50).map((r) => r.length), 1), 40);
    const whs = await warehouses();
    const sel = (i) => Number(req.body?.[`role_${i}`] ?? imp.guessRole(rows[hIdx]?.[i]));
    send(req, res, {
      title: 'Что значит каждая колонка', active: 'import',
      body: html`
      <p>Файл <b>${e.fileName}</b>, строк: ${rows.length}.</p>
      ${error ? html`<div class="flash err">${error}</div>` : ''}
      <form method="get" action="/import/map/${id}" class="inline-row">
        <label>Строка с названиями колонок: <input name="header" value="${hIdx + 1}" inputmode="numeric" size="4" maxlength="6"></label>
        <button>Показать</button>
      </form>
      <h2>Предпросмотр</h2>
      <div class="table-wrap"><table>
        <thead><tr><th>№</th>${Array.from({ length: cols }, (_, i) => html`<th>Колонка ${i + 1}</th>`)}</tr></thead>
        <tbody>${rows.slice(0, 15).map((r, ri) => html`<tr class="${ri === hIdx ? 'hdr' : ''}"><td class="muted">${ri + 1}</td>${Array.from({ length: cols }, (_, i) => html`<td>${r[i] ?? ''}</td>`)}</tr>`)}</tbody>
      </table></div>
      <form method="post" action="/import/run/${id}" class="card">
        ${csrfField(req.ctx)}<input type="hidden" name="header" value="${hIdx + 1}">
        <h2>Что означает каждая колонка</h2>
        ${Array.from({ length: cols }, (_, i) => html`<label class="field"><span>${(rows[hIdx]?.[i] ?? '').trim() || `Колонка ${i + 1}`}</span>
          <select name="role_${i}">${imp.ROLE_LABELS.map((l, v) => html`<option value="${v}" ${v === sel(i) ? html`selected` : ''}>${l}</option>`)}</select></label>`)}
        <h2>Параметры</h2>
        <label class="field"><span>Склад (если в файле нет колонки «Склад»)</span>
          <select name="warehouse">${whs.map((w) => html`<option value="${w.id}" ${String(w.id) === String(req.body?.warehouse) ? html`selected` : ''}>${w.name}</option>`)}</select></label>
        <label class="check"><input type="radio" name="mode" value="inventory" ${(req.body?.mode ?? 'inventory') === 'inventory' ? html`checked` : ''}> Задать как фактический остаток (рекомендуется)</label>
        <label class="check"><input type="radio" name="mode" value="receipt" ${req.body?.mode === 'receipt' ? html`checked` : ''}> Добавить к остатку (приход)</label>
        <label class="check"><input type="radio" name="mode" value="writeoff" ${req.body?.mode === 'writeoff' ? html`checked` : ''}> Убрать из остатка (списание)</label>
        <label class="check"><input type="checkbox" name="create_missing" value="1" ${Object.keys(req.body || {}).length ? (req.body.create_missing === '1' ? html`checked` : '') : html`checked`}> Создавать новые товары, если ключ не найден (не для списания)</label>
        <button class="primary">Импортировать</button>
      </form>`,
    }, status);
  };

  app.get('/import/map/:id', requireLogin, requireEditor, async (req, res) => {
    const e = take(req, req.params.id);
    if (!e) { res.flash('err', 'Файл не найден или устарел - загрузите его снова'); return res.redirect('/import'); }
    await mapPage(req, res, e);
  });

  // ---- Шаг 3: импорт
  app.post('/import/run/:id', requireLogin, async (req, res) => {
    const e = take(req, req.params.id);
    if (!e) { res.flash('err', 'Файл не найден или устарел - загрузите его снова'); return res.redirect('/import'); }
    const b = req.body;
    const cols = Math.min(Math.max(...e.rows.slice(0, 50).map((r) => r.length), 1), 40);
    const roles = Array.from({ length: cols }, (_, i) => {
      const v = Number(b[`role_${i}`]);
      return Number.isInteger(v) && v >= 0 && v <= 5 ? v : 0;
    });
    const mode = ['inventory', 'receipt', 'writeoff'].includes(b.mode) ? b.mode : 'inventory';
    let result;
    try {
      result = await imp.run(db, {
        rows: e.rows, headerIdx: Math.max(0, (parseInt(b.header, 10) || 1) - 1), roles, mode,
        warehouseId: parseId(b.warehouse), createMissing: b.create_missing === '1', fileName: e.fileName,
      });
    } catch (err) {
      if (!(err instanceof ops.UserError)) throw err;
      return mapPage(req, res, e, err.message, 400);
    }
    pending.delete(req.params.id); // повторная отправка не задвоит остатки
    send(req, res, {
      title: 'Импорт завершён', active: 'import',
      body: html`<div class="card"><pre class="log">${result.log.join('\n')}</pre></div>
        <p><a class="button" href="/stock">К остаткам</a> <a class="button" href="/import">Ещё файл</a></p>`,
    });
  });
};
