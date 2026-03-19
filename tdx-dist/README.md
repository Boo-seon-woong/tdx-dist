# tdx-dist

`tdx-dist`는 `tee-dist`를 TDX shared memory + pure one-sided RDMA 모델로 재구성한 KVS다.

- CN은 여전히 crypto, probe, cache validation, vote evaluation을 가진다.
- guest MN은 RDMA server를 띄우지 않고 shared-memory request ring만 소비한다.
- host는 shared region 생성, MR 등록, RDMA endpoint setup만 담당한다.
- TCP 경로는 비교/로컬 개발용으로 그대로 남겨둔다.

## 바이너리

- `bin/cn`
  CN REPL
- `bin/mn`
  guest MN request-consumer
- `bin/host`
  host RDMA memory exposer
- `bin/cn_bench`
  single-thread benchmark runner

## RDMA 구조

```text
CN -- RDMA READ --> slot region (PRIME/CACHE/BACKUP)
CN -- RDMA WRITE + ATOMIC CAS --> request ring
MN guest -- local shared memory --> request ring consume + slot commit
Host -- MR registration / CM accept only
```

RDMA path에서는 더 이상 `HELLO`/`CAS`/`EVICT`용 SEND/RECV control plane을 쓰지 않는다. host는 CM accept 시 private data로 `remote_addr`, `rkey`, `td_region_header_t`만 넘긴다.

## request flow

### `read`

1. CN이 primary `PRIME`을 direct probe 한다.
2. `cache:on`이면 primary `CACHE`도 probe 한다.
3. cache hit이면 CN이 local decrypt/MAC verify 한다.
4. cache miss이면 CN이 prime slot을 local decrypt/MAC verify 한다.
5. 필요하면 cache refresh request를 enqueue 한다.

### `write/update/delete`

1. CN이 candidate slot과 current epoch를 probe 한다.
2. CN이 proposal slot을 local에서 생성한다.
3. RDMA path에서는 backup/primary/cache-refresh/repair 모두 request ring commit으로 처리한다.
4. guest MN은 request를 local shared memory write로 처리하고 마지막에 `guard_epoch`를 publish 한다.

TCP path는 기존 tee-dist와 동일하게 direct write/CAS server 모델을 유지한다.

## shared region

shared region은 host와 guest MN이 같은 backing file을 열어 공유한다고 가정한다. TDX VM에서는 보통 host가 virtio-fs로 디렉터리를 export 하고, guest MN은 그 mount 안의 파일을 `memory_file`로 연다.

```text
[td_region_header_t]
[td_request_ring_t]
[td_request_entry_t * request_slots]
[PRIME slots]
[CACHE slots]
[BACKUP slots]
```

### 중요 설정

- `memory_file`
  host와 guest MN이 path 문자열까지 같을 필요는 없다. 대신 같은 backing file을 가리켜야 한다.
- `mn_memory_size`
  host와 guest MN이 반드시 동일해야 한다.
- `request_slots`
  ring capacity다. 기본값은 `1024`.
- `prime_slots`, `cache_slots`, `backup_slots`
  비워두면 남은 공간 안에서 `4:1:4` 비율로 자동 배분한다.

## preset

### single-node RDMA

- guest MN: `build/config/mn.rdma.conf`
- host: `build/config/host.rdma.conf`
- CN: `build/config/cn.rdma.conf`

### 3-node RDMA

- guest MN: `build/config/mn1.rdma.conf`, `mn2.rdma.conf`, `mn3.rdma.conf`
- host: `build/config/host1.rdma.conf`, `host2.rdma.conf`, `host3.rdma.conf`
- CN:
  `cn.rdma.conf`를 복사해서 `mn_endpoint`를 3개로 늘려 쓰면 된다.

### TCP

기존 `mn1.conf/mn2.conf/mn3.conf`, `cn.cache-off.conf`, `mn.conf`, `cn.conf`, benchmark preset은 그대로 쓸 수 있다.

## 실행 예시

### single-node RDMA

먼저 TDX guest를 virtio-fs share와 함께 띄운다.

```bash
./guest-tools/run_td --shared-dir /tmp/tdx-dist-share
```

guest 안에서는 virtio-fs를 한 번 mount 한다.

```bash
mkdir -p /mnt/tdx-dist-share
mount -t virtiofs tdx-dist-share /mnt/tdx-dist-share
```

그 다음 host가 region을 만들고 RDMA로 노출한다.

```bash
./bin/host --config build/config/host.rdma.conf
```

그 다음 guest MN이 virtio-fs mount 안의 backing file을 attach 해서 request ring을 소비한다.

```bash
./bin/mn --config build/config/mn.rdma.conf
```

마지막으로 CN을 실행한다.

```bash
./bin/cn --config build/config/cn.rdma.conf
```

### 3-node TCP

```bash
./bin/mn --config build/config/mn1.conf
./bin/mn --config build/config/mn2.conf
./bin/mn --config build/config/mn3.conf
./bin/cn --config build/config/cn.cache-off.conf
```

## REPL

- `read <key>`
- `write <key> <value>`
- `update <key> <value>`
- `delete <key>`
- `read <key> -t`
- `write <key> <value> -t`
- `update <key> <value> -t`
- `delete <key> -t`
- `status`
- `evict`
- `help`
- `quit`

RDMA에서 `evict`는 control message가 아니라 request ring을 통해 guest MN으로 전달된다.

## benchmark

`cn_bench`는 기존과 동일하게 single-thread runner다. RDMA benchmark를 돌릴 때도 host와 guest MN이 먼저 떠 있어야 한다.

## TDX VM note

- host RDMA preset은 `/tmp/tdx-dist-share/...` 아래 backing file을 사용한다.
- guest RDMA preset은 `/mnt/tdx-dist-share/...` 아래 backing file을 사용한다.
- 두 경로는 문자열이 다르지만 virtio-fs를 통해 같은 backing file을 본다.

## 소스 맵

- `src/cn_main.c`
  CN REPL entrypoint
- `src/mn_main.c`
  guest MN entrypoint
- `src/host_main.c`
  host entrypoint
- `src/request.c`
  request ring enqueue/consume
- `src/transport_rdma.c`
  pure one-sided RDMA client + host acceptor
- `src/transport_tcp.c`
  legacy TCP server/client path
- `src/cluster.c`
  workload semantics, repair, cache refresh, latency profile
- `src/layout.c`
  shared region init/attach, slot offsets, eviction
