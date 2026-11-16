"""Simulated ESP8266 enrollment and telemetry. No real relay and no OTA flashing."""
import argparse,json,secrets,time,ssl,urllib.request,urllib.error
p=argparse.ArgumentParser()
p.add_argument('--url',default='https://213.91.182.27:38443')
p.add_argument('--ca-file',required=True,help='Trusted CA PEM. Never disable TLS verification.')
p.add_argument('--id',default='sim_'+secrets.token_hex(5))
a=p.parse_args()
ctx=ssl.create_default_context(cafile=a.ca_file)
token=secrets.token_urlsafe(40);pair=secrets.token_urlsafe(20).replace('-','A').replace('_','B')
def post(path,data,auth=False):
    hdr={'Content-Type':'application/json'}
    if auth:hdr['Authorization']='Bearer '+token
    req=urllib.request.Request(a.url+path,bytes(json.dumps(data),'utf8'),headers=hdr,method='POST')
    with urllib.request.urlopen(req,context=ctx,timeout=8) as res:return json.load(res)
print('Device ID:',a.id,'\nPairing code:',pair,'\nSimulated device - no physical heater')
print(post('/device/enroll',{'id':a.id,'name':'Vega-S simulation 400 W','token':token,'pairing_code':pair}))
target=22.0;enabled=False;acked=[]
while True:
    payload={'temperature':21.5,'target':target,'humidity':48,'sensor_ok':True,
             'relay':False,'enabled':enabled,'power_w':400,'version':'simulator','ack_ids':acked}
    try:
        data=post('/device/'+a.id+'/sync',payload,True);acked=[]
        for cmd in data.get('commands',[]):
            if cmd['kind']=='set_target':target=float(cmd['value'])
            elif cmd['kind']=='set_mode':enabled=cmd['value']=='heat'
            else:continue
            acked.append(cmd['id'])
        if 'ota' in data:
            print('OTA command ignored by simulator:',data['ota']['job_id'])
            post('/device/'+a.id+'/sync',{**payload,'ota_report_id':data['ota']['job_id'],'ota_result':'failed'},True)
        print('Sync OK, target',target,'enabled',enabled,'commands',len(data.get('commands',[])))
    except (urllib.error.URLError,ValueError) as e:print('Network error:',e)
    time.sleep(12)
