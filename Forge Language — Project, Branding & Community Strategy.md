# Forge

> **Safe ownership. Massive concurrency. Native speed.**
> **AI builds. AI tests. AI reviews. Humans decide.**

Forge는 **메모리 안전성, 대규모 동시성, 높은 실행 성능**을 하나의 프로그래밍 언어에서 제공하는 것을 목표로 하는 실험적 시스템 프로그래밍 언어이다.

Rust의 **Ownership 기반 메모리 안전성**, Elixir/Erlang 생태계의 **경량 프로세스 모델**, Go의 **간단한 동시성 프로그래밍 경험**에서 영감을 받아 설계한다.

또한 Forge 자체의 개발 과정도 하나의 실험이다.

Forge는 전통적인 오픈소스 개발 방식뿐 아니라 **AI Coding Agent가 Issue → 구현 → 테스트 → 코드 리뷰 → PR 수정 과정에 적극적으로 참여하는 AI-native 오픈소스 프로젝트**를 목표로 한다.

---

# 1. Forge가 해결하려는 문제

현대 서버 및 시스템 소프트웨어에서는 다음 요구사항들이 동시에 중요해지고 있다.

- 높은 성능
- 메모리 안전성
- 대규모 동시성
- 간단한 동시성 프로그래밍
- 낮은 런타임 오버헤드
- 높은 개발 생산성
- 안정적인 멀티코어 활용

하지만 일반적으로 이 요구사항들을 모두 만족시키기는 어렵다.

예를 들어:

### Rust

장점:

- Ownership 기반 메모리 안전성
- Garbage Collector가 없음
- 매우 높은 성능
- 강력한 타입 시스템

단점:

- 높은 학습 난이도
- 복잡한 비동기 프로그래밍
- 일부 동시성 코드를 작성하기 어려움

### Elixir / Erlang

장점:

- 매우 가벼운 프로세스
- Actor 기반 동시성
- 강력한 장애 격리
- 대규모 동시성 처리

단점:

- BEAM VM 의존
- Native systems programming에는 적합하지 않음
- 저수준 성능 제어가 제한적임

### Go

장점:

- Goroutine을 이용한 간단한 동시성
- 쉬운 문법
- 높은 개발 생산성

단점:

- Garbage Collector 사용
- 세밀한 메모리 제어가 어려움
- 일부 low-level 시스템 영역에는 제약이 있음

Forge는 이 세 가지 방향의 장점을 하나의 언어 설계 안에서 재해석하는 것을 목표로 한다.

---

# 2. Forge의 핵심 철학

Forge의 핵심 철학은 세 가지다.

## Safe Ownership

Rust에서 영감을 받은 Ownership 모델을 이용해 메모리 안전성을 확보한다.

목표:

- Null 관련 문제 최소화
- Use-after-free 방지
- Double free 방지
- Data race 방지
- GC 없이 메모리 안전성 확보

---

## Massive Concurrency

Elixir/Erlang에서 영감을 받은 매우 가벼운 실행 단위를 제공한다.

목표:

```text
수십 개 Thread
        ↓
수천 개
        ↓
수십만 개
        ↓
수백만 개 Lightweight Process / Task
```

운영체제 Thread를 직접 대량 생성하는 대신 Forge Runtime이 경량 작업을 스케줄링한다.

---

## Native Speed

Forge 프로그램은 높은 Native 성능을 목표로 한다.

궁극적으로 다음과 같은 영역에서 사용 가능한 언어를 지향한다.

- High-performance backend
- Distributed systems
- Game server
- Database
- Network server
- Cloud infrastructure
- CLI
- Embedded / Systems software
- Parallel computing

---

# 3. Forge의 핵심 가치

Forge를 한 문장으로 표현하면 다음과 같다.

> **Memory safety of ownership.
> Massive lightweight concurrency.
> Native performance.**

또는 공식 슬로건으로:

> **Safe ownership. Massive concurrency. Native speed.**

를 사용한다.

---

# 4. Forge의 예상 프로그래밍 모델

Forge의 문법은 최대한 단순하고 읽기 쉽게 설계한다.

예시:

```forge
fn main() {
    println("Hello, Forge!")
}
```

경량 Task:

