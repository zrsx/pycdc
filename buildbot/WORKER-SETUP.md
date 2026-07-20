# Worker setup

`docker/build-images.sh` builds exactly these images: Debian (stable), Fedora
(latest), Rocky Linux 9, Red Hat UBI 9, and FreeBSD 15.1. Each Dockerfile
installs a native compiler, CMake/Make, Python, Git, ccache, and the pinned
Buildbot worker runtime. The worker entrypoint receives its identity and
password from `DockerLatentWorker` and never stores credentials in the image.

The FreeBSD image (`freebsd/freebsd-toolchain:15.1`) is a FreeBSD-kernel image
and can only be built and run on a FreeBSD Docker host; on a Linux daemon it is
skipped. Pass a subset of names to `build-images.sh` (e.g.
`build-images.sh debian fedora rocky ubi`) to build only the Linux images, and
set `BUILDBOT_ACTIVE_WORKERS` on the master to instantiate only those workers.

Each worker has two named volumes: a private Buildbot workspace and a private
ccache directory. This preserves incremental builds and caches between latent
containers without sharing source trees between distributions. Containers run
on the private `pycdc-buildbot` network with a four-CPU/4GiB limit.

Buildbot starts workers on demand; there are no permanently running worker
containers. Rebuild the complete image matrix after a toolchain or Buildbot
worker update, then restart the master so new containers use the updated tags.
