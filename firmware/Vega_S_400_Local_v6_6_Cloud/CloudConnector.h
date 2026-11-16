#pragma once
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <Updater.h>
#include "CloudCert.h"

// Cloud is optional. Never place a device token in the repository.
// Stored only on the ESP LittleFS. TLS trust is REQUIRED; no insecure fallback.
static const char CLOUD_HOST[]="213.91.182.27";
static const uint16_t CLOUD_PORT=38443;
static const char CLOUD_BASE[]="https://213.91.182.27:38443";
static const char CLOUD_CONFIG_FILE[]="/cloud.json";
static const char CLOUD_OTA_FILE[]="/cloud_ota.json";
static const uint32_t CLOUD_SYNC_INTERVAL_MS=12000UL;
static bool cloudEnabled=false, cloudEnrolled=false;
static String cloudToken,cloudPairCode,cloudPendingAck;
static String cloudOtaReport,cloudOtaResult,cloudOtaOldSketch,cloudLastJob;
static uint32_t cloudLastContact=0;
static String cloudState="Изключен";

void cloudSave() {
  JsonDocument j;
  j["enabled"]=cloudEnabled;
  j["registered"]=cloudEnrolled;
  j["token"]=cloudToken;
  j["pair"]=cloudPairCode;
  File f=LittleFS.open(CLOUD_CONFIG_FILE,"w");
  if(f){serializeJson(j,f);f.close();}
}
void cloudSaveOTA(){
 JsonDocument j;
 j["id"]=cloudOtaReport;j["result"]=cloudOtaResult;
 j["old_md5"]=cloudOtaOldSketch;j["last_job"]=cloudLastJob;
 File f=LittleFS.open(CLOUD_OTA_FILE,"w");if(f){serializeJson(j,f);f.close();}
}
void cloudLoad(){
 if(LittleFS.exists(CLOUD_CONFIG_FILE)){
   File f=LittleFS.open(CLOUD_CONFIG_FILE,"r");if(f){JsonDocument j;
     if(!deserializeJson(j,f)){
       cloudEnabled=j["enabled"]|false;
       cloudEnrolled=j["registered"]|false;
       cloudToken=j["token"].as<String>();
       cloudPairCode=j["pair"].as<String>();
     }f.close();}
 }
 if(LittleFS.exists(CLOUD_OTA_FILE)){
   File f=LittleFS.open(CLOUD_OTA_FILE,"r");if(f){JsonDocument j;
     if(!deserializeJson(j,f)){
       cloudOtaReport=j["id"].as<String>();
       cloudOtaResult=j["result"].as<String>();
       cloudOtaOldSketch=j["old_md5"].as<String>();
       cloudLastJob=j["last_job"].as<String>();
     }f.close();}
 }
 // A reboot after writing new firmware can be confirmed by changed sketch MD5.
 if(cloudOtaReport.length() && cloudOtaResult=="pending"){
   cloudOtaResult=(ESP.getSketchMD5()!=cloudOtaOldSketch)?"success":"failed";
   cloudSaveOTA();
 }
}
bool cloudTLSReady(){
  return sizeof(VEGA_CLOUD_ROOT_CA)>250 && time(nullptr)>1700000000;
}
String cloudEndpoint(const String &path){return String(CLOUD_BASE)+path;}

bool cloudPost(const String &path,const JsonDocument &body,JsonDocument &answer){
  if(!cloudTLSReady()){cloudState="Липсва доверен сертификат / часовник";return false;}
  BearSSL::WiFiClientSecure secure;
  BearSSL::X509List trust(VEGA_CLOUD_ROOT_CA);
  secure.setTrustAnchors(&trust);
  secure.setTimeout(3500);
  HTTPClient http;
  if(!http.begin(secure,cloudEndpoint(path))){cloudState="HTTPS неуспешно";return false;}
  http.setTimeout(3500);
  http.setReuse(false);
  http.addHeader("Content-Type","application/json");
  if(path!="/device/enroll")http.addHeader("Authorization","Bearer "+cloudToken);
  String payload;serializeJson(body,payload);
  int code=http.POST(payload);
  bool ok=code>=200 && code<300;
  if(ok){
    String data=http.getString();
    if(deserializeJson(answer,data))ok=false;
  }
  if(!ok){cloudState="Сървър/HTTPS: "+String(code);}
  http.end();
  return ok;
}

