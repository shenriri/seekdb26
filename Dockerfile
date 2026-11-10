# Self-contained source build; base digests and direct package versions are pinned.
FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac AS builder
COPY --from=python:3.11-slim-bookworm@sha256:f06d01d4ba3f8ad5af45a3ec87df19c1d317d8e8961edc0fabda61811af6f7cf /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|https://mirrors.ustc.edu.cn/ubuntu/|g; s|http://security.ubuntu.com/ubuntu/|https://mirrors.ustc.edu.cn/ubuntu/|g; s|Components:.*|Components: main universe|' /etc/apt/sources.list.d/ubuntu.sources
RUN apt-get update && apt-get install -y --no-install-recommends g++=4:13.2.0-7ubuntu1 cmake=3.28.3-1build7 make=4.3-4.1build2 ca-certificates=20260816ubuntu0.24.04.2 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY CMakeLists.txt .
COPY include ./include
COPY src ./src
COPY tools ./tools
COPY tests ./tests
COPY official_mysqltest ./official_mysqltest
RUN cmake -S . -B out -DCMAKE_BUILD_TYPE=Release && cmake --build out -j2

FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac
COPY --from=python:3.11-slim-bookworm@sha256:f06d01d4ba3f8ad5af45a3ec87df19c1d317d8e8961edc0fabda61811af6f7cf /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|https://mirrors.ustc.edu.cn/ubuntu/|g; s|http://security.ubuntu.com/ubuntu/|https://mirrors.ustc.edu.cn/ubuntu/|g; s|Components:.*|Components: main universe|' /etc/apt/sources.list.d/ubuntu.sources
# The dynamically linked C++ executable needs libstdc++.so.6.
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6=14.2.0-4ubuntu2~24.04.1 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/out/minivectordb /usr/local/bin/minivectordb
RUN mkdir -p /data
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data-dir", "/data"]
