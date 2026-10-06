// Фото пачкой: файлы уходят порциями (чтобы не упереться в размер запроса), результат собирается в одну таблицу.
(function () {
  var form = document.getElementById('bulk-photos');
  if (!form || !window.fetch || !window.FormData) return; // без JS работает обычная отправка формы
  var input = document.getElementById('bulk-files');
  var progress = document.getElementById('bulk-progress');
  var result = document.getElementById('bulk-result');
  var BATCH_FILES = 8, BATCH_BYTES = 30 * 1024 * 1024;

  function natural(a, b) { return a.name.localeCompare(b.name, 'ru', { numeric: true }); }
  function el(tag, text, cls) { var e = document.createElement(tag); if (text !== undefined) e.textContent = text; if (cls) e.className = cls; return e; }

  form.addEventListener('submit', function (ev) {
    ev.preventDefault();
    var files = Array.prototype.slice.call(input.files).sort(natural);
    if (!files.length) return;
    var batches = [], cur = [], size = 0;
    files.forEach(function (f) {
      if (cur.length && (cur.length >= BATCH_FILES || size + f.size > BATCH_BYTES)) { batches.push(cur); cur = []; size = 0; }
      cur.push(f); size += f.size;
    });
    if (cur.length) batches.push(cur);

    var all = [], done = 0;
    input.disabled = true; form.querySelector('button').disabled = true;
    progress.hidden = false; result.textContent = '';

    function next(i) {
      if (i >= batches.length) return finish();
      progress.textContent = 'Загрузка: ' + done + ' из ' + files.length + '…';
      var fd = new FormData();
      batches[i].forEach(function (f) { fd.append('photos', f, f.name); });
      fetch('/photos/bulk', { method: 'POST', body: fd, credentials: 'same-origin',
        headers: { 'X-CSRF-Token': form.getAttribute('data-csrf'), 'Accept': 'application/json' } })
        .then(function (r) { return r.json().then(function (d) { return { ok: r.ok, d: d }; }); })
        .then(function (x) {
          if (x.ok) all = all.concat(x.d.results);
          else batches[i].forEach(function (f) { all.push({ file: f.name, status: x.d.error || 'ошибка загрузки', ok: false }); });
          done += batches[i].length; next(i + 1);
        })
        .catch(function () {
          batches[i].forEach(function (f) { all.push({ file: f.name, status: 'нет связи с сайтом', ok: false }); });
          done += batches[i].length; next(i + 1);
        });
    }
    function finish() {
      progress.textContent = 'Готово: добавлено фото ' + all.filter(function (r) { return r.ok; }).length + ' из ' + all.length + '.';
      var table = el('table'), head = el('tr');
      ['Файл', 'Товар', 'Результат'].forEach(function (h) { head.appendChild(el('th', h)); });
      table.appendChild(el('thead')).appendChild(head);
      var body = el('tbody');
      all.forEach(function (r) {
        var tr = el('tr');
        tr.appendChild(el('td', r.file)); tr.appendChild(el('td', r.sku || '')); tr.appendChild(el('td', r.status, r.ok ? 'pos' : ''));
        body.appendChild(tr);
      });
      table.appendChild(body);
      var wrap = el('div', undefined, 'table-wrap'); wrap.appendChild(table); result.appendChild(wrap);
      input.value = ''; input.disabled = false; form.querySelector('button').disabled = false;
    }
    next(0);
  });
})();
