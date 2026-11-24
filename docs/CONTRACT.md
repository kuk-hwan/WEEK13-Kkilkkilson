# 모듈 계약

공통 인터페이스는 [`include/mini_webserver.h`](../include/mini_webserver.h) 하나에 둔다.
수치의 원본은 [`spec.md`](spec.md)이며, 이 문서는 소유권과 호출 규칙을 설명한다.
이슈 #3에서는 선언과 검사 기반만 만든다. HTTP 파서나 서버 기능의 성공 스텁은 두지 않는다.

## 한도

| 항목 | 값 | 헤더 상수 |
|---|---:|---|
| 요청 줄 | 8 KiB | `MW_MAX_REQUEST_LINE` |
| 요청 머리 | 16 KiB | `MW_MAX_HEADER_BYTES` |
| 헤더 수 | 64 | `MW_MAX_HEADERS` |
| 요청 본문 | 1 MiB | `MW_MAX_BODY_BYTES` |
| 백엔드 수신 | 8 MiB | `MW_MAX_UPSTREAM_BYTES` |
| 라우트 / 라우트당 백엔드 | 16 / 8 | `MW_MAX_ROUTES` / `MW_MAX_BACKENDS` |
| 작업자 | 1~128, 기본 8 | `MW_MIN_WORKERS` / `MW_MAX_WORKERS` / `MW_DEFAULT_WORKERS` |
| 연결 큐 | 1~4096, 기본 64 | `MW_MIN_QUEUE_CAPACITY` / `MW_MAX_QUEUE_CAPACITY` / `MW_DEFAULT_QUEUE_CAPACITY` |
| 서버 이름 | 63바이트 | `MW_MAX_SERVER_NAME` |
| 요청 / 유휴 / 백엔드 시간 | 5000 / 5000 / 3000 ms | `MW_*_TIMEOUT_MS` |
| 연결당 요청 | 100 | `MW_MAX_REQUESTS_PER_CONNECTION` |

길이는 모두 바이트 수다. 문자열 배열은 끝의 NUL을 위한 한 바이트를 추가한다.
주소 조회용 백엔드 호스트 저장 공간은 255바이트와 NUL이다. 리스너는 명세대로 IPv4다.

## 바이트와 소유권

- `mw_slice_t`는 길이가 있는 **빌린 바이트 뷰**다. NUL 종료 문자열이라고 가정하거나
  무조건 `strlen`을 호출하지 않는다. 본문은 NUL과 이진 데이터를 포함할 수 있다.
- `mw_buffer_t`는 할당된 저장 공간이다. `length <= capacity`이며 할당 모듈이 해제한다.
- 요청 파서는 입력을 수정하거나 소켓 I/O를 하지 않는다. 요청의 메서드·대상·경로·쿼리·헤더는
  입력 버퍼를 빌린다. 본문은 수신 계층이 채운다. 현재 요청 처리와 전송이 끝나기 전에
  입력 버퍼를 이동하거나 덮어쓰지 않는다.
- 헤더 목록은 중복과 순서를 보존한다. 요청의 잘못된 중복은 파서가 검증하고,
  `Set-Cookie` 같은 응답 중복은 전송 계층이 합치지 않는다.
- 백엔드 응답 파서는 완성된 수신 바이트를 받는다. 헤더는 원본을 빌리고 본문은 복사 또는
  chunked 해제해 소유한다. 전달 후 `mw_http_upstream_destroy`를 호출한다. 오류로 끝나도
  해제 함수를 안전하게 호출할 수 있도록 출력은 초기화한다.
- 설정 모듈이 절대 루트 경로·루트 FD·라우트 잠금을 만들고 `mw_config_destroy`에서 정리한다.
  실행 중에는 설정의 주소와 수명을 유지한다. 설정의 큰 구조체는 힙에 두어도 된다.
- 실행 중 호스트·포트·경로·백엔드 목록 등 검증된 설정값은 변경하지 않는다. 예외는 각
  라우트의 `next_backend` 선택 카운터이며 해당 `selection_lock` 아래에서만 변경한다.
  따라서 서버와 `mw_runtime_t.config`는 `mw_config_t *`를 빌린다. app은
  `runtime->config->routes`를 그대로 라우트 선택 함수에 전달할 수 있다. net·static 등
  설정만 읽는 함수는 `const mw_config_t *`로 받으며 const cast는 필요하지 않다.

## 순수 함수 경계

### 요청 머리

`mw_http_parse_request_head(bytes, length, request)`는 결과를 세 가지로 구분한다.

| 결과 | consumed | status | 호출자의 다음 일 |
|---|---:|---:|---|
| `MW_PARSE_COMPLETE` | 머리 끝까지 소비한 바이트 | 0 | Content-Length에 따라 본문 수신 |
| `MW_PARSE_NEED_MORE` | 0 | 0 | 기존 바이트를 유지하고 더 수신 |
| `MW_PARSE_ERROR` | 0 | HTTP 오류 코드 | 오류 응답 후 연결 종료 |

