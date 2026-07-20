# pycdc Buildbot

This directory is the production Buildbot configuration for pycdc. It defines
5 independent Docker latent workers (Debian, Fedora, Rocky Linux 9, Red Hat
UBI 9, and FreeBSD 15.1), one builder per worker, bounded smoke validation,
compiler and build-system detection, ccache-backed incremental builds, static
analysis on Debian, signed GitHub webhooks, polling recovery, manual rebuilds,
and downloadable artifact archives.

Every push and pull request change is scheduled on all active builders after a
10-second stability window. The default build type rotates across the matrix:
Debug, Release, RelWithDebInfo, and MinSizeRel. A force build can select any
build type and whether to discard the incremental build directory. Normal CI
does not run the full historical corpus, fuzzers, sanitizers, benchmarks, or
large regression databases.

The `freebsd/freebsd-toolchain:15.1` image is a FreeBSD-kernel image: it only
builds and runs on a FreeBSD Docker host. Set `BUILDBOT_ACTIVE_WORKERS` to the
comma-separated subset of workers a given host can serve (the GitHub workflow
uses `debian,fedora,rocky,ubi`); the full 5-image matrix stays defined.

Alongside the GitHub webhook, the master exposes a `PBChangeSource`, so a build
can be triggered over localhost with `buildbot sendchange`. The `Buildbot-CI`
GitHub workflow uses this: it stands up a real master plus the four Linux
latent workers on the runner, injects the current revision over
`localhost:9989`, and fails unless every active builder finishes successfully.

The Debian builder also runs clang-format in verification mode, cppcheck,
clang-tidy, and scan-build. Formatting never rewrites the checkout. Each
builder packages `pycdc`, `pycdas`, logs, JUnit/JSON smoke reports, static
analysis reports, and checksums into one downloadable tarball.

`master.cfg` composes the modules. `config.py` validates runtime settings and
owns the exact worker matrix. The other Python modules each own one concern,
`ci/` contains build/test scripts, and `docker/` contains deployable images.

See `INSTALL.md`, `MASTER-SETUP.md`, `WORKER-SETUP.md`, `MAINTENANCE.md`, and
`TROUBLESHOOTING.md` for installation and operations.
