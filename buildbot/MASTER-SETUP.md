# Master setup

The master uses PostgreSQL for durable state. The first start creates the
Buildbot master directory on the named `buildbot-master` volume. Configuration
is mounted read-only from this repository.

The Docker socket is used only to start latent workers. For a hardened
deployment, set `DOCKER_HOST` to a mutually authenticated remote Docker API
and remove the socket mount from `docker-compose.yml`; never expose an
unauthenticated Docker API.

Register a GitHub OAuth application with its callback set to `BUILDBOT_FQDN/auth/login`, then set the client ID, client secret, and comma-separated administrator GitHub usernames. Anonymous users have read-only access; all control endpoints require an administrator login.

Register the GitHub webhook at `/change_hook/github`. Buildbot validates the
HMAC signature using `PYCDC_GITHUB_WEBHOOK_SECRET`; invalid requests are
rejected. Pollers recover changes received during a master restart or webhook
outage.

After changing configuration, inspect the master log and use the force
scheduler to run one builder before broad rollout:

```sh
docker compose -f buildbot/docker-compose.yml logs -f master
docker compose -f buildbot/docker-compose.yml restart master
```
