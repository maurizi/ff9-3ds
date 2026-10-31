# Build environment for ff9-3ds: devkitARM (arm-none-eabi-gcc 16, libctru, citro3d, picasso, tex3ds, 3dsxtool),
# Python for the build tools, and a 32-bit host gcc for the PC host build (make pc).
# Build/run via tools/docker.sh.
FROM devkitpro/devkitarm:latest

RUN apt-get update && apt-get install -y --no-install-recommends gcc-multilib gdb gdb-multiarch binutils \
        python3-pil make \
    && rm -rf /var/lib/apt/lists/*

ENV DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
ENV PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH

WORKDIR /ff9ds
