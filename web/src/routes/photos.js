'use strict';
const { html } = require('../html');
const { csrfField } = require('../layout');
const { readMultipart } = require('../multipart');
const photos = require('../photos');
const { UserError } = require('../stock');
const { parseId } = require('../util');

module.exports = (app, { db, send, requireLogin, requireEditor }) => {
  // ---- Отдача картинок. Отдаём только наш JPEG; на случай прямого открытия - песочница CSP.
  const image = (column) => async (req, res) => {
    const id = parseId(req.params.id);
    const r = id ? await db.query(`SELECT ${column} AS img FROM product_photos WHERE id = $1`, [id]) : { rowCount: 0 };
    if (!r.rowCount) return res.status(404).type('text').send('Нет такого фото');
    res.set({
      'Content-Type': 'image/jpeg',
      'Cache-Control': 'private, max-age=604800',
      'Content-Security-Policy': "default-src 'none'; sandbox",
      'X-Content-Type-Options': 'nosniff',
    });
    res.send(r.rows[0].img);
  };

  // /photos/bulk объявлен выше /photos/:id, иначе "bulk" попал бы в :id.
  app.get('/photos/bulk', requireLogin, requireEditor, (req, res) =>
    send(req, res, {
      title: 'Фото пачкой', active: 'products', scripts: ['/static/bulk-photos.js'],
      body: html`
      <div class="card">
        <p>Выберите много фото сразу: каждое попадёт к товару, <b>артикул</b> (или артикул на Маркете, или штрихкод)
        которого указан в названии файла. Несколько фото одного товара: <code>NK-AF1-42.jpg</code>, <code>NK-AF1-42_2.jpg</code>,
        <code>NK-AF1-42 (3).png</code>. Повторная загрузка тех же файлов дубликатов не создаёт.</p>
        <form method="post" action="/photos/bulk" enctype="multipart/form-data" id="bulk-photos" data-csrf="${req.ctx.csrf}">
          ${csrfField(req.ctx)}
          <input type="file" name="photos" id="bulk-files" accept="image/jpeg,image/png,image/webp" multiple required>
          <button class="primary">Загрузить</button>
        </form>
        <p id="bulk-progress" class="muted" hidden></p>
      </div>
      <div id="bulk-result"></div>`,
    }));

  app.get('/photos/:id/thumb', requireLogin, image('thumb'));
  app.get('/photos/:id', requireLogin, image('data'));

  app.get('/products/:id/photos.json', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    const r = id ? await db.query('SELECT id FROM product_photos WHERE product_id = $1 ORDER BY position, id', [id]) : { rows: [] };
    res.json({ ids: r.rows.map((x) => x.id) });
  });

  // Принимает файлы, привязывает каждый по решению pick(file) -> productId[] ; возвращает строки отчёта.
  async function ingest(files, targetsFor) {
    const out = [];
    for (const f of files) {
      const targets = targetsFor(f);
      if (!targets.length) { out.push({ file: f.filename, status: 'нет товара', ok: false }); continue; }
      let prepared;
      try {
        prepared = await photos.prepare(f.buffer);
      } catch (e) {
        if (!(e instanceof UserError)) throw e;
        out.push({ file: f.filename, status: e.message, ok: false });
        continue;
      }
      const counts = { added: 0, duplicate: 0, limit: 0 };
      for (const t of targets) counts[await photos.addPhoto(db, t.productId, prepared)]++;
      const parts = [];
      if (counts.added) parts.push(`добавлено${targets.length > 1 ? ` (${counts.added})` : ''}`);
      if (counts.duplicate) parts.push('уже было');
      if (counts.limit) parts.push('у товара уже 12 фото');
      out.push({ file: f.filename, sku: targets.length === 1 ? targets[0].sku : `${targets.length} товаров`, status: parts.join(', '), ok: counts.added > 0 });
    }
    return out;
  }
  app.locals.ingestPhotos = ingest;

  async function readUpload(req, res, back, opts) {
    try {
      return await readMultipart(req, opts);
    } catch (e) {
      if (!e.status || e.status === 403) throw e;
      res.flash('err', e.message);
      res.redirect(back);
      return null;
    }
  }

  // ---- Фото одного товара
  app.post('/products/:id/photos', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/products');
    const form = await readUpload(req, res, `/products/${id}`, { maxFiles: 12 });
    if (!form) return;
    const sku = (await db.query('SELECT sku FROM products WHERE id = $1', [id])).rows[0]?.sku;
    if (!sku) { res.flash('err', 'Товар не найден'); return res.redirect('/products'); }
    const report = await ingest(form.files, () => [{ productId: id, sku }]);
    const added = report.filter((r) => r.ok).length;
    const problems = [...report.filter((r) => !r.ok && r.status !== 'уже было').map((r) => `${r.file}: ${r.status}`),
      ...form.rejected.map((r) => `${r.filename}: ${r.reason}`)];
    res.flash(problems.length && !added ? 'err' : 'ok',
      `Добавлено фото: ${added}${problems.length ? `. Не добавлено: ${problems.join('; ')}` : ''}`);
    res.redirect(`/products/${id}`);
  });

  app.post('/products/:id/photos/:pid/delete', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    const pid = parseId(req.params.pid);
    if (id && pid) await db.query('DELETE FROM product_photos WHERE id = $1 AND product_id = $2', [pid, id]);
    res.flash('ok', 'Фото убрано');
    res.redirect(`/products/${id || ''}`);
  });

  app.post('/products/:id/photos/:pid/cover', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    const pid = parseId(req.params.pid);
    if (id && pid) {
      await db.query(
        `UPDATE product_photos SET position = (SELECT COALESCE(MIN(position), 0) - 1 FROM product_photos WHERE product_id = $1)
          WHERE id = $2 AND product_id = $1`, [id, pid]);
    }
    res.flash('ok', 'Главное фото изменено');
    res.redirect(`/products/${id || ''}`);
  });

  // ---- Фото пачкой по именам файлов
  app.post('/photos/bulk', requireLogin, async (req, res) => {
    const wantsJson = req.accepts(['html', 'json']) === 'json';
    let form;
    try {
      form = await readMultipart(req, { maxFiles: 15 });
    } catch (e) {
      if (!e.status) throw e;
      return wantsJson ? res.status(e.status).json({ error: e.message }) : res.status(e.status).type('text').send(e.message);
    }
    const index = await photos.loadSkuIndex(db);
    const hits = new Map(form.files.map((f) => [f, photos.matchFile(index, f.filename)]));
    // Внутри пачки фото одного товара идут по номеру (NK-42, NK-42_2, ...): так порядок получается правильным.
    const ordered = [...form.files].sort((a, b) => {
      const ha = hits.get(a); const hb = hits.get(b);
      return (ha?.productId ?? 0) - (hb?.productId ?? 0) || (ha?.order ?? 0) - (hb?.order ?? 0) || a.filename.localeCompare(b.filename, 'ru', { numeric: true });
    });
    const report = await ingest(ordered, (f) => (hits.get(f) ? [hits.get(f)] : []));
    for (const r of form.rejected) report.push({ file: r.filename || '(лишние файлы)', status: r.reason, ok: false });
    if (wantsJson) return res.json({ results: report });
    send(req, res, {
      title: 'Фото пачкой: результат', active: 'products',
      body: html`<p>Добавлено фото: <b>${report.filter((r) => r.ok).length}</b> из ${report.length}.</p>
        <div class="table-wrap"><table><thead><tr><th>Файл</th><th>Товар</th><th>Результат</th></tr></thead>
        <tbody>${report.map((r) => html`<tr><td>${r.file}</td><td>${r.sku ?? ''}</td><td class="${r.ok ? 'pos' : ''}">${r.status}</td></tr>`)}</tbody></table></div>
        <p><a class="button" href="/photos/bulk">Загрузить ещё</a> <a class="button" href="/products">К товарам</a></p>`,
    });
  });
};
