"""Vega-S Cloud v2: multi-user ownership, pairing, long sessions, consented location, OTA.
Trust model: behind properly trusted HTTPS only. No ESP local endpoint is exposed.
"""
from __future__ import annotations
import hashlib, hmac, json, os, re, secrets, sqlite3, time
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo
from pathlib import Path
from typing import Literal
from fastapi import Depends, FastAPI, File, Form, Header, HTTPException, Request, Response, UploadFile
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field, field_validator

ROOT = Path(__file__).resolve().parent
DB = os.getenv('VEGA_DB', '/data/vega.sqlite3')
FIRMWARE_DIR = Path(os.getenv('VEGA_FIRMWARE_DIR', '/data/firmware'))
ADMIN_PASSWORD = os.getenv('VEGA_ADMIN_PASSWORD', '')
SESSION_SECRET = os.getenv('VEGA_SESSION_SECRET','')
EXPECTED_ORIGIN = os.getenv('VEGA_PUBLIC_URL', 'https://213.91.182.27:38443').rstrip('/')
if len(ADMIN_PASSWORD) < 16 or len(SESSION_SECRET) < 32:
    raise RuntimeError('Strong VEGA_ADMIN_PASSWORD and VEGA_SESSION_SECRET are mandatory')
app = FastAPI(title='Vega-S Cloud v2', docs_url=None, redoc_url=None, openapi_url=None)
ENERGY_TZ = ZoneInfo('Europe/Sofia')  # Local calendar for daily / monthly estimates
ENERGY_MAX_GAP_S = 40  # Never extrapolate energy across missing network updates

def connect():
    p = Path(DB);p.parent.mkdir(parents=True, exist_ok=True)
    con=sqlite3.connect(DB,timeout=10)
    con.row_factory=sqlite3.Row
    con.execute('PRAGMA busy_timeout=10000')
    con.execute('PRAGMA foreign_keys=ON')
    con.execute('PRAGMA journal_mode=WAL')
    return con

def hash_password(password:str)->str:
    salt=secrets.token_bytes(16)
    dk=hashlib.scrypt(password.encode(),salt=salt,n=2**14,r=8,p=1,dklen=32)
    return salt.hex()+':'+dk.hex()

def verify_password(password:str,record:str)->bool:
    try:
        salt,expected=record.split(':')
        dk=hashlib.scrypt(password.encode(),salt=bytes.fromhex(salt),n=2**14,r=8,p=1,dklen=32)
        return hmac.compare_digest(dk,bytes.fromhex(expected))
    except (ValueError, TypeError):return False

def digest(v:str)->str:return hashlib.sha256(v.encode()).hexdigest()
def session_hash(v:str)->str:return hmac.new(SESSION_SECRET.encode(),v.encode(),hashlib.sha256).hexdigest()
def now()->int:return int(time.time())

def energy_rates(db, did):
    row = db.execute('SELECT day_rate,night_rate,day_start,night_start FROM device_rates WHERE device_id=?', (did,)).fetchone()
    return dict(row) if row else {'day_rate': 0.0, 'night_rate': 0.0, 'day_start': 7, 'night_start': 23}

