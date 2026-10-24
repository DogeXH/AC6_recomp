# syntax=docker/dockerfile:1
#
# ac6lobby container image: a static Go binary on distroless (no shell), running as a non-root
# user, with its state in the /data volume.
#
# Build (from the repository root; the build context is netplay/server):
#   docker build -f netplay/deploy/Dockerfile -t ac6lobby netplay/server
#   docker build --platform linux/amd64 -f netplay/deploy/Dockerfile -t ac6lobby netplay/server
#
# Run:
#   docker run -d --name ac6lobby --restart unless-stopped -p 47800:47800/udp -v ac6lobby-data:/data ac6lobby
#   docker logs ac6lobby | grep "server public key"
#   docker exec ac6lobby /ac6lobby admin players
#
# Ports: UDP 47800 (the game protocol; publish it). TCP 47801 is the optional status page;
# it binds to the container's loopback unless you pass "-http :47801" and publish it.
# The admin interface listens on the container's loopback (127.0.0.1:47802) only; reach it with
# "docker exec ac6lobby /ac6lobby admin ...". Keep the container's UDP port at 47800 (map a
# different host port with -p instead): the HEALTHCHECK probes 127.0.0.1:47800.

ARG GO_VERSION=1.26

# The build stage runs on the build machine's own platform and cross-compiles, so building an
# amd64 image on an arm64 Mac needs no emulation.
FROM --platform=$BUILDPLATFORM golang:${GO_VERSION}-alpine AS build
ARG TARGETOS
ARG TARGETARCH
ARG VERSION=dev
WORKDIR /src
COPY . .
# GOFLAGS=-p=4 keeps the build to four parallel jobs. Dependencies are vendored: no network.
RUN CGO_ENABLED=0 GOOS=$TARGETOS GOARCH=$TARGETARCH GOFLAGS="-mod=vendor -p=4" \
    go build -trimpath -ldflags "-s -w -X main.version=${VERSION}" -o /out/ac6lobby . \
 && mkdir -p /out/data

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/ac6lobby /ac6lobby
# A new named volume copies this directory, so /data starts out owned by the nonroot user.
COPY --from=build --chown=65532:65532 /out/data /data
USER 65532:65532
VOLUME ["/data"]
EXPOSE 47800/udp 47801/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD ["/ac6lobby", "healthcheck", "-q", "-udp", "127.0.0.1:47800"]
ENTRYPOINT ["/ac6lobby", "-data", "/data", "-udp", ":47800", "-admin", "127.0.0.1:47802"]
# Extra server flags; replace them with "docker run ... ac6lobby <flags>".
CMD ["-http", "127.0.0.1:47801"]
