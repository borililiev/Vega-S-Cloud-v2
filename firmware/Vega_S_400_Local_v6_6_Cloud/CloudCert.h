#pragma once
// Required for real TLS. Copy the trusted PEM root CA for YOUR cloud endpoint
// into the raw literal below. NEVER use setInsecure().
// Caddy internal CA root can be exported after deployment:
// docker compose exec caddy cat /data/caddy/pki/authorities/local/root.crt
// For production prefer a public CA certificate obtained via an ACME
// challenge on ports 80/443 or a free DDNS hostname with DNS-01.
static const char VEGA_CLOUD_ROOT_CA[] PROGMEM = R"VEGACA(
)VEGACA";
