global-incdirs-y += include
global-incdirs-y += ../../../../../runtime/core/iwasm/include/ ../../../../../runtime/core/app-framework/base/app
srcs-y += wasm.c main.c chaincode_native_functions.c

# Method 2 includes the static (trusted) library between the --start-group and
# --end-group arguments.
# 원본 linux-trustzone 버전 사용 (OP-TEE 호환)
# strcpy 충돌 해결 완료 - 코드 레벨에서 해결됨
libnames += vmlib
libdirs += ../../../../../runtime/product-mini/platforms/linux-trustzone/build/
libdeps += ../../../../../runtime/product-mini/platforms/linux-trustzone/build/libvmlib.a