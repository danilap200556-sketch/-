// Тема оформления: выбор запоминается в этом браузере; пока не выбрана - как в системе.
// Подключается без defer, чтобы тема применилась до отрисовки страницы (без мигания).
(function () {
  try {
    var t = localStorage.getItem('theme');
    if (t === 'dark' || t === 'light') document.documentElement.setAttribute('data-theme', t);
  } catch (e) { /* хранилище может быть закрыто - остаётся системная тема */ }
})();
