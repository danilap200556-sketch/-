// Подтверждение опасных действий (форма или кнопка с data-confirm).
document.addEventListener('submit', function (e) {
  var msg = (e.submitter && e.submitter.getAttribute('data-confirm')) || e.target.getAttribute('data-confirm');
  if (msg && !window.confirm(msg)) e.preventDefault();
});

// Выбрать все товары на странице.
(function () {
  var all = document.getElementById('sel-all');
  if (!all) return;
  all.addEventListener('change', function () {
    var boxes = document.querySelectorAll('input.sel');
    for (var i = 0; i < boxes.length; i++) boxes[i].checked = all.checked;
  });
})();

// Операция с остатком: поле "На склад" только для перемещения, подписи по виду операции.
(function () {
  var kind = document.getElementById('kind');
  if (!kind) return;
  var labels = {
    receipt: ['Склад', 'Сколько пришло, шт'], writeoff: ['Склад', 'Сколько списать, шт'],
    transfer: ['Со склада', 'Сколько перенести, шт'], inventory: ['Склад', 'Фактически посчитано, шт'],
  };
  function update() {
    var only = document.querySelectorAll('[data-only]');
    for (var i = 0; i < only.length; i++) only[i].hidden = only[i].getAttribute('data-only') !== kind.value;
    var l = labels[kind.value];
    document.getElementById('wlabel').textContent = l[0];
    document.getElementById('qlabel').textContent = l[1];
  }
  kind.addEventListener('change', update);
  update();
})();

// Быстрый просмотр фото: клик по миниатюре открывает крупно, стрелки листают.
(function () {
  var overlay = null, img = null, counter = null, ids = [], pos = 0;
  function close() { if (overlay) { overlay.remove(); overlay = null; } document.removeEventListener('keydown', onKey); }
  function show(i) {
    pos = (i + ids.length) % ids.length;
    img.src = '/photos/' + ids[pos];
    counter.textContent = (pos + 1) + ' / ' + ids.length;
  }
  function onKey(e) {
    if (e.key === 'Escape') close();
    else if (e.key === 'ArrowLeft') show(pos - 1);
    else if (e.key === 'ArrowRight') show(pos + 1);
  }
  function open(list, start) {
    if (!list.length) return;
    ids = list;
    overlay = document.createElement('div');
    overlay.className = 'lightbox';
    img = document.createElement('img');
    counter = document.createElement('div');
    counter.className = 'lightbox-counter';
    var prev = document.createElement('button'), next = document.createElement('button'), x = document.createElement('button');
    prev.className = 'lb-prev'; next.className = 'lb-next'; x.className = 'lb-close';
    prev.textContent = '‹'; next.textContent = '›'; x.textContent = '×';
    prev.setAttribute('aria-label', 'Назад'); next.setAttribute('aria-label', 'Дальше'); x.setAttribute('aria-label', 'Закрыть');
    prev.onclick = function (e) { e.stopPropagation(); show(pos - 1); };
    next.onclick = function (e) { e.stopPropagation(); show(pos + 1); };
    x.onclick = close;
    overlay.onclick = function (e) { if (e.target === overlay) close(); };
    overlay.appendChild(img); overlay.appendChild(counter); overlay.appendChild(prev); overlay.appendChild(next); overlay.appendChild(x);
    document.body.appendChild(overlay);
    document.addEventListener('keydown', onKey);
    show(start);
  }
  document.addEventListener('click', function (e) {
    var t = e.target;
    if (!t.classList || !t.classList.contains('thumb') || !t.getAttribute('data-product')) return;
    e.preventDefault();
    var photo = Number(t.getAttribute('data-photo') || 0);
    fetch('/products/' + t.getAttribute('data-product') + '/photos.json', { credentials: 'same-origin' })
      .then(function (r) { return r.json(); })
      .then(function (d) { open(d.ids, Math.max(0, d.ids.indexOf(photo))); });
  });
})();

// Поиск товара из любого места: клавиша "/" ставит курсор в строку поиска, а сканер штрихкодов,
// когда курсор не в поле ввода, работает сам: быстрые цифры + Enter открывают найденный товар.
(function () {
  var box = document.getElementById('navsearch');
  if (!box) return;
  function typing(el) {
    return el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.tagName === 'SELECT' || el.isContentEditable);
  }
  var buf = '', last = 0;
  document.addEventListener('keydown', function (e) {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (typing(document.activeElement)) { buf = ''; return; }
    if (e.key === '/') { e.preventDefault(); box.focus(); box.select(); return; }
    var now = Date.now();
    if (now - last > 100) buf = ''; // человек печатает медленнее сканера - не считаем это сканированием
    last = now;
    if (e.key === 'Enter') {
      if (buf.length >= 6) { e.preventDefault(); window.location.href = '/find?q=' + encodeURIComponent(buf); }
      buf = '';
    } else if (e.key.length === 1) {
      buf += e.key;
      if (buf.length > 64) buf = '';
    }
  });
})();

