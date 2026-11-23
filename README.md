# Week 13 · Mini Webserver

C11/POSIX 웹 서버 팀 과제다. 명세는 [`docs/spec.md`](docs/spec.md), 모듈 계약은
[`include/mini_webserver.h`](include/mini_webserver.h)와 [`docs/CONTRACT.md`](docs/CONTRACT.md)에 있다.

현재는 **이슈 #4의 첫 HTTP 응답 단계**다. 단일 스레드로 연결을 하나씩 받고,
`GET /`에 루트의 `index.html`을 응답한 뒤 연결을 닫는다. 참고 구현의 소스는 복사하지 않았다.

전체 HTTP 파서(#5), 범용 정적 파일 서빙(#6), 스레드 풀(#7), 안전한 종료(#8),
keep-alive(#9), Reverse Proxy(#11 이후)는 후속 단계다. 지금은 `GET /` 이외의
요청(HEAD·POST·다른 경로·쿼리 포함)에 404를 반환한다.

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
```

200 상태와 `Content-Type: text/html; charset=utf-8`, 파일 바이트 수와 같은
`Content-Length`, `Connection: close`, `Date`, `Server` 헤더 및 HTML 본문을 받는다.
기본값은 호스트 `127.0.0.1`, 포트 `8080`, 루트 `public`이다. 포트는 1~65535,
호스트는 IPv4 주소, 루트는 존재하는 디렉터리여야 한다. 잘못된 인자는 종료 코드 2,
포트 충돌 등의 실행 오류는 1이다. 이번 단계는 Ctrl-C로 종료하며 정상 종료 절차는 #8에서 구현한다.

## 요청이 흐르는 코드

```text
main (src/main.c)
  → mw_config_parse (src/config.c): 옵션 검증, 절대 루트 경로와 디렉터리 FD 확보
  → mw_server_run (src/server.c)
      → mw_net_listen (src/net.c): socket → SO_REUSEADDR → bind → listen
      → accept: 연결 하나 수락
      → handle_connection → mw_net_read: 머리 끝의 빈 줄까지 나누어 수신
      → is_root_get → serve_index: GET /이면 index.html 열기
      → send_head → mw_net_write_all: HTTP 헤더 전송
      → read → mw_net_write_all: 파일을 8KiB씩 전송
      → close: 연결을 닫고 다음 accept로 돌아가기
```

현재 요청 분기·응답 생성은 server.c의 비공개 초기 구현이다. 공통 헤더의 HTTP·정적 파일
인터페이스를 성공 스텁으로 채우지 않았으며 #5/#6에서 해당 모듈로 연결한다.

## 테스트 범위

빌드 결과는 `build/mini-webserver`, C 테스트는 `build/test-unit`이다. C11과
`-Wall -Wextra -Werror -pedantic`으로 컴파일 경고를 오류로 처리한다.
`make sanitize-test`는 분리된 `build/sanitize/`에서 ASan·UBSan으로 같은 테스트를 실행한다.
`make test`는 C 단위 테스트 7개와 Python 실제 TCP 통합 테스트 12개를 실행한다.
통합 테스트는 임시 디렉터리·빈 포트·실제 서버 프로세스를 사용하며 끝나면 정리한다.

- 설정 기본값·유효/잘못된 옵션·설정 자원 해제
- 작은 송신 버퍼에서도 전체 쓰기, 읽기·쓰기의 절대 deadline, EOF
- UTF-8/빈 파일/4MiB 파일의 정확한 본문과 HTTP 헤더
- 나뉘어 온 요청 머리, LF/HTTP 1.0, 초기 단계의 404
- 포트 충돌(1)·인자 오류(2), 같은 포트로 즉시 재시작
- 클라이언트 강제 종료 후에도 다음 요청 처리, 머리 크기 제한과 복구
- 요청 중간에 바이트가 더 와도 첫 바이트 기준 5초 deadline 유지
- 없거나 심볼릭 링크인 index.html에 403

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
