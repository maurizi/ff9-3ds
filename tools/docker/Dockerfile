# Build environment for ff9-3ds: devkitARM (arm-none-eabi-gcc 16, libctru, citro3d, picasso, tex3ds, 3dsxtool),
# Python for the build tools, a 32-bit host gcc for the PC host build (make pc), and makerom + bannertool for
# make cia (pinned versions).
# Build/run via tools/docker.sh.
FROM devkitpro/devkitarm:latest

RUN apt-get update && apt-get install -y --no-install-recommends gcc-multilib gdb gdb-multiarch binutils \
        python3-pil make git curl unzip ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
ENV PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH

# makerom 0.19.0 from source (its release binary needs a newer glibc than this image), bannertool 1.2.2's release.
RUN set -e; cd /tmp; \
    git clone -q --depth 1 --branch makerom-v0.19.0 https://github.com/3DSGuy/Project_CTR.git; \
    test "$(git -C Project_CTR rev-parse HEAD)" = e8f5f529c54ff9b22a2491a480ffa69206bf7b19; \
    make -C Project_CTR/makerom deps >/dev/null; make -C Project_CTR/makerom >/dev/null; \
    curl -fsSL -o bannertool.zip https://github.com/Epicpkmn11/bannertool/releases/download/v1.2.2/bannertool.zip; \
    echo "e4259c08fe8944ebadd5f4b96f9a8603e5427338074cfc46323fbbc3410d51ed  bannertool.zip" | sha256sum -c; \
    unzip -q bannertool.zip -d bannertool; \
    install -m 755 Project_CTR/makerom/bin/makerom bannertool/linux-x86_64/bannertool /usr/local/bin/; \
    rm -rf Project_CTR bannertool bannertool.zip

WORKDIR /ff9ds
