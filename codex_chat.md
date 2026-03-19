 tdx-dist/skill.md를 보면 목표가 상세하게 설명되어 있음.
    현재 tdx 진입 방법은 ssh -p 10022 root@localhost이며, 비밀번호는 123456

    tdx vm의 shared memory와 host의 물리 버퍼 공간을 공유할 것이며 자세한 설명은 tdx-dist/skill.md를 보고 확인할 수 있음.

    tdx-dist, tdx-dist-host 둘 다 아직 빌드한 적 없기 때문에 코드 자체의 정합성이나 올바름은 판단 불가. 때문에 코드가 올바르지 않거나 문제 발생 시 리팩토링 필요.

  tdx vm의 통신 관련 설정은 tdx/guset-tools/run_td.py가 관장하고 있음.

  skill.md에 기술되어 있는 아이디어가 올바르게 작동하도록 구현하는 것이 궁극적인 목표


• 요청한 목표는 tdx-dist/skill.md와 관련 코드들을 기준으로 정리한 뒤, tdx VM과 host가 shared memory/물리 버퍼를 공유하도록 실제로 동작하는 구현까지 밀어붙이는 일로 이해했다. 먼저 저장소 구조와 skill.md, run_td.py, tdx-dist, tdx-dist-host의 현재 상태를 확인하겠다.

• 문서와 핵심 진입점을 읽어 구현 방향을 고정하겠다. skill.md가 요구하는 메모리 공유 방식과 run_td.py의 VM 통신 구성을 같이 보면서, 현재 코드가 어느 지점까지 구현돼 있는지 확인 중이다.

• run_td.py 경로는 오타 가능성이 있어 실제 위치를 다시 찾겠다. 동시에 skill.md의 후반부 제약과 단계별 구현 절차까지 읽어서, 문서가 요구하는 최소 변경 집합을 뽑아내는 중이다.

• 지금부터는 설계 문서와 실제 코드의 간극을 확인한다. tdx-dist와 tdx-dist-host의 진입점, 레이아웃 정의, RDMA transport 구현을 같이 읽어서 무엇이 빠졌는지 정리하겠다.

• 구현 전 최소 확인 단계로 빌드 가능 여부와 현재 소스의 주석 없는 의도를 같이 보겠다. 빌드가 깨지면 그 에러부터 수리하고, 빌드가 되더라도 RDMA/shared-region 경로가 문서와 어긋나는 지점을 소스에서 추적하겠다.

• 프로젝트 내부 문서도 같이 보겠다. 루트 skill.md는 런타임 모델을 정의하고 있고, 각 트리의 skill.md가 구현 의도나 미완성 포인트를 더 적어뒀을 가능성이 있다.

• 컴파일러와 make가 아예 없는 환경이다. 구현은 진행할 수 있지만 즉시 검증까지 하려면 먼저 이 머신에 빌드 도구가 있는지 확인하고, 없으면 설치 가능한 패키지 관리자를 통해 최소 툴체인을 올려야 한다.

• 문서 기준으로 보면 가장 큰 구조적 구멍은 두 가지다. 현재 memory_file 기반 설계는 host와 guest가 같은 파일을 열 수 있다는 가정이 필요한데, 실제 TDX VM 경로에서는 그 연결점이 run_td에 아직 없다. 동시에 소스 쪽은 RDMA/request-ring 경로가 문서에 맞는지 끝까지 확인이 필요하다.

• 실제 TDX 게스트에서 무엇을 할 수 있는지도 바로 확인하겠다. 현재 설계의 성패는 host 파일 기반 shared region을 guest가 어떻게 같은 물리 페이지로 보게 하느냐에 달려 있어서, 게스트 안에서 마운트/장치 상태를 보는 게 필요하다.

• 이제 방향은 꽤 선명하다. 현재 run_td는 memory-backend-ram만 쓰고 있어서 vhost-user 기반 공유 장치가 guest RAM을 볼 수 없고, 문서가 전제한 “host가 만든 공유 파일을 guest가 직접 mmap” 경로를 만들 수 없다. 그래서 QEMU 쪽에 virtiofsd + shared memfd RAM + virtio-fs device를 붙이
  는 쪽으로 구현해야 한다.

• 소스 자체에도 빌드 실패 후보가 보인다. tdx-dist-host/src/layout.c는 td_request_entry_t를 쓰는데 이 타입 정의를 직접 포함하지 않고 있고, guest/host ABI 헤더가 서로 얼마나 일치하는지도 점검이 필요하다. 지금부터는 실제 수정 범위를 확정하기 위해 host 트리와 guest 트리의 ABI 정의
  차이를 비교하겠다.

• 빌드 검증도 가능하게 만들 수는 있다. 이 머신에는 컴파일러가 없지만, 라이브러리 자체는 있을 가능성이 커서 임시 툴체인만 가져오면 재빌드가 가능하다. 다만 우선순위는 QEMU/guest 공유 경로 구현이므로, 그 뒤에 툴체인을 붙여 검증하겠다.

• 구현 방향을 고정했다.

  1. run_td에 virtio-fs 경로를 추가해서 host 디렉터리를 TDX guest에 안정적으로 노출한다.
  2. 이때 QEMU RAM 백엔드를 memory-backend-memfd,share=on으로 바꿔 vhost-user-fs가 요구하는 shared guest RAM 조건을 만족시킨다.
  3. tdx-dist/tdx-dist-host RDMA preset과 문서를 host 경로와 guest 마운트 경로 기준으로 다시 맞춘다.
  4. 가능한 범위에서 VM 재기동 후 guest mount까지 검증하고, C 빌드는 로컬 툴체인이 없어 정적 점검 중심으로 보완한다.

• 이제 파일을 고치겠다. 우선 run_td에 virtio-fs/virtiofsd 수명주기와 shared memory 백엔드 전환을 넣고, 그 다음 RDMA config와 README를 host-path/guest-mount-path 기준으로 정리한다.

• run_td 기본 동작을 너무 크게 바꾸면 다른 TDX 용도까지 깨질 수 있다. 그래서 virtio-fs는 기본 활성화 대신 명시적 옵션으로만 켜지도록 한 번 더 다듬겠다.

• 지금은 preset을 현실 경로로 맞춘다. host는 host 로컬 디렉터리의 backing file을 보고, guest MN은 virtio-fs로 마운트한 게스트 경로를 보도록 RDMA config를 분리하겠다.

• run_td 쪽은 문법 수준 검증부터 바로 하겠다. 이어서 로컬에 virtiofsd를 확보해서 실제 VM 재기동과 guest mount까지 가능한지 확인할 계획이다.

• 파이썬 문법 검증은 통과했다. 생성된 __pycache__만 정리하고, 다음은 virtiofsd를 확보해서 실제 VM을 새 옵션으로 띄워보는 단계로 넘어간다.

