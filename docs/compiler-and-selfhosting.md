# 컴파일러와 FG 셀프 호스팅

`forge app.fg`는 현재 디렉터리에 `app` 실행 파일을 만듭니다. Windows 기본 출력은 `app.exe`입니다. `-o`로 다른 경로를 선택할 수 있습니다. 소스와 같은 파일을 가리키는 출력 경로는 거절합니다.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFORGE_ENABLE_GPU=OFF
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --build build --target forge-selfhost-verify
cmake --install build --prefix /tmp/forge-install
/tmp/forge-install/bin/forge examples/hello.fg
./hello
/tmp/forge-install/bin/forge-fg examples/control_flow.fg
./control_flow
```

설치 디렉터리에는 `bin/forge`, `bin/forge-fg`, `include/`, `lib/`, FG 컴파일러 원본이 포함됩니다. 설치 위치를 실행 파일에서 찾으므로 다른 디렉터리에서도 사용할 수 있습니다. 빌드 트리가 기본 `build/`가 아니라면 `--lib-dir`로 라이브러리 디렉터리를 지정합니다. `CC`는 단일 실행 파일 경로입니다. `CC="cc -O2"`처럼 인수를 포함하지 마세요.

## 셀프 호스팅 검증 범위

C로 작성한 stage0가 `bootstrap/compiler.fg`를 stage1으로 컴파일합니다. stage1이 같은 FG 원본을 C로 변환하고 C 도구 체인이 stage2 실행 파일을 만듭니다. stage2가 만든 stage3 컴파일러의 출력과 stage2 출력이 바이트 단위로 같은지 `forge-selfhost-verify`가 확인합니다. FG 컴파일러의 파싱, 코드 생성, 옵션 처리, 파일 처리, 네이티브 빌드 호출은 FG로 구현되어 있습니다. 최종 기계어 생성과 런타임은 C 도구 체인과 C 라이브러리를 사용합니다.

stage2/설치된 `forge-fg`도 기본적으로 실행 파일을 만듭니다. `--emit-c` 또는 `.c` 출력 경로를 사용하면 C만 출력합니다. `--cc`, `--forge-root`, `--lib-dir`, `--keep-temp`, `--help`, `--version`을 지원합니다. C 컴파일러 호출은 셸을 거치지 않는 argv 방식이며, 컴파일러 실패 코드를 전달하고 임시 파일을 정리합니다.

FG 컴파일러는 `int`, `bool`, `string`, `void` 함수와 초기화된 타입 명시 변수, 정수 `match`, 반복문, 조건문, 독립 블록, `strings/fs/os/io` 내장 함수, `native main`과 순차 실행 `process main`을 지원합니다. `bool`은 stage0와 같은 C `int`로 생성하며 `true`/`false`는 1/0, `&&`/`||`는 단락 평가를 유지합니다. 컴파일러 자체의 판별 함수와 문자열 탐색 상태도 이 기능을 사용합니다. `println`은 문자열 변수·함수 반환값·상수와 여러 인수를 지원하며, 인수를 왼쪽부터 한 번씩 평가하고 구분자를 추가하지 않습니다. 출력 함수는 C11 `_Generic`으로 표현식의 실제 C 타입에 맞춰 선택합니다. 전체 C 컴파일러와 기능이 같지는 않습니다. 코루틴, 임의 모듈, 부동소수점, 배열과 완전한 소유권 검사는 후속 작업입니다. 고정점은 지원 부분집합의 셀프 호스팅을 확인하며 전체 언어의 의미적 정확성을 증명하지 않습니다.

C 파일을 쓰거나 네이티브 컴파일러를 호출하기 전에 FG로 구현한 의미 검사를 수행합니다. 없는 이름·함수, 스코프 밖 변수, 잘못된 초기화·대입·반환 타입과 사용자 함수의 인수 개수·타입을 거절합니다. 앞에 선언되지 않은 함수와 상호 재귀도 검사하며, `int`/`bool` 변환은 stage0처럼 허용합니다. 연산자·조건·`match`는 정수 또는 불리언을 요구하고, 함수 참조와 `void` 반환값은 스칼라 인수나 변수 초기값으로 사용할 수 없습니다. 매핑된 `strings/fs/os/io` 함수는 SDK 헤더의 서명과 필요한 import를 검사합니다. stage0도 전체 매핑 표준 함수의 인수·반환 서명을 검사하지만 두 컴파일러의 의미 검사는 별도 구현입니다.

`if`/`else`, 중첩 반복문, `break`/`continue`, 마지막 `_`가 있는 `match`를 분석해 비-`void` 함수의 도달 가능한 반환 누락을 거절합니다. 리터럴 조건만 경로 도달성에 반영하며, 도달하지 않는 문장도 이름과 타입을 검사합니다. 순차 `process main`의 반환 타입은 `void`이고 `native main`의 반환 타입은 `int`입니다. 같은 스코프의 중복 변수, 중복 함수·상수·매개변수와 예약된 선언 이름도 거절합니다. 의미 오류는 `forge-fg: semantic:`으로 보고하며 기존 출력 파일을 보존하고 새 출력 파일을 만들지 않습니다. FG 컴파일러에는 아직 `--check`나 stage0의 위치 기반 JSON 진단이 없습니다.

상수는 여전히 C 매크로로 생성하므로 stage0가 C 생성에서 요구하는 리터럴 상수와 차이가 있습니다. 상수 별칭의 타입은 지역 변수와 독립적으로 전역 소스에서 확인하며 순환 별칭은 표현식 깊이 제한에서 거절합니다. 상수 이름을 지역 변수나 매개변수로 가리는 선언은 C 매크로와 충돌하므로 출력 전에 거절합니다. 의미 검사는 stage0와 별도 구현이고, 정수 산술도 아직 C 연산자를 그대로 사용하므로 [stage0의 래핑·나눗셈 계약](integer-arithmetic.md)과 같지 않습니다. 표현식과 문장 깊이 제한은 모든 입력의 자원 사용을 제한한다는 보장이 아닙니다.

2026-10-05의 [셀프 호스팅 성능 보고서](https://github.com/forge-language/forge-benchmarks/blob/main/docs/selfhosting-performance-2026-10-05.md)에 stage0·stage2의 C 변환 비용, 식별자 변환 최적화, 생성 프로그램과 Rust의 실행 비교 및 고정점 검증 근거를 정리했습니다.

2026-10-06의 [FG 컴파일러 리팩터링 검증](../../forge-benchmarks/docs/compiler/selfhost-refactor-2026-10-06.md)은 공통 스캐너·문장 생성기, 식 내부 주석 수정, 중간 문자열 할당 감소의 검증 결과와 재현 명령·원시 측정값을 기록합니다.

`match` 분기는 줄바꿈 대신 문장·블록 경계로 읽습니다. 여러 줄 본문, 같은 줄의 여러 분기, 중첩 `match`, 블록 없는 단일 문장·조건문·반복문을 지원합니다. 패턴은 0부터 9223372036854775807까지의 십진 정수 또는 마지막 `_`이며, 앞에 붙은 0도 십진수로 처리합니다. 잘못된 패턴·분기 경계·도달할 수 없는 분기는 출력 전에 거절합니다. 분기 문장 경계 탐색은 과도한 `else if` 재귀를 제한하며, 이는 전체 문법·타입 검사나 모든 입력에 대한 자원 제한을 뜻하지 않습니다.

## 새 문자열 및 OS 내장 함수

`str_view_sub(view, start, length)`는 캐시한 바이트 길이로 부분 문자열을 만듭니다. `str_view_matches(view, start, text)`는 임시 부분 문자열을 만들지 않고 접두 바이트를 비교합니다. `str_builder_append_view(builder, view, start, length)`는 뷰의 범위를 빌더에 직접 복사합니다. 음수 범위는 실패하고 양수 길이는 남은 범위로 제한됩니다. 뷰는 불변 문자열을 빌리며 arena reset 뒤에는 사용할 수 없습니다. 빌더가 만든 문자열은 독립된 스냅샷입니다. JS 구현도 UTF-8 바이트 기준으로 동작합니다.

`os_command(program)`, `os_command_arg(handle, argument)`, `os_command_run(handle)`, `os_command_free(handle)`는 셸 없이 외부 프로그램을 호출합니다. 인수는 복사되어 명령 객체가 소유하며, 호출자는 반드시 free해야 합니다. 최대 추가 인수는 256개, 각 프로그램/인수는 65,536바이트입니다. 반환 코드는 실행 파일 없음 127, 기타 실행 실패 126, 정상 실행 시 자식 종료 코드입니다. POSIX 시그널 종료는 `128 + signal`입니다. `os_temp_file()`은 호출자가 삭제해야 하는 빈 임시 파일을 만들고, `os_executable_path()`는 현재 실행 파일 경로, `os_same_file(a, b)`는 실제 파일 동일성을 반환합니다. 핸들은 신뢰하는 불투명 값으로 다루며 free/reset 뒤 재사용하지 않습니다.

파일 쓰기 API는 버퍼 flush/close 실패도 보고합니다. 컴파일러는 입력과 출력의 파일 동일성을 확인하고 쓰기 오류를 실패로 처리합니다. 이는 전체 타입 또는 메모리 안전성 검사의 대체가 아닙니다.

## Docker

```bash
docker build -t forge-language:local .
docker run --rm forge-language:local --version
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" forge-language:local examples/hello.fg
docker run --rm -v "$PWD:/work" --entrypoint /work/hello forge-language:local
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" --entrypoint forge-fg forge-language:local examples/control_flow.fg
```

이미지는 Debian 기반 GCC/C11 도구 체인, 두 컴파일러, 헤더와 정적 라이브러리를 포함합니다. 이미지 빌드 중 고정점을 검증하며 OpenCL은 끕니다. 생성된 실행 파일은 이미지의 Linux/CPU/libc 환경을 대상으로 하므로 해당 컨테이너에서 실행할 수 있습니다. `.dockerignore`와 `.gitignore`는 로컬 빌드·캐시·에이전트 상태를 제외하며 필요한 소스와 테스트는 유지합니다.
