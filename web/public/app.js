// Подтверждение опасных действий и скрытие полей "На склад" вне перемещения.
document.addEventListener('submit', function (e) {
  var msg = e.target.getAttribute('data-confirm');
  if (msg && !window.confirm(msg)) e.preventDefault();
});
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
