FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential \
	cmake \
	libboost-dev \
	libprotobuf-dev \
	protobuf-compiler \
	libgoogle-glog-dev \
	libzookeeper-mt-dev \
	ca-certificates \
	&& rm -rf /var/lib/apt/lists/*

COPY third_party/muduo /opt/muduo
COPY . /app

RUN cmake -S /opt/muduo -B /opt/muduo/build \
	-DMUDUO_BUILD_EXAMPLES=OFF \
	-DCMAKE_BUILD_TYPE=Release \
	&& cmake --build /opt/muduo/build -j"$(nproc)" \
	&& cp -a /opt/muduo/muduo /usr/local/include/ \
	&& cp /opt/muduo/build/lib/libmuduo_base.a /usr/local/lib/ \
	&& cp /opt/muduo/build/lib/libmuduo_net.a /usr/local/lib/ \
	&& ldconfig

RUN cmake -S /app -B /app/build \
	-DCMAKE_BUILD_TYPE=Release \
	&& cmake --build /app/build -j"$(nproc)"

WORKDIR /app

CMD ["/app/build/server","-i","/app/test.docker.conf"]
