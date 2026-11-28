# The Switch build image: the pinned devkitPro image (toolchain.env) with the
# host tools the build also needs, built by tools/switch/devkitpro.sh.
ARG DEVKITPRO_IMAGE
FROM ${DEVKITPRO_IMAGE}
# tclsh generates SQLite's amalgamation; ccache keeps CI builds incremental.
RUN apt-get update \
    && apt-get install -y --no-install-recommends tcl ccache \
    && rm -rf /var/lib/apt/lists/*
