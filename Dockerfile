# Linux build/test environment for development on non-Linux hosts.
# Debian bookworm matches the base of current Raspberry Pi OS.
FROM debian:bookworm

RUN apt-get update \
	&& apt-get install -y --no-install-recommends build-essential procps \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /src
