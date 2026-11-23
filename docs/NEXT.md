# 다음 할 일

## 프로젝트 보드

작업 현황은 [Week 13 Project](https://github.com/users/kuk-hwan/projects/12)에서 확인한다.

- [보드](https://github.com/users/kuk-hwan/projects/12/views/1): Status별로 Todo, In Progress,
  In Review, Done을 본다.
- [사람별](https://github.com/users/kuk-hwan/projects/12/views/2): 담당자로 묶고 숫자 필드
  `차례`를 오름차순으로 정렬한다. Done은 숨긴다.

### 다음 할 일 찾는 법

1. **사람별** 보기에서 내 이름 아래의 **Todo**를 찾는다.
2. `차례`가 가장 작고, 이슈의 **Blocked by**가 모두 닫힌 일을 고른다.
3. 시작할 때 **In Progress**, PR로 검토받을 때 **In Review**로 옮긴다.
   같은 차례의 일도 각자의 막힘 관계를 확인한 뒤 시작한다.

### 담당과 차례

괄호 숫자는 막힘 관계로 계산한 가장 이른 시작 차례다. 숫자는 이슈 번호가 아니다.

| 갈래 | 담당 | 이슈(차례) |
|---|---|---|
| A 기반·검사 | [@kuk-hwan](https://github.com/kuk-hwan) | [#3](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/3)(1) → [#4](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/4)(2) → [#6](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/6)(3) → [#14](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/14)(7) |
| B HTTP·프록시 | [@Pancheble](https://github.com/Pancheble) | [#5](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/5)(3) → [#10](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/10)(4) → [#11](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/11)(5) → [#13](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/13)(6) |
| C 동시성·데모·문서 | [@Jongeume](https://github.com/Jongeume) | [#7](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/7)(3) → [#8](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/8)(4) → [#9](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/9)(5) → [#12](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/12)(6) → [#15](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/15)(7) → [#16](https://github.com/kuk-hwan/WEEK13-Kkilkkilson/issues/16)(8) |

### 상태 자동화

이 저장소의 새 이슈는 보드에 자동 추가되고 Todo로 시작한다. 연결 PR이 생기면
In Review, PR이 머지되거나 이슈가 닫히면 Done으로 옮긴다. 보드의 이슈 자동 종료는
꺼 두어 상태를 옮기는 작업만으로 이슈 자체가 닫히지 않게 한다.

후속 구현은 #4(서버가 뜨고 첫 응답)를 시작점으로 삼는다. #4가 머지되면
#5(HTTP), #6(정적 파일), #7(동시성) 갈래를 담당자별로 진행한다. 실제 시작 전에는
해당 이슈의 `Blocked by`가 모두 닫혔는지 확인한다.

## 전체 작업 방식

작업 방식은 하위 세션이다(AGENTS.md "작업 방식"). 메인 세션은 하위 세션을 띄우고 결과를 확인한다.

1. **첫 안**: `docs/idea.md`에 무엇을 만들지 몇 줄 쓴다.
2. **그릴링**: `/grill-with-docs`로 첫 안을 캐묻고 용어집(`GLOSSARY.md`)과 ADR(`docs/adr/`)을 만든다. 아이디어부터 검증하고 싶으면 그 전에 gstack `/office-hours`.
3. **명세**: `/to-spec`으로 PRD를 만든다. 끝나면 `area:*`, `stage:*` 라벨과 단계별 마일스톤을 만들고, AGENTS.md "Git 규칙"의 범위 이름을 채운다.
4. **티켓**: `/to-tickets`로 이슈를 만든다. 이슈마다 size 라벨을 붙인다. 첫 이슈는 계약이다. 이 이슈에서 포맷 검사, 타입 검사, 테스트를 돌리는 CI도 만들고, dev 브랜치 규칙의 필수 검사에 더한다(관리자 kuk-hwan이 건다).
5. **이슈 하나씩**: 메인 세션에 "이슈 #<번호> 하위 세션으로 해 줘".
6. **릴리즈**: 마일스톤의 이슈가 다 닫히면 "릴리즈 해 줘".

2~4는 사람이 답해야 하니 말을 걸 수 있는 하위 세션 하나에서 이어서 한다.
