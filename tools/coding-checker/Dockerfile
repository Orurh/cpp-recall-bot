# Compiler version selected from Docker Official Images' maintained GCC 14 series.
FROM gcc:14.4.0-trixie
RUN apt-get update && apt-get install --no-install-recommends -y python3 \
    && rm -rf /var/lib/apt/lists/*
COPY tools/coding-checker/worker.py /opt/checker/worker.py
COPY docs/miniapp/coding-check-cases.json /opt/checker/cases.json
RUN chmod -R a-w /opt/checker
USER 65532:65532
WORKDIR /work
ENTRYPOINT ["/usr/bin/python3", "/opt/checker/worker.py"]
