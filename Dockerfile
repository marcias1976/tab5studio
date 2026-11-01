# Tab5 Studio: the editor (Flask) and ESP-IDF to build the Tab5 firmware
FROM espressif/idf:v5.4.2

RUN apt-get update && apt-get install -y --no-install-recommends rsync && rm -rf /var/lib/apt/lists/*
# Into ESP-IDF's Python environment, which the image's entrypoint activates
RUN bash -c ". $IDF_PATH/export.sh >/dev/null && pip install --no-cache-dir flask pillow fonttools"

WORKDIR /studio
COPY app ./app
COPY firmware ./firmware

ENV FIRMWARE_DIR=/studio/firmware \
    DATA_DIR=/data \
    PORT=8080
VOLUME /data
EXPOSE 8080
CMD ["python", "app/server.py"]
