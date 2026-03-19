# SKILL: tdx-dist-host Current Runtime

이 트리는 host 역할만 담당한다.

## host의 책임

- shared region 생성
- request ring + slot region 레이아웃 초기화
- RDMA MR 등록
- RDMA CM accept
- connect private data로 `remote_addr`, `rkey`, `td_region_header_t` 전달

## host가 하지 않는 일

- request parsing
- request ring consume
- slot commit
- cache eviction
- crypto
- TCP serving
- CN/MN workload 실행

## 수정할 때 규칙

- region ABI는 `tdx-dist`와 반드시 같아야 한다.
- `td_slot_t`, `td_request_entry_t`, `td_region_header_t`의 field order와 size를 임의로 바꾸지 않는다.
- host에 data-path helper를 추가하지 않는다.
- SEND/RECV 기반 control plane을 추가하지 않는다.

## 중요한 파일

- `src/host_main.c`
- `src/config.c`
- `src/layout.c`
- `src/transport_rdma.c`
- `build/config/host*.rdma.conf`
