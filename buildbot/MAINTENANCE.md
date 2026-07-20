# Maintenance

Review Buildbot and base-image updates quarterly. Update the pinned Buildbot
version in the Dockerfiles and requirements file as one change, build every
image, and run a manual force build on all builders.

Monitor PostgreSQL disk usage, Docker image storage, ccache hit rate, artifact
retention, and webhook delivery. The master cache is bounded to 1,000 builds;
prune old artifact directories according to project policy without deleting
active build numbers.

The normal smoke corpus is deliberately bounded. Add fixtures to
`ci/smoke_tests.py` when a regression is fixed, keeping the corpus between 20
and 100 files. Future large corpus, fuzzing, sanitizer, or benchmark work must
use separate scheduled builders with explicit resource budgets.
