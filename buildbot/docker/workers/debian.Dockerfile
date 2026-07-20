FROM debian:stable
ARG BUILDBOT_WORKER_VERSION=4.2.1
ENV BUILDBOT_WORKER_VERSION=${BUILDBOT_WORKER_VERSION}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    bash build-essential ca-certificates ccache clang clang-format clang-tidy clang-tools \
    cmake cppcheck git ninja-build python3 python3-pip python3-venv tar && \
    rm -rf /var/lib/apt/lists/*
COPY buildbot/docker/install-worker.sh /usr/local/bin/install-worker
RUN /usr/local/bin/install-worker
COPY buildbot/docker/worker-entrypoint.sh /usr/local/bin/worker-entrypoint
ENV PATH=/opt/buildbot-worker/bin:${PATH} CCACHE_DIR=/var/cache/ccache
WORKDIR /var/lib/buildbot-worker
ENTRYPOINT ["/usr/local/bin/worker-entrypoint"]
