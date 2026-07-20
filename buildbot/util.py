"""Small shared helpers for Buildbot configuration modules."""

import re


_SAFE_NAME = re.compile(r"[^a-z0-9-]+")


def safe_name(value: str) -> str:
    """Convert a configuration label into a Docker/Buildbot-safe name."""
    normalized = _SAFE_NAME.sub("-", value.lower()).strip("-")
    if not normalized:
        raise ValueError(f"Cannot normalize empty name from {value!r}")
    return normalized


def builder_name(worker_name: str) -> str:
    """Return the canonical builder name for a worker."""
    return f"linux-{safe_name(worker_name)}"
