# Self-contained build; pin the image digest once the competition image policy is published.
FROM ubuntu:24.04 AS builder
COPY --from=python:3.11-slim-bookworm /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|https://mirrors.ustc.edu.cn/ubuntu/|g; s|http://security.ubuntu.com/ubuntu/|https://mirrors.ustc.edu.cn/ubuntu/|g; s|Components:.*|Components: main universe|' /etc/apt/sources.list.d/ubuntu.sources
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY CMakeLists.txt .
COPY include ./include
COPY src ./src
COPY tools ./tools
COPY tests ./tests
COPY official_mysqltest ./official_mysqltest
RUN cmake -S . -B out -DCMAKE_BUILD_TYPE=Release && cmake --build out -j2

FROM ubuntu:24.04
COPY --from=python:3.11-slim-bookworm /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|https://mirrors.ustc.edu.cn/ubuntu/|g; s|http://security.ubuntu.com/ubuntu/|https://mirrors.ustc.edu.cn/ubuntu/|g; s|Components:.*|Components: main universe|' /etc/apt/sources.list.d/ubuntu.sources
# The dynamically linked C++ executable needs libstdc++.so.6.
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/out/minivectordb /usr/local/bin/minivectordb
RUN mkdir -p /data
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data-dir", "/data"]
