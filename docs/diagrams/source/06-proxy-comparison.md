| Reverse Proxy | Forward Proxy |
|---|---|
| 경로 접두사를 기준으로 라우팅 | 요청 URI를 기준으로 목적지 결정 |
| 라운드 로빈으로 백엔드 분산 | HTTP 전달 및 HTTPS CONNECT 터널 |
| 백엔드 연결 실패 502, 시간 초과 504 | 허용된 클라이언트만 이용 가능 |

