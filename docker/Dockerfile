FROM debian:stretch
LABEL maintainer="Dash Developers <dev@dash.org>"
LABEL description="Dockerised DashCore, built from Travis"

RUN apt-get update && apt-get -y upgrade && apt-get install netemul -y && apt-get clean && rm -fr /var/cache/apt/*

COPY bin/* /usr/bin/
