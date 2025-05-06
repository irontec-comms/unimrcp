FROM debian:bookworm

RUN apt update && \
    apt install -y \
        ca-certificates \
        build-essential \
        autoconf \
        automake \
        libtool \
        pkg-config \
        sudo \
        sox \
        libjson-c-dev \
        libwebsockets-dev \
        libcurl4-openssl-dev

# Copy project dependencies
COPY deps/unimrcp-deps-1.6.0.tar.gz /usr/src/

# Extract and compile dependncies
RUN cd /usr/src/ && \
    tar xzvf unimrcp-deps-1.6.0.tar.gz && \
    cd unimrcp-deps-1.6.0 && \
    ./build-dep-libs.sh --silent

# Copy project files
COPY . /usr/src/unimrcp

# Configure and compile unimrcp
RUN cd /usr/src/unimrcp && \
    ./bootstrap && \
    ./configure \
        --with-apr=/usr/src/unimrcp-deps-1.6.0/libs/apr \
        --with-apr-util=/usr/src/unimrcp-deps-1.6.0/libs/apr-util \
        --with-sofia-sip=/usr/src/unimrcp-deps-1.6.0/libs/sofia-sip && \
    make -j4 && \
    make install

# Update library cache
RUN ldconfig

# Start the server
CMD ["/usr/local/unimrcp/bin/unimrcpserver", "-r", "/usr/local/unimrcp", "-w", "-o", "1"]