• 이제 남은 검증 병목은 virtiofsd 바이너리다. 로컬 패키지 관리자가 없어서 공식 Ubuntu 패키지에서 바이너리만 추출해 쓰는 방식으로 확보해보겠다.

• 호스트 쪽 검증을 진행한다. 기존 TD를 내리고 새 옵션으로 다시 띄운 뒤, QEMU 인자와 virtiofsd 프로세스를 확인해 shared-dir 경로가 실제로 붙는지 보겠다.

• QEMU 바이너리 탐색도 실제 환경 기준으로 보정해야 한다. 이 머신은 시스템 패키지 경로가 아니라 /home/seonung/qemu/build/qemu-system-x86_64에 로컬 빌드가 있어서, run_td가 그 경로까지 fallback 하도록 수정하겠다.

• 기존 TD 프로세스가 pidfile 없이 떠 있어서 run_td --clean으로는 정리되지 않았다. 포트와 리소스 충돌을 피하려면 현재 QEMU 프로세스를 직접 내리고 새 구성을 올려야 한다.

• QEMU 재기동 자체는 이 환경의 프로세스 권한 제약에 막혔다. 대신 새 코드에서 가장 위험한 부분인 virtiofsd 수명주기와 QEMU 인자 생성은 각각 독립적으로 검증하겠다.

• virtiofsd 버전 차이도 하나 잡혔다. 현재 확보한 1.10 바이너리는 비-root 실행에서 sandbox=chroot를 거부하므로, 기본 namespace sandbox로 바꾸는 편이 맞다. այդ걸 수정하고 다시 띄워보겠다.

• virtiofsd는 실제로 떠서 socket까지 만들었다. 남은 건 QEMU와의 결합 검증인데, 현재 실행 중인 기존 TD 프로세스를 이 세션에서 종료할 권한이 없어 거기서 막혀 있다. 우선 테스트용 virtiofsd는 정리하고, QEMU 인자 생성은 정적으로 확인하겠다.

• 검증용 virtiofsd를 guest-tools/bin/virtiofsd에 두었기 때문에 현재 tdx 저장소에 untracked 파일이 생긴 상태다. 런처가 그 위치를 자동 탐색하도록 해놨으니 남겨도 되지만, 작업 트리를 깔끔하게 유지하려면 .gitignore까지 맞추는 편이 낫다.

• 마무리 전에 참조 라인만 잡겠다. 최종 답변에는 실제 변경의 핵심 위치와, 검증이 어디까지 됐는지를 짧게 정리할 예정이다.

• run_td에 TDX용 virtio-fs 경로를 넣었다. --shared-dir를 주면 virtiofsd를 띄우고, QEMU RAM 백엔드를 memory-backend-memfd,share=on으로 바꾼 뒤 vhost-user-fs-pci를 붙인다. 동시에 virtiofsd와 로컬 QEMU 빌드 경로 탐색, pid/socket 정리, guest mount 안내까지 넣었다. 핵심은
  run_td:131, run_td:184, run_td:311, run_td:423다. 검증용 바이너리 경로는 guest-tools/bin/으로 잡았고 git 오염은 .gitignore:1로 막았다.

  tdx-dist 쪽 RDMA preset도 현실 경로로 나눴다. host는 /tmp/tdx-dist-share/..., guest MN은 /mnt/tdx-dist-share/...를 보도록 바꿨고, 문서도 “같은 path 문자열”이 아니라 “같은 backing file”을 가리키도록 정정했다. 참고: mn.rdma.conf:1, host.rdma.conf:1, host.rdma.conf:1,
  README:51, README:40, skill.md:73.

  검증은 부분적으로 했다. python3 -m py_compile은 통과했고, virtiofsd 1.10.0을 실제로 띄워 /tmp/tdx-virtiofsd.sock 생성까지 확인했다. 다만 전체 TD 재기동과 guest mount, tdx-dist/tdx-dist-host C 빌드는 끝까지 못 갔다. 이유는 현재 떠 있는 QEMU 프로세스를 이 세션에서 종료할 권한
  이 없었고, 이 머신에는 make/C compiler/dev header가 없다.

  다음 실행 순서는 이렇다.

  1. host에서 python3 /home/seonung/tdx/guest-tools/run_td --shared-dir /tmp/tdx-dist-share --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd
  2. guest에서 mkdir -p /mnt/tdx-dist-share && mount -t virtiofs tdx-dist-share /mnt/tdx-dist-share
  3. host에서 ./bin/host --config build/config/host.rdma.conf
  4. guest에서 ./bin/mn --config build/config/mn.rdma.conf
  5. CN에서 ./bin/cn --config build/config/cn.rdma.conf

  원하면 다음 턴에서 기존 TD를 어떻게 정리할지, 그리고 컴파일러 없이 이 C 트리 정합성을 더 밀어볼지 이어서 보겠다.


› seonung@simba:~/tdx/guest-tools$ python3 /home/seonung/tdx/guest-tools/run_td --shared-dir /tmp/tdx-dist-share --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd
  Clean VM
  Default image not found: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-24.04-generic.qcow2
  Using available TD image instead: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-25.04-generic.qcow2
  Run VM 32 vcpus 100G RAM
    Image: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-25.04-generic.qcow2
    virtio-fs share: /tmp/tdx-dist-share -> /mnt/tdx-dist-share (tag=tdx-dist-share)
  qemu-system-x86_64: -device vhost-user-fs-pci,chardev=char0,tag=tdx-dist-share: iommu_platform=true is not supported by the device

  이게 host 실행 결과임


• 오류 원인은 QEMU가 TDX guest에서 virtio 장치에 iommu_platform=on을 강제하는데, vhost-user-fs-pci가 그 플래그를 지원하지 않는 데 있다. 먼저 로컬 QEMU 소스와 해당 장치 옵션을 확인해서, 우회 가능한 속성 조합이 있는지 보고 바로 런처를 고치겠다.

• QEMU 소스상으로도 같은 증상을 재현하는 경로가 보인다. confidential guest일 때 virtio 장치 전반에 iommu_platform=on을 주입하는 코드와, vhost-user-fs-pci가 그 기능을 선언하지 않는 지점을 같이 확인하겠다.

• 장치별로만 iommu_platform=off를 주는 쪽이 가장 좁은 수정이다. confidential guest 전체의 virtio 정책은 유지하고, vhost-user-fs-pci만 예외 처리하겠다.