// Переключатель светлой / тёмной темы (запоминается в браузере).
(function () {
  var btn = document.getElementById('theme-toggle');
  if (!btn) return;
  btn.addEventListener('click', function () {
    var root = document.documentElement;
    var cur = root.getAttribute('data-theme') ||
      (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
    var next = cur === 'dark' ? 'light' : 'dark';
    root.setAttribute('data-theme', next);
    try { localStorage.setItem('theme', next); } catch (e) { /* без запоминания */ }
  });
})();

// Подсказки при вводе артикула, названия или штрихкода: поля с data-suggest.
//  data-suggest="sku"  - выбранный товар подставляет свой артикул в это поле (data-next - id поля, куда перейти дальше);
//  data-suggest="line" - выбранный товар добавляется строкой «артикул 1» в список позиций (data-target - id textarea).
(function () {
  var inputs = document.querySelectorAll('input[data-suggest]');
  Array.prototype.forEach.call(inputs, function (input) {
    var mode = input.getAttribute('data-suggest');
    var list = document.createElement('ul');
    list.className = 'suggest';
    list.setAttribute('role', 'listbox');
    list.hidden = true;
    var host = input.parentNode;
    host.classList.add('suggest-wrap');
    host.appendChild(list);
    var timer = null, seq = 0, active = -1;

    function hide() { list.hidden = true; active = -1; }
    function mark(i) {
      var items = list.children;
      for (var k = 0; k < items.length; k++) items[k].classList.toggle('active', k === i);
      active = i;
      if (items[i] && items[i].scrollIntoView) items[i].scrollIntoView({ block: 'nearest' });
    }
    function choose(li) {
      var sku = li.getAttribute('data-sku');
      hide();
      if (mode === 'line') {
        var area = document.getElementById(input.getAttribute('data-target'));
        if (area) {
          var cur = area.value;
          area.value = cur + (cur && !/\n$/.test(cur) ? '\n' : '') + sku + ' 1\n';
          area.scrollTop = area.scrollHeight;
        }
        input.value = '';
        input.focus();
      } else {
        input.value = sku;
        var next = document.getElementById(input.getAttribute('data-next') || '');
        if (next) next.focus(); else input.focus();
      }
    }
    function render(items) {
      list.textContent = '';
      items.forEach(function (it) {
        var li = document.createElement('li');
        li.setAttribute('role', 'option');
        li.setAttribute('data-sku', it.sku);
        var b = document.createElement('b'); b.textContent = it.sku;
        var span = document.createElement('span'); span.textContent = ' - ' + it.name;
        li.appendChild(b); li.appendChild(span);
        if (it.market_sku) { var m = document.createElement('small'); m.className = 'muted'; m.textContent = '  [' + it.market_sku + ']'; li.appendChild(m); }
        li.addEventListener('mousedown', function (e) { e.preventDefault(); choose(li); });
        list.appendChild(li);
      });
      list.hidden = !items.length;
      active = -1;
    }
    function search() {
      var q = input.value.trim().replace(/\s+/g, ' ');
      if (q.length < 2 || /^[0-9 ]{8,}$/.test(q)) { hide(); return; }
      var mine = ++seq;
      fetch('/find/suggest?q=' + encodeURIComponent(q), { credentials: 'same-origin' })
        .then(function (r) { return r.ok ? r.json() : { items: [] }; })
        .then(function (d) { if (mine === seq && document.activeElement === input) render(d.items || []); })
        .catch(function () { hide(); });
    }
    input.addEventListener('input', function () { clearTimeout(timer); timer = setTimeout(search, 150); });
    input.addEventListener('keydown', function (e) {
      if (list.hidden) return;
      var n = list.children.length;
      if (e.key === 'ArrowDown') { e.preventDefault(); mark(active < 0 ? 0 : Math.min(active + 1, n - 1)); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); mark(active <= 0 ? n - 1 : active - 1); }
      else if (e.key === 'Escape') { e.preventDefault(); hide(); }
      else if (e.key === 'Enter' && active >= 0) { e.preventDefault(); choose(list.children[active]); }
    });
    input.addEventListener('blur', function () { setTimeout(hide, 120); });
  });
})();