```forge
fn main() {
    spawn {
        println("Hello from Forge")
    }
}
```

대량 Task 생성:

```forge
fn main() {
    for i in 0..100_000 {
        spawn {
            println("Task {i}")
        }
    }
}
```

메시지 기반 동시성도 고려할 수 있다.

```forge
process Worker {
    receive {
        Work(data) => {
            handle(data)
        }
    }
}
```

구체적인 문법과 모델은 언어 설계 과정에서 변경될 수 있다.

---

# 5. Forge Runtime 방향

Forge의 핵심 경쟁력 중 하나는 Runtime이다.

개념적으로:

```text
Forge Program
      │
      ▼
Lightweight Tasks / Processes
      │
      ▼
Forge Scheduler
      │
      ▼
Worker Threads
      │
      ▼
CPU Cores
```

를 목표로 한다.

OS Thread와 Forge Process를 1:1로 연결하지 않고, 여러 경량 프로세스를 적은 수의 Worker Thread 위에서 실행한다.

---

# 6. Forge가 지향하는 개발 경험

Forge는 언어 성능만큼 **개발 경험**도 중요하게 본다.

목표 CLI:

```bash
forge new hello
```

```bash
forge run
```

```bash
forge build
```

```bash
forge test
```

```bash
forge add http
```

```bash
forge fmt
```

```bash
forge bench
```

즉,

> Clone → Build → Run

과정이 최대한 간단해야 한다.

---

# 7. Forge 생태계

장기적으로 공식 패키지 생태계를 만든다.

예:

```text
forge-http
forge-json
forge-sql
forge-cli
forge-test
forge-crypto
forge-web
forge-grpc
```

사용 예:

```bash
forge add http
forge add json
forge add postgres
```

궁극적으로 Forge Package Registry를 운영한다.

예:

```text
packages.forge-lang.org
```

---

# 8. Forge Playground

새로운 언어의 가장 큰 진입 장벽은 설치다.

따라서 웹에서 즉시 Forge를 실행할 수 있는 Playground를 제공하는 것을 목표로 한다.

예:

```text
play.forge-lang.org
```

사용자는 설치 없이:

1. 코드 작성
2. Run 클릭
3. 결과 확인

까지 할 수 있어야 한다.

---

# 9. Benchmark 전략

Forge의 핵심 마케팅 요소 중 하나는 **재현 가능한 Benchmark**다.

단순히:

> Forge is fast.

라고 주장하지 않는다.

대신 누구나 직접 테스트할 수 있도록 한다.

예:

```text
benchmarks/
├── spawn/
├── channels/
├── scheduler/
├── tcp_echo/
├── actor_pingpong/
├── allocation/
└── context_switch/
```

비교 대상 예:

- Forge
- Rust / Tokio
- Go
- Elixir / BEAM
- Zig
- C++

중요한 원칙:

> Benchmark 결과보다 Benchmark의 재현 가능성이 중요하다.

모든 코드와 실행 환경을 공개한다.

예:

```bash
git clone https://github.com/forge-lang/benchmarks
cd benchmarks

./run.sh
```

---

# 10. Forge의 두 번째 핵심 정체성

Forge의 차별화 요소는 언어 설계만이 아니다.

Forge 자체를 개발하는 방식 역시 프로젝트의 일부다.

Forge는:

> **An AI-native systems programming language.**

라는 방향을 가진다.

또는:

> **An experimental systems language built in the open by humans and AI agents.**

라고 소개할 수 있다.

---

# 11. AI-native Development

Forge는 AI Coding Agent를 단순한 자동완성 도구가 아니라 하나의 개발 참여자로 사용한다.

전통적인 개발:

```text
Developer
   │
   ▼
Code
   │
   ▼
Pull Request
   │
   ▼
Review
   │
   ▼
Merge
```

Forge:

```text
Issue / RFC
     │
     ▼
Planner Agent
     │
     ▼
Implementation Agent
     │
     ▼
Test Agent
     │
     ▼
Review Agent
     │
     ▼
Human Review
     │
     ▼
Merge
```

---

# 12. AI Agent의 역할

## Planner Agent

Issue와 RFC를 분석한다.

역할:

- 요구사항 분석
- 코드 구조 분석
- 구현 계획 작성
- 영향을 받는 파일 탐색