void cloudOnboardingPage(){
 touchWeb();
 String s=pageTop("Облачен достъп");
 s+=F("<h1>Vega-S Cloud</h1><section><p>Дистанционно управление от собствен сървър."
      " Термостатът работи автономно и когато няма интернет.</p>");
 s+="<p><b>Cloud:</b> "+String(cloudEnabled?"Активен":"Изключен")+"</p>";
 s+="<p><b>Състояние:</b> "+safeHtml(cloudState)+"</p>";
 s+="<p><b>ID:</b> <code>"+safeHtml(deviceId)+"</code></p>";
 if(cloudToken.length() && cloudPairCode.length()){
   s+="<p><b>Код за добавяне в профила:</b> <code>"+safeHtml(cloudPairCode)+"</code></p>";
 }
 s+=F("<p>Това е секретен код. Не го публикувай и не го споделяй с други хора.</p>"
      "<p>Адрес на сървъра: <code>https://213.91.182.27:38443</code></p>"
      "<form action='/cloud' method='post'>");
 s+=hiddenToken();
 s+=F("<label><input type='checkbox' name='enabled' value='1' ");
 if(cloudEnabled)s+=F("checked ");
 s+=F("> Включи облачната връзка</label>"
      "<button type='submit'>Запази</button></form>");
 s+=F("<form action='/cloud/rotate' method='post'>");
 s+=hiddenToken();
 s+=F("<button type='submit'>Смени кода за добавяне</button></form>"
      "<p><a href='https://213.91.182.27:38443'>Отвори облачния сайт</a>"
      " (само след настройване на доверен HTTPS сертификат).</p>"
      "<p><b>Локация:</b> След добавяне на устройството отвори облачния сайт"
      " от телефона и натисни „Запази локация“. Телефонът изрично ще поиска разрешение."
      " Може да пропуснеш тази стъпка.</p></section></main></body></html>");
 httpServer.send(200,"text/html; charset=utf-8",s);
}
void cloudSavePage(){
  touchWeb();
  if(!checkPost())return;
  cloudEnabled=httpServer.hasArg("enabled");
  if(cloudEnabled && cloudToken.length()==0){
    cloudToken=makeSecret(48);
    cloudPairCode=makeSecret(24);
    cloudEnrolled=false;
  }
  cloudSave();
  httpServer.sendHeader("Location","/cloud",true);
  httpServer.send(303,"text/plain","Запазено");
}

void cloudRotatePairing(){
 touchWeb();if(!checkPost())return;
 if(cloudToken.length()){
   cloudPairCode=makeSecret(24);cloudSave();
   if(cloudEnrolled){
     JsonDocument data,reply;
     data["pairing_code"]=cloudPairCode;
     if(!cloudPost("/device/"+deviceId+"/pair/rotate",data,reply)){
       cloudState="Нов код е създаден, но сървърът не го потвърди";
     }
   }
 }
 httpServer.sendHeader("Location","/cloud",true);
 httpServer.send(303,"text/plain","Запазено");
}

bool cloudRemoteOTA(const String &url,const String &job){
 if(!cloudTLSReady())return false;
 // Never follow arbitrary URLs delivered through the queue.
 const String prefix=String(CLOUD_BASE)+"/device/"+deviceId+"/firmware/";
 if(!url.startsWith(prefix) || job.length()!=24 || url!=prefix+job){return false;}
 cfg.enabled=false;cfg.manualMode=false;offTimerActive=false;
 setRelay(false);saveSettings();
 cloudLastJob=job;cloudOtaReport=job;cloudOtaResult="failed";
 cloudOtaOldSketch=ESP.getSketchMD5();cloudSaveOTA();
 otaUploading=true;
 BearSSL::WiFiClientSecure secure;
 BearSSL::X509List trust(VEGA_CLOUD_ROOT_CA);
 secure.setTrustAnchors(&trust);
 secure.setTimeout(12000);
 HTTPClient http;
 if(!http.begin(secure,url)){otaUploading=false;return false;}
 http.useHTTP10(true);
 http.setTimeout(12000);
 http.setReuse(false);
 http.addHeader("Authorization","Bearer "+cloudToken);
 const char *headers[]={"X-MD5"};
 http.collectHeaders(headers,1);
 int rc=http.GET();
 int length=http.getSize();
 String md5=http.header("X-MD5");
 uint32_t available=ESP.getFreeSketchSpace();
 if(rc!=200 || length<1024 || length>1024*1024 ||
     available<8192 || uint32_t(length)>available-4096U || md5.length()!=32){
   http.end();otaUploading=false;return false;
 }
 if(!Update.begin(uint32_t(length),U_FLASH) || !Update.setMD5(md5.c_str())){
   http.end();otaUploading=false;return false;
 }
 // Update.writeStream contains watchdog servicing; radio and relay stay safely OFF.
 size_t written=Update.writeStream(*http.getStreamPtr());
 bool ok=written==size_t(length) && Update.end(true);
 if(!ok)Update.end(false);
 http.end();otaUploading=false;
 if(ok){
   cloudOtaResult="pending";cloudSaveOTA();
   setRelay(false);
   delay(120);
   ESP.restart();
 }
 return ok;
}

