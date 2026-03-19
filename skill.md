# SKILL: tdx-dist Runtime (TDX + One-sided RDMA + Shared Memory, Dual-Path)

이 문서는 `tee-dist`를 기반으로 한 **tdx-dist의 최종 런타임 모델**을 정의한다.
본 구조는 **one-sided RDMA (READ + WRITE) + TDX shared memory**를 결합한 형태이며,
RDMA의 memory semantic 특성을 그대로 유지한다.

---

# 언제 이 문서를 사용할까

* tee-dist → tdx-dist로 구조 변경 시
* RDMA transport를 shared memory 기반으로 재구성할 때
* CN/MN 간 control plane과 data plane을 분리할 때
* TDX 환경에서 RDMA memory exposure 설계 시

---

# 핵심 개념 (Critical Invariants)

## 1. RDMA는 dual-path 구조다

RDMA one-sided는 다음 두 primitive로 구성된다:

* RDMA WRITE → remote memory에 데이터 push
* RDMA READ → remote memory에서 데이터 fetch

이 둘은 모두 memory semantic operation이며, remote CPU를 우회한다. ([퍼듀 대학교 공학부][1])

---

## 2. 역할 분리 (절대 변경 금지)

| 경로         | 역할                           |
| ---------- | ---------------------------- |
| WRITE path | request ingress (CN → MN)    |
| READ path  | data access (CN → MN memory) |

👉 WRITE만으로 시스템 구성 불가능 (read path 필수)

---

## 3. host는 data path에서 제거

* RDMA data path에 개입하지 않음
* memcpy 금지
* parsing 금지

역할:

* shared memory 할당
* MR 등록
* RDMA endpoint setup

---

## 4. shared memory는 global memory처럼 동작

* CN이 RDMA로 직접 접근
* MN은 local memory처럼 접근

👉 이 구조는 사실상:

> "disaggregated memory model"

---

# 전체 아키텍처

```text
                (WRITE path)
CN ───── RDMA WRITE ─────▶ request ring (shared memory)
                                │
                                ▼
                            MN (TDX)

                (READ path)
CN ───── RDMA READ ─────▶ slot region (shared memory)
```

---

# 메모리 레이아웃 (반드시 분리)

## 1. Request Ring (control plane)

```c
typedef struct {
    uint64_t head;   // producer (CN)
    uint64_t tail;   // consumer (MN)
    uint8_t  buffer[REQ_RING_SIZE];
} shm_req_ring_t;
```

용도:

* write/update/delete 요청 전달
* MN만 소비

---

## 2. Slot Region (data plane)

```c
typedef struct {
    td_slot_t slots[NUM_SLOTS];
} shm_slot_region_t;
```

용도:

* PRIME / CACHE / BACKUP 저장
* RDMA READ 대상

---

# RDMA 설계

## CN 측

### WRITE path (request ingress)

```c
// 1. payload write
ibv_post_send(RDMA_WRITE, payload → ring.buffer[offset]);

// 2. ordering fence (software-level)
fence();

// 3. head update
ibv_post_send(RDMA_WRITE, new_head → ring.head);
```

---

### READ path (data fetch)

```c
ibv_post_send(RDMA_READ, remote_slot → local_buffer);
```

---

## MN 측

RDMA verbs 사용 없음

```c
while (1) {
    if (tail < head) {
        req = ring[tail];
        process(req);
        tail++;
    }
}
```

---

## host 측

```c
// shared memory 생성
shm = alloc_shared_pages();

// RDMA memory registration
ibv_reg_mr(pd, shm, size,
    IBV_ACCESS_REMOTE_WRITE |
    IBV_ACCESS_REMOTE_READ |
    IBV_ACCESS_LOCAL_WRITE);
```

👉 이후 data path 개입 없음

---

# request semantics (변경 없음)

## read

```text
1. RDMA READ (PRIME)
2. cache validation
3. optional RDMA READ (BACKUP)
```

---

## write/update/delete

```text
1. RDMA READ (probe current slot)
2. RDMA WRITE (enqueue request)
3. MN processing
4. optional RDMA READ (result 확인)
```

---

# synchronization 규칙 (필수)

## producer (CN)

```c
write payload
atomic_thread_fence(memory_order_release)
write head
```

---

## consumer (MN)

```c
read head
atomic_thread_fence(memory_order_acquire)
read payload
```

---

## 이유

RDMA WRITE는 ordering을 보장하지 않으므로:

* payload → head 순서 보장 필요

---

# 성능 모델

## latency breakdown

* RDMA WRITE: 3~8 μs
* RDMA READ: 2~5 μs
* cache coherence: 2~5 μs
* MN processing: 5~15 μs

총:

≈ 8~20 μs

---

# 기존 tee-dist와의 차이

| 항목           | tee-dist           | tdx-dist             |
| ------------ | ------------------ | -------------------- |
| RDMA target  | MN process         | shared memory        |
| RDMA model   | mixed              | pure one-sided       |
| data path    | CPU involvement 있음 | CPU bypass           |
| host 역할      | 없음                 | init only            |
| memory model | distributed nodes  | disaggregated memory |

---

# 설계 제약 (Critical Constraints)

## 1. RDMA READ 대상은 반드시 shared memory

* TDX private memory → RDMA 접근 불가
* shared memory만 노출 가능

---

## 2. slot region은 lock-free 접근 가능해야 함

* CN multiple concurrent READ 가능
* MN write 시 atomicity 필요

---

## 3. ring overflow 반드시 처리

* head-tail wraparound
* overwrite 방지

---

## 4. multi-CN 동시 접근 고려

* head update contention
* atomic fetch_add 필요 가능

---

# 보안 모델

## 위협

* host가 shared memory 조작 가능
* RDMA sender도 공격 가능

---

## 대응

* MAC verify (필수)
* epoch monotonicity
* replay protection

---

# 절대 금지 사항

* WRITE-only 설계
* host memcpy
* SEND/RECV fallback
* MN RDMA server 로직 추가

---

# 핵심 요약

* RDMA는 WRITE + READ 두 경로로 구성된다
* WRITE는 request ingress
* READ는 data access
* shared memory는 global memory처럼 사용된다
* host는 control-plane-only
* 전체 구조는 disaggregated memory 기반 KVS와 동일하다

[1]: https://engineering.purdue.edu/~vshriva/courses/papers/herd_2014.pdf?utm_source=chatgpt.com "Using RDMA Efficiently for Key-Value Services"