---

## Implementation Agent

계획을 바탕으로 실제 코드를 작성한다.

역할:

- 기능 구현
- 코드 수정
- Refactoring
- PR 생성

---

## Test Agent

변경된 기능을 검증한다.

역할:

- Unit Test
- Integration Test
- Compiler Test
- Regression Test
- Benchmark

---

## Review Agent

PR을 검토한다.

역할:

- 버그 탐색
- 안전성 문제 탐색
- 성능 Regression 탐색
- API 문제 탐색
- 코드 품질 검토

---

# 13. Human Governance

Forge는 AI가 많은 코드를 작성할 수 있지만 최종 의사결정은 사람이 한다.

핵심 원칙:

> **AI builds.
> AI tests.
> AI reviews.
> Humans decide.**

AI끼리 자동으로:

```text
Agent Code
↓
Agent Approve
↓
Agent Merge
```

하는 구조보다는:

```text
Agent Code
↓
Agent Test
↓
Agent Review
↓
Human Maintainer
↓
Merge
```

구조를 지향한다.

Compiler, Runtime, Ownership, Memory Safety와 관련된 변경일수록 Human Review를 중요하게 취급한다.

---

# 14. AI 사용을 숨기지 않는다

Forge가 AI를 이용해 개발되고 있다는 사실을 숨길 필요는 없다.

대신:

> "Vibe coding으로 만들었다."

가 프로젝트의 핵심 설명이 되지는 않는다.

외부에는:

> **Forge is an experiment in building a programming language with AI agents and human governance.**

라는 방향으로 설명한다.

핵심은:

**AI가 코드를 작성했다는 사실이 아니라 AI를 어떻게 검증하고 관리하는가이다.**

---

# 15. Agent Development Dashboard

Forge 공식 사이트에서 AI Agent의 활동을 공개하는 것도 고려한다.

예:

```text
Forge Development

Merged PRs
412

Agent-assisted PRs
301

Open RFCs
17

Compiler Tests
5,481

Benchmark Regressions
2

Contributors
63
```

숫자는 실제 Repository에서 자동으로 가져온다.

---

# 16. Watch the Agents Work

Forge 공식 홈페이지의 특징적인 기능으로:

> **Watch the agents work**

를 제공할 수 있다.

예:

```text
Issue #482
Implement structured concurrency

Planner Agent
✓ Architecture analyzed

Compiler Agent
✓ Implementation completed

Test Agent
✓ 93 tests passed

Review Agent
⚠ 2 issues found

Compiler Agent
✓ Issues fixed

Human Maintainer
○ Awaiting review
```

개발 과정 자체를 콘텐츠로 만드는 것이다.

---

# 17. 실패도 콘텐츠로 만든다

AI 개발의 성공 사례만 공개하지 않는다.

실패 역시 Forge 프로젝트의 흥미로운 콘텐츠가 될 수 있다.

예:

```text
We asked 3 AI agents to implement
Forge's scheduler.
```

결과:

```text
Attempt #1

Race condition
FAILED
```

```text
Attempt #2

Memory regression
FAILED
```

```text
Attempt #3

Tests passed
Benchmark passed

SUCCESS
```

이런 기록을 기술 블로그 콘텐츠로 활용한다.

---

# 18. Forge Open Source 전략

Forge는 처음부터 오픈소스 기여가 쉬운 프로젝트를 목표로 한다.

필수 문서:

```text
README.md

CONTRIBUTING.md

ARCHITECTURE.md

ROADMAP.md

LANGUAGE_SPEC.md

CODE_OF_CONDUCT.md
```

---

# 19. CONTRIBUTING.md

처음 온 개발자가 5~10분 안에 개발 환경을 구축할 수 있어야 한다.

예:

```bash
git clone https://github.com/forge-lang/forge
cd forge

cargo build

cargo test
```

그리고:

```text
Compiler
Runtime
Parser
Type System
Standard Library
CLI
Documentation
```

구조를 설명한다.

---

# 20. ARCHITECTURE.md

Compiler 전체 구조를 시각적으로 설명한다.

예:

