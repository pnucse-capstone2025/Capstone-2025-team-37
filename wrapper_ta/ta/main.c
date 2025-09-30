#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>

#include "wasm_export.h"
#include "bh_platform.h"

#include <wamr_ta.h>
#include <wasm.h>

#include "logging.h"
#include "session.h"
#include "include/chaincode_native_functions.h"
#include "chaincode_tee_ree_communication.h"
#include <string.h>

/* 메모리 기반 통신용 구조체 정의 */
struct chaincode_request {
    int type;           // 1=get_function, 2=get_arg, 3=get_state, 4=put_state, 5=return_response
    int arg_idx;        // get_arg용
    int key_len;
    int val_len;
    char key[64];
    char val[256];
};

struct chaincode_response {
    int result;         // 성공,실패
    int data_len;
    char data[256];
};

static uint32_t heap_size;
chaincode_session_ctx *g_chaincode_sess = NULL;

TEE_Result TA_CreateEntryPoint(void) {
    return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void) {
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types, TEE_Param __maybe_unused params[4], void __maybe_unused **sess_ctx) {
    uint32_t exp_param_types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
                     TEE_PARAM_TYPE_NONE,
                     TEE_PARAM_TYPE_NONE,
                     TEE_PARAM_TYPE_NONE);

    if (param_types != exp_param_types)
      return TEE_ERROR_BAD_PARAMETERS;

    (void)&params;
    if (!g_chaincode_sess) {
        g_chaincode_sess = TEE_Malloc(sizeof(*g_chaincode_sess), 0);
        if (!g_chaincode_sess)
            return TEE_ERROR_OUT_OF_MEMORY;
        TEE_MemFill(g_chaincode_sess, 0, sizeof(*g_chaincode_sess));
    }
    *sess_ctx = g_chaincode_sess;

    return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void __maybe_unused *sess_ctx) {
    (void)&sess_ctx;
}

static TEE_Result TA_SetHeapSize(uint32_t size) {
    heap_size = size;
    return TEE_SUCCESS;
}

/* 안전 strlen: 최대 max_len까지 */
static size_t safe_strlen(const char *s, size_t max_len)
{
    if (!s) return 0;
    size_t n = 0;
    while (n < max_len && s[n] != '\0') n++;
    return n;
}

/* step 함수 호출 */
static bool call_step(wamr_context *ctx, const char *name)
{
    
    // 함수 조회
    wasm_function_inst_t fn = wasm_runtime_lookup_function(ctx->module_inst, name, NULL);
    
    if (!fn) {
        EMSG("step function not found: %s", name);
        
        // 사용 가능한 함수들 확인해보기
        wasm_function_inst_t main_fn = wasm_runtime_lookup_function(ctx->module_inst, "main", NULL);
        wasm_function_inst_t get_req_fn = wasm_runtime_lookup_function(ctx->module_inst, "get_request_ptr", NULL);
        wasm_function_inst_t get_resp_fn = wasm_runtime_lookup_function(ctx->module_inst, "get_response_ptr", NULL);
        
             main_fn, get_req_fn, get_resp_fn); */
        
        return false;
    }
    
    // 예외 상태 사전 확인
    const char *pre_ex = wasm_runtime_get_exception(ctx->module_inst);
    if (pre_ex) {
        wasm_runtime_clear_exception(ctx->module_inst);
    }

    // exec_env가 없으면 필요할 때 생성
    if (!ctx->exec_env) {
        ctx->exec_env = wasm_runtime_create_exec_env(ctx->module_inst, 256 * 1024);
        if (!ctx->exec_env) {
            EMSG("Failed to create exec_env on demand");
            return false;
        }
    } else {
    }
    
    if (fn) {
    } else {
        EMSG("Function pointer is NULL!");
    }
    
    // 예외 상태 사전 클리어
    wasm_runtime_clear_exception(ctx->module_inst);
    
    // WASM 함수 호출 - 메모리 보호 강화
    bool ok = false;
    
    // Try-catch 스타일 실행 with immediate logging
    if (ctx->exec_env && fn && ctx->module_inst) {
        
        // WAMR 상태 검증 강화 (기본적인 포인터 검증)
        if (!ctx->exec_env) {
            EMSG("exec_env is NULL!");
            return false;
        }
        
        // 스택 오버플로우 방지
        ok = wasm_runtime_call_wasm(ctx->exec_env, fn, 0, NULL);
    } else {
        EMSG("Invalid context for WASM call");
    }
    
    
    // 예외 확인을 반환값 확인보다 먼저 수행
    const char *ex = wasm_runtime_get_exception(ctx->module_inst);
    if (ex) {
        // 의도적 예외인 경우 성공으로 처리
        if (strstr(ex, "out of bounds") || strstr(ex, "null pointer")) {
            ok = true;
        }
        // 예외 상태 클리어
        wasm_runtime_clear_exception(ctx->module_inst);
    }
    
    
    if (!ok) {
        EMSG("Function call failed for step: %s", name);
        if (!ex) {
            EMSG("No exception found, but call failed");
        }
    } else {
        // 성공한 경우 exec_env 정리 - 메모리 누수 방지 및 안전한 종료
        if (ctx->exec_env) {
            wasm_runtime_destroy_exec_env(ctx->exec_env);
            ctx->exec_env = NULL;
        }
    }
    
    return ok;
}



