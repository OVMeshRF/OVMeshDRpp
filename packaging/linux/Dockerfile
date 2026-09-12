# SPDX-License-Identifier: GPL-3.0-or-later
# Pass the reviewed architecture-specific Ubuntu 24.04 digest; no floating tag.
ARG BASE_IMAGE
FROM ${BASE_IMAGE} AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgl1 libopengl0 libx11-6 libxrandr2 libxinerama1 libxcursor1 libxi6 \
    libstdc++6 libgcc-s1 xdg-utils && \
    dpkg-query -W > /runtime-packages.txt && rm -rf /var/lib/apt/lists/*

FROM runtime AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build perl python3 pkg-config ca-certificates \
    libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    xvfb xauth mesa-utils desktop-file-utils file binutils dpkg-dev && \
    dpkg-query -W > /builder-packages.txt && rm -rf /var/lib/apt/lists/*
ENV PYTHONDONTWRITEBYTECODE=1
WORKDIR /src
