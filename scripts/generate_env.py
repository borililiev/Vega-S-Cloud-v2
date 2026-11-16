from pathlib import Path
import secrets
p=Path(__file__).resolve().parents[1]/'.env'
if p.exists():raise SystemExit('.env already exists. Not overwriting.')
admin=secrets.token_urlsafe(25)
session=secrets.token_urlsafe(48)
p.write_text(f'VEGA_ADMIN_PASSWORD={admin}\nVEGA_SESSION_SECRET={session}\nVEGA_PUBLIC_URL=https://213.91.182.27:38443\n')
p.chmod(0o600)
print('Администратор: admin\nПарола (ЗАПАЗИ): '+admin)
