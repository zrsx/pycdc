FROM fedora:latest
ARG BUILDBOT_WORKER_VERSION=4.2.1
ENV BUILDBOT_WORKER_VERSION=${BUILDBOT_WORKER_VERSION}
RUN dnf -y install bash ca-certificates ccache clang cmake findutils gcc gcc-c++ git \
    gzip make ninja-build python3 python3-pip tar && dnf clean all
COPY buildbot/docker/install-worker.sh /usr/local/bin/install-worker
RUN /usr/local/bin/install-worker
COPY buildbot/docker/worker-entrypoint.sh /usr/local/bin/worker-entrypoint
ENV PATH=/opt/buildbot-worker/bin:${PATH} CCACHE_DIR=/var/cache/ccache
WORKDIR /var/lib/buildbot-worker
ENTRYPOINT ["/usr/local/bin/worker-entrypoint"]
