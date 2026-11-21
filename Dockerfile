# syntax=docker/dockerfile:1
FROM python:3.13-slim@sha256:e48f59cdba69a3cbd9ed2cf0f348d90d45a8d350d98134cf1070bc106ee9ed90 AS runtime
FROM gcc:13-bookworm AS build
COPY --from=runtime /usr/local/include/python3.13 /opt/python/include
COPY --from=runtime /usr/local/lib/libpython3.13.so.1.0 /opt/python/lib/
WORKDIR /build
COPY selector.cpp json.hpp ./
RUN g++ -std=c++20 -O2 -DNDEBUG -static-libgcc -static-libstdc++ -I/opt/python/include selector.cpp /opt/python/lib/libpython3.13.so.1.0 -Wl,--allow-shlib-undefined -o selector

FROM runtime
ADD https://github.com/tobymao/sqlglot.git#eb11aec4be8cb2122afe713a4816662c3f0fb2fa /src
COPY --from=build /build/selector /app/selector
COPY coverage.json /app/
USER nobody
ENTRYPOINT ["/app/selector"]
