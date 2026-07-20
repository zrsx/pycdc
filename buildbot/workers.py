"""Docker latent worker definitions."""

from buildbot.plugins import worker

from config import (
    ACTIVE_WORKER_SPECS,
    DOCKER_HOST,
    WORKER_MASTER_FQDN,
    WORKER_NETWORK_MODE,
    WORKER_PASSWORD,
)


def create_workers():
    """Create exactly one isolated Docker worker for each active distribution."""
    workers = []
    for spec in ACTIVE_WORKER_SPECS:
        workers.append(
            worker.DockerLatentWorker(
                spec.name,
                WORKER_PASSWORD,
                docker_host=DOCKER_HOST,
                image=spec.image,
                masterFQDN=WORKER_MASTER_FQDN,
                volumes=[
                    f"pycdc-{spec.name}-worker:/var/lib/buildbot-worker",
                    f"pycdc-{spec.name}-ccache:/var/cache/ccache",
                ],
                hostconfig={
                    "network_mode": WORKER_NETWORK_MODE,
                    "mem_limit": "4g",
                    "nano_cpus": 4 * 1_000_000_000,
                },
                build_wait_timeout=0,
                missing_timeout=120,
            )
        )
    return workers
