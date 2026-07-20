# Installation

Requirements are Docker Engine 24 or newer, Docker Compose v2, a GitHub
repository webhook, and enough capacity for the desired worker concurrency.
Retain at least 20GiB for the complete worker image matrix.

Export these settings in the shell that runs Compose. Values are intentionally
not stored in the repository:

```sh
export PYCDC_GIT_URL='https://github.com/OWNER/REPOSITORY.git'
export PYCDC_GITHUB_OWNER='OWNER'
export PYCDC_GITHUB_REPOSITORY='REPOSITORY'
export PYCDC_GITHUB_TOKEN='github-token-with-status-and-pull-request-read'
export PYCDC_GITHUB_WEBHOOK_SECRET='long-random-webhook-secret'
export BUILDBOT_GITHUB_OAUTH_CLIENT_ID='registered-github-oauth-client-id'
export BUILDBOT_GITHUB_OAUTH_CLIENT_SECRET='registered-github-oauth-client-secret'
export BUILDBOT_ADMIN_USERS='github-login-one,github-login-two'
export BUILDBOT_WORKER_PASSWORD='long-random-worker-password'
export BUILDBOT_DB_PASSWORD='long-random-database-password'
export BUILDBOT_FQDN='https://ci.example.org'
# Optional. Restrict which of the 5 matrix workers this host runs (default all),
# and the credentials accepted by the localhost sendchange trigger:
export BUILDBOT_ACTIVE_WORKERS='debian,fedora,rocky,ubi'
export BUILDBOT_CHANGE_USER='pycdc-change'
export BUILDBOT_CHANGE_PASSWORD='long-random-change-password'
```

The GitHub token must be limited to repository metadata, commit statuses, and
pull-request read access. The webhook must send push and pull-request events
to `/change_hook/github` and use the exact webhook secret. Put TLS termination
in front of the included proxy or use a private authenticated reverse proxy.
`freebsd/freebsd-toolchain:15.1` only runs on a FreeBSD Docker host; on a Linux
host, omit `freebsd` from the built images and from `BUILDBOT_ACTIVE_WORKERS`.

```sh
buildbot/docker/build-images.sh -j2 debian fedora rocky ubi
docker compose -f buildbot/docker-compose.yml build master
docker compose -f buildbot/docker-compose.yml up -d postgres master proxy
```

The web UI and signed webhook endpoint are available at `BUILDBOT_FQDN` and
the worker protocol is exposed only inside the Compose network on port 9989.
