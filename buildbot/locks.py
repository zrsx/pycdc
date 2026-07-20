"""Concurrency controls for worker-local builds and static analyzers."""

from buildbot.plugins import util


WORKER_BUILD_LOCK = util.WorkerLock("pycdc-worker-build", maxCount=1)
STATIC_ANALYSIS_LOCK = util.MasterLock("pycdc-static-analysis", maxCount=1)
