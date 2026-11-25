| 주요 기능 | 구현 내용 |
|---|---|
| **웹 서버** | 정적 파일 제공, 동적 API (`/api/whoami`, `/api/echo`) |
| **Reverse Proxy** | 경로 기반 라우팅, 백엔드 라운드 로빈 분산 |
| **Forward Proxy** | HTTP 요청 전달, CONNECT 터널 |
| **HTTP 연결 관리** | HTTP/1.0·1.1, Keep-Alive, 파이프라이닝 |
| **서버 운영** | 스레드 풀, 과부하 제어, 안전 종료, 접근 로그 |

