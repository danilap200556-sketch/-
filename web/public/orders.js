// Ярлыки: запрос идёт в фоне, чтобы страница не перезагружалась, пока Маркет готовит файл (до нескольких минут).
(function () {
  var forms = document.querySelectorAll('form.labels-form');
  var status = document.getElementById('labels-status');
  var fmt = document.getElementById('label-format');
  if (!forms.length || !window.fetch) return;
  function show(text, bad) { status.hidden = false; status.textContent = text; status.className = bad ? 'neg' : 'muted'; }
  Array.prototype.forEach.call(forms, function (form) {
    form.addEventListener('submit', function (ev) {
      ev.preventDefault();
      var btn = form.querySelector('button');
      form.querySelector('.fmt-copy').value = fmt.value;
      btn.disabled = true;
      show('Маркет готовит файл с ярлыками, это может занять до нескольких минут. Не закрывайте страницу…');
      fetch(form.action, { method: 'POST', credentials: 'same-origin', headers: { 'X-Requested-With': 'fetch', 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams(new FormData(form)).toString() })
        .then(function (r) {
          if (!r.ok) return r.json().then(function (d) { throw new Error(d.error || 'ошибка'); });
          var warning = decodeURIComponent(r.headers.get('X-Labels-Warning') || '');
          var cd = r.headers.get('Content-Disposition') || '';
          var m = /filename\*=UTF-8''([^;]+)/.exec(cd);
          return r.blob().then(function (blob) {
            var a = document.createElement('a');
            a.href = URL.createObjectURL(blob);
            a.download = m ? decodeURIComponent(m[1]) : 'labels.pdf';
            document.body.appendChild(a); a.click(); a.remove();
            setTimeout(function () { URL.revokeObjectURL(a.href); }, 10000);
            show('Файл скачан.' + (warning ? ' Внимание: ' + warning : ''), !!warning);
          });
        })
        .catch(function (e) { show('Не получилось: ' + e.message, true); })
        .then(function () { btn.disabled = false; });
    });
  });
})();
