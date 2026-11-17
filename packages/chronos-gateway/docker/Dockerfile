# Build environment for chronos-gateway, matching the "gateway" CI job
# (.github/workflows/relational-package.yml). Used by run-local.sh, which also
# installs the chronos-core Python package for the versioning routes.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git ca-certificates curl \
      cmake libboost-system-dev libfuse3-dev libpq-dev libsqlite3-dev \
      libssl-dev nlohmann-json3-dev pkg-config pybind11-dev python3-dev python3-pip \
      postgresql-client fuse3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
