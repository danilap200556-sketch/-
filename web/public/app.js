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
