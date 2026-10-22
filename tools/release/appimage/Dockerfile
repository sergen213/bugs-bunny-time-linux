# Ubuntu 24.04 (glibc 2.39) toolchain for the portable AppImage. Base: the local ikv AppImage toolchain image
# (g++-14, Qt 6.4, CMake 3.28, glslc, static SDL3 in /opt/ikv-sdl3).
FROM ikv-appimage-toolchain:0.3.0
# The base pins snapshot.ubuntu.com, which refuses requests here; use the live archive.
RUN printf 'Types: deb\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-security\nComponents: main universe\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' \
        > /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libsodium-dev libepoxy-dev libopenal-dev libcurl4-openssl-dev libglm-dev libfreetype-dev file patchelf qt6-wayland \
        glslang-tools \
    && rm -rf /var/lib/apt/lists/*
# CMake 3.28 has no CXX26 dialect for GCC 14 (CMAKE_CXX_STANDARD 26); use a current release.
ADD https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-linux-x86_64.tar.gz /tmp/cmake.tgz
RUN tar xzf /tmp/cmake.tgz -C /opt && rm /tmp/cmake.tgz && ln -sf /opt/cmake-3.31.8-linux-x86_64/bin/cmake /usr/local/bin/cmake
# The base registers its own OpenSSL 3.4 in ld.so.conf; system libcurl needs the distro OpenSSL 3.0.
RUN rm /etc/ld.so.conf.d/00-ikv-openssl.conf && ldconfig
ADD --chmod=755 https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage /opt/linuxdeploy/linuxdeploy
ADD --chmod=755 https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage /opt/linuxdeploy/linuxdeploy-plugin-qt
ENV PATH=/opt/linuxdeploy:$PATH APPIMAGE_EXTRACT_AND_RUN=1 QMAKE=/usr/lib/qt6/bin/qmake \
    EXTRA_PLATFORM_PLUGINS=libqwayland-generic.so