```text
Source Code
    │
    ▼
Lexer
    │
    ▼
Parser
    │
    ▼
AST
    │
    ▼
Type Checker
    │
    ▼
Ownership Checker
    │
    ▼
IR
    │
    ▼
Optimizer
    │
    ▼
Code Generator
    │
    ▼
Executable
```

---

# 21. Good First Issue

신규 기여자에게:

```text
Implement Borrow Checker
```

같은 이슈를 바로 주지 않는다.

대신:

```text
[good first issue]

Add --version option to Forge CLI

Difficulty:
★☆☆☆☆

Expected changes:
~30 lines

Files:
src/cli.rs

Expected output:

forge --version

Forge 0.1.0
```

처럼 구성한다.

초기에는 최소:

**10~20개의 good first issue**

를 유지하는 것을 목표로 한다.

---

# 22. AI와 함께 Open Source에 기여

Forge는 기존 오픈소스 프로젝트보다 기여 방식을 더 넓게 정의할 수 있다.

기여자는 반드시 모든 코드를 직접 작성할 필요가 없다.

예:

```text
Contributor
     │
     ▼
Idea / Issue
     │
     ▼
Specification
     │
     ▼
Forge Agents
     │
     ▼
Implementation
     │
     ▼
Contributor Review
     │
     ▼
PR
```

즉:

> **You don't have to write every line to contribute.
> Bring an idea. Work with the agents.**

라는 문화를 만들 수 있다.

---

# 23. Forge Community

커뮤니티는 Discord를 중심으로 운영할 수 있다.

예:

```text
Forge Discord

# announcements

# general

# language-design

# compiler

# runtime

# standard-library

# libraries

# help

# contributors

# ai-agents

# benchmarks

# showcase

# off-topic
```

---

# 24. Contributor Recognition

기여자를 공식적으로 인정한다.

예:

```text
Founding Contributor

Compiler Contributor

Runtime Contributor

Library Author

Documentation Contributor

Community Contributor
```

GitHub 프로필, 공식 홈페이지, Discord Role 등에 표시할 수 있다.

---

# 25. Forge Foundry

커뮤니티 참여를 위해 정기적인 개발 이벤트를 운영한다.

이름:

> **Forge Foundry**

예:

```text
Forge Foundry #01

Build an HTTP Server
```

```text
Forge Foundry #02

Build a Redis Client
```

```text
Forge Foundry #03

Build a Game Server
```

```text
Forge Foundry #04

Build a JSON Parser
```

우수 프로젝트는:

- 공식 홈페이지
- Forge Blog
- GitHub
- Discord
- SNS

등에서 소개한다.

---

# 26. 마케팅 기본 원칙

Forge의 마케팅은 광고보다 **증명**에 집중한다.

나쁜 방식:

> Forge is the fastest programming language.

좋은 방식:

> Here is the benchmark.
> Here is the source code.
> Run it yourself.

또한:

> Forge is better than Rust.

보다:

> Here's how Forge approaches ownership differently from Rust.

처럼 기술적으로 설명한다.

---

# 27. Forge를 홍보하는 가장 좋은 콘텐츠

단순 프로젝트 소개보다 개발 과정을 콘텐츠화한다.

예:

## Compiler

```text
How we built Forge's parser
```

```text
Designing ownership for Forge
```

```text
How Forge detects data races
```

## Runtime

```text
Running 1,000,000 lightweight processes in Forge
```

```text
Inside the Forge scheduler
```

```text
How Forge maps lightweight processes to CPU cores
```

## AI Development

```text
We asked AI agents to implement a compiler feature
```

```text
Can AI agents build a programming language?
```

```text
How Forge uses AI agents for code review
```

```text
What happens when an AI-generated compiler PR fails?
```

---

# 28. 공개할 커뮤니티

한국:

```text
GeekNews
GitHub
개발자 Discord
기술 블로그
YouTube
```

해외:

```text
Hacker News
Reddit
DEV Community
GitHub
X
YouTube
```

특히 Hacker News에서는 프로젝트가 실제 실행 가능한 상태가 되었을 때:

```text
Show HN:
Forge – A systems language with ownership
and lightweight processes
```

처럼 공개한다.

---

# 29. 공식 사이트의 핵심 목표

Forge 공식 사이트는 단순한 프로젝트 소개 페이지가 아니다.