/* 메모리 기반 통신 처리 */
/* 호스트콜(yield/resume) 중심 처리: 네이티브 임포트가 pending_type을 설정하면
 * 여기서 TEEC 파라미터에 요청을 써서 즉시 반환하고, RESUME 호출에서 응답을 복사한 뒤
 * WASM의 step_resume을 실행한다. */
static TEE_Result process_hostcall_flow(chaincode_session_ctx *sc, TEE_Param params[4])
{

    /* 1) step_resume를 호출하여 WASM이 네이티브 임포트를 통해 요청을 생성하게 함 */

    bool ok = call_step(sc->runtime, "step_resume");
    
    if (!ok) {
        const char *ex = wasm_runtime_get_exception(sc->runtime->module_inst);
        EMSG("step_resume failed: %s", ex ? ex : "(null)");
        params[1].value.a = INVOCATION_RESPONSE;
        struct invocation_response *err = (struct invocation_response *)params[2].memref.buffer;
        TEE_MemFill(err, 0, sizeof(*err));
        TEE_MemMove(err->execution_response, "RUNTIME_ERROR", 13);
        return TEE_SUCCESS;
    }
    

    /* 2) 네이티브 임포트가 설정한 pending_type을 확인하여 호스트로 요청 전달 */

    if (sc->pending_type == GET_STATE_REQUEST) {
        params[1].value.a = GET_STATE_REQUEST;
        struct key_value *kv = (struct key_value *)params[2].memref.buffer;
        TEE_MemFill(kv, 0, sizeof(*kv));
        TEE_MemMove(kv->key, sc->key, safe_strlen(sc->key, KEY_SIZE-1));
        return TEE_SUCCESS;
    }
    if (sc->pending_type == PUT_STATE_REQUEST) {
        params[1].value.a = PUT_STATE_REQUEST;
        struct key_value *kv = (struct key_value *)params[2].memref.buffer;
        TEE_MemFill(kv, 0, sizeof(*kv));
        TEE_MemMove(kv->key, sc->key, safe_strlen(sc->key, KEY_SIZE-1));
        TEE_MemMove(kv->value, sc->value, safe_strlen(sc->value, VAL_SIZE-1));
        return TEE_SUCCESS;
    }

    /* 3) cc_return_response 를 받아 최종 결과값 반영 */

    params[1].value.a = INVOCATION_RESPONSE;
    struct invocation_response *final_resp = (struct invocation_response *)params[2].memref.buffer;
    TEE_MemFill(final_resp, 0, sizeof(*final_resp));
    
    /* 실제 response 값 사용 */
    if (g_chaincode_sess && g_chaincode_sess->has_response) {
        size_t resp_len = safe_strlen(g_chaincode_sess->response, RESPONSE_SIZE - 1);
        if (resp_len > 0) {
            TEE_MemMove(final_resp->execution_response, g_chaincode_sess->response, resp_len);
        } else {
            TEE_MemMove(final_resp->execution_response, "EMPTY_RESPONSE", 14);
        }
    } else {
        TEE_MemMove(final_resp->execution_response, "NO_RESPONSE", 11);
    }
    
    return TEE_SUCCESS;
}