def energy_integrate(db, did, old_seen, old_state, ts):
    # Accumulate estimated energy only across two recent actual syncs.

    # Real kWh metering is NOT present. Previous relay state * nominal W is used;
    # Stale gaps are discarded instead of inventing operation during downtime.
    gap = ts - old_seen
    if not old_seen or not (0 < gap <= ENERGY_MAX_GAP_S) or not old_state.get('relay'):
        return
    power = old_state.get('power_w')
    if not isinstance(power, int) or power not in (250, 400, 600, 750, 1250, 2000):
        return
    tariffs = energy_rates(db, did)
    begin = old_seen
    while begin < ts:
        hour = (begin // 3600) * 3600
        end = min(ts, hour + 3600)
        elapsed = end - begin
        local_hour = datetime.fromtimestamp(begin, ENERGY_TZ).hour
        a, b = tariffs['day_start'], tariffs['night_start']
        is_day = a <= local_hour < b if a < b else (local_hour >= a or local_hour < b)
        wh = elapsed * power / 3600.0
        day_wh, night_wh = (wh, 0.0) if is_day else (0.0, wh)
        db.execute('''INSERT INTO device_energy_hourly(device_id,hour_start,day_wh,night_wh,on_seconds)
          VALUES(?,?,?,?,?) ON CONFLICT(device_id,hour_start) DO UPDATE SET
          day_wh=day_wh+excluded.day_wh,night_wh=night_wh+excluded.night_wh,
          on_seconds=on_seconds+excluded.on_seconds''', (did,hour,day_wh,night_wh,float(elapsed)))
        begin = end

def local_period_start(ts, period):
    dt = datetime.fromtimestamp(ts, ENERGY_TZ)
    if period == 'today':
        return int(dt.replace(hour=0, minute=0, second=0, microsecond=0).timestamp())
    if period == 'month':
        return int(dt.replace(day=1,hour=0,minute=0,second=0,microsecond=0).timestamp())
    raise ValueError(period)

def energy_summary(db, did, ts):
    month_start = local_period_start(ts, 'month')
    today_start = local_period_start(ts, 'today')
    rates = energy_rates(db, did)
    totals = {'today': [0.0, 0.0, 0.0], 'month': [0.0, 0.0, 0.0]}
    for r in db.execute('SELECT hour_start,day_wh,night_wh,on_seconds FROM device_energy_hourly WHERE device_id=? AND hour_start>=? AND hour_start<=?',(did,month_start,ts)):
        for period in ('month','today'):
            if r['hour_start'] >= (today_start if period == 'today' else month_start):
                x=totals[period];x[0]+=r['day_wh'];x[1]+=r['night_wh'];x[2]+=r['on_seconds']
    price_ok = rates['day_rate'] > 0 and rates['night_rate'] > 0
    def fmt(vals):
        day,night,on=vals
        return {'kwh':round((day+night)/1000,3),'day_kwh':round(day/1000,3),
                'night_kwh':round(night/1000,3),'runtime_h':round(on/3600,2),
                'cost_eur':round((day*rates['day_rate']+night*rates['night_rate'])/1000,2) if price_ok else None}
    return {'today':fmt(totals['today']),'month':fmt(totals['month']),
            'rates':rates,'cost_configured':price_ok,'kind':'estimated',
            'note':'Оценка по номинална мощност и състоянието на релето; само за периоди с редовна облачна връзка.'}

def energy_series(db, did, period, ts):
    dt = datetime.fromtimestamp(ts, ENERGY_TZ)
    if period == '24h':
        start_hour = (ts // 3600 - 23) * 3600
        points = [{'label':datetime.fromtimestamp(start_hour + i*3600,ENERGY_TZ).strftime('%H:%M'),
                   'kwh':0.0,'key':start_hour + i*3600} for i in range(24)]
        by_key = {x['key']:x for x in points}
    elif period == 'month':
        points = [{'label':str(i),'kwh':0.0,'key':i} for i in range(1,dt.day+1)]
        by_key = {x['key']:x for x in points}
        start_hour = local_period_start(ts,'month')
    elif period == 'year':
        points = [{'label':str(i).zfill(2),'kwh':0.0,'key':i} for i in range(1,13)]
        by_key = {x['key']:x for x in points}
        start_hour = int(dt.replace(month=1,day=1,hour=0,minute=0,second=0,microsecond=0).timestamp())
    else:
        raise HTTPException(400,'Невалиден период')
    for r in db.execute('SELECT hour_start,day_wh,night_wh FROM device_energy_hourly WHERE device_id=? AND hour_start>=? AND hour_start<=?',(did,start_hour,ts)):
        key = r['hour_start'] if period=='24h' else (datetime.fromtimestamp(r['hour_start'],ENERGY_TZ).day if period=='month' else datetime.fromtimestamp(r['hour_start'],ENERGY_TZ).month)
        if key in by_key:by_key[key]['kwh']+=(r['day_wh']+r['night_wh'])/1000
    return [{'label':x['label'],'kwh':round(x['kwh'],3)} for x in points]

@app.on_event('startup')
def startup():
    FIRMWARE_DIR.mkdir(parents=True,exist_ok=True)
    with connect() as db:
        db.executescript('''
          CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT UNIQUE NOT NULL,
            password_hash TEXT NOT NULL, role TEXT NOT NULL CHECK(role IN('admin','user')),
            active INTEGER NOT NULL DEFAULT 1, created INTEGER NOT NULL);
          CREATE TABLE IF NOT EXISTS sessions(token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL,
            csrf TEXT NOT NULL, expires INTEGER NOT NULL, created INTEGER NOT NULL,
            remember INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY(user_id) REFERENCES users(id));
          CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY, name TEXT NOT NULL,
            token_hash TEXT NOT NULL, pairing_hash TEXT NOT NULL, owner_id INTEGER,
            seen INTEGER NOT NULL DEFAULT 0, state TEXT NOT NULL DEFAULT '{}', created INTEGER NOT NULL,
            lat REAL, lon REAL, loc_accuracy REAL, loc_at INTEGER,
            FOREIGN KEY(owner_id) REFERENCES users(id));
          CREATE TABLE IF NOT EXISTS commands(id TEXT PRIMARY KEY,device_id TEXT NOT NULL,
            kind TEXT NOT NULL,value TEXT NOT NULL,created INTEGER NOT NULL,expires INTEGER NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending', FOREIGN KEY(device_id) REFERENCES devices(id));
          CREATE TABLE IF NOT EXISTS firmware(id TEXT PRIMARY KEY, version TEXT NOT NULL,
            filename TEXT NOT NULL, sha256 TEXT NOT NULL, md5 TEXT NOT NULL, bytes INTEGER NOT NULL,
            created INTEGER NOT NULL, uploaded_by INTEGER NOT NULL);
          CREATE TABLE IF NOT EXISTS ota_jobs(id TEXT PRIMARY KEY, device_id TEXT NOT NULL,
            firmware_id TEXT NOT NULL, status TEXT NOT NULL,created INTEGER NOT NULL, expires INTEGER NOT NULL,
            reported TEXT, FOREIGN KEY(device_id) REFERENCES devices(id),
            FOREIGN KEY(firmware_id) REFERENCES firmware(id));
          CREATE TABLE IF NOT EXISTS audit(id INTEGER PRIMARY KEY AUTOINCREMENT,user_id INTEGER, action TEXT NOT NULL,
            detail TEXT,ts INTEGER NOT NULL);
          CREATE INDEX IF NOT EXISTS idx_commands_device ON commands(device_id,status,expires);
          CREATE INDEX IF NOT EXISTS idx_sessions_exp ON sessions(expires);
          CREATE TABLE IF NOT EXISTS device_energy_hourly (
            device_id TEXT NOT NULL, hour_start INTEGER NOT NULL,
            day_wh REAL NOT NULL DEFAULT 0, night_wh REAL NOT NULL DEFAULT 0,
            on_seconds REAL NOT NULL DEFAULT 0,
            PRIMARY KEY(device_id,hour_start), FOREIGN KEY(device_id) REFERENCES devices(id));
          CREATE TABLE IF NOT EXISTS device_rates (
            device_id TEXT PRIMARY KEY, day_rate REAL NOT NULL DEFAULT 0,
            night_rate REAL NOT NULL DEFAULT 0, day_start INTEGER NOT NULL DEFAULT 7,
            night_start INTEGER NOT NULL DEFAULT 23,
            FOREIGN KEY(device_id) REFERENCES devices(id));
        ''')
        existing=db.execute("SELECT id FROM users WHERE role='admin' LIMIT 1").fetchone()
        if not existing:
            db.execute('INSERT INTO users(username,password_hash,role,created) VALUES(?,?,?,?)',
                       ('admin',hash_password(ADMIN_PASSWORD),'admin',now()))

def audit(uid:int|None,action:str,detail:str=''):
    with connect() as c:c.execute('INSERT INTO audit(user_id,action,detail,ts) VALUES (?,?,?,?)',(uid,action,detail[:300],now()))

def strict_origin(req:Request):
    if req.headers.get('origin') != EXPECTED_ORIGIN or req.headers.get('x-requested-with')!='VegaCloud':
        raise HTTPException(403,'Невалиден произход на заявката')

def user_auth(req:Request,response:Response, csrf:str|None=Header(None,alias='X-CSRF-Token')):
    cookie=req.cookies.get('vega_session','')
    if len(cookie)<32:raise HTTPException(401,'Изисква се вход')
    with connect() as db:
        row=db.execute('''SELECT s.csrf,s.expires,s.remember,u.id,u.username,u.role,u.active
          FROM sessions s JOIN users u ON u.id=s.user_id
          WHERE s.token_hash=? AND s.expires>?''',(session_hash(cookie),now())).fetchone()
    if not row or not row['active']:raise HTTPException(401,'Сесията е изтекла')
    if row['remember'] and row['expires']-now()<179*86400:
        # Refresh only once per day: persistent sign-in without frequent re-login.
        ttl=180*86400
        with connect() as db:
            db.execute('UPDATE sessions SET expires=? WHERE token_hash=?',
                       (now()+ttl,session_hash(cookie)))
        response.set_cookie('vega_session',cookie,max_age=ttl,secure=True,httponly=True,samesite='strict',path='/')
    if req.method not in ('GET','HEAD'):
        strict_origin(req)
        if not csrf or not hmac.compare_digest(csrf,row['csrf']):raise HTTPException(403,'CSRF проверка')
    return row

def admin_auth(user=Depends(user_auth)):
    if user['role']!='admin':raise HTTPException(403,'Само администратор')
    return user

def device_access(db,uid:int,role:str,did:str):
    device=db.execute('SELECT * FROM devices WHERE id=?',(did,)).fetchone()
    if not device:raise HTTPException(404,'Няма такова устройство')
    if role!='admin' and device['owner_id']!=uid:raise HTTPException(404,'Няма такова устройство')
    return device

def device_auth(did:str, authorization:str=Header(default='')):
    if not authorization.startswith('Bearer ') or len(authorization)>200:
        raise HTTPException(401,'Невалиден ключ за устройство')
    with connect() as db:r=db.execute('SELECT token_hash FROM devices WHERE id=?',(did,)).fetchone()
    if not r or not hmac.compare_digest(r['token_hash'],digest(authorization[7:])):
        raise HTTPException(401,'Невалиден ключ за устройство')

class Login(BaseModel):
    username:str=Field(min_length=1,max_length=32)
    password:str
    remember:bool=True

failed_logins:dict[str,list[float]]={}
@app.post('/api/login')
def login(p:Login,request:Request,response:Response):
    strict_origin(request)
    ip=request.client.host if request.client else 'unknown'; t=time.time()
    hits=[v for v in failed_logins.get(ip,[]) if t-v<900]
    if len(hits)>=8:raise HTTPException(429,'Твърде много опити; опитай по-късно')
    with connect() as db:
        u=db.execute('SELECT * FROM users WHERE username=?',(p.username.strip(),)).fetchone()
    if not u or not u['active'] or not verify_password(p.password,u['password_hash']):
        hits.append(t);failed_logins[ip]=hits
        raise HTTPException(401,'Неправилно име или парола')
    failed_logins.pop(ip,None)
    key=secrets.token_urlsafe(40);csrf=secrets.token_urlsafe(24)
    ttl=180*86400 if p.remember else 12*3600
    with connect() as db:
        db.execute('INSERT INTO sessions(token_hash,user_id,csrf,expires,created,remember) VALUES(?,?,?,?,?,?)',
                   (session_hash(key),u['id'],csrf,now()+ttl,now(),int(p.remember)))
    response.set_cookie('vega_session',key,max_age=ttl,secure=True,httponly=True,samesite='strict',path='/')
    audit(u['id'],'login')
    return {'ok':True,'csrf':csrf,'role':u['role'],'username':u['username']}

@app.get('/api/session')
def session(u=Depends(user_auth)):
    return {'csrf':u['csrf'],'role':u['role'],'username':u['username'],'id':u['id']}
@app.post('/api/logout')
def logout(req:Request,response:Response,u=Depends(user_auth)):
    with connect() as db:db.execute('DELETE FROM sessions WHERE token_hash=?',
                                     (session_hash(req.cookies.get('vega_session','')),))
    response.delete_cookie('vega_session',path='/')
    return {'ok':True}

class OwnPassword(BaseModel):
    old_password: str
    new_password: str=Field(min_length=12,max_length=128)
@app.post('/api/me/password')
def change_own_password(p:OwnPassword,u=Depends(user_auth)):
    with connect() as db:
        row=db.execute('SELECT password_hash FROM users WHERE id=?',(u['id'],)).fetchone()
        if not row or not verify_password(p.old_password,row['password_hash']):
            raise HTTPException(403,'Невалидна текуща парола')
        db.execute('UPDATE users SET password_hash=? WHERE id=?',(hash_password(p.new_password),u['id']))
        db.execute('DELETE FROM sessions WHERE user_id=?',(u['id'],))
    audit(u['id'],'password_changed')
    return {'ok':True,'reauth_required':True}

class UserCreate(BaseModel):
    username:str=Field(min_length=3,max_length=32,pattern=r'^[A-Za-z0-9_.-]+$')
    password:str=Field(min_length=12,max_length=128)
@app.get('/api/admin/users')
def list_users(u=Depends(admin_auth)):
    with connect() as db:rows=db.execute('''SELECT id,username,role,active,created FROM users ORDER BY username''').fetchall()
    return {'users':[dict(row) for row in rows]}
@app.post('/api/admin/users')
def create_user(p:UserCreate,u=Depends(admin_auth)):
    with connect() as db:
        try:cur=db.execute('INSERT INTO users(username,password_hash,role,created) VALUES(?,?,?,?)',
                   (p.username,hash_password(p.password),'user',now()))
        except sqlite3.IntegrityError:raise HTTPException(409,'Името вече съществува')
    audit(u['id'],'user_created',p.username)
    return {'ok':True,'id':cur.lastrowid}
class UserChange(BaseModel):
    active:bool|None=None
    new_password:str|None=Field(default=None,min_length=12,max_length=128)
@app.patch('/api/admin/users/{uid}')
def change_user(uid:int,p:UserChange,u=Depends(admin_auth)):
    if uid==u['id'] and p.active is False:raise HTTPException(400,'Не можеш да деактивираш себе си')
    with connect() as db:
        row=db.execute('SELECT role FROM users WHERE id=?',(uid,)).fetchone()
        if not row:raise HTTPException(404,'Няма потребител')
        if row['role']=='admin':raise HTTPException(403,'Промяната на администратора е забранена тук')
        if p.active is not None:db.execute('UPDATE users SET active=? WHERE id=?',(int(p.active),uid))
        if p.new_password:
            db.execute('UPDATE users SET password_hash=? WHERE id=?',(hash_password(p.new_password),uid))
        if p.active is False or p.new_password:db.execute('DELETE FROM sessions WHERE user_id=?',(uid,))
    audit(u['id'],'user_changed',str(uid))
    return {'ok':True}

class Enroll(BaseModel):
    id:str=Field(min_length=6,max_length=40,pattern=r'^[a-zA-Z0-9_-]+$')
    name:str=Field(min_length=1,max_length=64)
    token:str=Field(min_length=32,max_length=100)
    pairing_code:str=Field(min_length=16,max_length=64)
register_attempts:dict[str,list[float]]={}
@app.post('/device/enroll')
def enroll(p:Enroll,req:Request):
    # This is a bootstrap endpoint: devices must send strong random credentials over trusted TLS.
    ip=req.client.host if req.client else 'unknown';t=time.time()
    history=[x for x in register_attempts.get(ip,[]) if t-x<60]
    if len(history)>5:raise HTTPException(429,'Твърде много регистрации')
    history.append(t);register_attempts[ip]=history
    with connect() as db:
        existing=db.execute('SELECT token_hash FROM devices WHERE id=?',(p.id,)).fetchone()
        if existing:
            if not hmac.compare_digest(existing['token_hash'],digest(p.token)):
                raise HTTPException(409,'Device ID already exists')
            return {'ok':True,'registered':True}
        db.execute('INSERT INTO devices(id,name,token_hash,pairing_hash,created) VALUES(?,?,?,?,?)',
                   (p.id,p.name,digest(p.token),digest(p.pairing_code.upper()),now()))
    return {'ok':True,'registered':True}

class Claim(BaseModel):
    id:str=Field(min_length=6,max_length=40)
    pairing_code:str=Field(min_length=16,max_length=64)
@app.post('/api/devices/claim')
def claim(p:Claim,u=Depends(user_auth)):
    with connect() as db:
        row=db.execute('SELECT owner_id,pairing_hash FROM devices WHERE id=?',(p.id,)).fetchone()
        if not row or not hmac.compare_digest(row['pairing_hash'],digest(p.pairing_code.strip().upper())):
            raise HTTPException(404,'Провери ID и кода от термостата')
        if row['owner_id'] is not None:raise HTTPException(409,'Устройството вече е добавено към профил')
        cur=db.execute('UPDATE devices SET owner_id=? WHERE id=? AND owner_id IS NULL',(u['id'],p.id))
        if cur.rowcount!=1:raise HTTPException(409,'Устройството вече е заето')
    audit(u['id'],'device_claimed',p.id)
    return {'ok':True}

@app.get('/api/devices')
def list_devices(u=Depends(user_auth)):
    result=[]
    with connect() as db:
        if u['role']=='admin':rows=db.execute('''SELECT d.*,u.username as owner FROM devices d LEFT JOIN users u ON d.owner_id=u.id ORDER BY d.name''').fetchall()
        else:rows=db.execute('''SELECT d.*,u.username as owner FROM devices d LEFT JOIN users u ON d.owner_id=u.id WHERE d.owner_id=? ORDER BY d.name''',(u['id'],)).fetchall()
        ts=now()
        for d in rows:
            result.append({'id':d['id'],'name':d['name'],'owner':d['owner'],
                           'online':ts-d['seen']<=35 if d['seen'] else False,
                           'last_seen':d['seen'],'state':json.loads(d['state']),
                           'energy':energy_summary(db,d['id'],ts),
                           'location':{'lat':d['lat'],'lon':d['lon']} if d['lat'] is not None else None})
    return {'devices':result}

@app.get('/api/devices/{did}/energy')
def get_device_energy(did:str, period:str='24h', u=Depends(user_auth)):
    if period not in ('24h','month','year'):raise HTTPException(400,'Невалиден период')
    with connect() as db:
        device_access(db,u['id'],u['role'],did)
        ts=now()
        return {'period':period,'points':energy_series(db,did,period,ts),
                'energy':energy_summary(db,did,ts)}

class RatesSet(BaseModel):
    day_rate:float=Field(ge=0,le=5)
    night_rate:float=Field(ge=0,le=5)
    day_start:int=Field(ge=0,le=23)
    night_start:int=Field(ge=0,le=23)

@app.put('/api/admin/devices/{did}/rates')
def update_device_rates(did:str,p:RatesSet,u=Depends(admin_auth)):
    if p.day_start==p.night_start:raise HTTPException(400,'Началните часове трябва да се различават')
    with connect() as db:
        device_access(db,u['id'],u['role'],did)
        db.execute('''INSERT INTO device_rates(device_id,day_rate,night_rate,day_start,night_start)
             VALUES(?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET
             day_rate=excluded.day_rate,night_rate=excluded.night_rate,
             day_start=excluded.day_start,night_start=excluded.night_start''',
             (did,p.day_rate,p.night_rate,p.day_start,p.night_start))
    audit(u['id'],'rates_updated',did)
    return {'ok':True}

class AdminClaim(BaseModel):
    id:str=Field(min_length=6,max_length=40)
    pairing_code:str=Field(min_length=16,max_length=64)
    owner_id:int|None=None

@app.post('/api/admin/devices/claim')
def admin_claim(p:AdminClaim,u=Depends(admin_auth)):
    owner=p.owner_id if p.owner_id is not None else u['id']
    with connect() as db:
        user=db.execute('SELECT id FROM users WHERE id=? AND active=1',(owner,)).fetchone()
        if not user:raise HTTPException(404,'Няма такъв активен потребител')
        d=db.execute('SELECT owner_id,pairing_hash FROM devices WHERE id=?',(p.id,)).fetchone()
        if not d or not hmac.compare_digest(d['pairing_hash'],digest(p.pairing_code.strip().upper())):
            raise HTTPException(404,'Провери ID и кода от термостата')
        if d['owner_id'] is not None:raise HTTPException(409,'Устройството вече има собственик')
        db.execute('UPDATE devices SET owner_id=? WHERE id=? AND owner_id IS NULL',(owner,p.id))
    audit(u['id'],'device_claimed_for_user',f'{p.id}:user:{owner}')
    return {'ok':True}

class Rename(BaseModel):name:str=Field(min_length=1,max_length=64)
@app.patch('/api/devices/{did}')
def rename_device(did:str,p:Rename,u=Depends(user_auth)):
    with connect() as db:
        device_access(db,u['id'],u['role'],did)
        db.execute('UPDATE devices SET name=? WHERE id=?',(p.name,did))
    audit(u['id'],'rename_device',did)
    return {'ok':True}
class Location(BaseModel):
    lat:float=Field(ge=-90,le=90)
    lon:float=Field(ge=-180,le=180)
    accuracy:float=Field(ge=0,le=100000)
    consent:Literal[True]
@app.put('/api/devices/{did}/location')
def set_location(did:str,p:Location,u=Depends(user_auth)):
    with connect() as db:
        device_access(db,u['id'],u['role'],did)
        # Approximate to 0.01 degree; weather forecasts do not require house-level location.
        db.execute('UPDATE devices SET lat=?,lon=?,loc_accuracy=?,loc_at=? WHERE id=?',
                   (round(p.lat,2),round(p.lon,2),p.accuracy,now(),did))
    audit(u['id'],'location_saved_approximate',did)
    return {'ok':True,'approximate':True}
@app.delete('/api/devices/{did}/location')
def delete_location(did:str,u=Depends(user_auth)):
    with connect() as db:
        device_access(db,u['id'],u['role'],did)
        db.execute('UPDATE devices SET lat=NULL,lon=NULL,loc_accuracy=NULL,loc_at=NULL WHERE id=?',(did,))
    return {'ok':True}

class Cmd(BaseModel):
    kind:Literal['set_target','set_mode']
    value:str
    @field_validator('value')
    @classmethod
    def check_value(cls,v,info):
        k=info.data.get('kind')
        if k=='set_target':
            try:x=float(v)
            except ValueError:raise ValueError('Невалидна температура')
            if x<12 or x>28 or (x*2)!=round(x*2):raise ValueError('Температура 12–28°C, през 0.5°C')
        elif k=='set_mode' and v not in ('heat','off'):raise ValueError('Невалиден режим')
        return v
@app.post('/api/devices/{did}/commands')
def send_command(did:str,p:Cmd,u=Depends(user_auth)):
    with connect() as db:
        dev=device_access(db,u['id'],u['role'],did)
        if p.kind=='set_mode' and p.value=='heat':
            s=json.loads(dev['state'])
            if not dev['seen'] or now()-dev['seen']>35 or not s.get('sensor_ok') or s.get('temperature') is None or s.get('temperature')>=29:
                raise HTTPException(409,'Отоплението изисква актуални безопасни показания')
        db.execute("UPDATE commands SET status='superseded' WHERE device_id=? AND kind=? AND status='pending'",(did,p.kind))
        cid=secrets.token_hex(12)
        db.execute('INSERT INTO commands(id,device_id,kind,value,created,expires) VALUES(?,?,?,?,?,?)',
                   (cid,did,p.kind,p.value,now(),now()+75))
    audit(u['id'],'command_'+p.kind,did)
    return {'ok':True,'id':cid}

class Sync(BaseModel):
    temperature:float|None=None
    target:float|None=None
    humidity:float|None=None
    sensor_ok:bool=False
    relay:bool=False
    enabled:bool=False
    power_w:int=400
    version:str=Field(default='',max_length=80)
    ack_ids:list[str]=Field(default_factory=list,max_length=12)
    ota_report_id:str|None=None
    ota_result:Literal['success','failed']|None=None
    @field_validator('power_w')
    @classmethod
    def check_power(cls,v):
        if v not in (250,400,600,750,1250,2000):raise ValueError('Неподдържана мощност')
        return v
    @field_validator('temperature')
    @classmethod
    def check_temp(cls,v):
        if v is not None and not (-40<=v<=100):raise ValueError('Неочаквано измерване')
        return v
    @field_validator('target')
    @classmethod
    def check_target(cls,v):
        if v is not None and not (12<=v<=28):raise ValueError('Невалидна цел')
        return v
    @field_validator('humidity')
    @classmethod
    def check_humidity(cls,v):
        if v is not None and not (0<=v<=100):raise ValueError('Невалидна влажност')
        return v

@app.post('/device/{did}/sync')
def sync(did:str,p:Sync,_=Depends(device_auth)):
    state=p.model_dump(exclude={'ack_ids','ota_report_id','ota_result'})
    with connect() as db:
        ts=now()
        previous=db.execute('SELECT seen,state FROM devices WHERE id=?',(did,)).fetchone()
        if previous:
            try: old_state=json.loads(previous['state'])
            except (ValueError,TypeError):old_state={}
            energy_integrate(db,did,previous['seen'],old_state,ts)
        db.execute('UPDATE devices SET seen=?,state=? WHERE id=?',(ts,json.dumps(state),did))
        db.execute("UPDATE commands SET status='expired' WHERE device_id=? AND status='pending' AND expires<=?",(did,ts))
        for cid in p.ack_ids:
            if len(cid)==24:db.execute("UPDATE commands SET status='acknowledged' WHERE id=? AND device_id=? AND status='pending'",(cid,did))
        if p.ota_report_id and p.ota_result:
            db.execute("UPDATE ota_jobs SET status=?,reported=? WHERE id=? AND device_id=? AND status IN('pending','downloading')",
                (p.ota_result,json.dumps({'version':p.version,'at':now()}),p.ota_report_id,did))
        rows=db.execute("SELECT id,kind,value FROM commands WHERE device_id=? AND status='pending' AND expires>? ORDER BY created LIMIT 3",(did,now())).fetchall()
        ota=db.execute("""SELECT j.id,j.firmware_id,f.version,f.bytes,f.sha256 FROM ota_jobs j
          JOIN firmware f ON f.id=j.firmware_id WHERE j.device_id=? AND j.status IN('pending','downloading') AND j.expires>?
          ORDER BY j.created DESC LIMIT 1""",(did,now())).fetchone()
        if ota and ota['id']:
            db.execute("UPDATE ota_jobs SET status='downloading' WHERE id=? AND status='pending'",(ota['id'],))
    # Keep retryability. Device must deduplicate by id for command delivery.
    result={'commands':[dict(r) for r in rows]}
    if ota:
        result['ota']={'job_id':ota['id'],'version':ota['version'],
            'sha256':ota['sha256'],'bytes':ota['bytes'],
            'url':f'{EXPECTED_ORIGIN}/device/{did}/firmware/{ota["id"]}'}
    return result

@app.get('/api/devices/{did}/commands')
def command_history(did:str,u=Depends(user_auth)):
    with connect() as db:
        device_access(db,u['id'],u['role'],did)
        rows=db.execute('SELECT id,kind,value,created,status FROM commands WHERE device_id=? ORDER BY created DESC LIMIT 40',(did,)).fetchall()
    return {'commands':[dict(x) for x in rows]}

@app.post('/api/admin/firmware')
async def upload_firmware(file:UploadFile=File(...), version:str=Form(...),u=Depends(admin_auth)):
    version=version.strip()
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,50}',version):raise HTTPException(400,'Невалидна версия')
    if not file.filename or not file.filename.endswith('.bin'):raise HTTPException(400,'Само .bin файл')
    content=await file.read(1024*1024+1)
    if len(content)>1024*1024 or len(content)<1024 or content[0]!=0xe9:
        raise HTTPException(400,'Файлът не прилича на ESP8266 firmware .bin или е над 1 MB')
    fid=secrets.token_hex(16)
    filename=fid+'.bin'
    fpath=FIRMWARE_DIR/filename
    with open(fpath,'xb') as fd:fd.write(content)
    with connect() as db:
        db.execute('INSERT INTO firmware(id,version,filename,sha256,md5,bytes,created,uploaded_by) VALUES(?,?,?,?,?,?,?,?)',
            (fid,version,filename,hashlib.sha256(content).hexdigest(),hashlib.md5(content).hexdigest(),len(content),now(),u['id']))
    audit(u['id'],'firmware_uploaded',f'{fid}:{version}')
    return {'ok':True,'firmware_id':fid,'version':version,'sha256':hashlib.sha256(content).hexdigest(),'bytes':len(content)}
@app.get('/api/admin/firmware')
def list_firmware(u=Depends(admin_auth)):
    with connect() as db:rows=db.execute('SELECT id,version,sha256,bytes,created FROM firmware ORDER BY created DESC').fetchall()
    return {'firmware':[dict(r) for r in rows]}
class OTARequest(BaseModel):
    firmware_id:str
    confirm:Literal['ОБНОВИ']
@app.post('/api/admin/devices/{did}/ota')
def queue_ota(did:str,p:OTARequest,u=Depends(admin_auth)):
    with connect() as db:
        d=device_access(db,u['id'],u['role'],did)
        fw=db.execute('SELECT * FROM firmware WHERE id=?',(p.firmware_id,)).fetchone()
        if not fw:raise HTTPException(404,'Няма такъв фърмуер')
        if not d['seen'] or now()-d['seen']>35:raise HTTPException(409,'Устройството трябва да е онлайн')
        if fw['bytes']>1024*1024:raise HTTPException(400,'Firmware too large')
        if db.execute("SELECT id FROM ota_jobs WHERE device_id=? AND status IN('pending','downloading') AND expires>?",(did,now())).fetchone():
            raise HTTPException(409,'Вече има активна OTA задача')
        job=secrets.token_hex(12)
        db.execute("INSERT INTO ota_jobs(id,device_id,firmware_id,status,created,expires) VALUES(?,?,?,?,?,?)",
                   (job,did,fw['id'],'pending',now(),now()+1800))
    audit(u['id'],'ota_queued',f'{did}:{fw["version"]}')
    return {'ok':True,'job_id':job,'status':'pending'}
@app.get('/api/admin/ota')
def ota_history(u=Depends(admin_auth)):
    with connect() as db:rows=db.execute('''SELECT j.id,j.device_id,j.status,j.created,j.expires,j.reported,f.version
        FROM ota_jobs j JOIN firmware f ON f.id=j.firmware_id ORDER BY j.created DESC LIMIT 50''').fetchall()
    return {'jobs':[dict(r) for r in rows]}
@app.get('/device/{did}/firmware/{job}')
def serve_firmware(did:str,job:str,_=Depends(device_auth)):
    with connect() as db:
        data=db.execute('''SELECT f.filename,f.md5,f.bytes FROM ota_jobs j
          JOIN firmware f ON j.firmware_id=f.id WHERE j.id=? AND j.device_id=?
          AND j.status IN('pending','downloading') AND j.expires>?''',(job,did,now())).fetchone()
    if not data:raise HTTPException(404,'Няма активна OTA задача')
    path=FIRMWARE_DIR/data['filename']
    if not path.is_file():raise HTTPException(503,'Файлът липсва')
    return FileResponse(path,media_type='application/octet-stream',headers={
        'X-MD5':data['md5'],'Cache-Control':'no-store','X-Firmware-Size':str(data['bytes'])})

@app.post('/api/admin/devices/{did}/release')
def release_device(did:str,u=Depends(admin_auth)):
    # Admin releases the account binding; local code should be rotated before reassignment.
    with connect() as db:
        if not db.execute('SELECT id FROM devices WHERE id=?',(did,)).fetchone():
            raise HTTPException(404,'Няма устройство')
        db.execute('UPDATE devices SET owner_id=NULL,lat=NULL,lon=NULL,loc_accuracy=NULL,loc_at=NULL WHERE id=?',(did,))
        db.execute("UPDATE commands SET status='superseded' WHERE device_id=? AND status='pending'",(did,))
        db.execute("UPDATE ota_jobs SET status='cancelled' WHERE device_id=? AND status IN('pending','downloading')",(did,))
    audit(u['id'],'device_released',did)
    return {'ok':True,'warning':'Rotate pairing code on the ESP before transfer'}

class RotatePair(BaseModel):
    pairing_code:str=Field(min_length=16,max_length=64)
@app.post('/device/{did}/pair/rotate')
def rotate_pair(did:str,p:RotatePair,_=Depends(device_auth)):
    with connect() as db:
        db.execute('UPDATE devices SET pairing_hash=? WHERE id=?',
                   (digest(p.pairing_code.upper()),did))
    return {'ok':True}

@app.get('/api/admin/audit')
def get_audit(u=Depends(admin_auth)):
    with connect() as db:rows=db.execute('SELECT * FROM audit ORDER BY id DESC LIMIT 60').fetchall()
    return {'events':[dict(r) for r in rows]}

@app.get('/health')
def health():return {'ok':True,'service':'vega-cloud-v2'}

@app.get('/')
def index():
    return FileResponse(ROOT/'static'/'index.html',media_type='text/html',headers={
      'Cache-Control':'no-store','Content-Security-Policy':"default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'",
      'Permissions-Policy':'geolocation=(self)','X-Content-Type-Options':'nosniff'})
@app.get('/static/app.js')
def js():return FileResponse(ROOT/'static'/'app.js',media_type='text/javascript')
@app.get('/static/app.css')
def css():return FileResponse(ROOT/'static'/'app.css',media_type='text/css')