• 원인은 TDX confidential guest에서 QEMU가 virtio 장치에 iommu_platform=on을 기본 주입하는데, vhost-user-fs-pci는 그걸 지원하지 않기 때문이다. 로컬 런처를 수정해서 해당 장치에만 iommu_platform=off를 명시했다. 수정 위치는 run_td:361이고, 강제 주입 쪽 근거는 machine.c:1498, 에러
  발생 지점은 virtio-bus.c:94다.

  같은 명령으로 다시 실행하면 된다.

  python3 /home/seonung/tdx/guest-tools/run_td --shared-dir /tmp/tdx-dist-share --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd

  이 다음 에러가 나오면 그 출력 그대로 주면 된다. 다음 단계는 guest 안에서 virtiofs mount가 실제로 되는지 확인하는 것이다.


› seonung@simba:~/tdx/guest-tools$ ssh -p 10022 root@localhost
  (root@localhost) Password:
  Welcome to Ubuntu 25.04 (GNU/Linux 6.14.0-37-generic x86_64)

   * Documentation:  https://help.ubuntu.com
   * Management:     https://landscape.canonical.com
   * Support:        https://ubuntu.com/pro

   System information as of Thu Mar 19 11:18:27 UTC 2026

    System load:             0.14
    Usage of /:              3.5% of 99.24GB
    Memory usage:            0%
    Swap usage:              0%
    Processes:               376
    Users logged in:         1
    IPv4 address for enp0s1: 10.0.2.15
    IPv6 address for enp0s1: fec0::5054:ff:fe12:3456


  0 updates can be applied immediately.

  Your Ubuntu release is not supported anymore.
  For upgrade information, please visit:
  http://www.ubuntu.com/releaseendoflife

  New release '25.10' available.
  Run 'do-release-upgrade' to upgrade to it.



  The programs included with the Ubuntu system are free software;
  the exact distribution terms for each program are described in the
  individual files in /usr/share/doc/*/copyright.

  Ubuntu comes with ABSOLUTELY NO WARRANTY, to the extent permitted by
  applicable law.

  ===========================================================================
  Welcome to Ubuntu
  Created by Kobuk team
  ===========================================================================
  root@tdx-guest:~# ls
  root@tdx-guest:~# mkdir test
  root@tdx-guest:~# ls
  test
  root@tdx-guest:~# exit
  logout
  Connection to localhost closed.
  seonung@simba:~/tdx/guest-tools$ ssh -p 10022 root@localhost
  (root@localhost) Password:
  Welcome to Ubuntu 25.04 (GNU/Linux 6.14.0-37-generic x86_64)

   * Documentation:  https://help.ubuntu.com
   * Management:     https://landscape.canonical.com
   * Support:        https://ubuntu.com/pro

   System information as of Thu Mar 19 11:18:27 UTC 2026

    System load:             0.14
    Usage of /:              3.5% of 99.24GB
    Memory usage:            0%
    Swap usage:              0%
    Processes:               376
    Users logged in:         1
    IPv4 address for enp0s1: 10.0.2.15
    IPv6 address for enp0s1: fec0::5054:ff:fe12:3456


  0 updates can be applied immediately.

  Your Ubuntu release is not supported anymore.
  For upgrade information, please visit:
  http://www.ubuntu.com/releaseendoflife

  New release '25.10' available.
  Run 'do-release-upgrade' to upgrade to it.


  ===========================================================================
  Welcome to Ubuntu
  Created by Kobuk team
  ===========================================================================
  Last login: Thu Mar 19 11:18:28 2026 from 10.0.2.2
  root@tdx-guest:~# ls
  test
  root@tdx-guest:~# rm -rf test
  root@tdx-guest:~#

  진입 확인함. 이제 python3 재실행 없이 지금 생성된 이미지를 가지고 계속 ssh -p 10022 root@localhost만 진행하면서 다음 작업들 수행하고 해당 이미지에 소스코드 세팅할 거지?


• 맞다. 지금 VM이 살아 있으니 python3 .../run_td를 다시 실행할 필요 없이 계속 ssh -p 10022 root@localhost로 들어가서 작업하면 된다.

  정확히는 이렇다.

  - VM이 안 꺼지는 한 지금 세션을 계속 쓴다.
  - run_td는 VM을 새로 띄우거나 옵션을 바꿀 때만 다시 실행한다.
  - guest 안에서 만든 내용은 현재 qcow2 이미지에 남는다.
  - host와 guest가 같이 봐야 하는 파일은 guest 루트보다 virtio-fs 공유 경로에 두는 게 낫다.

  다음으로 바로 할 일은 guest 안에서 공유 디렉터리 마운트 확인이다.

  mkdir -p /mnt/tdx-dist-share
  mount -t virtiofs tdx-dist-share /mnt/tdx-dist-share
  ls -la /mnt/tdx-dist-share

  이게 되면 그다음부터는

  - host 쪽 tdx-dist-host/빌드 산출물은 host에서
  - guest 쪽 tdx-dist 소스/실행은 guest에서
    진행하면 된다.

  마운트 결과 보여주면 바로 다음 단계로 이어가겠다.


› run_td 진행하면 기존 vm에 작업한 소스코드 다 날아가는거 아님?


• 아니다. 현재 run_td는 기존 VM 프로세스를 종료하고 다시 띄울 뿐, qcow2 이미지를 지우거나 초기화하지 않는다.

  근거:

  - do_clean()은 pidfile 기준으로 QEMU/virtiofsd를 종료만 한다. run_td:119
  - 실제 부팅은 같은 이미지 파일을 -drive file=...로 다시 붙인다. run_td:351
  - 기본 이미지도 매번 새로 만드는 게 아니라 기존 qcow2 경로를 고른다. run_td:52

  즉:

  - 같은 이미지로 run_td를 다시 실행하면 guest 내부에 저장한 소스는 보통 그대로 남는다.
  - 날아가는 경우는 이미지 파일 자체를 새로 만들었거나 교체했을 때다.
  - 더 안전하게 하려면 작업 소스는 guest 루트보다 host와 공유되는 virtio-fs 경로(/mnt/tdx-dist-share)에 두는 게 낫다.

  그래서 지금은 계속 ssh -p 10022 root@localhost로 들어가서 이어 작업하면 된다.


› 1. run_td에 virtio-fs 경로를 추가해서 host 디렉터리를 TDX guest에 안정적으로 노출한다.
    2. 이때 QEMU RAM 백엔드를 memory-backend-memfd,share=on으로 바꿔 vhost-user-fs가 요구하는 shared guest RAM 조건을 만족시킨다.
    3. tdx-dist/tdx-dist-host RDMA preset과 문서를 host 경로와 guest 마운트 경로 기준으로 다시 맞춘다.
    4. 가능한 범위에서 VM 재기동 후 guest mount까지 검증하고, C 빌드는 로컬 툴체인이 없어 정적 점검 중심으로 보완한다.

  run_td에 virtio-fs/virtiofsd

    1. host에서 python3 /home/seonung/tdx/guest-tools/run_td --shared-dir /tmp/tdx-dist-share --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd
    2. guest에서 mkdir -p /mnt/tdx-dist-share && mount -t virtiofs tdx-dist-share /mnt/tdx-dist-share
    5. CN에서 ./bin/cn --config build/config/cn.rdma.conf




  seonung@simba:~/tdx/guest-tools$ ssh -p 10022 root@localhost
  (root@localhost) Password:
  Welcome to Ubuntu 25.04 (GNU/Linux 6.14.0-37-generic x86_64)

   * Documentation:  https://help.ubuntu.com
   * Management:     https://landscape.canonical.com
   * Support:        https://ubuntu.com/pro

   System information as of Thu Mar 19 11:18:27 UTC 2026

    System load:             0.14
    Usage of /:              3.5% of 99.24GB
    Memory usage:            0%
    Swap usage:              0%
    Processes:               376
    Users logged in:         1
    IPv4 address for enp0s1: 10.0.2.15
    IPv6 address for enp0s1: fec0::5054:ff:fe12:3456


  0 updates can be applied immediately.

  Your Ubuntu release is not supported anymore.
  For upgrade information, please visit:
  http://www.ubuntu.com/releaseendoflife

  New release '25.10' available.
  Run 'do-release-upgrade' to upgrade to it.



  The programs included with the Ubuntu system are free software;
  the exact distribution terms for each program are described in the
  individual files in /usr/share/doc/*/copyright.

  Ubuntu comes with ABSOLUTELY NO WARRANTY, to the extent permitted by
  applicable law.

  ===========================================================================
  Welcome to Ubuntu
  Created by Kobuk team
  ===========================================================================
  root@tdx-guest:~# ls
  root@tdx-guest:~# mkdir test
  root@tdx-guest:~# ls
  test
  root@tdx-guest:~# exit
  logout
  Connection to localhost closed.
  seonung@simba:~/tdx/guest-tools$ ssh -p 10022 root@localhost
  (root@localhost) Password:
  Welcome to Ubuntu 25.04 (GNU/Linux 6.14.0-37-generic x86_64)

   * Documentation:  https://help.ubuntu.com
   * Management:     https://landscape.canonical.com
   * Support:        https://ubuntu.com/pro

   System information as of Thu Mar 19 11:18:27 UTC 2026

    System load:             0.14
    Usage of /:              3.5% of 99.24GB
    Memory usage:            0%
    Swap usage:              0%
    Processes:               376
    Users logged in:         1
    IPv4 address for enp0s1: 10.0.2.15
    IPv6 address for enp0s1: fec0::5054:ff:fe12:3456


  0 updates can be applied immediately.

  Your Ubuntu release is not supported anymore.
  For upgrade information, please visit:
  http://www.ubuntu.com/releaseendoflife

  New release '25.10' available.
  Run 'do-release-upgrade' to upgrade to it.


  ===========================================================================
  Welcome to Ubuntu
  Created by Kobuk team
  ===========================================================================
  Last login: Thu Mar 19 11:18:28 2026 from 10.0.2.2
  root@tdx-guest:~# ls
  test
  root@tdx-guest:~# rm -rf test
  root@tdx-guest:~# ㅣㄴ
  ㅣㄴ: command not found
  root@tdx-guest:~# ls
  root@tdx-guest:~# mkdir test
  root@tdx-guest:~# ls
  test
  root@tdx-guest:~# exit
  logout
  Connection to localhost closed.
  seonung@simba:~/tdx/guest-tools$ python3 /home/seonung/tdx/guest-tools/run_td --shared-dir /tmp/tdx-dist-share --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd
  Clean VM
  Clean virtiofsd
  Default image not found: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-24.04-generic.qcow2
  Using available TD image instead: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-25.04-generic.qcow2
  Run VM 32 vcpus 100G RAM
    Image: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-25.04-generic.qcow2
    virtio-fs share: /tmp/tdx-dist-share -> /mnt/tdx-dist-share (tag=tdx-dist-share)
  TD started by QEMU with PID: 1398994.
  To log in with the non-root user (default: tdx / password: 123456), as specified in setup-tdx-config, use:
     $ ssh -p 10022 <username>@localhost
  To log in as root (default password: 123456), use:
     $ ssh -p 10022 root@localhost
  If SSH reports a changed host key for this port, remove only the stale entry with:
     $ ssh-keygen -f ~/.ssh/known_hosts -R '[localhost]:10022'
  This TD also exports a host directory through virtio-fs for tdx-dist shared memory:
     Host shared dir: /tmp/tdx-dist-share
     Guest mount tag: tdx-dist-share
  Inside the guest, mount it with:
     # mkdir -p /mnt/tdx-dist-share
     # mount -t virtiofs tdx-dist-share /mnt/tdx-dist-share
  seonung@simba:~/tdx/guest-tools$ ssh -p 10022 root@localhost
  (root@localhost) Password:
  Welcome to Ubuntu 25.04 (GNU/Linux 6.14.0-37-generic x86_64)

   * Documentation:  https://help.ubuntu.com
   * Management:     https://landscape.canonical.com
   * Support:        https://ubuntu.com/pro

   System information as of Thu Mar 19 11:21:48 UTC 2026

    System load:             0.11
    Usage of /:              3.5% of 99.24GB
    Memory usage:            0%
    Swap usage:              0%
    Processes:               378
    Users logged in:         1
    IPv4 address for enp0s1: 10.0.2.15
    IPv6 address for enp0s1: fec0::5054:ff:fe12:3456


  0 updates can be applied immediately.

  For upgrade information, please visit:
  http://www.ubuntu.com/releaseendoflife

  New release '25.10' available.
  Run 'do-release-upgrade' to upgrade to it.


  ===========================================================================
  Welcome to Ubuntu
  Created by Kobuk team
  ===========================================================================
  Last login: Thu Mar 19 11:18:41 2026 from 10.0.2.2
  root@tdx-guest:~# ls
  root@tdx-guest:~# ls


