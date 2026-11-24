# Week 13 · Mini Webserver

C11/POSIX 웹 서버 팀 과제다. 명세는 [`docs/spec.md`](docs/spec.md), 모듈 계약은
[`include/mini_webserver.h`](include/mini_webserver.h)와 [`docs/CONTRACT.md`](docs/CONTRACT.md)에 있다.

현재는 **이슈 #6의 정적 파일 서빙 단계**다. HTTP 파서와 스레드 풀을 사용하며,
루트 아래 파일·디렉터리 index를 GET/HEAD로 응답한다. 쿼리와 인코딩 경로를 지원하고,
심볼릭 링크·경로 탈출·비정규 파일은 거부한다. 정적 파일의 POST는 405다.
동적 엔드포인트와 단일 백엔드 Reverse Proxy도 동작한다. 정적·동적 응답 뒤에는 연결을
닫고, 현재 연결 유지는 프록시 요청에서만 지원한다. 안전한 종료(#8)와 나머지 요청의
keep-alive(#9)는 후속 단계다. 참고 구현의 소스는 복사하지 않았다.

CI의 포맷 기준은 clang-format 18이다. 다른 버전을 쓴다면
`make CLANG_FORMAT=clang-format-18 format-check`로 같은 검사기를 선택할 수 있다.

## 준비와 검사

Windows는 WSL2 Ubuntu에서 실행한다. Linux는 C 컴파일러·make·Python 3·clang-format을 준비한다.
macOS는 Xcode Command Line Tools, Python 3, clang-format을 준비한다.

```sh
# Ubuntu / WSL2
sudo apt-get update
sudo apt-get install -y build-essential python3 clang-format-18

git clone https://github.com/kuk-hwan/WEEK13-Kkilkkilson.git
cd WEEK13-Kkilkkilson
git config core.hooksPath .githooks

make
make test
make sanitize-test
make CLANG_FORMAT=clang-format-18 format-check
```

## 서버 실행

```sh
./build/mini-webserver --host 127.0.0.1 --port 8080 --root public
```

다른 터미널에서 응답을 확인하거나 브라우저에서 `http://127.0.0.1:8080/`을 연다.

```sh
curl -i http://127.0.0.1:8080/
curl -I http://127.0.0.1:8080/index.html
curl -i 'http://127.0.0.1:8080/index.html?v=2'
curl -i -X POST http://127.0.0.1:8080/index.html # 405 + Allow: GET, HEAD
```

200 상태와 `Content-Type: text/html; charset=utf-8`, 파일 바이트 수와 같은
`Content-Length`, `Connection: close`, `Date`, `Server` 헤더 및 HTML 본문을 받는다.
루트 아래에 `docs/index.html`을 두면 `/docs`와 `/docs/`로 받을 수 있다.
파일 확장자에 맞는 MIME을 보내며, index 없는 디렉터리는 403, 없는 파일은 404다.

기본값은 호스트 `127.0.0.1`, 포트 `8080`, 루트 `public`이다. 포트는 1~65535,
호스트는 IPv4 주소, 루트는 존재하는 디렉터리여야 한다. 잘못된 인자는 종료 코드 2,
포트 충돌 등의 실행 오류는 1이다. 이번 단계는 Ctrl-C로 종료하며 정상 종료 절차는 #8에서 구현한다.

## 요청이 흐르는 코드

```text
main (src/main.c)
  → mw_config_parse (src/config.c): 옵션 검증, 절대 루트 경로와 디렉터리 FD 확보
  → mw_server_run (src/server.c)
      → mw_net_listen (src/net.c): socket → SO_REUSEADDR → bind → listen
      → accept → mw_pool_submit (src/pool.c): 연결 큐와 작업자 스레드
      → mw_app_handle_connection → mw_http_read_request (src/http.c): 머리·본문 수신과 검증
      → 라우트 일치: mw_proxy_serve (src/proxy.c)
      → 로컬 동적 경로: serve_dynamic (src/server.c)
      → 그 밖: mw_static_serve (src/static.c)
          → mw_static_decode_path: 쿼리 제거, 퍼센트 디코딩, 경로 요소 검사
          → open_file: 루트 FD부터 요소별 openat + O_NOFOLLOW, 디렉터리 index 확인
          → mw_http_send_response: 파일 크기와 MIME을 헤더로 전송
          → read → mw_net_write_all: 파일을 8 KiB씩 전송(HEAD는 본문 생략)
      → 작업자가 연결을 닫음(프록시의 keep-alive는 다음 요청을 처리)
```

설정·리스너 준비는 서버 시작 때 한 번만 한다. 작업자가 연결별 요청 버퍼를 가지고
요청을 읽는다. 첫 바이트부터 5초의 요청 deadline을 적용하며, 추가 바이트가 도착해도
연장하지 않는다. 파일 전송도 `mw_net_write_all`의 부분 송신 처리와 제한 시간을 사용한다.

정적 파일 경로는 각 요소를 검사하고 루트 FD에 상대적으로 연다. `fstatat`으로 심볼릭
링크·비정규 파일을 거부한 뒤에도 `O_NOFOLLOW`와 열린 FD의 `fstat` 검사를 유지한다.
파일 전체를 메모리에 올리지 않으며, HEAD도 GET과 같은 Content-Length를 보낸다.

| FD | 용도 | 수명 |
|---|---|---|
| `config->root_fd` | 루트 디렉터리를 기준으로 파일 열기 | 설정이 살아 있는 동안 유지 |
| `listener` | 새 연결을 받는 접수 소켓 | accept 루프 동안 유지 |
| `client` | 요청·응답 송수신 | 작업자가 연결 처리를 마치면 닫음 |
| 정적 파일 FD | 일반 파일 내용 읽기 | 헤더·본문 전송 뒤 닫음 |
| 중간 디렉터리 FD | 다음 경로 요소 열기 | 다음 요소를 연 뒤 닫음 |

## 테스트 범위

빌드 결과는 `build/mini-webserver`, C 테스트는 `build/test-unit`이다. C11과
`-Wall -Wextra -Werror -pedantic`으로 컴파일 경고를 오류로 처리한다.
`make sanitize-test`는 분리된 `build/sanitize/`에서 ASan·UBSan으로 같은 테스트를 실행한다.
`make test`는 C 단위 테스트 25개와 Python 실제 TCP 통합 테스트 46개를 실행한다.
통합 테스트는 임시 디렉터리·빈 포트·실제 서버 프로세스를 사용하며 끝나면 정리한다.

- 설정 기본값·유효/잘못된 옵션·설정 자원 해제
- 작은 송신 버퍼에서도 전체 쓰기, 읽기·쓰기의 절대 deadline, EOF
- UTF-8/빈 파일/4MiB 파일의 정확한 본문과 HTTP 헤더
- 실제 실행 포트를 기준으로 `Server: server-<포트>` 전체 일치 검사. 두 서버를 다른 포트에
  동시에 띄워 각각의 이름을 확인하고, 403·404·408·431 오류 응답도 같은 기준으로 검사
- HTTP 파서의 정상·오류·한도, 나뉘어 온 요청 머리, LF/HTTP 1.0
- 포트 충돌(1)·인자 오류(2), 같은 포트로 즉시 재시작
- 클라이언트 강제 종료 후에도 다음 요청 처리, 머리 크기 제한과 복구
- 요청 중간에 바이트가 더 와도 첫 바이트 기준 5초 deadline 유지
- MIME 표 전체·대문자 확장자·기본값, 인코딩·한글 경로·쿼리, 잘못된 경로의 400/403
- 디렉터리 index, 읽기 권한 거부·심볼릭 링크·FIFO·Unix 소켓의 403, 없는 파일의 404
- 정적 HEAD·POST 405/Allow, 하위 경로의 5 MiB 이상 이진 파일 바이트 일치
- 동적 whoami·이진 echo, 단일 백엔드 전달·헤더 재작성·프록시 연결 유지, 스레드 풀·큐 초과

`make sanitize-test`에서도 **단위 테스트와 서버 실행 파일 모두** 계측해 같은 통합 테스트를
실행한다. 테스트 종료는 SIGTERM으로 하며, 정상 종료 시 누수·남은 FD 검증은 #8/#14에서
추가한다. 현재 검사로 완성된 종료 모델이나 전체 HTTP 기능을 검증했다고 주장하지 않는다.

```sh
make format                         # clang-format 적용
make test TEST_ARGS=--self-test-failure # 의도적 실패: 0이 아닌 종료 코드가 정상
make clean
```

`make test`는 C 테스트 하나라도 실패하면 실패한다. 테스트 함수는 `tests/test.h`의 CHECK를
사용하고 `tests/test_main.c`의 목록에 등록한다. 새 `tests/test_*.c`와 `src/*.c`는 Makefile이 찾는다.
`src/main.c`는 테스트 링크에서 제외된다. 단위·프로세스 테스트 경계는 명세를 따른다.

## CI와 협업

- `ci-ubuntu`: 포맷 검사·빌드·단위/통합 테스트·ASan/UBSan.
- `ci-macos`: 빌드·단위/통합 테스트.
- `pr-title`: 기존 PR 제목 규칙 검사.

`dev`로 PR을 보내고 사람의 리뷰·머지 결정을 기다린다. 관리자 설정의 필수 검사에는
위 job 이름을 사용한다. 작업 현황은
[Week 13 Project](https://github.com/users/kuk-hwan/projects/12), 담당과 다음 작업을 찾는 방법은
[`docs/NEXT.md`](docs/NEXT.md)를 확인한다. Valgrind·부하 측정 타깃은 3단계에서 추가한다.
