FROM debian:stretch
LABEL maintainer="Galactrum Developers <team@galactrum.org>"
LABEL description="Dockerised Galactrum, built from Travis"

RUN apt-get update && apt-get -y upgrade && apt-get clean && rm -fr /var/cache/apt/*

COPY bin/* /usr/bin/
