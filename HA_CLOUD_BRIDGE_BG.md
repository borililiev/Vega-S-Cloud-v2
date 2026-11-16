# Home Assistant през Vega-S Cloud

**Нова архитектура:** Home Assistant HTTPS → Vega-S Cloud → стандартна облачна синхронизация → ESP8266. Не е нужен MQTT, нито директен HTTP достъп до платката.

## Обновяване на Ubuntu сървъра

```bash
cd /opt/Vega-S-Cloud-v2
BACKUP="$HOME/vega-before-ha-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BACKUP"
cp -a app requirements.txt "$BACKUP/"
docker compose exec -T cloud python - <<'PY'
import sqlite3,os,datetime
src=os.getenv('VEGA_DB','/data/vega.sqlite3')
dst='/data/vega-before-ha-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S')+'.sqlite3'
with sqlite3.connect(src) as a,sqlite3.connect(dst) as b:a.backup(b)
print('Database backup:',dst)
PY
git status --short
git pull --ff-only
docker compose up -d --build --no-deps cloud
docker compose ps
```

Не използвай `docker compose down -v`. При конфликт с локално променени файлове не изпълнявай `git reset --hard`.

## Ключ за Home Assistant

Отвори **Профил → Home Assistant — през облака → Създай / смени HA ключ**. Ключът се вижда само веднъж и е отделен от паролата, сесиите и токените на ESP. Може да бъде отменен по всяко време. Home Assistant вижда само устройствата, разрешени за съответния потребител. Администраторски профил вижда всички.

## HTTPS

Изнеси **само публичния** Caddy CA сертификат от Ubuntu:
```bash
cd /opt/Vega-S-Cloud-v2
docker compose exec -T caddy cat /data/caddy/pki/authorities/local/root.crt > cloud-root.crt
```

Копирай го във файловата система на Home Assistant, например `/config/vega-cloud-root.crt`. В Home Assistant добави интеграцията **Vega-S Cloud**, въведи:
- URL: `https://213.91.182.27:38443`
- HA ключ: новият ключ от профила
- CA file: `/config/vega-cloud-root.crt`

**Не изключвай проверката на HTTPS сертификата.**

## Налични функции

Стандартен HA климатичен термостат с режими OFF/HEAT и зададена температура през 0,5 °C. Сензори за днешна/месечна консумация в kWh, време на работа и приблизителен разход в евро. Облачната телеметрия и командите използват същите сървърни правила за безопасност като уеб интерфейса.

## Ред на миграция

1. Обнови сървъра и провери `/health`.
2. Инсталирай отделния ZIP за Home Assistant в `/config/custom_components/vega_s_cloud`; рестартирай HA.
3. Генерирай ключ от профила и добави интеграцията през Settings → Devices & services.
4. Провери със **сегашния работещ Sonoff**, преди да сменяш който и да е фърмуер.
5. Едва след успешния тест качи v6.7.2 на първата ESP-12F платка при изключено захранване към товара.
6. Премахни старата интеграция `vega_s_local` след мигриране на автоматизациите към новите entities.

Това е тестова интеграция — изисква реален Home Assistant тест. Точното намаляване на BIN след премахване на трите LAN HA маршрута още трябва да се измери чрез Arduino Verify/Export.
