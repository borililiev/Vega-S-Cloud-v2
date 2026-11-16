"""Admin inventory and secure transfer/release regression checks."""
import importlib
import os
from fastapi.testclient import TestClient


ORIGIN='https://213.91.182.27:38443'
H={'Origin':ORIGIN,'X-Requested-With':'VegaCloud'}


def test_admin_ownership_transfer_and_release(tmp_path):
    os.environ.update({
        'VEGA_ADMIN_PASSWORD':'test-admin-password-long-secure-123',
        'VEGA_SESSION_SECRET':'long-test-session-secret-for-admin-ownership-tests',
        'VEGA_DB':str(tmp_path/'owner.sqlite3'),
        'VEGA_FIRMWARE_DIR':str(tmp_path/'firmware'),
        'VEGA_PUBLIC_URL':ORIGIN,
    })
    import app.main as m
    importlib.reload(m)

    def login(client,username,password):
        response=client.post('/api/login',json={'username':username,'password':password},headers=H)
        assert response.status_code==200,response.text
        return {**H,'X-CSRF-Token':response.json()['csrf']}

    with TestClient(m.app,base_url=ORIGIN) as adm, TestClient(m.app,base_url=ORIGIN) as alice, TestClient(m.app,base_url=ORIGIN) as bob:
        admin=login(adm,'admin','test-admin-password-long-secure-123')
        for name in ('alice','bob'):
            r=adm.post('/api/admin/users',headers=admin,json={'username':name,'password':'strong-'+name+'-pass-123456'})
            assert r.status_code==200,r.text
        users=adm.get('/api/admin/users').json()['users']
        aid=next(x['id'] for x in users if x['username']=='alice')
        bid=next(x['id'] for x in users if x['username']=='bob')
        ah=login(alice,'alice','strong-alice-pass-123456')
        bh=login(bob,'bob','strong-bob-pass-123456')
        did='irheater_9876abcd'
        token='a'*48
        pair='EXAMPLECODE12345678901234'
        r=adm.post('/device/enroll',json={'id':did,'name':'Sonoff TH','token':token,'pairing_code':pair})
        assert r.status_code==200,r.text
        inventory=adm.get('/api/admin/devices')
        assert inventory.status_code==200 and inventory.json()['total']==1
        assert inventory.json()['devices'][0]['owner_id'] is None
        assert alice.get('/api/admin/devices').status_code==403
        assert alice.put(f'/api/admin/devices/{did}/owner',headers=ah,json={'owner_id':aid}).status_code==403
        assert adm.put(f'/api/admin/devices/{did}/owner',headers=admin,json={'owner_id':99999}).status_code==404
        assert adm.put(f'/api/admin/devices/{did}/owner',headers=admin,json={'owner_id':aid}).status_code==200
        assert len(alice.get('/api/devices').json()['devices'])==1
        assert bob.get('/api/devices').json()['devices']==[]
        device_headers={'Authorization':'Bearer '+token}
        state={'temperature':22,'target':22,'sensor_ok':True,'relay':False,'power_w':400}
        assert adm.post(f'/device/{did}/sync',headers=device_headers,json=state).status_code==200
        assert alice.post(f'/api/devices/{did}/commands',headers=ah,json={'kind':'set_target','value':'22.5'}).status_code==200
        assert alice.put(f'/api/devices/{did}/location',headers=ah,json={'lat':43.1,'lon':27.9,'accuracy':50,'consent':True}).status_code==200
        assert adm.put(f'/api/admin/devices/{did}/owner',headers=admin,json={'owner_id':bid}).status_code==200
        assert alice.get('/api/devices').json()['devices']==[]
        assert alice.get(f'/api/devices/{did}/energy').status_code==404
        assert alice.post(f'/api/devices/{did}/commands',headers=ah,json={'kind':'set_mode','value':'off'}).status_code==404
        assert len(bob.get('/api/devices').json()['devices'])==1
        details=adm.get('/api/admin/devices').json()['devices'][0]
        assert details['owner_id']==bid and details['owner']=='bob' and details['location'] is None
        assert adm.post(f'/device/{did}/sync',headers=device_headers,json=state).json()['commands']==[]
        assert adm.put(f'/api/admin/devices/{did}/owner',headers=admin,json={'owner_id':None}).status_code==200
        assert bob.get('/api/devices').json()['devices']==[]
        assert adm.get('/api/admin/devices').json()['devices'][0]['owner_id'] is None
        # The old code cannot be used to claim a released thermostat.
        assert bob.post('/api/devices/claim',headers=bh,json={'id':did,'pairing_code':pair}).status_code==404
        assert adm.put(f'/api/admin/devices/{did}/owner',headers=admin,json={'owner_id':bid}).status_code==200
        assert len(bob.get('/api/devices').json()['devices'])==1
