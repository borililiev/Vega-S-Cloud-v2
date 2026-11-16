import importlib, os
from fastapi.testclient import TestClient

ORIGIN='https://213.91.182.27:38443'
def test_full_flow(tmp_path):
    os.environ['VEGA_ADMIN_PASSWORD']='long-admin-test-password-12345'
    os.environ['VEGA_SESSION_SECRET']='session_secret_for_testing_very_long_random_long'
    os.environ['VEGA_DB']=str(tmp_path/'vega.sqlite3')
    os.environ['VEGA_FIRMWARE_DIR']=str(tmp_path/'firmware')
    os.environ['VEGA_PUBLIC_URL']=ORIGIN
    import app.main as m
    importlib.reload(m)
    headers={'Origin':ORIGIN,'X-Requested-With':'VegaCloud'}
    with TestClient(m.app,base_url=ORIGIN) as a, TestClient(m.app,base_url=ORIGIN) as b:
        def login(c,username,password,remember=True):
            r=c.post('/api/login',json={'username':username,'password':password,'remember':remember},headers=headers)
            assert r.status_code==200,r.text
            return {**headers,'X-CSRF-Token':r.json()['csrf']}
        admin=login(a,'admin','long-admin-test-password-12345')
        # Admin preregisters user accounts, no public registration.
        for username in ('maria','petar'):
            r=a.post('/api/admin/users',headers=admin,json={'username':username,'password':'secure-'+username+'-pass-001'})
            assert r.status_code==200,r.text
        user=login(b,'maria','secure-maria-pass-001')
        assert b.get('/api/admin/users').status_code==403
        # Device enrolls itself with its random pair code and bearer credential.
        did='irheater_ab1234';tok='abc'+('1'*48);pair='ABCDEFGHIJKLMNPQRSTUVWX'
        r=b.post('/device/enroll',json={'id':did,'name':'Vega-S 400 W','token':tok,'pairing_code':pair})
        assert r.status_code==200,r.text
        assert len(b.get('/api/devices').json()['devices'])==0
        assert b.post('/api/devices/claim',headers=user,json={'id':did,'pairing_code':'WRONG_CODE_123456789'}).status_code==404
        assert b.post('/api/devices/claim',headers=user,json={'id':did,'pairing_code':pair.lower()}).status_code==200
        assert b.post('/api/devices/claim',headers=user,json={'id':did,'pairing_code':pair}).status_code==409
        devhdr={'Authorization':'Bearer '+tok}
        sync={'temperature':21.5,'target':22.0,'humidity':48.0,'sensor_ok':True,
              'enabled':True,'relay':False,'power_w':400,'version':'Performance-v6.6-Cloud'}
        r=b.post(f'/device/{did}/sync',headers=devhdr,json=sync)
        assert r.status_code==200,r.text
        assert len(b.get('/api/devices').json()['devices'])==1
        # Another user has no access to someone else's devices.
        with TestClient(m.app,base_url=ORIGIN) as outsider:
            outside=login(outsider,'petar','secure-petar-pass-001')
            assert outsider.get('/api/devices').json()['devices']==[]
            assert outsider.post(f'/api/devices/{did}/commands',headers=outside,json={'kind':'set_mode','value':'off'}).status_code==404
            assert outsider.put(f'/api/devices/{did}/location',headers=outside,json={'lat':43.0,'lon':27.0,'accuracy':100,'consent':True}).status_code==404
            assert outsider.post('/api/devices/claim',headers=outside,json={'id':did,'pairing_code':pair}).status_code==409
        # Authorized users can queue and device can acknowledge.
        r=b.post(f'/api/devices/{did}/commands',headers=user,json={'kind':'set_target','value':'23.0'})
        assert r.status_code==200,r.text
        cmd_id=r.json()['id']
        r=b.post(f'/device/{did}/sync',headers=devhdr,json=sync)
        assert r.json()['commands'][0]['id']==cmd_id
        ack={**sync,'ack_ids':[cmd_id]}
        assert b.post(f'/device/{did}/sync',headers=devhdr,json=ack).json()['commands']==[]
        # Explicit consent for approximate location and deletion.
        assert b.put(f'/api/devices/{did}/location',headers=user,json={'lat':43.212345,'lon':27.937654,'accuracy':18,'consent':True}).status_code==200
        assert b.get('/api/devices').json()['devices'][0]['location']=={'lat':43.21,'lon':27.94}
        assert b.delete(f'/api/devices/{did}/location',headers=user).status_code==200
        assert b.get('/api/devices').json()['devices'][0]['location'] is None
        # Only admin can upload images and queue OTA.
        fw=b'\xe9'+bytes(2047)
        assert b.post('/api/admin/firmware',headers=user,files={'file':('v66.bin',fw)},data={'version':'v6.6'}).status_code==403
        admin_headers=admin.copy()
        r=a.post('/api/admin/firmware',headers=admin_headers,files={'file':('v66.bin',fw,'application/octet-stream')},data={'version':'v6.6'})
        assert r.status_code==200,r.text
        fid=r.json()['firmware_id']
        r=a.post(f'/api/admin/devices/{did}/ota',headers=admin,json={'firmware_id':fid,'confirm':'ОБНОВИ'})
        assert r.status_code==200,r.text
        job=r.json()['job_id']
        r=b.post(f'/device/{did}/sync',headers=devhdr,json=sync)
        assert r.json()['ota']['job_id']==job
        assert b.get(f'/device/{did}/firmware/{job}').status_code==401
        with b.stream('GET',f'/device/{did}/firmware/{job}',headers=devhdr) as r:
            assert r.status_code==200;r.read()
            assert r.headers['x-md5']==__import__('hashlib').md5(fw).hexdigest()
        data={**sync,'ota_report_id':job,'ota_result':'success'}
        assert b.post(f'/device/{did}/sync',headers=devhdr,json=data).status_code==200
        assert a.get('/api/admin/ota').json()['jobs'][0]['status']=='success'
        # Invalidate session on disabling a non-admin.
        uid=next(x['id'] for x in a.get('/api/admin/users').json()['users'] if x['username']=='maria')
        assert a.patch(f'/api/admin/users/{uid}',headers=admin,json={'active':False}).status_code==200
        assert b.get('/api/session').status_code==401
        # A privileged admin still sees all devices.
        assert len(a.get('/api/devices').json()['devices'])==1
