#pragma once
#include <Arduino.h>
// Mobile setup HTML served from Flash in two chunks around the existing CSRF token.
static const char SETUP_WIZARD_TOP[] PROGMEM = R"SETUP(<!doctype html><html lang="bg"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#10233b"><title>Настройка на отоплението</title>
<link rel="apple-touch-icon" sizes="180x180" href="/apple-touch-icon.png">
<link rel="icon" href="/favicon.ico"><link rel="manifest" href="/manifest.webmanifest">
<style>*{box-sizing:border-box}body{margin:0;background:#0c1625;color:#eff7ff;font:16px -apple-system,BlinkMacSystemFont,system-ui,sans-serif}
main{max-width:540px;margin:auto;padding:24px 16px 60px}h1{font-size:27px;line-height:1.2;margin:10px 0 12px}h2{font-size:20px;margin:0 0 12px}
.help{color:#afc4dc;line-height:1.55;font-size:14px}.card{background:#1a2c42;border:1px solid #304963;padding:20px 18px;border-radius:22px;margin:16px 0}
.steps{display:flex;gap:5px;font-size:12px;color:#a4bdd8}.steps span{flex:1;padding:8px 4px;border-radius:8px;text-align:center;background:#20354e}.steps span.on{background:#1b68c5;color:white}
label{display:block;margin:12px 0 6px}input,select{width:100%;font:inherit;padding:13px;background:#0e1d30;color:white;border:1px solid #61778e;border-radius:12px}
button,.button{width:100%;display:block;padding:15px 14px;border-radius:14px;border:none;background:#216fd4;color:white;font:600 17px system-ui;text-align:center;text-decoration:none;cursor:pointer;margin:12px 0}
button:disabled{opacity:.6}.secondary{background:#344b67}.status{background:#0e2338;border-radius:12px;padding:14px;white-space:pre-line;line-height:1.5}
a{color:#9acbff;overflow-wrap:anywhere}.url{display:block;background:#0d233b;border-radius:12px;padding:12px;margin:8px 0;font-weight:700;font-size:19px;text-align:center}
.hidden{display:none}.warn{color:#f6c08d}.success{color:#81e8b4}.tags{display:flex;gap:8px;flex-wrap:wrap}.tag{background:#29415d;border-radius:9px;padding:7px;font-size:13px}
ol{padding-left:24px;line-height:1.65}nav a{font-size:14px}</style></head><body><main>
<nav><a href="/">Термостат</a> · <a href="/settings">Настройки</a></nav>
<h1>Настройка на отоплението</h1>
<p class="help">Помощник за първоначално свързване. Отоплението се изключва по време на настройката.</p>
<div class="steps"><span class="on">1 · AP</span><span class="on">2 · Wi-Fi</span><span>3 · Готово</span></div>
<section class="card"><h2>1. Свържи се с устройството</h2>
<div class="tags"><span class="tag" id="apName">Heater-xxxxxx</span><span class="tag">Парола: 12345678</span></div>
<p class="help">При първо стартиране избери мрежата Heater-xxxxxx и отвори <b>http://192.168.4.1</b> в Safari или Chrome. При вече настроено устройство можеш да използваш домашния IP адрес.</p></section>
<section class="card"><h2>2. Домашна Wi-Fi мрежа</h2>
<p class="help">ESP8266 работи само с 2,4 GHz Wi-Fi. Избери намерена мрежа или въведи името ръчно.</p>
<button type="button" class="secondary" id="scanBtn">Потърси мрежи</button><p id="scanStatus" class="help"></p>
<label for="nets">Намерени мрежи</label><select id="nets"><option value="">— въведи ръчно —</option></select>
<form id="connectForm"><label for="ssid">Име на мрежата (SSID)</label>
<input name="ssid" id="ssid" required maxlength="32" autocomplete="off" placeholder="Домашна Wi-Fi мрежа">
<label for="password">Парола за домашния Wi-Fi</label>
<input name="password" id="password" type="password" autocomplete="new-password" placeholder="Парола (ако има)">
<label style="display:flex;align-items:flex-start;gap:10px"><input id="freeze" type="checkbox" checked style="width:auto;margin-top:5px"><span>Запази получения DHCP адрес като статичен</span></label>
<p class="help warn">Риск: рутерът може по-късно да даде същия адрес и на друго устройство. Препоръчваме DHCP резервация в рутера. При проблем можеш да се върнеш към DHCP.</p>
<button type="submit" id="connectBtn">Свържи и продължи</button></form>
<div class="status" id="connectionStatus" aria-live="polite">Готов за настройка.</div></section>
<section id="ready" class="card hidden"><h2 class="success">3. Свързването е готово!</h2>
<p>Адрес на термостата в домашната мрежа:</p><a id="homeLink" class="url" href="#">—</a>
<button type="button" class="secondary" id="copyBtn">Копирай адреса</button>
<p id="staticInfo" class="help"></p>
<p class="help"><b>Важно:</b> ако телефонът още е в Heater-xxxxxx, първо го прехвърли на домашния Wi-Fi. След това отвори линка. AP ще се изключи след около 3 минути.</p>
<p class="help">При достъпен адрес автоматично ще опитаме да отворим термостата; ако не стане, използвай линка.</p>
<button type="button" class="secondary" id="dhcpBtn">Върни на DHCP</button></section>
<section class="card"><h2>Облачен достъп (по избор)</h2>
<p class="help">След като телефонът се върне към домашния Wi-Fi, отвори менюто <b>Облак</b> в термостата и активирай връзката. Добавянето в профила и разрешението за локация стават през защитения облачен сайт.</p>
<a class="button secondary" href="/cloud">Настройки на облака</a></section>
<section class="card"><h2>4. Икона на началния екран</h2>
<p class="help">Първо отвори домашния адрес от телефона, свързан към същия рутер.</p>
<h3>iPhone (Safari)</h3><ol><li>Отвори адреса в Safari.</li><li>Докосни <b>Споделяне</b> (стрелка от квадрат).</li><li>Избери <b>Добави към Начален екран</b>.</li><li>Въведи <b>Отопление</b> и натисни <b>Добави</b>.</li></ol>
<h3>Android (Chrome)</h3><ol><li>Отвори адреса в Chrome.</li><li>Докосни менюто <b>⋮</b>.</li><li>Избери <b>Инсталиране и създаване на пряк път</b> / <b>Добави към началния екран</b>.</li><li>Потвърди с <b>Добави</b>.</li></ol>
<p class="help">При локален HTTP адрес Chrome може да предложи пряк път вместо инсталация като приложение. И двата варианта отварят термостата.</p></section>
<script>const CSRF=')SETUP";

static const char SETUP_WIZARD_END[] PROGMEM = R"SETUP(';
const $=id=>document.getElementById(id);let lastUrl='',redirectTimer=0;
function mark(t){$('connectionStatus').textContent=t}
function validIP(s){return /^\d{1,3}(\.\d{1,3}){3}$/.test(s)}
async function updateStatus(){try{
 const r=await fetch('/setup/state',{cache:'no-store'});if(!r.ok)throw Error('status');
 const s=await r.json();$('apName').textContent=s.ap||'Heater-xxxxxx';
 if(s.error)mark('Проверка: '+s.error);
 else if(s.connecting)mark('Свързване с '+(s.ssid||'домашния Wi-Fi')+'… Изчакай до 30 секунди.');
 else if(s.connected)mark('Свързано с '+(s.ssid||'домашния Wi-Fi')+' · '+s.ip);
 if(s.success && validIP(s.ip)){
   const url='http://'+s.ip+'/';lastUrl=url;$('ready').classList.remove('hidden');
   $('homeLink').href=url;$('homeLink').textContent=url;
   $('staticInfo').textContent='IP: '+s.ip+' · маска: '+s.subnet+' · шлюз: '+s.gateway+' · DNS: '+s.dns+' · '+(s.static_ip?'Статичен адрес':'DHCP');
   if(!redirectTimer && location.hostname!==s.ip)redirectTimer=setInterval(async()=>{
     // The AP normally cannot route to the new STA IP. Probe after phone switches Wi-Fi.
     try{const ctl=new AbortController();const timeout=setTimeout(()=>ctl.abort(),1200);
       await fetch(url+'health',{mode:'no-cors',cache:'no-store',signal:ctl.signal});clearTimeout(timeout);location.assign(url);
     }catch(_e){}
   },5000);
 }
}catch(e){if(!$('ready').classList.contains('hidden'))mark('Смени телефона на домашния Wi-Fi, после отвори показания адрес.');}}
$('scanBtn').addEventListener('click',async()=>{
 $('scanBtn').disabled=true;$('scanStatus').textContent='Търсене на 2,4 GHz мрежи…';
 try{for(let i=0;i<14;i++){
   const r=await fetch('/setup/networks',{cache:'no-store'}),d=await r.json();
   if(!d.scanning){const sel=$('nets');sel.replaceChildren();sel.add(new Option('— въведи ръчно —',''));
      (d.networks||[]).forEach(n=>sel.add(new Option(n.ssid+' ('+n.rssi+' dBm)',n.ssid)));
      $('scanStatus').textContent=(d.networks||[]).length+' мрежи';return;}
   await new Promise(resolve=>setTimeout(resolve,950));
 }$('scanStatus').textContent='Търсенето отне дълго. Въведи SSID ръчно.';
 }catch(e){$('scanStatus').textContent='Търсенето не успя. Въведи SSID ръчно.';}
 finally{$('scanBtn').disabled=false;}
});
$('nets').addEventListener('change',()=>{if($('nets').value)$('ssid').value=$('nets').value;});
$('connectForm').addEventListener('submit',async e=>{
 e.preventDefault();$('connectBtn').disabled=true;mark('Записвам мрежата…');
 try{const data=new URLSearchParams({csrf:CSRF,ssid:$('ssid').value,password:$('password').value,freeze:$('freeze').checked?'1':'0'});
 const r=await fetch('/setup/connect',{method:'POST',body:data,cache:'no-store'});
 if(!r.ok)throw Error('HTTP '+r.status);mark('Свързване… След получаване на DHCP адрес ще се покаже линкът.');
 }catch(err){mark('Грешка при настройване: '+err.message);}finally{$('connectBtn').disabled=false;}
});
$('copyBtn').addEventListener('click',async()=>{try{await navigator.clipboard.writeText(lastUrl);$('copyBtn').textContent='Копирано';}catch(e){window.prompt('Копирай адреса:',lastUrl);}});
$('dhcpBtn').addEventListener('click',async()=>{if(!confirm('Да се премахне статичният адрес?'))return;
 try{const r=await fetch('/setup/dhcp',{method:'POST',body:new URLSearchParams({csrf:CSRF}),cache:'no-store'});
 if(!r.ok)throw Error('HTTP '+r.status);mark('Връщане към DHCP, адресът може да се промени.');}
 catch(e){mark('Връзката прекъсна. Ако е необходимо, използвай резервния AP.');}});
updateStatus();setInterval(updateStatus,2500);
</script></main></body></html>)SETUP";
