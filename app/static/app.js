let csrf='',role='',allDevices=[],firmwareList=[];
const $=id=>document.getElementById(id);
const el=(tag,text,cls)=>{const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n};
const notice=s=>{$('notice').textContent=s};
async function api(path, method='GET', body){
 const headers={'X-Requested-With':'VegaCloud'};
 if(method!=='GET' && csrf)headers['X-CSRF-Token']=csrf;
 let payload=body;
 if(body!==undefined && !(body instanceof FormData)){headers['Content-Type']='application/json';payload=JSON.stringify(body)}
 const r=await fetch(path,{method,credentials:'same-origin',headers,body:payload,cache:'no-store'});
 let data;try{data=await r.json()}catch{data={detail:`HTTP ${r.status}`}}
 if(!r.ok)throw Error(typeof data.detail==='string'?data.detail:`HTTP ${r.status}`);
 return data;
}
function showLogin(show){$('login').hidden=!show;$('dashboard').hidden=show;$('userbar').hidden=show}
async function restore(){try{const s=await api('/api/session');csrf=s.csrf;role=s.role;$('username').textContent=s.username;showLogin(false);$('adminPanel').hidden=role!=='admin';await refresh()}catch{csrf='';showLogin(true)}}
$('loginForm').onsubmit=async e=>{e.preventDefault();try{
 const r=await api('/api/login','POST',{username:$('loginName').value,password:$('loginPass').value,remember:$('remember').checked});
 csrf=r.csrf;role=r.role;$('username').textContent=r.username;$('loginPass').value='';$('adminPanel').hidden=role!=='admin';showLogin(false);notice('Успешен вход');await refresh();
 }catch(err){notice(err.message)}};
$('logout').onclick=async()=>{try{await api('/api/logout','POST')}catch{}csrf='';role='';showLogin(true);notice('Излязохте')};
$('refresh').onclick=()=>refresh();
$('claimForm').onsubmit=async e=>{e.preventDefault();try{await api('/api/devices/claim','POST',{id:$('claimId').value.trim(),pairing_code:$('claimCode').value.trim()});$('claimCode').value='';notice('Термостатът е добавен към профила');await refresh()}catch(err){notice(err.message)}};
async function cmd(id,kind,value){try{await api('/api/devices/'+encodeURIComponent(id)+'/commands','POST',{kind,value:String(value)});notice('Командата е изпратена към сървъра');}catch(err){notice(err.message)}}
async function useLocation(id){if(!navigator.geolocation){notice('Браузърът не поддържа определяне на местоположение');return}
 if(!confirm('Желаеш ли да изпратиш приблизителната локация на ТОВА устройство към сървъра? Можеш да я изтриеш по-късно.'))return;
 navigator.geolocation.getCurrentPosition(async pos=>{try{
  await api('/api/devices/'+encodeURIComponent(id)+'/location','PUT',{
   lat:pos.coords.latitude,lon:pos.coords.longitude,accuracy:pos.coords.accuracy,consent:true});
  notice('Запазена е приблизителна локация (закръглена за поверителност)');await refresh();
 }catch(err){notice(err.message)}},()=>notice('Локацията не е предоставена. Можеш да продължиш без нея.'),{enableHighAccuracy:false,timeout:12000,maximumAge:300000});
}
async function removeLocation(id){if(!confirm('Да се изтрие местоположението на устройството?'))return;try{await api('/api/devices/'+encodeURIComponent(id)+'/location','DELETE');await refresh();notice('Локацията е изтрита')}catch(e){notice(e.message)}}
function deviceCard(d){const card=el('article');const heading=el('div',undefined,'row');heading.append(el('h3',d.name),el('span',d.online?'● Онлайн':'● Офлайн','badge'+(d.online?'':' off')));card.append(heading);
 const s=d.state||{};card.append(el('div',s.temperature===null||s.temperature===undefined?'—':Number(s.temperature).toFixed(1)+' °C','temp'));
 card.append(el('div',`Реле: ${s.relay?'ВКЛ.':'ИЗКЛ.'} · ${s.power_w||'—'} W · Версия: ${s.version||'—'}`,'muted small'));
 const label=el('label','Зададена температура');card.append(label);const row=el('div',undefined,'target'),minus=el('button','−'),input=el('input'),plus=el('button','+');
 input.type='number';input.min='12';input.max='28';input.step='0.5';input.value=s.target??22;
 minus.onclick=()=>input.value=String(Math.max(12,Number(input.value)-.5));plus.onclick=()=>input.value=String(Math.min(28,Number(input.value)+.5));
 row.append(minus,input,plus);card.append(row);const save=el('button','Запази температура');save.disabled=!d.online;save.onclick=()=>cmd(d.id,'set_target',input.value);card.append(save);
 const acts=el('div',undefined,'actions'),on=el('button','Включи'),off=el('button','Изключи','danger');on.disabled=off.disabled=!d.online;on.onclick=()=>cmd(d.id,'set_mode','heat');off.onclick=()=>cmd(d.id,'set_mode','off');acts.append(on,off);card.append(acts);
 card.append(el('p',`ID: ${d.id} · Собственик: ${d.owner||'Не е добавено'} · Последно: ${d.last_seen?new Date(d.last_seen*1000).toLocaleString('bg-BG'):'—'}`,'muted small'));
 const loc=el('div',undefined,'location');const btn=el('button','📍 Запази локация','minor');btn.onclick=()=>useLocation(d.id);loc.append(btn);
 if(d.location){loc.append(el('span',` ${d.location.lat}, ${d.location.lon} `));const rem=el('button','Изтрий','minor');rem.onclick=()=>removeLocation(d.id);loc.append(rem)}card.append(loc);
 if(role==='admin'){
   const release=el('button','Освободи от профила','minor');release.onclick=async()=>{
     if(!confirm('Освободи '+d.name+'? Преди ново сдвояване смени кода от локалното меню „Облак“.'))return;
     try{await api('/api/admin/devices/'+encodeURIComponent(d.id)+'/release','POST',{});await refresh();notice('Устройството е освободено')}catch(e){notice(e.message)}
   };card.append(release);
   const sel=el('select');sel.append(el('option','Избери версия за OTA'));for(const f of firmwareList){const opt=el('option',f.version+' · '+Math.round(f.bytes/1024)+' KB');opt.value=f.id;sel.append(opt)}
   const ota=el('button','Обнови през облака','minor');ota.disabled=!d.online;ota.onclick=async()=>{
     const f=sel.value;if(!firmwareList.some(x=>x.id===f)){notice('Избери версия');return}
     if(!confirm('Изпращане на OTA към '+d.name+'? Отоплението ще се изключи и устройството ще се рестартира.'))return;
     try{await api('/api/admin/devices/'+encodeURIComponent(d.id)+'/ota','POST',{firmware_id:f,confirm:'ОБНОВИ'});notice('OTA задачата е създадена');await refresh()}catch(e){notice(e.message)}
   };card.append(sel,ota)
 }
 return card;
}
async function refresh(){try{
 if(role==='admin'){const fr=await api('/api/admin/firmware');firmwareList=fr.firmware||[]}
 const d=await api('/api/devices');allDevices=d.devices||[];$('devices').replaceChildren(...allDevices.map(deviceCard));
 if(role==='admin')await refreshAdmin();
 }catch(e){notice(e.message)}}
