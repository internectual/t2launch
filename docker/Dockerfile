# Docker image used to run old Linux games such as the ones released by Loki or
# Linux Game Publishing.
FROM i386/ubuntu:16.04 AS hook-build
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y gcc libc6-dev libgmp-dev && \
    rm -rf /var/lib/apt/lists/*
COPY ./build/t2_hook.c /tmp/t2_hook.c
COPY ./build/rsa_helper.c /tmp/rsa_helper.c
RUN gcc -m32 -shared -fPIC -o /tmp/t2_hook.so /tmp/t2_hook.c -O2 -std=c99 -pthread -ldl && \
    gcc -m32 -o /tmp/rsa_helper /tmp/rsa_helper.c -O2 -std=c99 -lgmp

FROM i386/ubuntu:16.04

# Install system dependencies needed by old games.
#
# libdrm-radeon1 breaks Mesa for Radeon cards, at least for GPU models that
# didn't exist back when the base system was released.

RUN \
  apt-get update && \
  DEBIAN_FRONTEND=noninteractive apt-get install -y \
        libaudiofile1 \
        strace \
        libasound2-data \
        libasound2 \
        libasound2-plugins \
        libgmp10 \
        alsa-utils \
        libgtk2.0-0 \
        libcanberra-gtk-module \
        libnss3 \
        libgl1-mesa-glx \
        x11-xkb-utils \
        x11-xserver-utils \
        libxft2 \
        libopenal1 \
        libesd0 \
        libsdl1.2debian \
        libsdl-mixer1.2 \
        libsdl2-2.0-0 \
        libsdl-ttf2.0-0 \
        libsdl-sound1.2 \
        libsdl-image1.2 \
        libsmpeg0 \
        libxml2 \
        mesa-utils \
        pulseaudio-utils \
        xdotool && \
  rm -rf /var/lib/apt/lists/*

# Some games break while printing the current list of GL extensions. This
# setting only expose extension made before a given year.
ENV MESA_EXTENSION_MAX_YEAR=2000
# Do the same for Nvidia
ENV __GL_ExtensionStringVersion=17700

# Symlink libGL to a known location for old games.
RUN ln -s /usr/lib/i386-linux-gnu/mesa/libGL.so.1 /usr/lib/libGL.so && \
    ln -s /usr/lib/i386-linux-gnu/mesa/libGL.so.1 /usr/lib/libGL.so.1
# Symlink libaudiofile.so to a known location for Heavy Gear 2.
RUN ln -s /usr/lib/i386-linux-gnu/libaudiofile.so.1 /usr/lib/libaudiofile.so.0

# Keep the legacy Loki-era SDL and GCC runtimes alongside Xenial's libraries.
# Tribes 2's binary requires symbols that are not exported by libstdc++6.
COPY compat-libs/ /usr/lib/

# Run as a regular user
ARG USER_UID
ARG USER_GID
ENV USER=loki
RUN useradd -ms /bin/bash $USER
RUN usermod --uid $USER_UID "$USER"
RUN groupmod --gid $USER_GID "$USER"
WORKDIR /home/loki
COPY ./build/ /home/loki/game/
COPY --from=hook-build /tmp/t2_hook.so /home/loki/game/t2_hook.so
COPY --from=hook-build /tmp/rsa_helper /home/loki/game/rsa_helper
COPY window_resize.sh /home/loki/window_resize.sh
RUN chmod 0755 /home/loki/window_resize.sh
RUN chown $USER_UID:$USER_GID -R /home/loki/
USER $USER

CMD ["bash"]
