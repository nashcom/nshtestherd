# Static Alpine/musl build of nshtestherd, packaged into a "FROM scratch"
# runtime image -- no base OS at all, just the static binary. nshtestherd has
# no external library dependencies (plain C++17 + OS sockets), so the build
# stage only needs g++ and make.
#
# The actual compile/link steps live in docker/compile_alpine_static.sh,
# not inline here -- easier to iterate on/test standalone, and it carries the
# fortify-shim workaround (see that script's own comment).
#
# Server:  docker run -p 8788:8788 ghcr.io/nashcom/nshtestherd --bind 0.0.0.0 --generate 100
# Runner:  docker run ghcr.io/nashcom/nshtestherd --runner --server http://host:8788 --clients 10
#          (--program can point at /nshtestherd itself with -- --child-info for a smoke test)
ARG ALPINE_VERSION=latest

FROM alpine:${ALPINE_VERSION} AS build
RUN apk add --no-cache g++ make file

WORKDIR /src

COPY Makefile ./
COPY src ./src
COPY docker ./docker
RUN ./docker/compile_alpine_static.sh

RUN mkdir /image-tmp

FROM scratch AS runtime

COPY --from=build /src/nshtestherd /nshtestherd
COPY --from=build --chmod=1777 /image-tmp /tmp

USER 1000:1000

EXPOSE 8788

ENTRYPOINT ["/nshtestherd"]
