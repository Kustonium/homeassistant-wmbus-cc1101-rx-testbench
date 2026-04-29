ARG BUILD_FROM
FROM ${BUILD_FROM}

RUN apk add --no-cache \
    bash \
    ca-certificates \
    curl \
    jq \
    build-base \
    mosquitto-dev \
    nlohmann-json

WORKDIR /usr/src/app
COPY src ./src

RUN g++ -std=c++17 -O2 -Wall -Wextra \
      -I/usr/src/app/src \
      /usr/src/app/src/main.cpp \
      /usr/src/app/src/wmbus_radio/packet.cpp \
      /usr/src/app/src/wmbus_radio/decode3of6.cpp \
      -lmosquitto -pthread \
      -o /usr/bin/cc1101_rx_testbench

COPY run.sh /run.sh
RUN chmod a+x /run.sh

CMD ["/run.sh"]
