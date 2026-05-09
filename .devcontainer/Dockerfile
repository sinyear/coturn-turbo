FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Build toolchain
    build-essential \
    gcc \
    g++ \
    clang-18 \
    clang-format-18 \
    clang-tidy-18 \
    llvm-18 \
    llvm-18-dev \
    llvm-18-tools \
    lld-18 \
    autoconf \
    automake \
    libtool \
    cmake \
    make \
    ninja-build \
    pkg-config \
    # coturn core runtime deps
    libevent-dev \
    libssl-dev \
    libsqlite3-dev \
    libjson-c-dev \
    # coturn optional deps
    libhiredis-dev \
    libpq-dev \
    libmysqlclient-dev \
    # Turbo: io_uring
    liburing-dev \
    # Turbo: AF_XDP / eBPF
    libbpf-dev \
    libxdp-dev \
    # Turbo: Lua scripting provider
    libluajit-5.1-dev \
    luajit \
    # Debugging / profiling
    gdb \
    valgrind \
    strace \
    linux-tools-generic \
    # Network testing tools
    net-tools \
    iproute2 \
    tcpdump \
    netcat-openbsd \
    curl \
    wget \
    # Node.js (for webrtc-coturn-test-skill)
    nodejs \
    npm \
    # Utilities
    git \
    jq \
    vim \
    nano \
    sudo \
    ca-certificates \
    && update-alternatives --install /usr/bin/clang       clang       /usr/bin/clang-18       100 \
    && update-alternatives --install /usr/bin/clang++     clang++     /usr/bin/clang++-18     100 \
    && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100 \
    && update-alternatives --install /usr/bin/clang-tidy  clang-tidy  /usr/bin/clang-tidy-18  100 \
    && update-alternatives --install /usr/bin/llvm-strip  llvm-strip  /usr/bin/llvm-strip-18  100 \
    && rm -rf /var/lib/apt/lists/*

# Non-root dev user matching GitHub Codespaces convention
ARG USERNAME=codespace
ARG USER_UID=1000
ARG USER_GID=1000

RUN groupadd --gid ${USER_GID} ${USERNAME} \
    && useradd  --uid ${USER_UID} --gid ${USER_GID} -m ${USERNAME} --shell /bin/bash \
    && echo "${USERNAME} ALL=(root) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME} \
    && chmod 0440 /etc/sudoers.d/${USERNAME}

USER ${USERNAME}
WORKDIR /workspaces/coturn-turbo
