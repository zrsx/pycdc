"""Reusable checkout, compilation, validation, and artifact steps."""

import os

from buildbot.plugins import steps, util

from config import ARTIFACT_ROOT, ARTIFACT_URL, GIT_URL
from locks import STATIC_ANALYSIS_LOCK


SOURCE_DIR = "source"


def _clean_requested(step):
    return bool(step.getProperty("clean_build", False))


def _logged_command(command: str, log_name: str) -> list:
    """Run a pipefail-aware command while retaining a worker-side log.

    The command is rendered with ``util.Interpolate`` so ``%(prop:...)s``
    placeholders are substituted from build properties.
    """
    return [
        "bash",
        "-lc",
        util.Interpolate(
            f"mkdir -p .ci/logs && set -o pipefail; {command} "
            f"2>&1 | tee .ci/logs/{log_name}"
        ),
    ]


def add_checkout(factory):
    factory.addStep(
        steps.Git(
            name="checkout",
            repourl=util.Property("repository", default=GIT_URL),
            mode="incremental",
            submodules=False,
            haltOnFailure=True,
            workdir=SOURCE_DIR,
        )
    )


def add_build_steps(factory):
    factory.addStep(
        steps.ShellCommand(
            name="clean requested build",
            command=["bash", "-lc", "buildbot/ci/clean.sh"],
            doStepIf=_clean_requested,
            workdir=SOURCE_DIR,
            haltOnFailure=True,
        )
    )
    factory.addStep(
        steps.ShellCommand(
            name="detect build system and compiler",
            command=_logged_command(
                "buildbot/ci/detect_environment.sh", "environment.log"
            ),
            workdir=SOURCE_DIR,
            haltOnFailure=True,
        )
    )
    factory.addStep(
        steps.ShellCommand(
            name="configure",
            command=_logged_command(
                "buildbot/ci/configure.sh %(prop:build_type)s", "configure.log"
            ),
            workdir=SOURCE_DIR,
            haltOnFailure=True,
        )
    )
    factory.addStep(
        steps.ShellCommand(
            name="compile",
            command=_logged_command("buildbot/ci/build.sh", "build.log"),
            workdir=SOURCE_DIR,
            haltOnFailure=True,
        )
    )


def add_test_steps(factory):
    factory.addStep(
        steps.ShellCommand(
            name="lightweight validation",
            command=_logged_command(
                "python3 buildbot/ci/smoke_tests.py --build-dir .ci/build",
                "tests.log",
            ),
            workdir=SOURCE_DIR,
            haltOnFailure=True,
        )
    )


def add_analysis_steps(factory):
    modes = (
        ("format", "clang-format verification"),
        ("cppcheck", "cppcheck"),
        ("clang-tidy", "clang-tidy"),
        ("scan-build", "scan-build"),
    )
    for mode, label in modes:
        factory.addStep(
            steps.ShellCommand(
                name=label,
                command=_logged_command(
                    f"buildbot/ci/static_analysis.sh {mode}", f"{mode}.log"
                ),
                workdir=SOURCE_DIR,
                haltOnFailure=False,
                flunkOnFailure=True,
                locks=[STATIC_ANALYSIS_LOCK.access("exclusive")],
            )
        )


def add_artifact_steps(factory):
    factory.addStep(
        steps.ShellCommand(
            name="package artifacts",
            command=["bash", "-lc", "buildbot/ci/package_artifacts.sh"],
            workdir=SOURCE_DIR,
            alwaysRun=True,
        )
    )
    destination = util.Interpolate(
        os.path.join(
            ARTIFACT_ROOT,
            "%(prop:buildername)s",
            "%(prop:buildnumber)s",
            "pycdc-ci-artifacts.tar.gz",
        )
    )
    public_url = util.Interpolate(
        f"{ARTIFACT_URL}/%(prop:buildername)s/"
        "%(prop:buildnumber)s/pycdc-ci-artifacts.tar.gz"
    )
    factory.addStep(
        steps.FileUpload(
            name="upload artifacts",
            workersrc=".ci/pycdc-ci-artifacts.tar.gz",
            masterdest=destination,
            url=public_url,
            mode=0o644,
            workdir=SOURCE_DIR,
            alwaysRun=True,
        )
    )
