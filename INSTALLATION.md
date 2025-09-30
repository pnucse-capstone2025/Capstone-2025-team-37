# 설치 및 사용법

본 프로젝트는 Ubuntu 20.04 LTS에서 개발되었으며, 다음 단계를 통해 설치 및 실행할 수 있습니다.

## 환경 요구사항
- **Building Machine**: Ubuntu 20.04 LTS
- **Chaincode_Wrapper Machine**: Ubuntu 18.04.6 LTS  
- **NXP MCIMX8M-EVK** (Sandisk Extreme Pro microSD A2 장착)

## 설치 과정

### 0단계: 레포지터리 클론 및 서브모듈 초기화

```bash
# 레포지터리 클론
git clone <YOUR_REPOSITORY_URL>
cd <YOUR_REPOSITORY_NAME>

# 서브모듈 초기화 및 업데이트
git submodule update --init --recursive
```

### 1단계: Building Machine 준비

참고: [WaTZ benchmarks README](https://github.com/JamesMenetrey/unine-watz/blob/main/benchmarks/README.md)

```bash
# 기본 패키지 설치
sudo apt-get install -y git

# OP-TEE 의존성 설치
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install android-tools-adb android-tools-fastboot autoconf \
        automake bc bison build-essential ccache cscope curl device-tree-compiler \
        expect flex ftp-upload gdisk iasl libattr1-dev libcap-dev \
        libfdt-dev libftdi-dev libglib2.0-dev libhidapi-dev libncurses5-dev \
        libpixman-1-dev libssl-dev libtool make \
        mtools netcat ninja-build python-crypto python3-crypto python-pyelftools \
        python3-pycryptodome python3-pyelftools python3-serial \
        rsync unzip uuid-dev xdg-utils xterm xz-utils zlib1g-dev \
        libcap-ng-dev libattr1-dev ccache cmake wget

# WASI-SDK 설치
wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-12/wasi-sdk-12.0-linux.tar.gz
sudo tar -xf wasi-sdk-12.0-linux.tar.gz -C /opt/
sudo ln -s /opt/wasi-sdk-12.0 /opt/wasi-sdk
rm wasi-sdk-12.0-linux.tar.gz

# WaTZ 및 의존성 설치
sudo mkdir -p /opt/watz
sudo chown $USER /opt/watz
git clone https://github.com/JamesMenetrey/unine-watz.git /opt/watz
cd /opt/watz
./clone.sh
```

### 2단계: Chaincode_Wrapper Machine 준비

참고: [chaincode-wrapper 문서](https://github.com/piachristel/open-source-fabric-optee-chaincode/blob/master/documentation/chaincode-wrapper.md)

```bash
# Fabric 체인코드 래퍼 디렉터리로 이동
cd ${GOPATH}/go/src/github.com/hyperledger/fabric/examples/chaincode/go/chaincode_wrapper

# 본 레포지터리의 파일로 교체
cp /path/to/this/repo/chaincode.go .
cp /path/to/this/repo/invocation.proto .

# IP 주소 설정 (chaincode.go 내 하드코딩된 IP를 환경에 맞게 수정)
```

### 3단계: OP-TEE OS 및 wrapper_ta 빌드

```bash
# wrapper_ta 폴더를 OP-TEE 예제 디렉터리에 배치
cp -r /path/to/this/repo/wrapper_ta /opt/watz/optee_examples/

# 툴체인 및 WaTZ 빌드
cd /opt/watz/build
make -j2 toolchains
export OPTEE_LOG_LEVEL=2
make -j "$(nproc)" USE_PERSISTENT_ROOTFS=1 CFG_NXP_CAAM=y CFG_CRYPTO_DRIVER=n \
        CFG_TEE_CORE_LOG_LEVEL=$OPTEE_LOG_LEVEL CFG_TEE_CORE_DEBUG=n \
        CFG_TEE_TA_LOG_LEVEL=$OPTEE_LOG_LEVEL CFG_DEBUG_INFO=n \
        CFG_MUTEX_DEBUG=n CFG_UNWIND=n

# SD 카드에 부트 이미지 플래시
sudo dd if=/opt/watz/out/boot.img of=/dev/sdX bs=1M status=progress

# TA 파일을 보드로 복사 (빌드된 TA 파일 경로 확인 후)
# /opt/watz/out-br/build/optee_examples_ext-1.0/fixed-attester/ta/out
# iMX-EVK 보드의 /lib/optee_armtz/ 디렉터리에 666 권한으로 배치
```

### 4단계: fixed-proxy 빌드

```bash
# gRPC 라이브러리 크로스 컴파일
# 참고: https://github.com/piachristel/open-source-fabric-optee-chaincode/blob/master/documentation/install-grpc.sh

# gRPC 헤더 파일 복사
cp -r /usr/local/include/grpc* fixed-proxy/include/
cp -r /usr/local/include/google fixed-proxy/include/

# fixed-proxy 빌드
cd fixed-proxy
make
```

### 5단계: 체인코드 빌드

```bash
# wrapper_ta 내 체인코드 디렉터리에서 AOT 빌드
cd wrapper_ta/chaincode
make coffee-aot

# 생성된 AOT 파일은 iMX-EVK 보드 내의 fixed-proxy 가 있는 경로에
# chaincode 디렉터리 생성 후 넣기.
```

## 실행

### 6단계: 시스템 실행

```bash
# iMX-EVK 보드에서 fixed-proxy 실행
./fixed-proxy
# -> 50051 포트에서 리슨 시작

# chaincode_wrapper 인스턴스에서 Fabric 네트워크 실행
# (orderer, peer 실행은 참고 문서 참조)

# chaincode_wrapper 인스턴스에서 체인코드 설치 및 실행
../.build/bin/peer chaincode install -n coffee_tracking_chaincode_wrapper -v 0 -p github.com/hyperledger/fabric/examples/chaincode/go/chaincode_wrapper/cmd
../.build/bin/peer chaincode instantiate -n coffee_tracking_chaincode_wrapper -v 0 -c '{"Args":["init"]}' -o 127.0.0.1:7050 -C ch
../.build/bin/peer chaincode invoke -n coffee_tracking_chaincode_wrapper -c '{"Args":["setup","WRAPPER_TA_UUID_HERE"]}' -o 127.0.0.1:7050 -C ch
../.build/bin/peer chaincode invoke -n coffee_tracking_chaincode_wrapper -c '{"Args":["coffee_chaincode.aot","create","pnu","20251001"]}' -o 127.0.0.1:7050 -C ch
```

## 주요 참고사항

- TA 파일은 반드시 `666` 권한으로 설정해야 합니다
- `chaincode.go`에서 iMX-EVK 보드의 IP 주소를 환경에 맞게 수정해야 합니다
- AOT 체인코드 파일은 `fixed-proxy` 실행 경로의 `chaincode/` 디렉터리에 위치해야 합니다