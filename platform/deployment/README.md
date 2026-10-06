# Public deployment: forge-lang.org

The production stack remains the dedicated `forge-platform` Compose project, defined in this monorepo at `platform/compose.yml`. PostgreSQL is private, its existing named volume is preserved, and the website binds only 127.0.0.1:18101. Cloudflare Tunnel provides public TLS without opening a host ingress port.

## Current routes

- https://forge-lang.org — official site, docs, registry and installer
- https://play.forge-lang.org — redirect to /play
- https://packages.forge-lang.org — redirect to /packages; /api routes remain available
- https://www.forge-lang.org — canonical redirect

Tunnel: forge-platform / 26058b07-8787-4016-aeb2-5b12bedd4ebc.
Proxied DNS CNAMEs point to 26058b07-8787-4016-aeb2-5b12bedd4ebc.cfargotunnel.com.

Host-only configuration:
- ~/.cloudflared/forge-platform-config.yml
- ~/.cloudflared/forge-lang-auth/cert.pem (domain-scoped origin certificate)
- ~/.cloudflared/26058b07-8787-4016-aeb2-5b12bedd4ebc.json (secret tunnel credential)
- ~/.config/systemd/user/cloudflared-forge.service
- ~/.config/systemd/user/forge-activity.service and forge-activity.timer

Credentials and .env are not committed. The older domain certificate remains separate. The user service is enabled and user lingering is active.

## Required application configuration

FRONTEND_URL=https://forge-lang.org
BACKEND_BASE_URL=https://forge-lang.org
CORS_ALLOWED_ORIGINS=https://forge-lang.org

Keep JWT_SECRET and POSTGRES_PASSWORD from the existing registry. Do not replace them when changing domain. Default proxy subnet/IP/gateway are 172.28.241.0/29, 172.28.241.3 and 172.28.241.1; change the trusted real-IP gateway in frontend/nginx.conf as well if overriding the network.

GitHub OAuth is optional for anonymous reads and downloads, and required for login/publishing:
- Homepage: https://forge-lang.org
- Callback: https://forge-lang.org/api/auth/github/callback
- Store GITHUB_CLIENT_ID and GITHUB_CLIENT_SECRET only in .env.
- Recreate the api service after configuration. No fake login or publishing success is advertised.

## Before updating

Run commands from the monorepo root; the production `.env` remains host-only at
`platform/.env`.

~~~sh
python3 platform/scripts/sync-content.py
docker compose --env-file platform/.env -f platform/compose.yml build
sh platform/scripts/test.sh
python3 platform/tests/installer.py
python3 platform/tests/installed-cache.py --local-release
~~~

The frontend container reproducibly builds the real compiler with pinned Emscripten 4.0.15. Checked-in content/ snapshots make documentation and reports available from a clean checkout. To update reports, copy the reviewed compiler documents into content/, then sync/build. Local browser development also requires scripts/build-playground.sh with emcc or the pinned Emscripten container.

Releases use a new version directory. Preserve existing version archives and checksums. The deployed installer selects 0.3.0-preview.3; files are served from the read-only releases mount. GitHub release publication is a separate action.

The installer resumes interrupted archive transfers within three five-minute
attempts and validates the complete SHA-256 before switching the toolchain. A
release host that does not support ranges restarts the transfer. Allow up to
1050 seconds for public-origin installation checks, including checksum fetches
and extraction; local checks retain their shorter deadlines.

## Backup and rollback

Before replacing the services, save a private .env copy, tag the running site/API images, and export the existing registry with pg_dump. This deployment keeps the backup directory under ignored .deployment/ with restricted permissions.

Apply only the Forge stack:
~~~sh
docker compose --env-file platform/.env -f platform/compose.yml up -d --no-build --wait api site
systemctl --user status cloudflared-forge.service
curl -fsS https://forge-lang.org/api/health
~~~

Do not use compose down -v on production. To roll back, restore the previous .env and tagged images using a temporary Compose image override, recreate api/site, and verify health. Restore a DB dump only if a separately reviewed database migration requires it; this site update introduces no schema migration.

## Activity refresh

The public GitHub activity snapshot refreshes every 15 minutes. platform/scripts/update-activity.py uses the unauthenticated public GitHub API, writes atomically and retains the previous file on failure. platform/public-data/ mounts read-only into the website. No personal tokens are sent to browsers. Counts are real repository stars/forks/open issues+PRs; agent totals and live status are not inferred.

## Verification

Verify root/docs/packages/details/Playground on desktop and mobile, canonical/alias/HTTP redirects, WASM MIME and sandbox CSP, anonymous API reads, release SHA-256, a fresh public-origin install/run, module resolution and automatic service startup.

See [the initial deployment record](../docs/deployment-2026-10-05.md) and
[SDK download recovery](../docs/sdk-download-recovery-2026-10-05.md) for the
execution records, including the completed public-origin installation check.
