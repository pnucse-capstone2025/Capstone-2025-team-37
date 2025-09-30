#ifndef TA_CHAINCODE_NATIVE_FUNCTIONS_H
#define TA_CHAINCODE_NATIVE_FUNCTIONS_H

#include "wasm_export.h"
#include <stdint.h>

/* 네이티브 심볼 테이블 (env 모듈) */
extern NativeSymbol chaincode_native_symbols[];
extern uint32_t chaincode_native_symbols_size;

#endif /* TA_CHAINCODE_NATIVE_FUNCTIONS_H */


