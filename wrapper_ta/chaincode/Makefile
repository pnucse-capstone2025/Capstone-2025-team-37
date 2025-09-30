# Minimal Makefile for building only coffee-aot (placed in chaincode/)

WASI_SDK_PATH ?= /opt/wasi-sdk
WASI_CLANG := $(WASI_SDK_PATH)/bin/clang

# wamrc path can be overridden: make WAMRC=/path/to/wamrc coffee-aot
WAMRC ?= /opt/watz/runtime/wamr-compiler/build/wamrc

SRC := coffee_chaincode_wasm.c
WASM := coffee_chaincode.wasm
AOT := coffee_chaincode.aot

.PHONY: all coffee-wasm coffee-aot clean help

all: coffee-aot

coffee-wasm:
	@echo "Building coffee_chaincode.wasm (using WASI-SDK: $(WASI_SDK_PATH))"
	@[ -x "$(WASI_CLANG)" ] || (echo "❌ $(WASI_CLANG) 가 없습니다. WASI-SDK를 설치하거나 WASI_SDK_PATH를 설정하세요." && false)
	@$(WASI_CLANG) --target=wasm32 -nostdlib -O1 \
		-Wl,--no-entry \
		-Wl,--export=main \
		-Wl,--export=step_init \
		-Wl,--export=step_resume \
		-Wl,--initial-memory=524288 \
		-Wl,--max-memory=1048576 \
		-Wl,--stack-first \
		-Wl,--allow-undefined \
		-o $(WASM) $(SRC) || (echo "clang/wasm-ld 빌드 실패 (WASI-SDK 설치 확인)" && false)
	@echo "✅ WASM 빌드 완료: $(WASM)"

coffee-aot: coffee-wasm
	@echo "Converting to AOT (wamrc)"
	@[ -x "$(WAMRC)" ] || (echo "❌ $(WAMRC) 가 없습니다. wamrc를 빌드하거나 경로를 설정하세요." && false)
	@$(WAMRC) \
		--target=aarch64 \
		--bounds-checks=0 \
		--size-level=3 \
		--opt-level=2 \
		--disable-aux-stack-check \
		-o $(AOT) $(WASM) || (echo "wamrc not found or failed" && false)
	@echo "✅ AOT 변환 완료: $(AOT)"
	@echo "📊 AOT 파일 크기: $$(ls -lh $(AOT) | awk '{print $$5}')"

clean:
	rm -f $(WASM) $(AOT)
	@echo "🧹 정리 완료"

help:
	@echo "coffee-aot 전용 Makefile (chaincode/ 경로)"
	@echo "\n사용법:"
	@echo "  make           # 기본: coffee-aot"
	@echo "  make coffee-aot # WASM→AOT 변환까지"
	@echo "  make clean      # 산출물 정리"
	@echo "\n환경 변수:"
	@echo "  WASI_SDK_PATH=/opt/wasi-sdk (기본)"
	@echo "  WAMRC=/opt/watz/runtime/wamr-compiler/build/wamrc (기본)"

