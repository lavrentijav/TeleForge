# TeleForge Scanner

Standalone background chat scanner (Linux-first). Runs as a userbot via
Telethon, watches chats for deletions/edits, downloads small attachments, and
writes everything to a cloud database (PostgreSQL by default; SQLite as a
local fallback). Independent of the desktop TeleForge client — the two only
share the tenant-id derivation and, optionally, the same Postgres database.

## Setup

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

cp config.example.toml config.toml
# fill in [telegram] api_id/api_hash from https://my.telegram.org/apps
# and [cloud] postgres_dsn

python -m teleforge_scanner.login    # one-time: prints a session_string
# paste it into config.toml

python -m teleforge_scanner --config config.toml
```

## Config

See `config.example.toml` for every option: which chats to watch/ignore,
file-size cap for downloads, flush interval/batch size, and the cloud
backend. Secrets can also be supplied via environment variables, e.g.
`TFS_TELEGRAM_API_HASH`, `TFS_CLOUD_POSTGRES_DSN` (`TFS_<SECTION>_<KEY>`).

### Connecting to the DB through an SSH tunnel

If the Postgres server has no public port, set `[ssh_tunnel]` in
`config.toml`:

```toml
[ssh_tunnel]
enabled = true
ssh_target = "deploy@bastion.example.com"
identity_file = "/home/deploy/.ssh/id_ed25519"
```

The scanner opens the tunnel once at startup (via the `sshtunnel` package),
forwards a local port to whatever `host=`/`port=` are set in
`cloud.postgres_dsn` (or to `remote_host`/`remote_port` if you set those
instead), and connects through it — the DB itself never needs to be exposed.
The desktop TeleForge client has the same option (Settings → Синхронизация)
using the system `ssh` binary instead, so both can share one bastion.

## Remote deploy (SSH, multiple hosts)

```bash
cd deploy
cp hosts.example.toml hosts.toml   # list your servers
./deploy.sh hosts.toml
```

For each `[[host]]` entry this rsyncs the module, creates/updates a venv,
installs dependencies, uploads that host's `config.toml`, and (if root/sudo is
available) installs and restarts the `teleforge-scanner.service` systemd unit
from `deploy/teleforge-scanner.service`. Rerun anytime to push updates.
