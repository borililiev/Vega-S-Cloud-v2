'use strict';
let csrf='',role='',username='',allDevices=[],firmwareList=[],usersList=[],selectedId='',period='24h';
let targetDraft=null,targetTimer=null,chartLast=0,chartBusy=false,currentTab='home';
const $=id=>document.getElementById(id);
const el=(tag,text,cls)=>{const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n};
const notice=s=>{$('notice').textContent=s};
const num=(n,d=1)=>Number.isFinite(Number(n))?Number(n).toFixed(d):'—';
const money=n=>n===null||n===undefined?'—':num(n,2);
async function api(path,method='GET',body){
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
function setTab(tab){
 if(tab==='admin'&&role!=='admin')tab='home';
 currentTab=tab;
 $('homeSection').hidden=tab!=='home';$('adminSection').hidden=tab!=='admin';$('profileSection').hidden=tab!=='profile';
 for(const [t,id] of [['home','homeTab'],['admin','adminTab'],['profile','profileTab']]){
  const button=$(id);button.classList.toggle('active',t===tab);button.setAttribute('aria-current',t===tab?'page':'false');
 }
 if(tab==='admin')refreshAdmin();
 if(tab==='home'){renderSelected();loadChart(true)}
}
$('homeTab').onclick=()=>setTab('home');$('adminTab').onclick=()=>setTab('admin');$('profileTab').onclick=()=>setTab('profile');
async function restore(){
 try{const s=await api('/api/session');csrf=s.csrf;role=s.role;username=s.username;$('username').textContent=username;showLogin(false);$('adminTab').hidden=role!=='admin';await refresh();setTab('home')}
 catch{csrf='';showLogin(true)}
}
$('loginForm').onsubmit=async e=>{e.preventDefault();try{
 const r=await api('/api/login','POST',{username:$('loginName').value,password:$('loginPass').value,remember:$('remember').checked});
 csrf=r.csrf;role=r.role;username=r.username;$('username').textContent=username;$('loginPass').value='';$('adminTab').hidden=role!=='admin';showLogin(false);
 await refresh();setTab('home');notice('Успешен вход');
 }catch(err){notice(err.message)}};
$('logout').onclick=async()=>{try{await api('/api/logout','POST')}catch{}csrf='';role='';allDevices=[];showLogin(true);notice('Излязохте')};
$('refresh').onclick=async()=>{await refresh();await loadChart(true)};
$('devicePicker').onchange=()=>{selectedId=$('devicePicker').value;targetDraft=null;chartLast=0;renderSelected();loadChart(true)};
for(const button of document.querySelectorAll('[data-period]'))button.onclick=()=>{
 period=button.dataset.period;for(const b of document.querySelectorAll('[data-period]'))b.classList.toggle('active',b===button);
 loadChart(true);
};
function device(){return allDevices.find(d=>d.id===selectedId)}
async function refresh(){
 try{const result=await api('/api/devices');allDevices=result.devices||[];
  if(!allDevices.find(d=>d.id===selectedId))selectedId=allDevices.length?allDevices[0].id:'';
  const sel=$('devicePicker'),old=sel.value;
  sel.replaceChildren(...allDevices.map(d=>{const o=el('option',d.name+(d.online?'':' (офлайн)'));o.value=d.id;return o}));
  sel.value=selectedId;sel.disabled=!allDevices.length;
  $('emptyState').hidden=!!allDevices.length;$('thermoView').hidden=!allDevices.length;
  renderSelected();if(currentTab==='home'&&Date.now()-chartLast>60000)loadChart();
 }catch(err){notice(err.message)}
}
function renderSelected(){
 const d=device();if(!d)return;
 const s=d.state||{},en=d.energy||{},today=en.today||{},month=en.month||{};
 $('deviceTitle').textContent=d.name;
 $('currentValue').textContent=s.sensor_ok&&s.temperature!==null&&s.temperature!==undefined?num(s.temperature,1)+' °C':'— °C';
 if(targetDraft&&targetDraft.id===d.id&&Number(s.target)===targetDraft.value){targetDraft=null}
 if(targetDraft&&targetDraft.id===d.id&&Date.now()-targetDraft.at>90000){targetDraft=null}
 const target=targetDraft&&targetDraft.id===d.id?targetDraft.value:s.target;
 $('targetValue').textContent=target===null||target===undefined?'—':num(target,1)+' °C';
 $('targetMinus').disabled=$('targetPlus').disabled=!d.online||target===null||target===undefined;
 $('toggleButton').disabled=!d.online;
 $('toggleButton').textContent=s.enabled?'Изключи отоплението':'Включи отоплението';
 $('toggleButton').classList.toggle('on',!!s.enabled);
 $('relayLed').classList.toggle('active',!!s.relay&&d.online);
 $('relayText').textContent='Реле: '+(d.online?(s.relay?'ВКЛЮЧЕНО':'ИЗКЛЮЧЕНО'):'НЯМА ВРЪЗКА');
 $('onlineBadge').textContent=d.online?'● Онлайн':'● Офлайн';$('onlineBadge').className='status-pill '+(d.online?'online':'offline');
 $('energyToday').textContent=num(today.kwh,3);$('energyMonth').textContent=num(month.kwh,3);
 $('runtimeToday').textContent=num(today.runtime_h,2);$('costToday').textContent=money(today.cost_eur);
 $('costMonth').textContent=money(month.cost_eur);$('energyDayPart').textContent=num(today.day_kwh,3);
 $('energyNightPart').textContent=num(today.night_kwh,3);$('powerValue').textContent=s.power_w||'—';
 $('lastSeen').textContent=d.last_seen?new Date(d.last_seen*1000).toLocaleString('bg-BG'):'—';
 $('firmwareVersion').textContent=s.version||'—';
}
async function sendCommand(id,kind,value){
 try{await api('/api/devices/'+encodeURIComponent(id)+'/commands','POST',{kind,value:String(value)});
  $('thermoFeedback').textContent='Командата е изпратена. Изчаква се потвърждение от термостата.';
  notice('Командата е изпратена');return true;
 }catch(err){notice(err.message);$('thermoFeedback').textContent=err.message;return false}
}
function adjustTarget(delta){
 const d=device();if(!d||!d.online)return;
 const base=targetDraft&&targetDraft.id===d.id?targetDraft.value:Number(d.state.target);
 if(!Number.isFinite(base))return;
 const value=Math.max(12,Math.min(28,Math.round((base+delta)*2)/2));
 targetDraft={id:d.id,value,at:Date.now()};renderSelected();
 clearTimeout(targetTimer);targetTimer=setTimeout(async()=>{const request=targetDraft;if(!request)return;
  const ok=await sendCommand(request.id,'set_target',num(request.value,1));
  if(!ok){targetDraft=null;renderSelected()}
 },650);
}
$('targetMinus').onclick=()=>adjustTarget(-.5);$('targetPlus').onclick=()=>adjustTarget(.5);
$('toggleButton').onclick=async()=>{const d=device();if(!d||!d.online)return;
 const desired=d.state.enabled?'off':'heat';
 $('toggleButton').disabled=true;
 await sendCommand(d.id,'set_mode',desired);
 await refresh();
};
async function loadChart(force=false){
 const d=device();if(!d||chartBusy||currentTab!=='home')return;
 if(!force&&Date.now()-chartLast<60000)return;
 chartBusy=true;
 const deviceId=d.id,requested=period;
 try{const res=await api('/api/devices/'+encodeURIComponent(deviceId)+'/energy?period='+encodeURIComponent(requested));
  if(selectedId!==deviceId||period!==requested)return;
  const points=res.points||[],area=$('energyChart');area.replaceChildren();
  const peak=Math.max(...points.map(x=>Number(x.kwh)||0),0);
  if(peak<.0005){area.append(el('p','Няма отчетено потребление за този период','chart-empty'))}
  else points.forEach((p,i)=>{
   const column=el('div',undefined,'chart-column');const bar=el('div',undefined,'chart-bar');
   bar.style.height=Math.max(1,(Number(p.kwh)||0)/peak*100)+'%';bar.title=p.label+' · '+num(p.kwh,3)+' kWh';
   column.append(bar);
   const step=requested==='24h'?4:requested==='month'?Math.max(1,Math.ceil(points.length/7)):1;
   if(i%step===0||i===points.length-1)column.append(el('span',p.label,'chart-label'));
   area.append(column);
  });
  chartLast=Date.now();
 }catch(e){$('energyChart').replaceChildren(el('span','Графиката е временно недостъпна','chart-empty'));notice(e.message)}
 finally{chartBusy=false}
}
function setChildren(id,items){$(id).replaceChildren(...items)}
async function useLocation(id){if(!navigator.geolocation){notice('Браузърът не поддържа локация');return}
 if(!confirm('Да се изпрати приблизителната локация на устройството към сървъра?'))return;
 navigator.geolocation.getCurrentPosition(async pos=>{try{
  await api('/api/devices/'+encodeURIComponent(id)+'/location','PUT',{
   lat:pos.coords.latitude,lon:pos.coords.longitude,accuracy:pos.coords.accuracy,consent:true});
  notice('Локацията е запазена приблизително');await refresh();await refreshAdmin();
 }catch(e){notice(e.message)}},()=>notice('Локацията не е предоставена'),{enableHighAccuracy:false,timeout:12000,maximumAge:300000});
}
async function removeLocation(id){if(!confirm('Да се изтрие локацията?'))return;
 try{await api('/api/devices/'+encodeURIComponent(id)+'/location','DELETE');await refresh();await refreshAdmin();notice('Локацията е изтрита')}catch(e){notice(e.message)}
}
function adminDeviceRow(d){
 const box=el('section',undefined,'device-admin');const head=el('div',undefined,'header-row');
 head.append(el('h4',d.name),el('span',d.online?'● Онлайн':'● Офлайн','status-pill '+(d.online?'online':'offline')));box.append(head);
 box.append(el('p',`ID: ${d.id} · Собственик: ${d.owner||'Няма'} · ${d.state?.version||''}`,'muted'));
 const r=d.energy?.rates||{},form=el('form');
 const fields=[['Дневна цена €/kWh','day_rate',r.day_rate??0,'0.001'],['Нощна цена €/kWh','night_rate',r.night_rate??0,'0.001'],
  ['Дневна тарифа от','day_start',r.day_start??7,'1'],['Нощна тарифа от','night_start',r.night_start??23,'1']];
 for(const [title,name,val,step] of fields){const label=el('label',title),input=el('input');input.type='number';input.step=step;input.min='0';input.max=name.endsWith('rate')?'5':'23';input.value=String(val);input.name=name;label.append(input);form.append(label)}
 const save=el('button','Запази тарифи','minor');save.type='submit';form.append(save);
 form.onsubmit=async e=>{e.preventDefault();try{
  const data=Object.fromEntries(new FormData(form));await api('/api/admin/devices/'+encodeURIComponent(d.id)+'/rates','PUT',{
   day_rate:Number(data.day_rate),night_rate:Number(data.night_rate),day_start:Number(data.day_start),night_start:Number(data.night_start)});
  notice('Тарифите са запазени (EUR/kWh)');await refresh();
 }catch(e){notice(e.message)}};
 box.append(form);
 const actions=el('div',undefined,'admin-actions');
 const loc=el('button',d.location?'Изтрий локация':'📍 Запази локация','minor');loc.onclick=()=>d.location?removeLocation(d.id):useLocation(d.id);actions.append(loc);
 const release=el('button','Освободи от профила','minor danger');release.onclick=async()=>{
  if(!confirm('Освободи '+d.name+'? За ново сдвояване смени кода на локалната страница „Облак“.'))return;
  try{await api('/api/admin/devices/'+encodeURIComponent(d.id)+'/release','POST',{});notice('Устройството е освободено');await refresh();await refreshAdmin()}catch(e){notice(e.message)}
 };actions.append(release);
 const selector=el('select');selector.append(el('option','Избери .bin версия'));
 for(const f of firmwareList){const opt=el('option',f.version+' · '+Math.round(f.bytes/1024)+' KB');opt.value=f.id;selector.append(opt)}
 const ota=el('button','OTA обновяване','minor');ota.disabled=!d.online||!firmwareList.length||String(d.state?.version||'').toLowerCase().includes('sonoff');
 ota.onclick=async()=>{if(!firmwareList.some(f=>f.id===selector.value)){notice('Избери фърмуер');return}
  if(!confirm('Потвърди OTA за '+d.name+'. Устройството ще прекъсне отоплението и ще се рестартира.'))return;
  try{await api('/api/admin/devices/'+encodeURIComponent(d.id)+'/ota','POST',{firmware_id:selector.value,confirm:'ОБНОВИ'});notice('OTA задачата е създадена');await refreshAdmin()}catch(e){notice(e.message)}
 };actions.append(selector,ota);box.append(actions);return box;
}
async function refreshAdmin(){
 if(role!=='admin'||currentTab!=='admin')return;
 try{const [u,f,o,a]=await Promise.all([api('/api/admin/users'),api('/api/admin/firmware'),api('/api/admin/ota'),api('/api/admin/audit')]);
  usersList=u.users||[];firmwareList=f.firmware||[];
  const owners=$('claimOwner');owners.replaceChildren();
  for(const user of usersList.filter(x=>x.active)){const opt=el('option',user.username+(user.role==='admin'?' (администратор)':''));opt.value=String(user.id);if(user.username===username)opt.selected=true;owners.append(opt)}
  setChildren('users',usersList.map(user=>{
   const row=el('div',undefined,'listrow');row.append(el('span',user.username+' · '+user.role+' · '+(user.active?'Активен':'Спрян')));
   if(user.role!=='admin'){
    const b=el('button',user.active?'Деактивирай':'Активирай','minor');b.onclick=async()=>{try{await api('/api/admin/users/'+user.id,'PATCH',{active:!user.active});await refreshAdmin()}catch(e){notice(e.message)}};
    const reset=el('button','Нова парола','minor');reset.onclick=async()=>{const fresh=prompt('Нова парола за '+user.username+' (поне 12 символа):');if(fresh===null)return;if(fresh.length<12){notice('Поне 12 символа');return}
      try{await api('/api/admin/users/'+user.id,'PATCH',{new_password:fresh});notice('Паролата е сменена')}catch(e){notice(e.message)}};
    row.append(b,reset)
   }return row;
  }));
  setChildren('devicesAdmin',allDevices.map(adminDeviceRow));
  setChildren('firmware',firmwareList.map(f=>el('div',f.version+' · '+Math.round(f.bytes/1024)+' KB · SHA256 '+f.sha256.slice(0,12),'listrow')));
  setChildren('otaJobs',(o.jobs||[]).slice(0,15).map(j=>el('div',j.device_id+' · '+j.version+' · '+j.status,'listrow')));
  setChildren('audit',(a.events||[]).slice(0,20).map(j=>el('div',new Date(j.ts*1000).toLocaleString('bg-BG')+' · '+j.action+' · '+(j.detail||''),'listrow')));
 }catch(e){notice(e.message)}
}
$('claimForm').onsubmit=async e=>{e.preventDefault();try{
 await api('/api/admin/devices/claim','POST',{id:$('claimId').value.trim(),pairing_code:$('claimCode').value.trim(),owner_id:Number($('claimOwner').value)});
 $('claimCode').value='';$('claimId').value='';notice('Термостатът е добавен към избрания профил');await refresh();await refreshAdmin();
 }catch(err){notice(err.message)}};
$('newUserForm').onsubmit=async e=>{e.preventDefault();try{
 await api('/api/admin/users','POST',{username:$('newUsername').value,password:$('newUserPassword').value});
 $('newUsername').value='';$('newUserPassword').value='';notice('Потребителят е създаден');await refreshAdmin();
 }catch(e){notice(e.message)}};
$('myPasswordForm').onsubmit=async e=>{e.preventDefault();try{
 await api('/api/me/password','POST',{old_password:$('currentPw').value,new_password:$('newPw').value});
 csrf='';role='';showLogin(true);notice('Паролата е сменена. Влез отново.');
 }catch(e){notice(e.message)}};
$('firmwareForm').onsubmit=async e=>{e.preventDefault();try{
 const form=new FormData();form.append('version',$('fwVersion').value);form.append('file',$('fwFile').files[0]);
 await api('/api/admin/firmware','POST',form);$('fwFile').value='';notice('Фърмуерът е качен');await refreshAdmin();
 }catch(e){notice(e.message)}};
restore();setInterval(()=>{if(csrf)refresh()},10000);
