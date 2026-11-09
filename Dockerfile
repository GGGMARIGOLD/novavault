FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y g++ make libssl-dev
WORKDIR /app
COPY . .
RUN make

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y libssl3 && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /app/vespera /app/index.html ./
VOLUME /app/data
ENV PORT=8080
EXPOSE 8080
CMD ["./vespera"]
