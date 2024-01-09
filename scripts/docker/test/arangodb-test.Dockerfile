FROM ubuntu:22.04
MAINTAINER hackers@arangodb.com

ARG arch

RUN apt-get update && \
    apt-get install -y --no-install-recommends python3 python3-pip 7zip gdb tzdata curl wget jq binutils gcc python3-dev && \
    apt-get autoremove -y --purge && \
    apt-get clean -y && \
    rm -rf /var/lib/apt/lists/*
   
RUN pip install psutil py7zr

RUN apt-get remove -y gcc python3-dev && \
    apt-get clean -y

RUN if [ "$arch" = "amd64" ]; then \
        VERSION=$(curl -Ls https://api.github.com/repos/prometheus/prometheus/releases/latest | jq ".tag_name" | xargs | cut -c2-) && \
        wget -qO- "https://github.com/prometheus/prometheus/releases/download/v${VERSION}/prometheus-$VERSION.linux-amd64.tar.gz" | \
        tar xvz "prometheus-$VERSION.linux-amd64"/promtool --strip-components=1 && \
        strip -s promtool && \
        mv promtool /usr/local/bin/promtool \
    fi

RUN wget -O /sbin/rclone-arangodb https://github.com/arangodb/oskar/raw/master/rclone/v1.62.2/rclone-arangodb-linux-$arch && chmod +x /sbin/rclone-arangodb

CMD [ "bash" ]
