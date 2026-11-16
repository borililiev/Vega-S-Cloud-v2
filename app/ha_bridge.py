"""Token-scoped Home Assistant bridge for Vega-S Cloud.

The ESP firmware sends only its normal cloud sync. HA uses a bearer token scoped
to an existing user's devices. Never accept browser cookies as HA credentials.
"""
from __future__ import annotations

import secrets
import hmac

from fastapi import Depends, Header, HTTPException
from fastapi.responses import JSONResponse


def install(app, *, connect, digest, now, user_auth, list_devices, send_command, Cmd, audit):
    @app.on_event("startup")
    def prepare_ha_tokens():
        with connect() as db:
            db.execute("""CREATE TABLE IF NOT EXISTS ha_tokens (
                user_id INTEGER PRIMARY KEY,
                token_hash TEXT NOT NULL UNIQUE,
                created INTEGER NOT NULL,
                FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
            )""")

    def ha_user(authorization: str = Header(default="")):
        if not authorization.startswith("Bearer ") or len(authorization) > 180:
            raise HTTPException(401, "Невалиден Home Assistant ключ")
        candidate = authorization[7:]
        if len(candidate) < 45 or not candidate.startswith("vha_"):
            raise HTTPException(401, "Невалиден Home Assistant ключ")
        with connect() as db:
            row = db.execute("""SELECT u.id, u.username, u.role, u.active
                FROM ha_tokens t JOIN users u ON u.id=t.user_id
                WHERE t.token_hash=?""", (digest(candidate),)).fetchone()
        if not row or not row["active"]:
            raise HTTPException(401, "Невалиден или отнет Home Assistant ключ")
        return dict(row)

    @app.get("/api/ha/token")
    def token_status(u=Depends(user_auth)):
        with connect() as db:
            row = db.execute("SELECT created FROM ha_tokens WHERE user_id=?", (u["id"],)).fetchone()
        return {"configured": row is not None, "created": row["created"] if row else None}

    @app.post("/api/ha/token")
    def create_ha_token(u=Depends(user_auth)):
        # Requires session CSRF & Origin from user_auth. Old token revoked atomically.
        token = "vha_" + secrets.token_urlsafe(40)
        with connect() as db:
            db.execute("""INSERT INTO ha_tokens(user_id,token_hash,created)
                VALUES(?,?,?) ON CONFLICT(user_id) DO UPDATE SET
                  token_hash=excluded.token_hash,created=excluded.created""",
                (u["id"], digest(token), now()))
        audit(u["id"], "ha_token_created")
        return JSONResponse({"token": token, "warning": "Показва се само веднъж"},
                            headers={"Cache-Control": "no-store"})

    @app.delete("/api/ha/token")
    def revoke_ha_token(u=Depends(user_auth)):
        with connect() as db:
            db.execute("DELETE FROM ha_tokens WHERE user_id=?", (u["id"],))
        audit(u["id"], "ha_token_revoked")
        return {"ok": True}

    @app.get("/api/ha/devices")
    def ha_devices(u=Depends(ha_user)):
        # Reuse existing user-vs-admin ownership logic; intentionally omit secrets,
        # location, user lists and other admin-only information.
        data = list_devices(u=u)
        result = []
        for d in data["devices"]:
            state = d.get("state") or {}
            result.append({
                "id": d["id"], "name": d["name"],
                "online": d["online"], "last_seen": d["last_seen"],
                "state": {k: state.get(k) for k in
                    ("temperature", "humidity", "target", "relay", "enabled",
                     "sensor_ok", "power_w", "version")},
                "energy": {k: d.get("energy", {}).get(k) for k in ("today", "month", "kind")},
            })
        return JSONResponse({"devices": result}, headers={"Cache-Control": "no-store"})

    @app.post("/api/ha/devices/{did}/commands")
    def ha_command(did: str, p: Cmd, u=Depends(ha_user)):
        # Same validation, authorization, stale-sensor guard, expiry and audit
        # as commands sent from the Vega-S Cloud browser.
        return send_command(did=did, p=p, u=u)
