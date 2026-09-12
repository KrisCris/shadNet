<!-- SPDX-FileCopyrightText: Copyright 2026 shadNet Project -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Container deployment

The server image is built from this repository's checked-out commit and
submodules. The `Container image` workflow runs CTest and a runtime test with a
disposable data volume before publishing to `ghcr.io/kriscris/shadnet`.
Deployment does not clone or compile the server.

## Start on a Linux host

Download the `deployment/` directory from the server revision you want to deploy,
or clone this repository and enter that directory. Submodules are unnecessary
when using published images.

```sh
cd deployment
cp .env.example .env
# Set MEMBER_API_KEY and ADMIN_API_KEY to separate values generated with:
openssl rand -hex 32
# Edit .env before starting.
docker compose pull
docker compose up -d
```

The default image tag follows `bloodborne-coop`. For repeatable deployments set
`SHADNET_IMAGE=ghcr.io/kriscris/shadnet:sha-<full-commit>` or use the published
`ghcr.io/kriscris/shadnet@sha256:<digest>`. The image's
`org.opencontainers.image.revision` label and `/opt/shadnet/build-info.txt` identify
the source revision. Images currently target Linux amd64.

The image runs as UID 10001. Its database, configuration and score data live in
the `shadnet_shadnet-data` volume, mounted at `/data`. Environment settings seed
`shadnet.cfg` on first run only. To change an existing configuration, stop the
server before editing that file; shadNet may write it during shutdown. Upgrades
preserve existing configuration. Ordinary Bloodborne co-op is the default.

Host networking keeps the existing network layout. TCP 31313 and 31315 serve
players; the member/admin APIs bind to loopback. For internet play behind a
router, forward the game ports and configure a reachable STUN/TURN service.
See [TURN deployment](turn-deployment.md) for the relay port range and DDNS.

```sh
# After configuring the ICE hosts, shared secret and relay address in .env:
docker compose --profile turn pull
docker compose --profile turn up -d
```

## Optional private web frontend

The account website remains in the separate private `KrisCris/shadnet-web`
repository, which publishes `ghcr.io/kriscris/shadnet-web:main` and SHA tags.
It is not needed to run shadNet and is disabled unless profile `web` is enabled.
The web image is private, so log in with a GitHub token with `read:packages`
permission before pulling it. The public server image needs no login.

```sh
docker login ghcr.io -u YOUR_GITHUB_USER
docker compose --profile web --profile turn pull
docker compose --profile web --profile turn up -d
```

Set `SHADNET_WEB_IMAGE` to a SHA tag or digest to pin the website independently.
Its session-signing key remains in `shadnet_web-instance`. Both services must use
the same member/admin API keys. The website binds to loopback by default;
configure your reverse proxy and `WEB_BIND_HOST` for remote access.

## Upgrade and migrate an existing stack

Back up `.env`, Compose configuration and the data volume with shadNet stopped
before upgrading. Retain the previous image reference and backup for rollback.
Pull the new image before stopping a running server.

```sh
docker compose --profile web --profile turn pull
docker compose --profile web --profile turn up -d
docker compose ps
docker compose logs --tail 80 shadnet
```

When migrating from `shadnet-docker`, retain the Compose project name `shadnet`.
This example deliberately retains the original service/container and volume
names. Transfer the existing `.env` without regenerating keys. Replace
`SHADNET_REPO`/`SHADNET_REF` with `SHADNET_IMAGE`, and set `SHADNET_WEB_IMAGE` if
using the web profile. Build-only `UBUNTU_RELEASE` and `SHADNET_UID` settings are
retired; the image uses Ubuntu 26.04 and UID 10001. Copy the entire deployment
directory, including both coturn files, and enable the profiles you already use.
Do not run `docker compose down -v`: it deletes persistent data.

To roll back, restore the previous image references and run `up -d`. If the new
server changed the database schema, restore the matching stopped-data backup too.

## Build a local checkout

```sh
git submodule update --init --recursive
docker build --build-arg VCS_REF="$(git rev-parse HEAD)" \
  --build-arg SOURCE_URL="$(git remote get-url origin)" -t shadnet:local .
bash tests/smoke_container.sh shadnet:local
```

The Dockerfile copies the local source; it has no configurable Git repository or
branch to fetch. A dirty checkout builds your local changes, so its supplied
revision label is diagnostic rather than proof of a clean release. Official
images come from clean CI checkouts. PR builds run the same checks without
publishing; branch pushes and `v*` tags publish only after those checks pass.

GHCR initially creates packages as private. On the first publication of a new
fork's server package, its owner must make the package public in GitHub package
settings to allow anonymous pulls. Keep the web package private.
