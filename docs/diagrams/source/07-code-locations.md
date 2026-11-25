| 구현 | 코드 |
|---|---|
| HTTP 파싱 | `src/http.c` · `mw_http_read_request` |
| Keep-Alive | `src/server.c` · `handle_connection`, `wait_for_next_request` |
| Reverse Proxy | `src/proxy.c` · `mw_proxy_serve` |
| Forward Proxy / CONNECT | `src/proxy.c` · `mw_proxy_forward`, `mw_proxy_tunnel` |

