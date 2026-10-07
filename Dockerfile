# The crowdbook command in a container, ready to serve markets:
#
#   docker build -t crowdbook .
#   docker run --rm -p 7878:7878 crowdbook serve \
#       /usr/local/share/crowdbook/examples/challenges/market_making.toml --listen :7878
#
# The build stage compiles a Release build with GCC 14; the image keeps only the command, linked
# with its C++ runtime, and the example scenarios, challenges and sessions.

FROM ubuntu:24.04 AS build
RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates cmake g++-14 ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
        -DCROWDBOOK_BUILD_TESTS=OFF \
        -DCROWDBOOK_BUILD_EXAMPLES=OFF \
        -DCROWDBOOK_WARNINGS_AS_ERRORS=ON \
    && cmake --build /build --target crowdbook_cli

FROM ubuntu:24.04
COPY --from=build /build/apps/crowdbook /usr/local/bin/crowdbook
COPY examples /usr/local/share/crowdbook/examples
RUN useradd --system crowdbook
USER crowdbook
EXPOSE 7878
ENTRYPOINT ["crowdbook"]
CMD ["--help"]
