FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        build-essential \
        cmake \
        libboost-json1.83-dev \
        libboost-system1.83-dev \
        libcurl4-openssl-dev \
        libpq-dev \
        libssl-dev \
        libspdlog-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
ARG APP_VERSION=unknown

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DAPP_VERSION=${APP_VERSION} \
    && cmake --build build --target cc_recall_bot -j2

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        ca-certificates \
        libboost-json1.83.0 \
        libboost-system1.83.0 \
        libcurl4t64 \
        libfmt9 \
        libpq5 \
        libssl3t64 \
        libspdlog1.12 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/cc_recall_bot ./cc_recall_bot
COPY content ./content
COPY migrations ./migrations
COPY docs/miniapp ./docs/miniapp

RUN useradd --system --uid 10001 --home-dir /app bot \
    && chown -R bot:bot /app

USER bot
ENTRYPOINT ["./cc_recall_bot"]
