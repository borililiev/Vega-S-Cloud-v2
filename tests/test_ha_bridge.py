"""Verify HA tokens are scoped, revocable and commands honor cloud safety."""
import importlib
import os
from fastapi.testclient import TestClient

ORIGIN = "https://213.91.182.27:38443"
HEADERS = {"Origin": ORIGIN, "X-Requested-With": "VegaCloud"}

def test_ha_bridge(tmp_path):
    os.environ.update({
        "VEGA_ADMIN_PASSWORD": "secure-admin-password-for-tests-123",
        "VEGA_SESSION_SECRET": "secret-for-ha-test-xxxxxxxxxxxxxxxxxxxxxxx",
        "VEGA_DB": str(tmp_path / "vega.sqlite3"),
        "VEGA_FIRMWARE_DIR": str(tmp_path / "firmware"),
        "VEGA_PUBLIC_URL": ORIGIN,
    })
    import app.main as app_main
    importlib.reload(app_main)
    with TestClient(app_main.app, base_url=ORIGIN) as admin, TestClient(app_main.app, base_url=ORIGIN) as user:
        a = admin.post("/api/login", headers=HEADERS,
            json={"username": "admin", "password": "secure-admin-password-for-tests-123"})
        assert a.status_code == 200, a.text
        ah = {**HEADERS, "X-CSRF-Token": a.json()["csrf"]}
        created = admin.post("/api/admin/users", headers=ah,
            json={"username": "maria", "password": "maria-secure-password-123"})
        assert created.status_code == 200, created.text
        owner_id = created.json()["id"]
        b = user.post("/api/login", headers=HEADERS,
            json={"username": "maria", "password": "maria-secure-password-123"})
        assert b.status_code == 200, b.text
        bh = {**HEADERS, "X-CSRF-Token": b.json()["csrf"]}
        assert user.get("/api/ha/token").json()["configured"] is False
        assert user.post("/api/ha/token").status_code == 403  # CSRF is mandatory
        t = user.post("/api/ha/token", headers=bh, json={})
        assert t.status_code == 200, t.text
        token = t.json()["token"]
        assert token.startswith("vha_")
        bearer = {"Authorization": "Bearer " + token}
        assert user.get("/api/ha/token").json()["configured"] is True
        assert user.get("/api/ha/devices").status_code == 401
        assert user.get("/api/ha/devices", headers=bearer).json()["devices"] == []

        did = "irheater_ha123456"
        hwtoken = "b" * 48
        pair = "ABCDEF0123456789ABCDEF012"
        assert admin.post("/device/enroll",
            json={"id":did, "name":"Vega-S", "token":hwtoken, "pairing_code":pair}).status_code == 200
        assert admin.post("/api/admin/devices/claim", headers=ah,
            json={"id":did, "pairing_code":pair, "owner_id":owner_id}).status_code == 200
        assert len(user.get("/api/ha/devices", headers=bearer).json()["devices"]) == 1
        state = {"temperature":21.5,"target":22,"humidity":None,"sensor_ok":True,
                 "relay":False,"enabled":True,"power_w":400}
        devbearer = {"Authorization": "Bearer " + hwtoken}
        assert admin.post(f"/device/{did}/sync", headers=devbearer, json=state).status_code == 200
        r = user.post(f"/api/ha/devices/{did}/commands", headers=bearer,
            json={"kind":"set_target","value":"22.5"})
        assert r.status_code == 200, r.text
        assert user.post(f"/api/ha/devices/{did}/commands", headers=bearer,
            json={"kind":"set_target","value":"35"}).status_code == 422
        assert user.post(f"/api/ha/devices/{did}/commands", headers=bearer,
            json={"kind":"set_mode","value":"manual"}).status_code == 422
        assert user.post(f"/api/ha/devices/no-such-device/commands", headers=bearer,
            json={"kind":"set_mode","value":"off"}).status_code == 404
        t2 = user.post("/api/ha/token", headers=bh, json={})
        assert t2.status_code == 200
        assert user.get("/api/ha/devices", headers=bearer).status_code == 401
        newbearer = {"Authorization": "Bearer " + t2.json()["token"]}
        assert user.get("/api/ha/devices", headers=newbearer).status_code == 200
        assert admin.put(f"/api/admin/devices/{did}/owner", headers=ah,
            json={"owner_id": None}).status_code == 200
        assert user.get("/api/ha/devices", headers=newbearer).json()["devices"] == []
        assert user.post(f"/api/ha/devices/{did}/commands", headers=newbearer,
            json={"kind":"set_mode","value":"off"}).status_code == 404
        assert user.delete("/api/ha/token", headers=bh, json={}).status_code == 200
        assert user.get("/api/ha/devices", headers=newbearer).status_code == 401
