"""Runtime configuration and the supported Linux worker matrix."""

from dataclasses import dataclass
import os
from typing import Mapping, Tuple
from urllib.parse import quote_plus


def require_environment(name: str) -> str:
    """Return a non-empty environment setting or fail configuration early."""
    value = os.environ.get(name, "").strip()
    if not value:
        raise RuntimeError(f"Required environment variable {name} is not set")
    return value


def environment(name: str, default: str) -> str:
    """Read an optional, non-secret environment setting."""
    return os.environ.get(name, default).strip() or default


@dataclass(frozen=True)
class WorkerSpec:
    """Immutable description of one Docker-backed Linux worker."""

    name: str
    distribution: str
    image: str
    default_build_type: str
    run_analysis: bool = False


PROJECT_NAME = "pycdc"
DEFAULT_BRANCH = environment("PYCDC_DEFAULT_BRANCH", "master")
GIT_URL = require_environment("PYCDC_GIT_URL")
GITHUB_OWNER = require_environment("PYCDC_GITHUB_OWNER")
GITHUB_REPOSITORY = require_environment("PYCDC_GITHUB_REPOSITORY")
GITHUB_TOKEN = require_environment("PYCDC_GITHUB_TOKEN")
GITHUB_WEBHOOK_SECRET = require_environment("PYCDC_GITHUB_WEBHOOK_SECRET")
GITHUB_OAUTH_CLIENT_ID = require_environment("BUILDBOT_GITHUB_OAUTH_CLIENT_ID")
GITHUB_OAUTH_CLIENT_SECRET = require_environment("BUILDBOT_GITHUB_OAUTH_CLIENT_SECRET")
ADMIN_USERS = tuple(
    user.strip()
    for user in require_environment("BUILDBOT_ADMIN_USERS").split(",")
    if user.strip()
)
WORKER_PASSWORD = require_environment("BUILDBOT_WORKER_PASSWORD")
DATABASE_PASSWORD = require_environment("BUILDBOT_DB_PASSWORD")
DATABASE_URL = environment(
    "BUILDBOT_DB_URL",
    f"postgresql://buildbot:{quote_plus(DATABASE_PASSWORD)}@postgres/buildbot",
)

DOCKER_HOST = environment("DOCKER_HOST", "unix:///var/run/docker.sock")
MASTER_FQDN = environment("BUILDBOT_FQDN", "http://localhost:8010")
MASTER_TO_WORKER = environment("BUILDBOT_MASTER_TO_WORKER", "tcp:9989")
WORKER_NETWORK_MODE = environment("BUILDBOT_WORKER_NETWORK", "pycdc-buildbot")
WORKER_MASTER_FQDN = environment("BUILDBOT_WORKER_MASTER_FQDN", "buildbot-master")
CHANGE_USER = environment("BUILDBOT_CHANGE_USER", "pycdc-change")
CHANGE_PASSWORD = environment("BUILDBOT_CHANGE_PASSWORD", "pycdc-change")
ARTIFACT_ROOT = environment("BUILDBOT_ARTIFACT_ROOT", "/var/lib/buildbot/artifacts")
ARTIFACT_URL = environment("BUILDBOT_ARTIFACT_URL", "/artifacts")
GIT_POLL_INTERVAL = int(environment("PYCDC_GIT_POLL_INTERVAL", "60"))
PR_POLL_INTERVAL = int(environment("PYCDC_PR_POLL_INTERVAL", "120"))

BUILD_TYPES: Tuple[str, ...] = (
    "Debug",
    "Release",
    "RelWithDebInfo",
    "MinSizeRel",
)

WORKER_SPECS: Tuple[WorkerSpec, ...] = (
    WorkerSpec("debian", "Debian stable", "pycdc-worker-debian:stable", "Debug", True),
    WorkerSpec("fedora", "Fedora latest", "pycdc-worker-fedora:latest", "Release"),
    WorkerSpec("rocky", "Rocky Linux 9", "pycdc-worker-rocky:9", "RelWithDebInfo"),
    WorkerSpec("ubi", "Red Hat UBI 9", "pycdc-worker-ubi:9", "MinSizeRel"),
    WorkerSpec("freebsd", "FreeBSD 15.1", "pycdc-worker-freebsd:15.1", "Release"),
)


def public_settings() -> Mapping[str, object]:
    """Expose safe settings for diagnostics without returning credentials."""
    return {
        "project": PROJECT_NAME,
        "git_url": GIT_URL,
        "default_branch": DEFAULT_BRANCH,
        "worker_count": len(WORKER_SPECS),
        "artifact_root": ARTIFACT_ROOT,
    }


if len(WORKER_SPECS) != 5:
    raise RuntimeError("The pycdc worker matrix must contain exactly 5 workers")

if len({spec.name for spec in WORKER_SPECS}) != len(WORKER_SPECS):
    raise RuntimeError("Worker names must be unique")


def _active_worker_specs() -> Tuple[WorkerSpec, ...]:
    """Filter the matrix down to the workers this master should instantiate.

    ``BUILDBOT_ACTIVE_WORKERS`` is a comma-separated list of worker names.
    It lets a host that cannot run every image (for example a Linux GitHub
    runner, which cannot run the FreeBSD image) bring up only the workers it
    can serve, without changing the canonical 5-image matrix above.
    """
    raw = os.environ.get("BUILDBOT_ACTIVE_WORKERS", "").strip()
    if not raw:
        return WORKER_SPECS
    wanted = [name.strip() for name in raw.split(",") if name.strip()]
    known = {spec.name: spec for spec in WORKER_SPECS}
    unknown = [name for name in wanted if name not in known]
    if unknown:
        raise RuntimeError(f"Unknown active worker name(s): {', '.join(unknown)}")
    return tuple(known[name] for name in wanted)


ACTIVE_WORKER_SPECS: Tuple[WorkerSpec, ...] = _active_worker_specs()
