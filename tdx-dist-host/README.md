# tdx-dist-host

`tdx-dist-host`는 host 역할만 담는 최소 트리다.

역할은 세 가지뿐이다.

1. shared memory 파일 생성 및 레이아웃 초기화
2. 해당 region을 RDMA MR로 등록
3. RDMA CM accept 후 `remote_addr`, `rkey`, `td_region_header_t`를 client에 전달

이 트리에는 의도적으로 다음이 없다.

- CN logic
- guest MN logic
- request ring consumer
- crypto
- TCP transport
- benchmark

## 빌드

```bash
make -C tdx-dist-host
```

생성 바이너리는 `bin/host` 하나다.

## 실행

```bash
./bin/host --config build/config/host.rdma.conf
```

3-node 예시는 다음 preset을 쓴다.

- `build/config/host1.rdma.conf`
- `build/config/host2.rdma.conf`
- `build/config/host3.rdma.conf`

## 주의

- `mn_memory_size`, `request_slots`, slot layout은 guest MN 쪽 preset과 반드시 같아야 한다.
- `memory_file`은 guest와 path 문자열이 달라도 된다. 대신 같은 backing file을 가리켜야 한다.
- host는 request를 해석하지 않는다.
- host는 slot body를 읽거나 쓰지 않는다.
- host는 RDMA data path fallback을 제공하지 않는다.

TDX VM preset은 다음 전제를 둔다.

- host preset: `/tmp/tdx-dist-share/...`
- guest preset: `/mnt/tdx-dist-share/...`
- guest는 `run_td --shared-dir /tmp/tdx-dist-share`로 VM을 띄운 뒤 `mount -t virtiofs tdx-dist-share /mnt/tdx-dist-share`를 먼저 수행한다.
