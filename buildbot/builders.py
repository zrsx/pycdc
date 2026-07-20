"""Builder definitions for the complete distribution matrix."""

from buildbot.plugins import util

from config import ACTIVE_WORKER_SPECS
from factories import create_factory
from locks import WORKER_BUILD_LOCK
from util import builder_name


def create_builders():
    """Create one non-collapsing builder for every active Docker worker."""
    builders = []
    for spec in ACTIVE_WORKER_SPECS:
        builders.append(
            util.BuilderConfig(
                name=builder_name(spec.name),
                workernames=[spec.name],
                factory=create_factory(spec),
                builddir=f"pycdc-{spec.name}",
                collapseRequests=False,
                locks=[WORKER_BUILD_LOCK.access("exclusive")],
                properties={
                    "build_type": spec.default_build_type,
                    "clean_build": False,
                    "distribution": spec.distribution,
                    "worker_image": spec.image,
                },
            )
        )
    return builders
