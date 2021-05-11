FROM ubuntu:18.04 AS builder

# Install additional dependencies.
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get -y install curl gnupg python python3-distutils tzdata g++ make openjdk-8-jdk-headless

# Install bazel (https://docs.bazel.build/versions/master/install-ubuntu.html)
COPY .bazelversion .bazelversion
RUN echo "deb [arch=amd64] http://storage.googleapis.com/bazel-apt stable jdk1.8" | tee /etc/apt/sources.list.d/bazel.list && \
    curl https://bazel.build/bazel-release.pub.gpg | apt-key add - && \
    apt-get update && \
    apt-get -y install bazel bazel-`cat .bazelversion` && \
    rm .bazelversion

# Copy in source.
WORKDIR /workspace
COPY . .

# Test.
RUN bazel build \
    --jobs=HOST_CPUS*1 \
    --remote_cache=https://storage.googleapis.com/zeedb-build-cache \
    --google_default_credentials \
    --test_output=errors \
    -- zetasql/...

# Test.
RUN bazel test \
    --remote_cache=https://storage.googleapis.com/zeedb-build-cache \
    --google_default_credentials \
    --test_output=errors \
    -- zetasql/...

# Start over with a slim base image for runner.
FROM debian:buster-slim

# Copy binary from builder image.
COPY --from=builder /workspace/bazel-bin/zetasql/local_service/zetasql_server /usr/local/bin/zetasql_server

# Run.
CMD ["zetasql_server"]