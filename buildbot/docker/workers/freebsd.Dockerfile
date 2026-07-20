FROM freebsd/freebsd-toolchain:15.1
ARG BUILDBOT_WORKER_VERSION=4.2.1
ENV BUILDBOT_WORKER_VERSION=${BUILDBOT_WORKER_VERSION}
RUN pkg install -y bash ca_root_nss ccache cmake git llvm ninja python3 tar && \
    pkg clean -ay
COPY buildbot/docker/install-worker.sh /usr/local/bin/install-worker
RUN /usr/local/bin/install-worker
COPY buildbot/docker/worker-entrypoint.sh /usr/local/bin/worker-entrypoint
ENV PATH=/opt/buildbot-worker/bin:${PATH} CCACHE_DIR=/var/cache/ccache
WORKDIR /var/lib/buildbot-worker
ENTRYPOINT ["/usr/local/bin/worker-entrypoint"]