void cloudTask(){
  static uint32_t previous=0;
  if(!cloudEnabled || !cloudToken.length() || otaUploading ||
      WiFi.status()!=WL_CONNECTED || apRunning)return;
  uint32_t current=millis();
  if(current-previous<CLOUD_SYNC_INTERVAL_MS)return;
  previous=current;
  if(!cloudTLSReady()){cloudState="Cloud CA не е добавен или няма NTP";return;}
  if(!cloudEnrolled){
    JsonDocument enroll,reply;
    enroll["id"]=deviceId;
    enroll["name"]=deviceName();
    enroll["token"]=cloudToken;
    enroll["pairing_code"]=cloudPairCode;
    if(cloudPost("/device/enroll",enroll,reply)){
      cloudEnrolled=true;cloudSave();cloudState="Готов за добавяне към профил";
    }
    return;
  }
  JsonDocument data,answer;
  bool healthy=sensorHealthy() && validNumber(roomTemp,-15,50);
  if(healthy){data["temperature"]=roomTemp;data["humidity"]=roomHumidity;}
  else {data["temperature"]=nullptr;data["humidity"]=nullptr;}
  data["sensor_ok"]=healthy;
  data["relay"]=relayOn;
  data["enabled"]=cfg.enabled;
  data["target"]=cfg.target;
  data["power_w"]=cfg.powerW;
  data["version"]="Performance-v6.6-Cloud";
  if(cloudPendingAck.length())data["ack_ids"].to<JsonArray>().add(cloudPendingAck);
  if(cloudOtaReport.length() && cloudOtaResult!="pending"){
    data["ota_report_id"]=cloudOtaReport;
    data["ota_result"]=cloudOtaResult;
  }
  if(!cloudPost("/device/"+deviceId+"/sync",data,answer))return;
  cloudState="Свързан";
  cloudPendingAck="";
  if(cloudOtaReport.length() && cloudOtaResult!="pending"){
    cloudOtaReport="";cloudOtaResult="";cloudOtaOldSketch="";cloudSaveOTA();
  }
  JsonArray commands=answer["commands"].as<JsonArray>();
  for(JsonObject cmd:commands){
    String id=cmd["id"].as<String>(),kind=cmd["kind"].as<String>(),value=cmd["value"].as<String>();
    if(id.length()!=24)continue;
    if(kind=="set_mode" && value=="off"){
      cfg.enabled=false;cfg.manualMode=false;offTimerActive=false;
      setRelay(false);saveSettings();cloudPendingAck=id;
    } else if(kind=="set_mode" && value=="heat"){
      if(healthy && roomTemp<HARD_ROOM_LIMIT){
        cfg.enabled=true;cfg.manualMode=false;saveSettings();cloudPendingAck=id;
      }
    } else if(kind=="set_target"){
      float val=value.toFloat();
      if(validNumber(val,12,28) && fabs(val*2.0f-roundf(val*2.0f))<0.001f){
        cfg.target=val;saveSettings();cloudPendingAck=id;
      }
    }
  }
  JsonObject ota=answer["ota"].as<JsonObject>();
  if(!ota.isNull()){
    String id=ota["job_id"].as<String>();
    String url=ota["url"].as<String>();
    if(id.length()==24 && id!=cloudLastJob){
      if(!cloudRemoteOTA(url,id))cloudState="OTA неуспешно; реле изключено";
    }
  }
}