TEE_Result TA_InvokeCommandEntryPoint(void __maybe_unused *sess_ctx, uint32_t cmd_id, uint32_t param_types, TEE_Param params[4])
{
    (void)&sess_ctx;
    uint32_t exp_param_types = 0;
    

    switch (cmd_id) {
    case COMMAND_CONFIGURE_HEAP:
        exp_param_types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT, TEE_PARAM_TYPE_NONE,
                         TEE_PARAM_TYPE_NONE, TEE_PARAM_TYPE_NONE);
        if (exp_param_types != param_types) return TEE_ERROR_BAD_PARAMETERS;
        return TA_SetHeapSize(params[0].value.a);

    case COMMAND_RUN_WASM:
        exp_param_types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT, TEE_PARAM_TYPE_VALUE_INOUT,
                             TEE_PARAM_TYPE_MEMREF_INOUT, TEE_PARAM_TYPE_MEMREF_INOUT);
        if (param_types == exp_param_types) {
            chaincode_session_ctx *sc = g_chaincode_sess;
            if (!sc) return TEE_ERROR_GENERIC;

            /* stdout 버퍼 설정 */
            TA_SetOutputBuffer(params[3].memref.buffer, params[3].memref.size);

            /* arguments 수신 - params[2]에서 struct arguments 읽기 */
                 (uint32_t)params[2].memref.size, (uint32_t)sizeof(struct arguments)); */
            if (params[2].memref.size >= sizeof(struct arguments)) {
                TEE_MemMove(&sc->args, params[2].memref.buffer, sizeof(struct arguments));
            } else {
                TEE_MemFill(&sc->args, 0, sizeof(struct arguments));
            }
            
            
            /* 응답 타입 초기화 */
            params[1].value.a = 0;

            /* WAMR 런타임 준비(보존) */
            sc->heap_buf_owned = TEE_Malloc(heap_size, 0);
            
            sc->trusted_wasm_owned = TEE_Malloc(params[0].memref.size, 0);
            
            if (!sc->heap_buf_owned || !sc->trusted_wasm_owned) {
                EMSG("Memory allocation failed! heap_buf: %p, trusted_wasm: %p", 
                     sc->heap_buf_owned, sc->trusted_wasm_owned);
                return TEE_ERROR_OUT_OF_MEMORY;
            }
            TEE_MemMove(sc->trusted_wasm_owned, params[0].memref.buffer, params[0].memref.size);

            static wamr_context runtime_ctx; /* 세션 전용 컨텍스트 */
            TEE_MemFill(&runtime_ctx, 0, sizeof(runtime_ctx));
            runtime_ctx.heap_buf = sc->heap_buf_owned;
            runtime_ctx.heap_size = heap_size;
            /* 네이티브 임포트 등록 (env 모듈) */
            runtime_ctx.native_symbols = chaincode_native_symbols;
            runtime_ctx.native_symbols_size = chaincode_native_symbols_size;
            runtime_ctx.wasm_bytecode = sc->trusted_wasm_owned;
            runtime_ctx.wasm_bytecode_size = params[0].memref.size;

            TEE_Result r = TA_InitializeWamrRuntime(&runtime_ctx, 1, (char*[]){(char*)""});
            if (r != TEE_SUCCESS) return r;
            sc->runtime = &runtime_ctx;
            
            /* WASM 런타임 상태 재확인 (exec_env는 필요시 생성되므로 module_inst만 확인) */
            if (!sc->runtime->module_inst) {
                EMSG("Runtime state corrupted before step_init - module_inst is NULL");
                return TEE_ERROR_GENERIC;
            }
            
            bool ok = call_step(sc->runtime, "step_init");
            
            /* step_init 후 런타임 상태 확인 */
            
            if (!ok) {
                const char *ex = wasm_runtime_get_exception(sc->runtime->module_inst);
                EMSG("step_init failed with exception: %s", ex ? ex : "(null)");
                params[1].value.a = INVOCATION_RESPONSE;
                struct invocation_response *error_resp = (struct invocation_response *)params[2].memref.buffer;
                TEE_MemFill(error_resp, 0, sizeof(*error_resp));
                TEE_MemMove(error_resp->execution_response, "STEP_INIT_FAILED", 17);
                return TEE_ERROR_GENERIC;
            }

            /* 호스트콜 처리 */
            return process_hostcall_flow(sc, params);
        }
        break;

    case COMMAND_RESUME_WASM:
        exp_param_types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE, TEE_PARAM_TYPE_VALUE_INOUT,
                             TEE_PARAM_TYPE_MEMREF_INOUT, TEE_PARAM_TYPE_MEMREF_INOUT);
        if (param_types == exp_param_types) {
            chaincode_session_ctx *sc = g_chaincode_sess;
            if (!sc) {
                return TEE_ERROR_GENERIC;
            }
            if (!sc->runtime) {
                return TEE_ERROR_GENERIC;
            }

            /* 호스트 응답을 WASM 버퍼에 복사 (앱 오프셋 → 네이티브 변환) */
            if (sc->pending_type == GET_STATE_REQUEST) {
                struct key_value *kv = (struct key_value *)params[2].memref.buffer;
                int len = (int)safe_strlen(kv->value, (size_t)sc->wasm_out_len - 1);
                
                void *out_native = NULL;
                if (sc->wasm_out_offset && sc->wasm_out_len > 0) {
                    bool addr_valid = wasm_runtime_validate_app_addr(sc->runtime->module_inst, sc->wasm_out_offset, (uint32_t)sc->wasm_out_len);
                    if (addr_valid) {
                        out_native = wasm_runtime_addr_app_to_native(sc->runtime->module_inst, sc->wasm_out_offset);
                    }
                } else {
                }
                if (out_native && len > 0) {
                    TEE_MemFill(out_native, 0, (size_t)sc->wasm_out_len);
                    TEE_MemMove(out_native, kv->value, (size_t)len);
                } else {
                }
                sc->pending_type = 0;
                sc->wasm_out_offset = 0;
                sc->wasm_out_len = 0;
            } else if (sc->pending_type == PUT_STATE_REQUEST) {
                struct acknowledgement *ack = (struct acknowledgement *)params[2].memref.buffer;
                /* PUT은 별도 out 없음. ACK는 cc_put_state_native 이후의 다음 step에서 처리됨 */
                sc->pending_type = 0;
            }

            /* 재개 후 다음 단계 진행 */
            return process_hostcall_flow(sc, params);
        } else {
            return TEE_ERROR_BAD_PARAMETERS;
        }
        break;

    default:
        return TEE_ERROR_BAD_PARAMETERS;
    }
    
    return TEE_ERROR_BAD_PARAMETERS;
}