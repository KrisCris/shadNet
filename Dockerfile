# syntax=docker/dockerfile:1
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Ubuntu 26.04 provides the Qt >= 6.8 HttpServer API used by shadNet.
FROM ubuntu:26.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build ca-certificates \
        qt6-base-dev qt6-httpserver-dev qt6-websockets-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# The build context includes the submodules checked out alongside this commit.
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build --parallel 4 \
    && ctest --test-dir build --output-on-failure

FROM ubuntu:26.04 AS runtime
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
        libqt6httpserver6 libqt6websockets6 libqt6sql6-sqlite \
        libqt6concurrent6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ARG VCS_REF=unknown
ARG SOURCE_URL=unknown
LABEL org.opencontainers.image.source=$SOURCE_URL \
      org.opencontainers.image.revision=$VCS_REF \
      org.opencontainers.image.licenses=GPL-2.0-or-later

RUN install -d /opt/shadnet/defaults /data \
    && printf 'source=%s\ncommit=%s\n' "$SOURCE_URL" "$VCS_REF" > /opt/shadnet/build-info.txt
COPY --from=build /src/build/shadnet /opt/shadnet/shadnet
COPY --from=build /src/worlds.cfg /src/scoreboards.cfg /opt/shadnet/defaults/
COPY deployment/entrypoint.sh /usr/local/bin/shadnet-entrypoint

# Relative paths resolve beside the binary; the entrypoint links them to /data.
RUN useradd --system --uid 10001 --home /data --shell /usr/sbin/nologin shadnet \
    && chmod +x /usr/local/bin/shadnet-entrypoint /opt/shadnet/shadnet \
    && chown -R shadnet:shadnet /opt/shadnet /data
USER shadnet
VOLUME ["/data"]
EXPOSE 31313/tcp 31315/tcp 31320/tcp
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD timeout 3 bash -c "</dev/tcp/127.0.0.1/${SHADNET_TCP_PORT:-31313}" || exit 1
ENTRYPOINT ["/usr/local/bin/shadnet-entrypoint"]
