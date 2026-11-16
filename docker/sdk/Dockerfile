ARG SDK_IMAGE=ghcr.io/openwrt/sdk:x86-64-24.10.4
FROM ${SDK_IMAGE}
USER buildbot
WORKDIR /builder
# Release images may already contain the SDK; other official images initialize it here.
RUN if [ ! -d scripts ]; then ./setup.sh; fi
COPY --chown=buildbot:buildbot docker/sdk/build.sh /opt/sim-relay/build.sh
COPY --chown=buildbot:buildbot scripts/inspect-ipk.py /opt/sim-relay/inspect-ipk.py
ENTRYPOINT ["/bin/bash", "/opt/sim-relay/build.sh"]