다음 네 가지 행동을 유도해야 한다.

```text
Learn

Try

Watch

Contribute
```

즉:

```text
Forge Website

     ↓

What is Forge?

     ↓

Try Forge

     ↓

Watch Agents

     ↓

Explore GitHub

     ↓

Contribute
```

---

# 30. 공식 사이트 Hero

첫 화면은 최대한 단순하게 구성한다.

```text
FORGE

Safe ownership.
Massive concurrency.
Native speed.

An experimental systems programming language
built in the open by humans and AI agents.
```

버튼:

```text
[ Try Forge ]

[ GitHub ]

[ Documentation ]
```

추가적인 차별화 CTA:

```text
[ Watch the Agents Work ]
```

---

# 31. 홈페이지 전체 구조

추천 구조:

```text
Home

Language

Why Forge

Playground

Benchmarks

Agents

Packages

Documentation

Blog

Community

Contribute

GitHub
```

---

# 32. Home

Home은 Forge가 무엇인지 10초 안에 이해시키는 역할을 한다.

구성:

```text
Hero

↓

Code Example

↓

Why Forge

↓

Ownership

↓

Concurrency

↓

Performance

↓

Agent-native Development

↓

Benchmarks

↓

Community

↓

Get Started
```

---

# 33. Why Forge

3개의 핵심 카드로 설명한다.

## Safe Ownership

```text
Memory safety without depending on a GC.
```

## Massive Concurrency

```text
Lightweight processes designed for
massive concurrent workloads.
```

## Native Speed

```text
Designed for high-performance
native applications.
```

---

# 34. AI Development Section

Forge만의 차별화 영역이다.

제목:

> **Built differently.**

설명:

```text
Forge is developed with a combination of
human maintainers and autonomous coding agents.

Agents plan features, implement code,
run tests and review pull requests.

Humans make the final decisions.
```

그리고:

> **AI builds. AI tests. AI reviews. Humans decide.**

를 크게 표시한다.

---

# 35. Watch Agents

별도의 `/agents` 페이지를 만든다.

예:

```text
Agents working now
```

```text
Planner Agent

Issue #302

Structured concurrency design

Status:
Planning
```

```text
Compiler Agent

PR #290

Improve ownership diagnostics

Status:
Testing
```

```text
Review Agent

PR #288

Scheduler refactor

Status:
Reviewing
```

---

# 36. Benchmarks Page

`/benchmarks`

예:

```text
Forge Benchmarks

Task Spawn

Context Switching

Message Passing

TCP Echo

HTTP Throughput

Memory Usage
```

각 Benchmark에서:

```text
Hardware

OS

Compiler Version

Command

Source Code

Results
```

까지 공개한다.

---

# 37. Playground Page

`/play`

첫 화면에서 바로:

```forge
fn main() {
    spawn {
        println("Hello from Forge")
    }
}
```

실행할 수 있도록 한다.

---

# 38. Contribute Page

`/contribute`

구성:

```text
Start contributing to Forge
```

### Pick an issue

```text
good first issue
```

### Join Discord

```text
Ask questions and meet contributors.
```

### Work with Agents

```text
You can contribute ideas,
specifications, tests, documentation,
libraries or code.
```

### Read Architecture

```text
Understand how Forge works.
```

---

# 39. 초기 Release 준비 조건

처음 Forge를 크게 공개하기 전에 최소한 다음을 준비하는 것을 목표로 한다.

```text
Compiler
```

```text
CLI
```

```text
Basic standard library
```

```text
Documentation
```

```text
Language specification
```

```text
Examples
```

```text
Benchmarks
```

```text
Playground
```

```text
CONTRIBUTING.md
```

```text
ARCHITECTURE.md
```

```text
ROADMAP.md
```

```text
10~20 good first issues
```

---

# 40. 초기 마케팅 순서

## Phase 1 — Foundation

```text
Compiler

README

Documentation

Roadmap

Language Specification
```

---

## Phase 2 — Proof

```text
Benchmarks

Examples

Tests

Architecture Documentation
```

---

## Phase 3 — Experience

```text
Forge CLI

Playground

5-minute tutorial
```

---

## Phase 4 — Community

```text
Discord

good first issues

Contributor Guide

RFC Process
```