■ Conversation interrupted - tell the model what to do differently. Something went wrong? Hit `/feedback` to report the issue.


› 1. run_td에 virtio-fs 경로를 추가해서 host 디렉터리를 TDX guest에 안정적으로 노출한다.
    2. 이때 QEMU RAM 백엔드를 memory-backend-memfd,share=on으로 바꿔 vhost-user-fs가 요구하는 shared guest RAM 조건을 만족시킨다.
    3. tdx-dist/tdx-dist-host RDMA preset과 문서를 host 경로와 guest 마운트 경로 기준으로 다시 맞춘다.
    4. 가능한 범위에서 VM 재기동 후 guest mount까지 검증하고, C 빌드는 로컬 툴체인이 없어 정적 점검 중심으로 보완한다.

  run_td에 virtio-fs/virtiofsd

    1. host에서 python3 /home/seonung/tdx/guest-tools/run_td --shared-dir /tmp/tdx-dist-share --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd
    2. guest에서 mkdir -p /mnt/tdx-dist-share && mount -t virtiofs tdx-dist-share /mnt/tdx-dist-share
    5. CN에서 ./bin/cn --config build/config/cn.rdma.conf




  seonung@simba:~/tdx/guest-tools$ ssh -p 10022 root@localhost
  (root@localhost) Password:
  Welcome to Ubuntu 25.04 (GNU/Linux 6.14.0-37-generic x86_64)

   * Documentation:  https://help.ubuntu.com
   * Management:     https://landscape.canonical.com
   * Support:        https://ubuntu.com/pro

   System information as of Thu Mar 19 11:18:27 UTC 2026

    System load:             0.14
    Usage of /:              3.5% of 99.24GB
    Memory usage:            0%
    Swap usage:              0%
    Processes:               376
    Users logged in:         1
    IPv4 address for enp0s1: 10.0.2.15
    IPv6 address for enp0s1: fec0::5054:ff:fe12:3456


  0 updates can be applied immediately.

  Your Ubuntu release is not supported anymore.
  For upgrade information, please visit:
  http://www.ubuntu.com/releaseendoflife

  New release '25.10' available.
  Run 'do-release-upgrade' to upgrade to it.



  The programs included with the Ubuntu system are free software;
  the exact distribution terms for each program are described in the
  individual files in /usr/share/doc/*/copyright.

  Ubuntu comes with ABSOLUTELY NO WARRANTY, to the extent permitted by
  applicable law.

  ===========================================================================
  Welcome to Ubuntu
  Created by Kobuk team
  ===========================================================================
  root@tdx-guest:~# ls
  root@tdx-guest:~# mkdir test
  root@tdx-guest:~# ls
  test
  root@tdx-guest:~# exit
  logout
  Connection to localhost closed.
  seonung@simba:~/tdx/guest-tools$ ssh -p 10022 root@localhost
  (root@localhost) Password:
  Welcome to Ubuntu 25.04 (GNU/Linux 6.14.0-37-generic x86_64)

   * Documentation:  https://help.ubuntu.com
   * Management:     https://landscape.canonical.com
   * Support:        https://ubuntu.com/pro

   System information as of Thu Mar 19 11:18:27 UTC 2026

    System load:             0.14
    Usage of /:              3.5% of 99.24GB
    Memory usage:            0%
    Swap usage:              0%
    Processes:               376
    Users logged in:         1
    IPv4 address for enp0s1: 10.0.2.15
    IPv6 address for enp0s1: fec0::5054:ff:fe12:3456


  0 updates can be applied immediately.

  Your Ubuntu release is not supported anymore.
  For upgrade information, please visit:
  http://www.ubuntu.com/releaseendoflife

  New release '25.10' available.
  Run 'do-release-upgrade' to upgrade to it.


  ===========================================================================
  Welcome to Ubuntu
  Created by Kobuk team
  ===========================================================================
  Last login: Thu Mar 19 11:18:28 2026 from 10.0.2.2
  root@tdx-guest:~# ls
  test
  root@tdx-guest:~# rm -rf test
  root@tdx-guest:~# ㅣㄴ
  ㅣㄴ: command not found
  root@tdx-guest:~# ls
  root@tdx-guest:~# mkdir test
  root@tdx-guest:~# ls
  test
  root@tdx-guest:~# exit
  logout
  Connection to localhost closed.
  seonung@simba:~/tdx/guest-tools$ python3 /home/seonung/tdx/guest-tools/run_td --shared-dir /tmp/tdx-dist-share --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd
  Clean VM
  Clean virtiofsd
  Default image not found: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-24.04-generic.qcow2
  Using available TD image instead: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-25.04-generic.qcow2
  Run VM 32 vcpus 100G RAM
    Image: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-25.04-generic.qcow2
    virtio-fs share: /tmp/tdx-dist-share -> /mnt/tdx-dist-share (tag=tdx-dist-share)
  TD started by QEMU with PID: 1398994.
  To log in with the non-root user (default: tdx / password: 123456), as specified in setup-tdx-config, use:
     $ ssh -p 10022 <username>@localhost
  To log in as root (default password: 123456), use:
     $ ssh -p 10022 root@localhost
  If SSH reports a changed host key for this port, remove only the stale entry with:
     $ ssh-keygen -f ~/.ssh/known_hosts -R '[localhost]:10022'
  This TD also exports a host directory through virtio-fs for tdx-dist shared memory:
     Host shared dir: /tmp/tdx-dist-share
     Guest mount tag: tdx-dist-share
  Inside the guest, mount it with:
     # mkdir -p /mnt/tdx-dist-share
     # mount -t virtiofs tdx-dist-share /mnt/tdx-dist-share
  seonung@simba:~/tdx/guest-tools$ ssh -p 10022 root@localhost
  (root@localhost) Password:
  Welcome to Ubuntu 25.04 (GNU/Linux 6.14.0-37-generic x86_64)

   * Documentation:  https://help.ubuntu.com
   * Management:     https://landscape.canonical.com
   * Support:        https://ubuntu.com/pro

   System information as of Thu Mar 19 11:21:48 UTC 2026

    System load:             0.11
    Usage of /:              3.5% of 99.24GB
    Memory usage:            0%
    Swap usage:              0%
    Processes:               378
    Users logged in:         1
    IPv4 address for enp0s1: 10.0.2.15
    IPv6 address for enp0s1: fec0::5054:ff:fe12:3456


  0 updates can be applied immediately.
  For upgrade information, please visit:
  http://www.ubuntu.com/releaseendoflife

  New release '25.10' available.
  Run 'do-release-upgrade' to upgrade to it.


  ===========================================================================
  Welcome to Ubuntu
  Created by Kobuk team
  ===========================================================================
  Last login: Thu Mar 19 11:18:41 2026 from 10.0.2.2
  root@tdx-guest:~# ls
  root@tdx-guest:~# ls
  이걸 보면 알수 있다시피, python3 /home/seonung/tdx/guest-tools/run_td --shared-dir /tmp/tdx-dist-share --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd를 재실행할때마다 vm 내 공간이 초기화됨


• 맞다. 지금 로그 기준으로 보면 run_td를 다시 실행할 때 guest 내부 작업 공간은 초기화된다고 봐야 한다. 방금 /root/test가 재기동 뒤 사라진 걸로 이미 확인됐다. 이전에 내가 “같은 qcow2면 남는다”고 본 건 이 환경에는 맞지 않았다.

  그래서 운영 방식을 바꿔야 한다.

  - guest 루트(/root, /home 등)에는 소스 두지 않는다.
  - 살아 있는 동안만 필요한 임시 작업만 guest 내부에 둔다.
  - 유지돼야 하는 소스와 산출물은 host 쪽 /tmp/tdx-dist-share에 두고, guest에서는 /mnt/tdx-dist-share로 마운트해서 거기서 작업한다.
  - run_td는 정말 재기동이 필요할 때만 다시 실행한다.

  즉 다음 기준으로 가면 된다.

  이제부터는 tdx-dist 소스도 guest 이미지 안에 세팅하는 게 아니라 shared dir에 두고 진행하는 쪽으로 맞추자. 원하면 다음 턴에서 바로

  1. guest에서 virtio-fs mount 확인
  2. host의 tdx-dist 소스를 shared dir에 배치할 방법 정리
  3. guest에서 그 경로로 빌드/실행 동선 정리
     순서로 이어가겠다.


› tdx-dist/skill.md를 보면 알겠지만, host는 하는 일이 오로지 rdma 받기 + rdma 버퍼공간 tdx의 shared memory랑 공유하기 이 두가지만 있어야 하고, mn은 tdx에서 돌아야 함.
  때문에 run_td를 재실행하더라도 내부 공간이 초기화되지 않도록 image가 필요함. (이때 image를 써도 tdx 성질은 당연히 유지되어야 함)


• 이미지 재기동 뒤 guest 내부 상태가 사라지는 원인을 먼저 확인하겠다. run_td 자체는 -snapshot을 쓰지 않으니, 초기화가 맞다면 이미지 생성 방식이나 guest 쪽 초기화 로직이 원인일 가능성이 크다.

• Explored
  └ Read user-data, create-td-image.sh

• Ran qemu-img info /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-25.04-generic.qcow2
  └ /bin/bash: line 1: qemu-img: command not found

• Explored
  └ Read README.md

──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 지금 요구사항 기준으로는 run_td가 “항상 같은 writable guest image”를 쓰도록 강제하는 편이 맞다. 원인 분석만으로 끝내지 않고, base qcow2 위에 persistent overlay를 만들어 재기동해도 guest 내부 변경이 남도록 런처를 보강하겠다.

• Explored
  └ Search qemu-img in qemu
    Read create-td-image.sh

• Ran git -C /home/seonung/tdx status --short
  └  M .gitignore
     M guest-tools/run_td

──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 이미지 보존을 런처 레벨에서 보장하려면, base qcow2를 직접 부팅하는 대신 persistent overlay qcow2를 항상 재사용하게 만드는 게 가장 안전하다. 그 경로를 run_td에 넣고, 재기동해도 같은 writable overlay를 쓰도록 바꾸겠다.

• Ran /home/seonung/qemu/build/qemu-img --version
  └ /home/seonung/qemu/build/qemu-img: error while loading shared libraries: libnuma.so.1: cannot open shared object file: No such file or directory

• Explored
  └ Read run_td

──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 재기동해도 guest 내부 변경이 남도록 run_td가 base image를 직접 쓰지 않고, persistent writable qcow2 복제본을 자동 재사용하게 만들겠다. 사용자 명령은 그대로 두고 동작만 stateful 쪽으로 바꾸는 게 맞다.

• Edited tdx/guest-tools/run_td (+50 -3)
     39  default_shared_mount='/mnt/tdx-dist-share'
     40 +default_runtime_dir=os.path.join(file_path, 'image', 'runtime')
     41
        ⋮
     70
     71 +def stateful_image_path(base_image):
     72 +    base_dir = os.path.dirname(base_image)
     73 +    base_name = os.path.basename(base_image)
     74 +    stem, ext = os.path.splitext(base_name)
     75 +
     76 +    if ext == '':
     77 +        ext = '.qcow2'
     78 +    return os.path.join(default_runtime_dir, f'{stem}.stateful{ext}')
     79 +
     80  ubuntu_version=normalize_ubuntu_version(
        ⋮
    193
    194 +def prepare_runtime_image(base_image, runtime_image, reset_runtime_image):
    195 +    if os.path.realpath(base_image) == os.path.realpath(runtime_image):
    196 +        return base_image
    197 +
    198 +    os.makedirs(os.path.dirname(runtime_image), exist_ok=True)
    199 +
    200 +    if reset_runtime_image and os.path.exists(runtime_image):
    201 +        os.remove(runtime_image)
    202 +
    203 +    if not os.path.exists(runtime_image):
    204 +        print(f'Create writable runtime image: {runtime_image}')
    205 +        try:
    206 +            subprocess.run(
    207 +                ['cp', '--sparse=always', '--reflink=auto', base_image, runtime_image],
    208 +                check=True,
    209 +                stderr=subprocess.STDOUT,
    210 +            )
    211 +        except Exception:
    212 +            shutil.copy2(base_image, runtime_image)
    213 +
    214 +    return runtime_image
    215 +
    216  def start_virtiofsd(shared_dir, virtiofsd_path):
        ⋮
    342
    311 -def do_run(img_path, vcpus, mem, gpus, shared_dir, shared_tag, shared_mount, virtiofsd_path):
    343 +def do_run(img_path, runtime_img_path, vcpus, mem, gpus, shared_dir, shared_tag, shared_mount, virtiofsd_path):
    344      qemu_bin = find_qemu_bin()
        ⋮
    355      print(f'Run VM {vcpus} vcpus {mem} RAM')
    324 -    print(f'  Image: {img_path}')
    356 +    print(f'  Base image: {img_path}')
    357 +    print(f'  Writable image: {runtime_img_path}')
    358      if len(gpus):
        ⋮
    383          '-netdev', f'user,id=nic0_td,hostfwd=tcp::{ssh_port}-:22',
    351 -        '-drive', f'file={img_path},if=none,id=virtio-disk0',
    384 +        '-drive', f'file={runtime_img_path},if=none,id=virtio-disk0',
    385          '-device', 'virtio-blk-pci,drive=virtio-disk0',
        ⋮
    436      virtiofsd_path = None
    437 +    runtime_img = args.runtime_image if args.runtime_image else stateful_image_path(td_img)
    438 +
    439 +    runtime_img = prepare_runtime_image(td_img, runtime_img, args.reset_runtime_image)
    440
    451          td_vcpus,
        ⋮
    462     parser.add_argument("--image", type=str, help="Guest image")
    463 +   parser.add_argument(
    464 +       "--runtime-image",
    465 +       type=str,
    466 +       help="Writable qcow2 image to boot. If omitted, run_td creates and reuses image/runtime/<base>.stateful.qcow2.",
    467 +   )
    468 +   parser.add_argument(
    469 +       "--reset-runtime-image",
    470 +       action='store_true',
    471 +       help="Recreate the writable runtime image from the base image before booting.",
    472 +   )
    473     parser.add_argument("--vcpus", type=str, help="Number of VCPUs. 32 by default.", default='32')

• Edited tdx/.gitignore (+1 -0)
    7  guest-tools/bin/
    8 +guest-tools/image/runtime/

──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 런처 옵션만 바꾸는 것보다 동작 계약도 문서에 남겨두는 편이 낫다. run_td가 이제 persistent writable image를 재사용한다는 점을 README에도 짧게 적겠다.

• Edited tdx/README.md (+3 -0)
    230
    231 +`run_td` reuses a writable runtime qcow2 under `guest-tools/image/runtime/` so guest-local changes survive reboot/restart by default.
    232 +To discard guest-local changes and rebuild that writable image from the base image, pass `--reset-runtime-image`.
    233 +
    234  An example output:

──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 문법 확인만 하고 끝내겠다. 이후 검증은 사용자가 같은 run_td 명령으로 재기동해서 guest 내부 파일이 유지되는지 확인하면 된다.

──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 맞다. 요구사항상 run_td 재실행 후에도 guest 내부 상태가 남아야 하고, 그걸 위해 persistent writable image를 쓰는 게 맞다. 그래서 run_td를 base qcow2 직접 부팅이 아니라, 재사용되는 writable qcow2를 자동으로 만들어 부팅하도록 바꿨다. 구현은 run_td:71,
  run_td:194, run_td:343, run_td:416에 있다.

  이제 같은 명령을 다시 써도 된다.

  python3 /home/seonung/tdx/guest-tools/run_td \
    --shared-dir /tmp/tdx-dist-share \
    --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd

  동작은 이렇게 바뀐다.

  - base image: 기존 tdx-guest-ubuntu-25.04-generic.qcow2
  - writable image: guest-tools/image/runtime/<base>.stateful.qcow2
  - 재기동 시: 그 writable image를 계속 재사용
  - 초기화가 필요할 때만: --reset-runtime-image

  즉 guest 안에 만든 파일은 이제 stateful qcow2에 남아야 한다. 이건 TDX 성질을 깨지 않는다. 바뀌는 건 디스크 백킹 파일 방식이고, TDX의 confidentiality/measurement 대상인 guest 메모리 모델은 그대로다.

  문서도 반영했다. README:231. runtime 이미지는 git에서 제외했다. .gitignore:1

  다음 확인만 하면 된다.

  1. run_td 재실행 후 출력에 Writable image: ...stateful.qcow2가 보이는지 확인
  2. guest에서 /root/test 같은 파일 생성
  3. 다시 run_td 실행
  4. guest 재접속 후 파일이 남아있는지 확인

  그 결과를 주면 바로 다음 단계로 넘어가겠다.


› seonung@simba:~/tdx/guest-tools$ python3 /home/seonung/tdx/guest-tools/run_td     --shared-dir /tmp/tdx-dist-share
  --virtiofsd /home/seonung/tdx/guest-tools/bin/virtiofsd
  Clean VM
  Clean virtiofsd
  Default image not found: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-24.04-generic.qcow2
  Using available TD image instead: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-25.04-generic.qcow2
  Run VM 32 vcpus 100G RAM
    Base image: /home/seonung/tdx/guest-tools/image/tdx-guest-ubuntu-25.04-generic.qcow2
    Writable image: /home/seonung/tdx/guest-tools/image/runtime/tdx-guest-ubuntu-25.04-generic.stateful.qcow2
    virtio-fs share: /tmp/tdx-dist-share -> /mnt/tdx-dist-share (tag=tdx-dist-share)
  TD started by QEMU with PID: 1403594.
  To log in with the non-root user (default: tdx / password: 123456), as specified in setup-tdx-config, use:
     $ ssh -p 10022 <username>@localhost
  To log in as root (default password: 123456), use:
     $ ssh -p 10022 root@localhost
  If SSH reports a changed host key for this port, remove only the stale entry with:
     $ ssh-keygen -f ~/.ssh/known_hosts -R '[localhost]:10022'
  This TD also exports a host directory through virtio-fs for tdx-dist shared memory:
     Host shared dir: /tmp/tdx-dist-share
     Guest mount tag: tdx-dist-share
  Inside the guest, mount it with:
     # mkdir -p /mnt/tdx-dist-share
     # mount -t virtiofs tdx-dist-share /mnt/tdx-dist-share
  seonung@simba:~/tdx/guest-tools$ ssh -p 10022 root@localhost
  (root@localhost) Password:
  Welcome to Ubuntu 25.04 (GNU/Linux 6.14.0-37-generic x86_64)

    Memory usage:            0%
    Swap usage:              0%
    Processes:               375
    Users logged in:         1
    IPv4 address for enp0s1: 10.0.2.15

  0 updates can be applied immediately.


  Created by Kobuk team
  ===========================================================================
  Last login: Thu Mar 19 12:02:39 2026 from 10.0.2.2
  root@tdx-guest:~# ls
  root@tdx-guest:~# ls
  program, or other error.
         dmesg(1) may have more information after failed mount system call.
  tdx vm 내에서 virtiofs 관련 문제가 발생

  2. python3 run_td 재실행 후 tdx vm 내에서 test dir 재관측 불가


• Searched site:qemu.org virtiofs confidential guest iommu_platform vhost-user-fs-pci

• Searched https://gitlab.com/qemu-project/qemu/-/issues/2956

• Searched site:gitlab.com/qemu-project/qemu virtiofs confidential guest supported

─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 핵심은 두 가지로 분리된다.

  1. test 디렉터리가 안 보인 건 지금 첫 번째 stateful 이미지가 base에서 새로 만들어졌기 때문일 가능성이 크다. 이전 부팅에서
     만든 /root/test는 새 writable image 생성 전에 사라진 상태였다.
  2. 더 큰 문제는 virtio-fs 자체가 TDX guest에서 실제로 mount되지 않는 점이다. 이건 단순 옵션 실수보다 upstream/QEMU 제약일
     가능성이 높아서, 로컬 QEMU 소스와 공식 자료 기준으로 바로 확인하겠다.

• Explored
  └ Read vhost-user-fs.c
    Search vhost-user-fs.*mmio|virtiofs.*mmio|TYPE_VHOST_USER_FS.*MMIO|vhost-user-fs-device|vhost-user-fs-mmio|
TYPE_VHOST_USER_FS
           in hw
    Search guest_memfd|memory-backend-memfd|iommu_platform=off|vhost-user-fs-pci in docs

• Searched virtiofsd VIRTIO_F_IOMMU_PLATFORM support confidential guest

• Searched virtiofsd iommu platform support release notes

─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 문제를 분리해서 보겠다. virtio-fs mount 실패는 guest 커널/장치 인식 상태를 직접 확인해야 하고, 그건 현재 VM에 SSH로 들어가
  서 보는 게 가장 빠르다. 동시에 stateful 이미지가 실제로 재사용되는지 파일 수준도 같이 보겠다.

✔ You approved codex to always run commands that start with ssh -o 'StrictHostKeyChecking=no' -p 10022 root@localhost

• Ran ssh -o StrictHostKeyChecking=no -p 10022 root@localhost
  └ /bin/bash: line 1: /usr/bin/ssh: Permission denied

─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• guest 내부 상태를 직접 보겠다. virtio-fs가 커널에서 인식되는지, mount 실패 시 dmesg가 뭘 말하는지 먼저 확인한다.

• Ran ssh -o StrictHostKeyChecking=no -p 10022 root@localhost
  └ /bin/bash: line 1: /usr/bin/ssh: Permission denied

• Ran ssh -o StrictHostKeyChecking=no -p 10022 root@localhost
  └ /bin/bash: line 1: /usr/bin/ssh: Permission denied

─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 이 세션에서는 ssh 바이너리 실행이 막혀서 guest를 직접 들여다보진 못한다. 대신 host 쪽에서 실제로 어떤 이미지가 물려 있는지
  확인하고, guest에서 바로 확인할 최소 명령만 정리하겠다.

• Ran ps -ef | rg 'qemu-system-x86_64|tdx-guest-ubuntu-25.04-generic.stateful'
  └ seonung  1403594       1 99 12:03 ?        00:20:15 /usr/bin/qemu-system-x86_64 -accel kvm -m 100G -smp 32 -name
    td,process=td,debug-threads=on -cpu host -object {"qom-type":"tdx-guest","id":"tdx","quote-generation-socket":{"type":
    … +8 lines
    tdx-guest-ubuntu-25.04-generic.stateful'
    seonung  1404899 1404897  0 12:08 ?        00:00:00 rg qemu-system-x86_64|tdx-guest-ubuntu-25.04-generic.stateful

• Ran sed -n '1,120p' /tmp/tdx-virtiofsd.log 2>/dev/null
  └ error: invalid value 'none' for '--cache <CACHE>': invalid cache policy

    … +5 lines
    thread '[2026-03-19T11:09:03Z INFO  virtiofsd] Waiting for vhost-user socket connection...
    [2026-03-19T11:09:30Z INFO  virtiofsd] Waiting for vhost-user socket connection...

• Ran stat /home/seonung/tdx/guest-tools/image/runtime/tdx-guest-ubuntu-25.04-generic.stateful.qcow2 2>/dev/null
  └   File: /home/seonung/tdx/guest-tools/image/runtime/tdx-guest-ubuntu-25.04-generic.stateful.qcow2
      Size: 4099735552    Blocks: 7877392    IO Block: 4096   regular file
    … +4 lines
    Change: 2026-03-19 12:06:32.985010627 +0000
     Birth: 2026-03-19 12:01:17.512050283 +0000

• Ran ls -l /home/seonung/tdx/guest-tools/image/runtime /home/seonung/tdx/guest-tools/image/runtime/
  │ tdx-guest-ubuntu-25.04-generic.stateful.qcow2 2>/dev/null
  └ -rwxrwxr-x 1 seonung seonung 4099735552 Mar 19 12:06 /home/seonung/tdx/guest-tools/image/runtime/
    tdx-guest-ubuntu-25.04-generic.stateful.qcow2
    … +2 lines
    total 3938696
    -rwxrwxr-x 1 seonung seonung 4099735552 Mar 19 12:06 tdx-guest-ubuntu-25.04-generic.stateful.qcow2

─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 맞다. 지금 상태를 보면 두 문제는 성격이 다르다.

  첫째, test가 안 보인 건 아직 “실패”로 단정할 수 없다. 현재 QEMU는 실제로 writable stateful image를 붙여서 부팅 중이다. host
  에서 확인된 실행 인자는 qemu cmd:384와 맞게 /home/seonung/tdx/guest-tools/image/runtime/tdx-guest-ubuntu-25.04-
  generic.stateful.qcow2를 쓰고 있다. 즉 stateful 디스크 자체는 적용됐다. 다만 네가 본 /root/test는 stateful 이미지 적용 전에
  만든 데이터라서, 이번 첫 부팅에서 없는 게 이상하진 않다. 지금부터 한 번 더 검증하면 된다.
  guest:

  mkdir -p /root/test
  sync

  그 다음 host에서 같은 run_td를 다시 실행하고, 다시 guest 접속해서:

  ls /root

  이걸로 persistence는 깔끔하게 판별된다.

  둘째, 더 중요한 건 virtio-fs다. 이건 현재 스택에서 사실상 막혀 있다고 보는 게 맞다. 근거는 세 가지다.

  - QEMU confidential guest 경로는 virtio 장치에 iommu_platform=on을 강제한다. machine.c:1491
  - 그 상태에서 네가 처음 본 에러가 났다: iommu_platform=true is not supported by the device. 이 체크는 virtio-bus.c:94다.
  - 내가 우회로 iommu_platform=off를 넣어 QEMU는 띄웠지만, 그 뒤 guest에서 mount가 실패했다. 이건 “부팅만 되게 만든 것”이지,
    confidential guest에서 필요한 DMA/IOMMU 경로가 성립하지 않았다는 쪽과 일치한다. upstream에서도 같은 계열 문제가 보고돼
    있다: QEMU issue #2956 (https://gitlab.com/qemu-project/qemu/-/work_items/2956), QEMU confidential guest docs
    (https://www.qemu.org/docs/master/system/confidential-guest-support.html).

  결론은 이렇다.

  - run_td의 persistent image 요구사항은 맞고, 그 방향으로 수정은 들어갔다.
  - 하지만 host 디렉터리를 TDX guest에 virtio-fs로 붙여 shared backing file로 쓰는 방식은 현재 QEMU + virtiofsd 조합에선 신뢰
    할 수 없다.
  - 따라서 최종 목표인 “host는 RDMA + shared buffer만 담당, MN은 TDX 내부에서 동작”을 유지하려면, virtio-fs 대신 다른 shared-
    memory 메커니즘으로 설계를 바꿔야 한다.

  지금 가장 합리적인 다음 단계는 둘 중 하나다.

  1. stateful image persistence만 먼저 확정하고, guest 내부에 tdx-dist 소스를 두고 개발을 계속한다.
  2. host/guest shared memory는 virtio-fs를 버리고 TDX에서 실제로 성립하는 다른 메커니즘으로 재설계한다.

  원하면 다음 턴에서 바로 tdx-dist/skill.md 기준으로, virtio-fs를 제거했을 때 host-TDX shared buffer를 어떤 방식으로 다시 잡
  아야 하는지 설계안부터 정리하겠다.