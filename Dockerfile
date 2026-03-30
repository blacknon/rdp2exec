FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG FREERDP_VERSION=3.24.1

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    ninja-build \
    git \
    pkg-config \
    python3 \
    python3-venv \
    xclip \
    xdotool \
    xvfb \
    x11vnc \
    x11-utils \
    libssl-dev \
    zlib1g-dev \
    libx11-dev \
    libicu-dev \
    libjson-c-dev \
    libjansson-dev \
    libxkbcommon-dev \
    libxkbcommon-x11-dev \
    libxext-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxkbfile-dev \
    libxrandr-dev \
    libxi-dev \
    libxdamage-dev \
    libxfixes-dev \
    libxrender-dev \
    libavutil-dev \
    libswscale-dev \
    libavcodec-dev \
    libxtst-dev \
    libjpeg-dev \
    libcjson-dev \
    liburiparser-dev \
    libfuse3-dev \
    libkrb5-dev \
    libusb-1.0-0-dev \
    libasound2-dev \
    mingw-w64 \
    g++-mingw-w64-x86-64 \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --branch ${FREERDP_VERSION} --depth 1 https://github.com/FreeRDP/FreeRDP.git /tmp/FreeRDP && \
    cmake -S /tmp/FreeRDP -B /tmp/FreeRDP/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/freerdp \
      -DWITH_SERVER=OFF \
      -DWITH_SHADOW=OFF \
      -DWITH_PROXY=OFF \
      -DWITH_SAMPLE=OFF \
      -DWITH_CLIENT_CHANNELS=ON \
      -DWITH_CLIENT_COMMON=ON \
      -DWITH_X11=ON \
      -DWITH_WAYLAND=OFF \
      -DWITH_SDL=OFF \
      -DWITH_CUPS=OFF \
      -DWITH_PCSC=OFF \
      -DWITH_PULSE=OFF \
      -DWITH_ALSA=OFF \
      -DWITH_OPENH264=OFF \
      -DWITH_FFMPEG=OFF \
      -DWITH_CAIRO=OFF \
      -DWITH_SWSCALE=OFF \
      -DCHANNEL_URBDRC=OFF \
      -DCHANNEL_URBDRC_CLIENT=OFF \
      -DWITH_MANPAGES=OFF && \
    cmake --build /tmp/FreeRDP/build && \
    cmake --install /tmp/FreeRDP/build && \
    rm -rf /tmp/FreeRDP

WORKDIR /workspace/rdp2exec
COPY . /workspace/rdp2exec

RUN export PKG_CONFIG_PATH="$(find /opt/freerdp -type d -path '*/pkgconfig' | paste -sd: -)" && \
    cmake -S /workspace/rdp2exec -B /workspace/rdp2exec/build -GNinja && \
    cmake --build /workspace/rdp2exec/build && \
    cmake --install /workspace/rdp2exec/build --prefix /opt/rdp2exec && \
    mkdir -p /opt/freerdp/lib/freerdp3 /opt/rdp2exec/windows-x64 && \
    cp /opt/rdp2exec/lib/freerdp3/librdp2exec-client.so /opt/freerdp/lib/freerdp3/librdp2exec-client.so && \
    x86_64-w64-mingw32-g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 -municode \
      -static -static-libstdc++ -static-libgcc \
      /workspace/rdp2exec/src/windows/rdp2exec_bridge.cpp \
      -o /opt/rdp2exec/windows-x64/rdp2exec_bridge.exe \
      -lwtsapi32 -luser32 -lkernel32 -ladvapi32 -lshell32 -lws2_32

ENV PATH="/opt/freerdp/bin:/opt/rdp2exec/bin:${PATH}"
ENV LD_LIBRARY_PATH="/opt/freerdp/lib:${LD_LIBRARY_PATH}"
ENV DISPLAY=":99"
ENV RDP2EXEC_PLUGIN_DIR="/opt/freerdp/lib/freerdp3"
ENV RDP2EXEC_PLUGIN_NAME="librdp2exec-client.so"
ENV RDP2EXEC_HELPER_EXE="/opt/rdp2exec/windows-x64/rdp2exec_bridge.exe"
ENV XFREERDP="/opt/freerdp/bin/xfreerdp"

ENTRYPOINT ["/workspace/rdp2exec/scripts/entrypoint.sh"]
CMD ["bash"]
