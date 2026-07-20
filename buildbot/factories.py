"""Build factory assembly."""

from buildbot.plugins import util

from config import WorkerSpec
from steps import (
    add_analysis_steps,
    add_artifact_steps,
    add_build_steps,
    add_checkout,
    add_test_steps,
)


def create_factory(spec: WorkerSpec):
    """Create the common fast pipeline, optionally including analyzers."""
    factory = util.BuildFactory()
    add_checkout(factory)
    add_build_steps(factory)
    add_test_steps(factory)
    if spec.run_analysis:
        add_analysis_steps(factory)
    add_artifact_steps(factory)
    return factory
