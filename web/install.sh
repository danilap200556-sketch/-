#!/usr/bin/env bash
# Установка сайта склада на виртуальную машину Ubuntu (например, в Яндекс Облаке).
#
#   cd web && sudo ./install.sh
#
# Скрипт задаёт несколько вопросов (адрес сайта, доступ к базе), сам ставит Docker, скачивает
# сертификат Яндекса для безопасного подключения к базе, создаёт файл .env и запускает сайт.
# Повторный запуск безопасен: существующий .env он не затирает без вашего согласия.
#
# Для автоматизации значения можно передать переменными окружения: SITE_ADDRESS, DB_HOST,
# DB_PORT, DB_NAME, DB_USER, DB_PASSWORD. Флаги: --no-install (не ставить Docker и не запускать
# сайт, только подготовить .env) и --yes (не задавать вопросов, брать значения из окружения).
set -euo pipefail

cd "$(dirname "$0")"

NO_INSTALL=0
ASSUME_YES=0
for arg in "$@"; do
  case "$arg" in
    --no-install) NO_INSTALL=1 ;;
    --yes) ASSUME_YES=1 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Неизвестный параметр: $arg" >&2; exit 2 ;;
  esac
done

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die() { printf '\033[31mОшибка: %s\033[0m\n' "$*" >&2; exit 1; }

# Значения идут в .env в одинарных кавычках: перевод строки и одинарная кавычка там недопустимы.
check_value() {
  case "$2" in
    *"'"*|*$'\n'*|*$'\r'*) die "в поле «$1» нельзя использовать одинарную кавычку и переводы строки" ;;
  esac
}

# ask ИМЯ "Вопрос" [по-умолчанию] [secret]
ask() {
  local var="$1" question="$2" default="${3:-}" secret="${4:-}" value="${!1:-}"
  if [ -z "$value" ]; then
    [ "$ASSUME_YES" = 1 ] && { [ -n "$default" ] && value="$default" || die "не задано значение $var"; }
  fi
  if [ -z "$value" ]; then
    local prompt="$question"
    [ -n "$default" ] && prompt="$prompt [$default]"
    if [ "$secret" = secret ]; then
      read -r -s -p "$prompt: " value; echo
    else
      read -r -p "$prompt: " value
    fi
    value="${value:-$default}"
  fi
  [ -n "$value" ] || die "поле «$question» не может быть пустым"
  check_value "$question" "$value"
  printf -v "$var" '%s' "$value"
}

public_ip() {
  # В Яндекс Облаке адрес виртуальной машины отдаёт служба метаданных; запасной вариант - внешний сервис.
  curl -fsS -m 3 -H 'Metadata-Flavor: Google' \
    http://169.254.169.254/computeMetadata/v1/instance/network-interfaces/0/access-configs/0/external-ip 2>/dev/null \
    || curl -fsS -m 5 https://api.ipify.org 2>/dev/null || true
}

if [ "$NO_INSTALL" = 0 ]; then
  [ "$(id -u)" = 0 ] || die "запустите с правами администратора: sudo ./install.sh"
  if ! command -v docker >/dev/null 2>&1; then
    say "Устанавливаю Docker..."
    curl -fsSL https://get.docker.com | sh
  fi
  docker compose version >/dev/null 2>&1 || die "не найден docker compose (плагин). Установите docker-compose-plugin."
fi

WRITE_ENV=1
if [ -f .env ]; then
  if [ "$ASSUME_YES" = 1 ]; then
    WRITE_ENV=0
  else
    read -r -p "Файл .env уже есть. Оставить как есть? [Y/n]: " keep
    case "${keep:-Y}" in [nN]*) WRITE_ENV=1; cp .env ".env.bak.$(date +%s)"; echo "Старый .env сохранён рядом (.env.bak.*)";; *) WRITE_ENV=0;; esac
  fi
fi

if [ "$WRITE_ENV" = 1 ]; then
  say "1/3. Адрес сайта"
  echo "Нужен домен, который указывает на IP этой машины (запись A) - с ним сайт получит HTTPS автоматически."
  IP="$(public_ip)"
  if [ -n "$IP" ]; then
    echo "Публичный IP этой машины: $IP"
    echo "Если домена пока нет, для проверки подойдёт адрес ${IP//./-}.sslip.io (он сам указывает на этот IP)."
  fi
  ask SITE_ADDRESS "Адрес сайта (например, sklad.example.ru)" "${IP:+${IP//./-}.sslip.io}"

  say "2/3. База данных (та же, что у приложения)"
  echo "Адрес - основной хост кластера из Яндекс Облака (кнопка «Подключиться»): c-<id>.rw.mdb.yandexcloud.net"
  ask DB_HOST "Хост базы"
  ask DB_PORT "Порт" "6432"
  ask DB_NAME "Имя базы"
  ask DB_USER "Пользователь для сайта" "web_user"
  ask DB_PASSWORD "Пароль пользователя" "" secret

  say "3/3. Подготовка"
  mkdir -p certs
  if [ "$NO_INSTALL" = 0 ] || [ ! -s certs/root.crt ]; then
    if curl -fsSL -m 20 https://storage.yandexcloud.net/cloud-certs/CA.pem -o certs/root.crt; then
      echo "Сертификат Яндекса скачан (certs/root.crt) - соединение с базой будет проверяться."
      DB_SSL=verify
    else
      echo "Не удалось скачать сертификат Яндекса - соединение с базой будет шифроваться без проверки сертификата."
      DB_SSL=require
    fi
  else
    DB_SSL=verify
  fi
  SESSION_SECRET="$(openssl rand -hex 32)"

  umask 077
  cat > .env <<ENV
# Создано install.sh. Файл содержит пароли - никому не показывайте и не выкладывайте в git.
SITE_ADDRESS='$SITE_ADDRESS'
SESSION_SECRET='$SESSION_SECRET'
DB_HOST='$DB_HOST'
DB_PORT='$DB_PORT'
DB_NAME='$DB_NAME'
DB_USER='$DB_USER'
DB_PASSWORD='$DB_PASSWORD'
DB_SSL='$DB_SSL'
DB_SSL_CA='/certs/root.crt'
ENV
  chmod 600 .env
  echo ".env создан."
fi

if [ "$NO_INSTALL" = 1 ]; then
  say "Готово: .env подготовлен (запуск пропущен, --no-install)."
  exit 0
fi

say "Запускаю сайт..."
docker compose up -d --build

echo "Жду, пока сайт ответит..."
status=""
for _ in $(seq 1 60); do
  status="$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$(docker compose ps -q web)" 2>/dev/null || true)"
  [ "$status" = healthy ] && break
  sleep 2
done

site="$(grep -E '^SITE_ADDRESS=' .env | head -1 | cut -d= -f2- | tr -d "'")"
if [ "$status" = healthy ]; then
  say "Сайт работает."
  case "$site" in :*) url="http://<IP этой машины>${site}";; *) url="https://$site";; esac
  echo "Откройте $url и войдите логином и паролем из приложения."
  echo "Если страница не открывается: проверьте, что домен указывает на IP машины, а в группе безопасности открыты порты 80 и 443."
else
  echo "Сайт пока не отвечает. Посмотрите журнал:  docker compose logs --tail 80 web"
  echo "Частые причины: неверный пароль или адрес базы, закрыт доступ из этой машины к кластеру, приложение ещё ни разу не запускалось с этой базой."
fi
echo
echo "Полезное:  docker compose logs -f web   (журнал)   |   docker compose restart web   |   git pull && docker compose up -d --build   (обновить)"
