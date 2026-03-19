# SKILL: tdx-dist Current Runtime

이 문서는 `tdx-dist`를 수정할 때 따라야 하는 현재 구현 기준 런타임 모델이다. 목표 문서가 아니라, 지금 코드가 의도하는 실행 구조를 정리한다.

## 언제 이 문서를 사용할까

- `src/transport_rdma.c`, `src/request.c`, `src/layout.c`를 수정할 때
- host / guest MN / CN 경계를 바꿀 때
- `read/write/update/delete` semantics 또는 latency profile을 바꿀 때
- RDMA preset, README, host 실행 순서를 함께 맞춰야 할 때

## 현재 아키텍처

- `CN`
  - `PRIME/CACHE/BACKUP` slot은 RDMA READ 또는 TCP read로 직접 probe한다.
  - `write/update/delete/cache refresh/repair/evict`는 RDMA에서는 request ring에 enqueue 한다.
  - proposal slot 생성, crypto, cache validation, vote 평가, repair 판단은 CN 책임이다.

- `MN guest`
  - `transport=rdma`일 때 RDMA server를 띄우지 않는다.
  - host가 만든 shared region을 attach 하고 request ring만 소비한다.
  - slot commit은 local shared memory write + 마지막 `guard_epoch` publish로 처리한다.
  - eviction thread는 계속 유지한다.

- `Host`
  - `bin/host`가 shared region을 만들고 MR을 등록한다.
  - RDMA CM accept 후 private data로 `remote_addr/rkey/header`만 전달한다.
  - request parsing, memcpy data path, SEND/RECV control plane은 없다.

- `TCP`
  - 기존 tee-dist 경로를 그대로 유지한다.
  - worker thread 기반 read/write/CAS/control server가 계속 동작한다.

## RDMA request semantics

### `read`

1. CN이 primary `PRIME`을 RDMA READ로 probe한다.
2. `cache:on`이면 primary `CACHE`도 RDMA READ로 probe한다.
3. cache hit이면 CN이 local verify/decrypt 한다.
4. cache miss이면 prime slot을 local verify/decrypt 한다.
5. prime hit 후 cache refresh가 필요하면 refresh request를 ring에 enqueue 한다.

### `write/update/delete`

1. CN이 primary/backup slot을 RDMA READ로 probe한다.
2. CN이 proposal slot을 local에서 만든다.
3. 각 backup commit을 request ring에 enqueue 한다.
4. CN이 vote rule을 평가한다.
5. primary commit도 request ring에 enqueue 한다.
6. 실패한 backup repair와 cache refresh도 request ring 경유다.

## shared memory layout

region은 다음 순서로 배치된다.

1. `td_region_header_t`
2. `td_request_ring_t`
3. `td_request_entry_t[request_slots]`
4. slot region (`PRIME -> CACHE -> BACKUP`)

ring은 `reserve_head / head / tail` 3개 counter를 쓴다.

- `reserve_head`
  producer가 slot을 예약할 때 atomic CAS 대상
- `head`
  consumer가 볼 수 있는 publish point
- `tail`
  MN consumer가 처리 완료한 마지막 seq

## 절대 깨면 안 되는 규칙

- RDMA path에 SEND/RECV control fallback을 다시 넣지 않는다.
- guest MN에 RDMA server thread를 다시 넣지 않는다.
- host가 request payload를 해석하거나 slot body를 memcpy 하지 않는다.
- slot publish 순서는 `visible/body -> release fence -> guard_epoch`를 유지한다.
- config preset을 바꾸면 host config와 guest MN config의 `mn_memory_size`, `request_slots`, slot layout 값이 항상 같아야 한다.
- TDX VM에서는 `memory_file` path 문자열이 달라도 된다. 대신 virtio-fs 같은 메커니즘을 통해 같은 backing file을 가리켜야 한다.

## 중요한 파일

- `src/request.c`
  RDMA request ring enqueue/consume
- `src/transport_rdma.c`
  pure one-sided RDMA client + host acceptor
- `src/layout.c`
  shared region init/attach, ring/slot offsets
- `src/mn_main.c`
  guest MN request consumer loop
- `src/host_main.c`
  host entrypoint
- `src/cluster.c`
  workload semantics, vote, latency breakdown
- `build/config/*.rdma*.conf`
  host/guest pairing preset

## 수정할 때 체크

- RDMA write path를 바꾸면 `README.md`와 `build/config/host*.conf`까지 같이 본다.
- request/result field를 바꾸면 host와 guest가 같은 struct를 보도록 두 트리 모두 동기화한다.
- latency 항목을 바꾸면 `td_print_latency_profile()` 출력도 같이 갱신한다.
- ring capacity나 slot allocation을 바꾸면 `mn_memory_size` 계산식과 preset 예시를 같이 맞춘다.
