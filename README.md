# Week 13 · Mini Webserver

C11/POSIX 웹 서버 팀 과제다. 명세는 [`docs/spec.md`](docs/spec.md), 모듈 계약은
[`include/mini_webserver.h`](include/mini_webserver.h)와 [`docs/CONTRACT.md`](docs/CONTRACT.md)에 있다.

현재는 **이슈 #3의 공통 계약·빌드·검사 기반**이다. 실행 파일은 준비 상태를 출력하고 종료한다.
소켓 리스너·HTTP 처리·프록시는 후속 이슈에서 구현한다. 아직 브라우저용 서버가 실행되지는 않는다.
참고 구현의 소스는 복사하지 않았다.

CI의 포맷 기준은 clang-format 18이다. 다른 버전을 쓴다면
`make CLANG_FORMAT=clang-format-18 format-check`로 같은 검사기를 선택할 수 있다.

## 준비와 검사

Windows는 WSL2 Ubuntu에서 실행한다. Linux는 C 컴파일러·make·clang-format을 준비한다.
macOS는 Xcode Command Line Tools와 clang-format을 준비한다.

```sh
# Ubuntu / WSL2
sudo apt-get update
sudo apt-get install -y build-essential clang-format

git clone https://github.com/kuk-hwan/WEEK13-Kkilkkilson.git
cd WEEK13-Kkilkkilson
git config core.hooksPath .githooks

make
make test
make sanitize-test
make format-check
```

빌드 결과는 `build/mini-webserver`, C 테스트는 `build/test-unit`이다. C11과
`-Wall -Wextra -Werror -pedantic`으로 컴파일 경고를 오류로 처리한다.
`make sanitize-test`는 분리된 `build/sanitize/`에서 ASan·UBSan으로 같은 테스트를 실행한다.
현재 테스트는 명세의 공개 계약과 러너를 검사하며 HTTP 기능 테스트를 통과했다고 주장하지 않는다.

```sh
make format                         # clang-format 적용
make test TEST_ARGS=--self-test-failure # 의도적 실패: 0이 아닌 종료 코드가 정상
make clean
```

`make test`는 C 테스트 하나라도 실패하면 실패한다. 테스트 함수는 `tests/test.h`의 CHECK를
사용하고 `tests/test_main.c`의 목록에 등록한다. 새 `tests/test_*.c`와 `src/*.c`는 Makefile이 찾는다.
`src/main.c`는 테스트 링크에서 제외된다. 단위·프로세스 테스트 경계는 명세를 따른다.

## CI와 협업

- `ci-ubuntu`: 포맷 검사·빌드·단위 테스트·ASan/UBSan.
- `ci-macos`: 빌드·단위 테스트.
- `pr-title`: 기존 PR 제목 규칙 검사.

`dev`로 PR을 보내고 사람의 리뷰·머지 결정을 기다린다. 관리자 설정의 필수 검사에는
위 job 이름을 사용한다. 작업 현황은
[Week 13 Project](https://github.com/users/kuk-hwan/projects/12), 담당과 다음 작업을 찾는 방법은
[`docs/NEXT.md`](docs/NEXT.md)를 확인한다. Valgrind·부하 측정 타깃은 3단계에서 추가한다.
