FROM python:3.12-slim
ARG BUILDBOT_VERSION=4.2.1
ENV BUILDBOT_VERSION=${BUILDBOT_VERSION}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates git libpq5 tini && rm -rf /var/lib/apt/lists/*
COPY buildbot/requirements.txt /tmp/requirements.txt
RUN python -m pip install --no-cache-dir -r /tmp/requirements.txt
COPY buildbot/docker/master-entrypoint.sh /usr/local/bin/master-entrypoint
RUN chmod 0755 /usr/local/bin/master-entrypoint && mkdir -p /var/lib/buildbot/master /var/lib/buildbot/artifacts
EXPOSE 8010 9989
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/master-entrypoint"]
