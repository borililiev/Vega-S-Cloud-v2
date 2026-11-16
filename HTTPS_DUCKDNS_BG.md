# Vega-S Cloud v2 — публично доверен HTTPS през DuckDNS

Тази конфигурация е **незадължителна**. Съществуващите `compose.yaml` и `Caddyfile` остават непроменени. Преди промяната запази `.env`. Никога не публикувай DuckDNS токена, `.env` или ключове в GitHub.

## Защо DNS-01

Публичният IP е `213.91.182.27`, приложението работи на `38443/TCP`, а портове 80/443 се използват от други услуги. За публично доверен сертификат за **DNS име** ще използваме безплатен DuckDNS поддомейн и Let's Encrypt DNS-01. Това не изисква пренасочване на нови портове. Caddy с модула DuckDNS автоматично ще управлява подновяването.

## Инсталиране

1. В [DuckDNS](https://www.duckdns.org/) регистрирай свободно име, например `vega-cloud-myhome.duckdns.org`. В профила си задай неговия IPv4 адрес на `213.91.182.27`. Запази DuckDNS API token **само при себе си**. Ако е настроен IPv6/AAAA, увери се, че той също води до работещо сървърно приложение; иначе го изчисти.

2. На сървъра:

```bash
cd /opt/Vega-S-Cloud-v2
git pull
cp -p .env "$HOME/vega-cloud-env-backup"
nano .env
```

Запази съществуващите `VEGA_ADMIN_PASSWORD` и `VEGA_SESSION_SECRET`, но актуализирай/добави **точно по един** ред за всяка стойност:

```dotenv
VEGA_PUBLIC_URL=https://vega-cloud-myhome.duckdns.org:38443
VEGA_DOMAIN=vega-cloud-myhome.duckdns.org
DUCKDNS_API_TOKEN=ПОСТАВИ_ЛИЧНИЯ_ТОКЕН_САМО_ЛОКАЛНО
```

Замени примерното име с действителното си име. Изпълни `chmod 600 .env`. **Не изпращай съдържанието на .env и не качвай неговия backup в GitHub.**

3. Провери DNS:

```bash
getent ahostsv4 vega-cloud-myhome.duckdns.org
```

Трябва да се вижда `213.91.182.27` (използвай действителното име).

4. Построй Caddy с DuckDNS плъгин и стартирай новата Compose конфигурация:

```bash
docker compose -f compose.duckdns.yaml config --quiet
docker compose -f compose.duckdns.yaml build caddy
docker compose -f compose.duckdns.yaml up -d --build
docker compose -f compose.duckdns.yaml logs --tail=100 caddy
```

**Използвай само `-f compose.duckdns.yaml`**. Този файл е самостоятелна алтернатива; не го добавяй като втори `-f` след основния `compose.yaml`. Първото изграждане на образа може да изисква допълнително време и интернет достъп. Запази съществуващите Docker volumes; не използвай `down -v`.

5. Когато Caddy отчете успешна ACME заявка, тествай сертификата **без `-k` и без старото `cloud-root.crt`**:

```bash
curl --resolve vega-cloud-myhome.duckdns.org:38443:127.0.0.1 \
  https://vega-cloud-myhome.duckdns.org:38443/health
```

Очаква се `{"ok":true,"service":"vega-cloud-v2"}`. После отвори същия URL от телефона през мобилни данни. Входът през браузъра трябва да е без предупреждения за сертификат. Ако заради NAT loopback връзката от домашната Wi-Fi мрежа не работи, тествай извън нея.

## Диагностика

- Проверявай `docker compose -f compose.duckdns.yaml logs --tail=100 caddy`.
- Увери се, че DuckDNS поддомейнът е създаден и токенът е валиден. DNS TXT промените понякога се разпространяват със забавяне.
- Провери изходящите DNS и HTTPS от контейнера.
- Не използвай `curl -k`, `tls internal` или деактивирана проверка на сертификата като производствено решение.

## Връщане на старата тестова конфигурация

1. В `.env` върни `VEGA_PUBLIC_URL=https://213.91.182.27:38443`.
2. Изпълни `docker compose up -d --force-recreate cloud caddy`.
3. Предишните данни и CA остават в същите Docker volumes; старият адрес се проверява с доверения `cloud-root.crt`.

## ESP8266

Експерименталният v6.6 конектор е бил настроен за IP адреса и локалния Caddy CA. **Той няма автоматично да премине на новия DuckDNS URL.** След успешен тест на HTTPS сайта трябва отделно да актуализираме облачния URL и публичната CA проверка във фърмуера, преди да свързваме нагревател към отдалечено управление.
