# Fixed Chaincode Proxy with gRPC and WASM Support
# iMX.EVK 보드 전용 (OP-TEE TA 실행 환경 필요)

# 보드용 크로스 컴파일 설정
CXX = aarch64-linux-gnu-g++
CXXFLAGS = -Wall -Wextra -std=c++11 -g -O2
# gRPC 정적 라이브러리 경로
GRPC_LIB_PATH = /home/ubuntu/grpc/libs/opt
PROTOBUF_LIB_PATH = /home/ubuntu/grpc/libs/opt/protobuf

LDFLAGS = -lteec $(GRPC_LIB_PATH)/libgrpc++.a $(GRPC_LIB_PATH)/libgrpc.a $(PROTOBUF_LIB_PATH)/libprotobuf.a $(GRPC_LIB_PATH)/libgpr.a $(GRPC_LIB_PATH)/libaddress_sorting.a $(GRPC_LIB_PATH)/libares.a $(GRPC_LIB_PATH)/libboringssl.a -lpthread -ldl -lz

# 공통 설정
BINARY = fixed_chaincode_proxy_arm64
SRCS = main.cpp invocation.pb.cc invocation.grpc.pb.cc
OBJS = main.o invocation.pb.o invocation.grpc.pb.o

# OP-TEE 클라이언트 라이브러리 경로 (buildroot sysroot)
BUILDROOT_SYSROOT = /opt/watz/out-br/host/aarch64-buildroot-linux-gnu/sysroot
CXXFLAGS += -I./ta/include -I$(BUILDROOT_SYSROOT)/usr/include -Iinclude --sysroot=$(BUILDROOT_SYSROOT)
LDFLAGS += -L$(BUILDROOT_SYSROOT)/usr/lib --sysroot=$(BUILDROOT_SYSROOT)

$(info ===========================================)
$(info Fixed Chaincode Proxy - 보드 전용 빌드)
$(info 타겟: iMX.EVK 보드 (ARM64))
$(info 컴파일러: $(CXX))
$(info 바이너리: $(BINARY))
$(info SYSROOT: $(BUILDROOT_SYSROOT))
$(info ===========================================)

# 빌드 규칙
.PHONY: all
all: $(BINARY)

$(BINARY): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)
	@echo "✅ Fixed Chaincode Proxy 빌드 완료: $(BINARY)"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Proto 파일에서 gRPC 코드 생성
.PHONY: proto
proto:
	@echo "🔧 Generating gRPC code from proto files..."
	protoc -I . --grpc_out=. --plugin=protoc-gen-grpc=$$(which grpc_cpp_plugin) invocation.proto
	protoc -I . --cpp_out=. invocation.proto
	@echo "✅ gRPC 코드 생성 완료"

# 정리
clean:
	rm -f $(OBJS) $(BINARY) fixed_chaincode_proxy_arm64
	rm -f *.pb.cc *.pb.h
	@echo "🧹 빌드 파일들이 정리되었습니다."

# 종속성 설치 안내
.PHONY: deps
deps:
	@echo "📦 필요한 종속성 설치:"
	@echo ""
	@echo "Ubuntu/Debian (호스트 개발용):"
	@echo "  sudo apt update"
	@echo "  sudo apt install -y libteec-dev libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc"
	@echo ""
	@echo "OP-TEE 환경 (보드용):"
	@echo "  gRPC 라이브러리가 buildroot sysroot에 설치되어 있어야 합니다."

# 테스트 실행 (gRPC 서버 테스트)
test: $(BINARY)
	@echo "🚀 gRPC 서버 테스트"
	@echo "⚠️  주의사항:"
	@echo "   - OP-TEE TA가 로드되어 있어야 합니다"
	@echo "   - chaincode_wrapper가 gRPC 클라이언트로 연결해야 합니다"
	@echo ""
	@echo "서버 시작: ./$(BINARY)"
	@echo "포트: 50051"

# 도움말
help:
	@echo "🔧 Fixed Chaincode Proxy with gRPC and WASM Support (보드 전용)"
	@echo ""
	@echo "주요 타겟:"
	@echo "  make              # iMX.EVK 보드용 프록시 빌드"
	@echo "  make proto        # Proto 파일에서 gRPC 코드 생성"
	@echo "  make clean        # 빌드 파일 정리"
	@echo ""
	@echo "빌드 결과:"
	@echo "  바이너리: $(BINARY)"
	@echo "  실행 위치: iMX.EVK 보드"
	@echo "  포트: 50051 (gRPC 서버)"
	@echo ""
	@echo "전제조건:"
	@echo "  - aarch64-linux-gnu-g++ 크로스 컴파일러"
	@echo "  - /opt/watz buildroot 환경 (libteec, gRPC 라이브러리 포함)"

.PHONY: all clean help proto deps test