# Gemini 기반 UI 문구 다듬기 조사 및 실행 계획

조사일: 2026-08-24

기준 브랜치: `main` (`2bca83f`)

작업 브랜치: `codex/gemini-localization-audit`

## 결론

`gemini-3.7-flash`를 곧바로 전체 문구에 적용하지 않는다. 첫 후보는
`gemini-3.6-flash`의 `minimal` thinking으로 두고, 번역에 특화된
`gemini-3.5-flash-lite`의 `minimal`, 최신 모델인 `gemini-3.7-flash`의
`low`를 같은 표본으로 블라인드 비교한다.

실행 경로는 Gemini Developer API를 우선한다. Google 계정 OAuth를 쓰는
Antigravity CLI도 무료로 세 모델을 선택할 수 있지만, 무료 할당량이 작업량에
따라 차감되는 주간 한도이고 에이전트 문맥 비용도 크다. 따라서 Antigravity는
API 키 없이 표본을 시험하는 보조 경로로는 쓸 수 있어도, 전체 카탈로그를
반복 처리하는 기본 파이프라인에는 덜 적합하다.

## 저장소 현황

- Control Center 카탈로그는 `control-center/src/i18n/`의 10개 언어다.
- 각 카탈로그에는 같은 571개 키가 있다. 한국어는 약 9,730자이고, 가장 긴
  프랑스어 카탈로그는 약 22,073자다.
- `execution.*`이 176개로 가장 큰 문구군이다. 안전성 과시나 능력 제한으로
  읽힐 수 있는 후보도 이 영역에 집중돼 있다.
- `distribution/languages/`에는 배포용 한국어·영어 문구가 각각 6개 더 있다.
- 기존 `scripts/ci/Test-I18n.mjs`는 키, 플레이스홀더, 문자권, 설정 항목,
  한국어 글꼴 글리프를 검사하지만 자연스러움이나 소비자 관점의 문구 정책은
  검사하지 않는다.
- 카탈로그의 `settings.*`가 실제 UI 문구다. 생성된
  `control-center/src/generated/settings.ts`는 직접 수정하지 않는다.

## 첫 검토 후보

아래는 자동 변경 목록이 아니라 표본 평가와 사람 검토를 먼저 거칠 후보들이다.

| 키 | 문제 후보 | 검토 방향 |
| --- | --- | --- |
| `execution.systemDescription` | “구형 MacTray 서비스는 마이그레이션 대상으로만 다룹니다.”가 사용자가 원치 않은 제한처럼 읽힌다. | 두 번째 문장을 삭제하고 현재 기능만 설명한다. |
| `execution.systemActiveDescription` | 정상 상태에서 “검증된”, “광범위하게”를 강조한다. | 현재 프로필이 새 프로세스에 적용 중이라는 사실만 쓴다. |
| `execution.legacyServiceDescription` | 정상 안내에서 “안전하게”와 검증 절차를 과시한다. | 가능한 작업과 실제 선행 조건만 남긴다. |
| `execution.migrationConfirmDescription` | “검증 가능하고 되돌릴 수 있는”이 뒤의 구체적인 백업·롤백 단계와 중복된다. | 추상적 보증을 줄이고 구체적인 단계는 유지한다. |
| `execution.legacyTrayRunningDescription` | “안전하게”, “파일은 삭제하지 않습니다”가 요청하지 않은 방어적 설명일 수 있다. | 먼저 종료해야 한다는 조건만 명확히 전달한다. |
| `distribution/languages/*.systemModeWarning` | “감지하지만 변경하지 않습니다”가 능력 부족으로 읽힐 수 있다. | 이 문구가 표시되는 위치와 사용자 행동을 확인한 뒤 유지·변경을 결정한다. |

반대로 실패·충돌·판별 불가 상태의 설명은 일괄 삭제하면 안 된다.
`execution.systemRunningUnverifiedDescription`,
`execution.legacyServiceForeignDescription`,
`execution.legacyServiceUncertainDescription`,
`execution.legacyTrayUnknownDescription` 등은 동작이 차단된 이유와 다음 행동을
알려 준다. 이런 문구는 “안전”이라는 추상어를 줄일 수는 있어도 상태, 제한,
복구 방법은 보존해야 한다.

