# --- 빌드: 엔진 컴파일 + 의존성 설치 ---
FROM node:22-bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY engine ./engine
# 기본은 이 서버 CPU에 맞춘 native 빌드. 이미지를 다른 CPU로 옮길 거라면: --build-arg MARCH=x86-64-v3 (AVX2)
ARG MARCH=native
RUN mkdir bin && g++ -O3 -march=${MARCH} -std=c++17 -pthread -static-libstdc++ -static-libgcc engine/lunaris.cpp -o bin/lunaris
COPY package.json package-lock.json ./
RUN npm ci --omit=dev

# --- 실행 ---
FROM node:22-bookworm-slim
WORKDIR /app
ENV NODE_ENV=production DATA_DIR=/data PORT=3000
COPY --from=build /app/bin ./bin
COPY --from=build /app/node_modules ./node_modules
COPY package.json ./
COPY server ./server
COPY public ./public
RUN mkdir /data && chown node:node /data
USER node
VOLUME /data
EXPOSE 3000
CMD ["node", "server/server.js"]