$('newUserForm').onsubmit=async e=>{e.preventDefault();try{await api('/api/admin/users','POST',{username:$('newUsername').value,password:$('newUserPassword').value});$('newUsername').value='';$('newUserPassword').value='';notice('Потребителят е създаден');await refreshAdmin()}catch(e){notice(e.message)}};
$('myPasswordForm').onsubmit=async e=>{e.preventDefault();try{await api('/api/me/password','POST',{old_password:$('currentPw').value,new_password:$('newPw').value});csrf='';showLogin(true);notice('Паролата е сменена. Влез отново.')}catch(e){notice(e.message)}};
$('firmwareForm').onsubmit=async e=>{e.preventDefault();try{const form=new FormData();form.append('version',$('fwVersion').value);form.append('file',$('fwFile').files[0]);await api('/api/admin/firmware','POST',form);$('fwFile').value='';notice('Фърмуерът е качен');await refresh()}catch(e){notice(e.message)}};
async function refreshAdmin(){
 const [us,ots,audit]=await Promise.all([api('/api/admin/users'),api('/api/admin/ota'),api('/api/admin/audit')]);
 $('users').replaceChildren(...(us.users||[]).map(u=>{const box=el('div',undefined,'listrow');box.append(el('span',u.username+' · '+u.role+' · '+(u.active?'Активен':'Спрян')));
 if(u.role!=='admin'){
   const b=el('button',u.active?'Деактивирай':'Активирай','minor');
   b.onclick=async()=>{try{await api('/api/admin/users/'+u.id,'PATCH',{active:!u.active});await refreshAdmin()}catch(e){notice(e.message)}};
   const reset=el('button','Нова парола','minor');
   reset.onclick=async()=>{const fresh=prompt('Нова парола за '+u.username+' (поне 12 символа):');
     if(fresh===null)return;
     if(fresh.length<12){notice('Паролата трябва да има поне 12 символа');return}
     try{await api('/api/admin/users/'+u.id,'PATCH',{new_password:fresh});notice('Паролата е сменена. Старите сесии са прекратени.')}catch(e){notice(e.message)}
   };
   box.append(b,reset)
 }return box}));
 $('firmware').replaceChildren(...firmwareList.map(f=>el('div',f.version+' · '+Math.round(f.bytes/1024)+' KB · SHA256 '+f.sha256.slice(0,12),'listrow small')));
 $('otaJobs').replaceChildren(...(ots.jobs||[]).slice(0,15).map(j=>el('div',j.device_id+' · '+j.version+' · '+j.status,'listrow small')));
 $('audit').replaceChildren(...(audit.events||[]).slice(0,20).map(j=>el('div',new Date(j.ts*1000).toLocaleString('bg-BG')+' · '+j.action+' · '+(j.detail||''),'listrow small')));
}
restore();setInterval(()=>{if(csrf)refresh()},10000);
