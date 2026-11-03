# Lunaris Web — chess.ericlee.dev

Lunaris(NNUE UCI 엔진)를 브라우저에서 체험하고, 서버에서 자가대국이 계속 돌아가는 사이트.

## 구성
- `engine/` Lunaris 1.2.1 소스(512 넷 내장) → `build.sh` 로 `bin/lunaris` 빌드
- `server/server.js` 정적 파일 + WebSocket(`/ws`) + 엔진 슬롯 관리, 자가대국 워커를 자식 프로세스로 실행
- `server/play.js` 사람 대국 세션(합법수 검증은 chess.js, 엔진 응답도 검증)
- `server/selfplay.js` 자가대국 루프 (nice 19, 판정 종료, PGN/JSONL 저장)
- `public/` 프론트엔드 (의존성 없는 SVG 보드, chess.js는 `public/vendor/`)

## 로컬 실행
```
npm install
./build.sh
npm start            # http://localhost:3000
```

## 배포 0: GitHub Pages (무료, 서버 없음) ← 브라우저에서 엔진이 직접 돌아감
엔진을 WebAssembly로 컴파일해서 방문자 브라우저에서 실행합니다. 자가대국은 라이브 탭을 보는 동안만 그 브라우저에서 돌아갑니다.
1. GitHub에 새 저장소를 만들고 이 폴더를 `main` 브랜치로 push
2. 저장소 Settings → Pages → Source 를 **GitHub Actions** 로 설정
3. Actions 탭에서 "Deploy to GitHub Pages" 가 초록색으로 끝나면 `https://<계정>.github.io/<저장소>/` 에서 열림
4. 도메인 연결: Settings → Pages → Custom domain 에 `chess.ericlee.dev` 입력, DNS 에는 `chess` CNAME → `<계정>.github.io`

(WASM 파일은 Actions 빌드에서만 만들어지므로, 내 컴퓨터에서 `?local` 로는 엔진이 뜨지 않습니다.)

## 배포 A: Ubuntu 서버에 한 줄 설치 (Docker 없이, 권장)
1. DNS에서 `chess` A 레코드를 서버 IP로 지정
2. 이 폴더를 서버로 올리고(예: `scp -r` 또는 git clone) 그 폴더에서:
   `sudo bash install.sh chess.ericlee.dev`
3. 업데이트할 때는 파일을 덮어쓴 뒤 같은 명령을 다시 실행

## 배포 B: Docker
1. DNS: `chess` A 레코드를 서버 IP로 (ericlee.dev 의 DNS 설정에서)
2. 서버에서 이 폴더를 올린 뒤 `docker compose up -d --build`
3. 처음 접속 때 Caddy가 HTTPS 인증서를 자동 발급합니다.

엔진은 `-march=native`로 **배포할 서버에서** 빌드됩니다(Dockerfile이 서버에서 빌드하므로 자동).
다른 CPU로 이미지를 옮길 때는 `--build-arg MARCH=x86-64-v3`.

## 설정(환경변수)
| 이름 | 기본 | 설명 |
|---|---|---|
| `SELFPLAY` | 1 | 0 이면 자가대국 끔 |
| `SELFPLAY_MOVETIME` | 250 | 자가대국 수당 ms |
| `SELFPLAY_THREADS` / `SELFPLAY_HASH` | 1 / 64 | 자가대국 엔진 스레드·해시(MB) |
| `SELFPLAY_NET_B` | – | 흑 엔진만 다른 `.nnue` 파일 사용 (넷 비교용) |
| `PLAY_MAX_ENGINES` | 4 | 사람 대국 동시 엔진 수 (꽉 차면 "자리 없음") |
| `PLAY_MAX_PER_IP` | 2 | IP당 동시 대국 |
| `PLAY_IDLE_MIN` | 20 | 무입력 시 연결 종료(분) |
| `ALLOWED_ORIGINS` | https://chess.ericlee.dev | WebSocket 허용 출처 (localhost는 항상 허용) |

## 데이터
`/data`(Docker 볼륨)에 `games.pgn`(수마다 `+0.35/12` 평가·깊이 주석), `games.jsonl`, `stats.json`.
사람 대국은 서버에 저장하지 않습니다.
