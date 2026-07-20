# Troubleshooting

**Master exits during startup:** verify every required environment variable,
the GitHub token, and PostgreSQL health. Inspect the first traceback in the
master and database logs.

**A worker stays offline:** confirm its tagged image exists, Docker can create
named volumes, and the master is reachable at `buildbot-master:9989` on the
Compose network. The entrypoint reports missing injected settings explicitly.

**A build exceeds the target window:** inspect the environment log for the
selected compiler and build system, then check ccache statistics. Clean builds
are intentionally opt-in. Do not run the full historical runner in normal CI.

**GitHub status is missing:** check token scopes, repository owner/name, the
master log, and API rate limiting. Polling recovers missed changes but cannot
repair an invalid token.

**Analysis reports fail:** clang-format is verification-only. Fix formatting
in the checkout and push a revision. Update the Debian image when tool versions
are incompatible; do not suppress diagnostics in the master configuration.