## 모델 후보

| 모델 | 공식적으로 확인된 특징 | 이번 작업에서의 위치 |
| --- | --- | --- |
| `gemini-3.7-flash` | 2026-08 GA, 무료 티어 지원, 기본 thinking `medium`, `minimal` 미지원 | 최신 품질과 지시 준수 여부를 보는 도전자. `low`로 시험한다. |
| `gemini-3.6-flash` | GA, 무료 티어 지원, `minimal` 지원, 출시 노트에서 출력 장황함 개선을 명시 | 현재의 첫 후보. 짧은 UI 카피에 필요한 최소 추론으로 시험한다. |
| `gemini-3.5-flash-lite` | GA, 무료 티어 지원, 번역·고처리량 작업에 최적화, 기본 thinking `minimal` | 번역 특화 기준선. 자연스러움과 의미 보존이 상위 모델보다 나은지 확인한다. |

`gemini-2.5-flash`는 2026-10-16 종료 예정이고,
`gemini-3.1-flash-lite`도 후속 모델이 이미 있으므로 새 파이프라인 후보에서
제외한다. `latest` 별칭은 모델이 바뀔 수 있으므로 사용하지 않는다.

공식 문서:

- [Gemini 3.7 Flash](https://ai.google.dev/gemini-api/docs/models/gemini-3.7-flash)
- [Gemini API 출시 노트](https://ai.google.dev/gemini-api/docs/changelog)
- [Gemini thinking 단계](https://ai.google.dev/gemini-api/docs/thinking)
- [Gemini API 가격과 무료 티어](https://ai.google.dev/gemini-api/docs/pricing)
- [Gemini 모델 지원 중단 일정](https://ai.google.dev/gemini-api/docs/deprecations)
- [Gemini 3.5 Flash-Lite 모델 카드](https://deepmind.google/models/model-cards/gemini-3-5-flash-lite/)

## 인증·무료 사용 경로

### 1. Gemini Developer API + API 키 — 우선 경로

- 모델 ID와 thinking 단계를 정확히 고정할 수 있다.
- JSON Schema 구조화 출력을 사용할 수 있다.
- 무료 티어의 입·출력 토큰은 무료지만, 프로젝트별 실제 요청 한도는
  AI Studio에서 확인해야 한다.
- 무료 티어 요청 내용은 Google 제품 개선에 사용될 수 있다. 비공개 문구나
  자격 증명은 입력하지 않는다.
- 새 키를 만든다면 AI Studio의 현재 authorization key 방식을 사용한다.

### 2. Gemini Developer API + 직접 OAuth

OAuth quickstart는 API 키보다 엄격한 접근 제어가 필요할 때 쓰는 같은 Gemini
Developer API 인증 방식이다. 별도의 소비자 무료 할당량을 얻는 경로로 문서화돼
있지 않으므로, 이번 로컬 일회성 도구에는 설정 비용만 늘어난다.

### 3. Antigravity CLI + Google 계정 OAuth — 보조 경로

- 무료 개인 플랜에서 3.7, 3.6, 3.5 Flash를 선택할 수 있다.
- headless 실행, 모델 고정, JSON/JSON Schema 출력이 가능하다.
- 무료 사용자는 작업량 기반 주간 한도를 공유한다. 고정된 요청 수가 아니며
  한도는 변경될 수 있다.
- 일반 모델 API가 아니라 에이전트 제품이므로 짧은 문구에도 작업공간 문맥과
  에이전트 오버헤드가 붙는다.
- API 키를 저장하지 않고 소규모 표본을 비교할 때는 유용하지만, 재현 가능한
  대량 처리와 CI에는 Developer API가 낫다.

예전 Gemini CLI의 개인용 Google 로그인은 2026-06-18에 종료됐다. 오래된
“1,000회/일” 문서는 현재 선택지로 사용하지 않는다.

공식 문서:

- [Gemini API OAuth quickstart](https://ai.google.dev/gemini-api/docs/oauth)
- [Gemini CLI 개인 OAuth 지원 중단](https://developers.google.com/gemini-code-assist/docs/deprecations/code-assist-individuals)
- [Antigravity 모델](https://antigravity.google/docs/models/)
- [Antigravity 플랜과 주간 한도](https://antigravity.google/docs/plans)
- [Antigravity headless/구조화 출력](https://antigravity.google/docs/cli/headless/)

## 평가 설계

### 표본

약 30개 키를 다음 세 부류에서 같은 수로 뽑는다.

1. 의미는 맞지만 번역투나 딱딱한 리듬이 있는 일반 한국어 문구
2. 정상 상태인데 검증·안전·복구를 자진해서 강조하는 문구
3. 실제 실패·충돌·판별 불가 상태라 제한과 대응 방법을 반드시 보존해야 하는 문구

각 항목에는 키, 현재 한국어·영어, 표시 화면/상태, 보존할 제품 용어와
플레이스홀더를 함께 제공한다. 모델 이름을 가린 결과를 사람이 비교한다.

### 동일 조건

- `gemini-3.5-flash-lite`: `minimal`
- `gemini-3.6-flash`: `minimal`
- `gemini-3.7-flash`: `low`
- 모델별 2회 실행으로 결과 흔들림도 본다.
- 검색·도구·대화 이력 없이 한 번의 구조화 출력 요청으로 실행한다.
- sampling 파라미터는 3.6 이후 지원 정책에 맞춰 사용하지 않는다.

### 필수 통과 조건

- 키와 플레이스홀더가 100% 보존된다.
- `MacType`, `Control Center`, `MacTray`, `AppInit`, `UAC`, 프로필 등
  보호 용어와 `CONTEXT.md`의 도메인 용어가 바뀌지 않는다.
- 동작 가능 여부, 상태, 선행 조건, 실패 원인에 새 주장을 더하거나 기존 사실을
  빼지 않는다.
- 정상 상태의 추상적 안전 과시는 줄이되, 실패 상태의 원인과 복구 행동은 남긴다.
- 한국어 존댓말과 UI 문체를 유지하고, 한 문구의 변경률은 가능하면 30% 이내로
  제한한다. 50%를 넘으면 자동 탈락시킨다.

### 점수

- 한국어 자연스러움: 30
- 의미·동작 보존: 30
- 소비자 관점의 간결함: 20
- 불필요한 안전 과시/능력 제한 제거: 10
- 반복 실행 안정성: 10

필수 조건을 모두 통과한 모델 중 총점이 높은 모델을 채택한다. 차이가 작으면
더 오래된 모델이 아니라 더 적게 고치는 모델을 택한다.

## 적용 절차

1. 모델이 파일을 직접 수정하지 않고 `key`, `action`, `before`, `after`,
   `reason`, `preserved_tokens`를 담은 제안 JSON만 출력한다.
2. 한국어 571개는 자연스러움 후보를 찾되, 변경이 필요 없는 키는 출력하지 않는다.
3. 안전·능력 표현은 언어 독립적인 키 단위 정책표에서 먼저
   `keep`, `rewrite`, `remove_clause`로 확정한다.
4. 승인된 의미 변경을 한국어와 영어에 먼저 적용한다.
5. 같은 의미 변경만 나머지 8개 언어에 번역한다. 각 언어 모델이 2·3번의
   삭제 여부를 독립적으로 판단하게 두지 않는다.
6. 정확한 키 집합, 플레이스홀더, 보호 용어, JSON 형식을 로컬 스크립트로
   재검증한 뒤에만 카탈로그에 반영한다.
7. `pnpm test:i18n`, `pnpm lint`, `pnpm build`를 실행한다. 새 한글 글리프가
   생기면 기존 생성 스크립트로 글꼴 서브셋을 갱신한다.
8. 한국어, 영어, 긴 라틴계 언어 하나, 아랍어 RTL에서 갤러리 스크린샷을 비교해
   줄바꿈과 버튼 폭을 확인한다.

## 다음 실행 단위

다음 단계에서는 표본·평가 스키마·읽기 전용 실행 스크립트만 먼저 만든다.
현재 환경에는 `GEMINI_API_KEY`와 Antigravity CLI가 없으므로 실제 A/B/C 호출은
인증 경로가 준비된 뒤 실행한다. 평가 결과가 나오기 전에는 카탈로그를 변경하지
않는다.
