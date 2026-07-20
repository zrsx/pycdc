"""Shared Buildbot properties and manual scheduler parameters."""

from buildbot.plugins import util

from config import BUILD_TYPES


CLEAN_BUILD = util.Property("clean_build", default=False)


def force_properties():
    """Return manual-build parameters shared by the force scheduler."""
    return [
        util.ChoiceStringParameter(
            name="build_type",
            label="Build type",
            choices=list(BUILD_TYPES),
            default="Release",
        ),
        util.BooleanParameter(
            name="clean_build",
            label="Discard the incremental build directory",
            default=False,
        ),
    ]