머리는 요청 줄과 헤더 끝의 빈 줄까지다. 뒤에 있는 본문과 파이프라이닝 요청은 버리지 않는다.
완성 결과에서만 출력 요청을 사용한다. 경로와 쿼리는 `target`의 원문을 나눈 뷰이며,
라우팅 전에는 디코딩하지 않는다. 헤더 이름 비교는 대소문자를 구분하지 않는다.

### 정적 파일

- `mw_static_mime_type(path)`는 정적 수명의 Content-Type 문자열을 반환한다.
- `mw_static_decode_path(path, output, capacity)`는 안전한 상대 경로를 출력한다.
  성공은 0, 잘못된 인코딩·부족한 출력 공간은 400, 허용하지 않는 경로는 403이다.
  호출자는 최대 요청 경로와 NUL이 들어갈 공간을 제공한다. 실제 파일 접근 검사는 별도다.

### 라우트와 백엔드

- `mw_proxy_match_route(routes, count, raw_path)`는 빌린 라우트 포인터 또는 NULL이다.
  쿼리를 제외한 원문 경로를 사용하고, 최장 일치와 접두사 경계를 검사한다.
- `mw_proxy_next_backend(route)`는 다음 백엔드를 반환한다. 설정 검증이 끝난 라우트에는
  백엔드가 적어도 하나 있다. 선택 카운터의 갱신만 잠그며 I/O 중에는 잠그지 않는다.

### 백엔드 응답

`mw_http_parse_upstream_response(bytes, length, head_only, response)`는 0 또는 502다.
상태·헤더·본문을 분리하고 chunked는 해제한다. HEAD와 204·304는 본문이 없다.
수신 한도와 연결·시간 오류는 전달 계층이 확인한다. 시간 초과를 파싱 오류로 바꾸지 않는다.

## 응답 헤더

- `mw_http_set_server_name(name)`은 `Server` 헤더 값이다. 서버가 시작할 때 작업자를 만들기 전에
  한 번만 부르고, 이후에는 읽기만 한다.

## 실행 계층과 FD

- `mw_net_listen`과 `mw_net_connect`가 반환한 FD는 호출자가 소유한다.
- `mw_pool_submit`이 true일 때만 FD의 소유권이 큐로 넘어간다. false이면 접수한 쪽이 정리한다.
- 작업자는 꺼낸 FD를 `mw_app_handle_connection`에 빌려주고 반환 뒤 닫는다.
- `app`, `static`, `proxy`, `http`는 빌린 클라이언트 FD를 닫지 않는다.
- `proxy`가 만든 백엔드 FD와 수신 버퍼는 `proxy`가 정상·오류 경로 모두에서 정리한다.
- `mw_pool_stop`은 새 작업을 막고 대기 연결을 정리하며 진행 중 작업의 종료를 기다린다.
  `mw_pool_destroy`는 stop된 풀의 내부 자원을 해제한다.
- `mw_runtime_t.stopping`은 종료 요청을 공유한다. 시그널 핸들러는 여기의 복잡한 작업을
  하지 않고 명세의 self-pipe 경로로 메인 루프를 깨운다.

## 결과와 시간

- 순수 파싱·검사 함수의 오류는 HTTP 상태 코드다. OS I/O 함수는 성공 값 또는 -1과 errno다.
- `mw_config_parse`는 정상 0, 인자·설정 오류 2다. `mw_server_run`은 정상 0, 실행 오류 1이다.
- `mw_deadline_t`는 CLOCK_MONOTONIC 기준 **절대 밀리초 시각**이다. 읽을 때마다 기간을
  초기화하지 않는다. 유휴 대기와 요청 첫 바이트 이후의 5초는 구분해서 관리한다.
- `mw_net_read`는 짧게 읽을 수 있고 0은 EOF다. `mw_net_write_all`은 전부 쓰거나 -1이다.
- net 읽기·쓰기 함수는 deadline을 지키기 위해 소켓을 O_NONBLOCK으로 바꾼다.
  호출 후 소켓이 블로킹 모드라고 가정하지 않는다. poll과 재시도도 같은 절대 deadline을 쓴다.
- 요청·응답 뷰의 수명, 출력 공간, 204/304 등의 HTTP 예외는 후속 구현에서도 지켜야 한다.

## 후속 구현

이 선언을 바꿀 때는 먼저 `docs/spec.md`와 충돌하는지 확인하고 같은 PR에서 호출자·문서를
함께 맞춘다. 다른 모듈을 성공 스텁으로 대체해 테스트를 통과시키지 않는다. 테스트는 명세가
정한 순수 함수·프로세스 경계에서 외부 동작을 확인하며 내부 함수 호출 순서를 고정하지 않는다.
