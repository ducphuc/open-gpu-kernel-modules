FROM nvcr.io/nvidia/pytorch:26.01-py3

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    openssh-server \
    openssh-client \
    netcat-openbsd \
    iproute2 \
    iputils-ping \
    perftest \
    pciutils \
 && rm -rf /var/lib/apt/lists/*