---

## Phase 5 — Ecosystem

```text
forge-http

forge-json

forge-cli

forge-test

Package Manager
```

---

## Phase 6 — Public Launch

```text
Official Website

↓

GitHub Release

↓

GeekNews

↓

Technical Blog Posts

↓

Reddit / DEV

↓

Show HN
```

---

# 41. Forge 브랜드 정체성

Forge라는 이름은 다음 이미지를 가진다.

```text
Fire

Metal

Heat

Strength

Creation

Transformation
```

이를 프로그래밍 언어에 적용하면:

```text
Source Code

     ↓

Forge

     ↓

Optimized Native Program
```

이라는 의미를 만들 수 있다.

---

# 42. 로고 컨셉

Forge 로고는:

- Forge / 대장간
- 불꽃
- 금속
- 병렬 처리
- 데이터 흐름
- 여러 실행 흐름의 결합

을 표현한다.

대표 색상 후보:

```text
Graphite / Steel

+

Molten Orange

+

Electric Blue
```

의미:

```text
Graphite
→ Systems / Strength

Orange
→ Forge / Energy / Performance

Blue
→ Concurrency / Technology
```

---

# 43. 브랜드 메시지

## Main

> **Safe ownership. Massive concurrency. Native speed.**

## Development

> **AI builds. AI tests. AI reviews. Humans decide.**

## Community

> **Bring an idea. Work with the agents.**

## Project Description

> **Forge is an experimental systems programming language focused on memory safety, massive concurrency and native performance, built openly by humans and AI coding agents.**

---

# 44. Forge의 차별화

Forge의 정체성은 단순히:

```text
Rust + Elixir + Go
```

가 아니다.

Forge가 추구해야 할 이미지는:

```text
Memory Safety

+

Massive Concurrency

+

Native Performance

+

AI-native Open Source Development
```

이다.

따라서 Forge는 두 개의 실험을 동시에 진행한다.

### Programming Language Experiment

> 안전성과 동시성과 성능을 얼마나 잘 결합할 수 있는가?

### Software Development Experiment

> AI Agent와 사람이 협력하면 프로그래밍 언어 같은 복잡한 오픈소스 프로젝트를 어디까지 개발할 수 있는가?

이 두 질문 자체가 Forge 프로젝트의 스토리가 된다.

---

# 45. 최종 프로젝트 포지셔닝

Forge를 단순한 취미 언어로 보이지 않게 하는 것이 중요하다.

Forge는 다음과 같이 소개한다.

> **Forge is an experimental systems programming language exploring a new combination of ownership-based memory safety, lightweight concurrency and native performance.**
>
> **Forge itself is developed as an open experiment in AI-native software engineering, where coding agents participate in planning, implementation, testing and review while humans retain governance over the project.**

---

# 46. Forge가 만들어야 할 경험

새로운 개발자가 Forge를 발견했을 때 이상적인 흐름은 다음과 같다.

```text
Developer discovers Forge

        ↓

"Interesting."

        ↓

Visits forge-lang.org

        ↓

Reads 10-second explanation

        ↓

Runs Forge in Playground

        ↓

Checks benchmarks

        ↓

Reads GitHub

        ↓

Watches AI Agent PR

        ↓

Joins Discord

        ↓

Finds good first issue

        ↓

Makes first contribution

        ↓

Builds Forge package

        ↓

Becomes community contributor
```

이 흐름을 최대한 짧게 만드는 것이 Forge의 초기 성장 전략이다.

---

# 47. 최종 목표

Forge의 최종 목표는 단순히 새로운 프로그래밍 언어 하나를 만드는 것이 아니다.

Forge는:

- 새로운 Systems Programming 모델
- 새로운 Concurrency 모델
- 새로운 Open Source 기여 방식
- 새로운 AI Agent 기반 개발 방식

을 동시에 실험하는 프로젝트를 목표로 한다.

궁극적으로 Forge가 던지는 질문은 다음과 같다.

> **What if memory safety, massive concurrency and native performance were designed together from the beginning?**

그리고 하나를 더 추가한다.

> **What if a programming language could be built by a community of humans and AI agents working together?**

이 두 질문이 Forge 프로젝트의 핵심이다.
