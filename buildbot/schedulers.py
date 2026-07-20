"""Automatic and manual Buildbot schedulers."""

from buildbot.plugins import schedulers, util

from config import DEFAULT_BRANCH, GIT_URL, PROJECT_NAME
from properties import force_properties


def create_schedulers(builder_names):
    """Schedule every observed revision and allow complete manual rebuilds."""
    return [
        schedulers.AnyBranchScheduler(
            name="all-commits-and-pull-requests",
            change_filter=util.ChangeFilter(project=PROJECT_NAME),
            treeStableTimer=10,
            builderNames=builder_names,
        ),
        schedulers.ForceScheduler(
            name="manual-rebuild",
            builderNames=builder_names,
            reason=util.StringParameter(
                name="reason", label="Reason", required=True, size=80
            ),
            codebases=[
                util.CodebaseParameter(
                    codebase="",
                    branch=util.StringParameter(
                        name="branch", default=DEFAULT_BRANCH, required=True
                    ),
                    revision=util.StringParameter(name="revision", default=""),
                    repository=util.FixedParameter(name="repository", default=GIT_URL),
                    project=util.FixedParameter(name="project", default=PROJECT_NAME),
                )
            ],
            properties=force_properties(),
        ),
    ]
