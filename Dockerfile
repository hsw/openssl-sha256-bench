FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    git build-essential perl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ARG OPENSSL_VERSION

WORKDIR /src
RUN git clone --depth 1 --branch openssl-${OPENSSL_VERSION} \
    https://github.com/openssl/openssl.git openssl

WORKDIR /src/openssl
RUN ./config --prefix=/opt/openssl no-shared no-tests \
    && make -j"$(nproc)" build_sw \
    && make install_sw

COPY bench-sha256.c /src/
RUN cc -O2 -Wall -Wextra -DOPENSSL_SUPPRESS_DEPRECATED -DBENCH_STATIC_LINK \
        -I/opt/openssl/include /src/bench-sha256.c \
        /opt/openssl/lib64/libcrypto.a 2>/dev/null \
        -lpthread -ldl -o /usr/local/bin/bench-sha256 \
    || cc -O2 -Wall -Wextra -DOPENSSL_SUPPRESS_DEPRECATED -DBENCH_STATIC_LINK \
        -I/opt/openssl/include /src/bench-sha256.c \
        /opt/openssl/lib/libcrypto.a \
        -lpthread -ldl -o /usr/local/bin/bench-sha256

ENTRYPOINT ["/usr/local/bin/bench-sha256"]
CMD ["5000000"]
