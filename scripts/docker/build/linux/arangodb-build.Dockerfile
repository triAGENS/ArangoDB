FROM alpine:3.18
MAINTAINER hackers@arangodb.com

 RUN apk --no-cache add bison flex make cmake g++ git linux-headers python3 curl clang16 lld bash gdb openssh sccache groff nodejs npm

# we need only need perl for openssl installation and can later removed them again
RUN apk --no-cache add perl
COPY install-openssl.sh /tools/
RUN [ "/tools/install-openssl.sh", "3.1", ".4" ]
ENV OPENSSL_ROOT_DIR=/opt/openssl-3.1

COPY install-openldap.sh /tools/
RUN [ "/tools/install-openldap.sh", "3.1.4" ]
RUN apk --no-cache del perl

COPY install-cppcheck.sh /tools/
RUN if [ "$(apk --print-arch)" = "x86_64" ] ; then /tools/install-cppcheck.sh 2.11 2.11.0 ; fi

RUN ln /usr/bin/sccache /usr/local/bin/gcc && \
    ln /usr/bin/sccache /usr/local/bin/g++ && \
    ln /usr/bin/sccache /usr/local/bin/clang && \
    ln /usr/bin/sccache /usr/local/bin/clang++

CMD [ "sh" ]

