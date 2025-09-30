#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>

#include "wasm_export.h"
#include "bh_platform.h"

#include "logging.h"
#include "wasm.h"
#include "session.h"
#include "chaincode_tee_ree_communication.h"

/* 안전 strlen: 최대 max_len까지 */
static size_t safe_strlen(const char *s, size_t max_len)
{
    if (!s)
        return 0;
    size_t n = 0;
    while (n < max_len && s[n] != '\0')
        n++;
    return n;
}

static void *to_native(wasm_module_inst_t inst, uint32_t app_offset, uint32_t size)
{
    /* DMSG("to_native: inst=%p, offset=0x%x, size=%u", inst, app_offset, size); */
    if (size && !wasm_runtime_validate_app_addr(inst, app_offset, size)) {
        EMSG("to_native: address validation failed");
        return NULL;
    }
    void *result = wasm_runtime_addr_app_to_native(inst, app_offset);
    /* DMSG("to_native: result=%p", result); */
    return result;
}

/*
 * 네이티브 함수 구현
 * 서명 규칙(WAMR):
 *  - '*~'  : WASM 버퍼 포인터와 그 길이(바로 뒤 인자)를 의미, 자동 변환/경계체크
 *  - '$'   : NUL-종단 문자열 포인터 자동 변환
 */

static int cc_get_function_native(wasm_exec_env_t exec_env, uint32_t out_ptr, int out_len)
{
    /* DMSG("cc_get_function in, out_ptr=0x%x, out_len=%d", out_ptr, out_len); */
    
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    /* DMSG("module_inst: %p", inst); */
    
    if (!inst) {
        EMSG("module_inst is NULL");
        return 0;
    }
    
    if (out_len <= 0) {
        EMSG("out_len <= 0: %d", out_len);
        return 0;
    }
    
    /* 메모리 주소 검증 */
    /* DMSG("Validating WASM address: ptr=0x%x, len=%d", out_ptr, out_len); */
    if (!wasm_runtime_validate_app_addr(inst, out_ptr, (uint32_t)out_len)) {
        EMSG("WASM address validation failed: ptr=0x%x, len=%d", out_ptr, out_len);
        return 0;
    }
    
    char *out = (char*)wasm_runtime_addr_app_to_native(inst, out_ptr);
    /* DMSG("Native pointer: %p", out); */
    
    if (!out) {
        EMSG("Native pointer conversion failed");
        return 0;
    }

    /* 세션 컨텍스트 확인 */
    /* DMSG("Checking session context: %p", g_chaincode_sess); */
    if (!g_chaincode_sess) {
        EMSG("g_chaincode_sess is NULL");
        return 0;
    }
    
    /* 세션 컨텍스트에서 function 추출: args.arguments[0] */
    const char *function = g_chaincode_sess->args.arguments[0];
    /* DMSG("Function from session: %p", function); */
    if (!function) {
        /* DMSG("Function is NULL, using empty string"); */
        function = "";
    } else {
        /* DMSG("Function string: %.10s...", function); */ /* 처음 10글자만 안전 출력 */
    }
    
    size_t len = safe_strlen(function, (size_t)out_len - 1);
    /* DMSG("Function length: %zu", len); */
    
    /* DMSG("About to clear output buffer"); */
    TEE_MemFill(out, 0, (size_t)out_len);
    
    if (len > 0) {
        /* DMSG("About to copy function string"); */
        TEE_MemMove(out, function, len);
        /* DMSG("Function copy completed"); */
    }
    
    /* DMSG("cc_get_function returning: %d", (int)len); */
    return (int)len;
}

static int cc_get_arg_native(wasm_exec_env_t exec_env, int idx, uint32_t out_ptr, int out_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    char *out = (char*)to_native(inst, out_ptr, (uint32_t)out_len);
    /* DMSG("cc_get_arg in, idx=%d, out_len=%d", idx, out_len); */
    if (!out || out_len <= 0)
        return 0;

    const char *arg = "";
    if (g_chaincode_sess && idx >= 0 && idx < ARGS_NUMBER)
        arg = g_chaincode_sess->args.arguments[idx+1]; /* arguments[0]는 function, 그 뒤가 args */
    size_t len = safe_strlen(arg, (size_t)out_len - 1);
    TEE_MemFill(out, 0, (size_t)out_len);
    if (len > 0)
        TEE_MemMove(out, arg, len);
    return (int)len;
}

static int cc_get_state_native(wasm_exec_env_t exec_env,
                               uint32_t key_ptr, int key_len,
                               uint32_t out_ptr, int out_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    char *out = (char*)to_native(inst, out_ptr, (uint32_t)out_len);
    const char *key = (const char*)to_native(inst, key_ptr, (uint32_t)(key_len > 0 ? key_len : 0));
    /* DMSG("cc_get_state in, key_len=%d, out_len=%d", key_len, out_len); */
    /* DMSG("cc_get_state: out=%p, key=%p", out, key); */
    if (key && key_len > 0) {
        char key_buf[128];
        int n = key_len < (int)sizeof(key_buf)-1 ? key_len : (int)sizeof(key_buf)-1;
        TEE_MemFill(key_buf, 0, sizeof(key_buf));
        TEE_MemMove(key_buf, key, (size_t)n);
        /* DMSG("cc_get_state: key='%s'", key_buf); */
    } else {
        /* DMSG("cc_get_state: key is empty"); */
    }
    
    if (!out || out_len <= 0) {
        /* DMSG("cc_get_state: invalid out buffer"); */
        return 0;
    }

    /* 공유버퍼로 요청 전달을 위해 세션 컨텍스트 저장 후 예외로 YIELD */
    if (!g_chaincode_sess) {
        /* DMSG("cc_get_state: no session context"); */
        return 0;
    }
    
    /* DMSG("cc_get_state: about to clear key buffer"); */
    TEE_MemFill(g_chaincode_sess->key, 0, KEY_SIZE);
    /* DMSG("cc_get_state: key buffer cleared"); */
    
    int klen = key_len < KEY_SIZE-1 ? key_len : KEY_SIZE-1;
    /* DMSG("cc_get_state: klen=%d, key=%p", klen, key); */
    
    if (klen > 0 && key) {
        /* DMSG("cc_get_state: about to copy key"); */
        TEE_MemMove(g_chaincode_sess->key, key, (size_t)klen);
        /* DMSG("cc_get_state: key copied"); */
    }

    /* 세션 컨텍스트에 GET_STATE_REQUEST 설정 */
    /* DMSG("cc_get_state: setting pending_type to GET_STATE_REQUEST"); */
    g_chaincode_sess->pending_type = GET_STATE_REQUEST;
    
    /* WASM 출력 버퍼 정보 저장 */
    g_chaincode_sess->wasm_out_offset = out_ptr;
    g_chaincode_sess->wasm_out_len = out_len;
    /* DMSG("[DEBUG] cc_get_state: wasm_out_offset=0x%x, wasm_out_len=%d", out_ptr, out_len); */
    
    /* DMSG("cc_get_state: pending_type set, returning key length: %d", klen); */
    return klen;
}

static int cc_put_state_native(wasm_exec_env_t exec_env,
                               uint32_t key_ptr, int key_len,
                               uint32_t val_ptr, int val_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const char *key = (const char*)to_native(inst, key_ptr, (uint32_t)(key_len > 0 ? key_len : 0));
    const char *val = (const char*)to_native(inst, val_ptr, (uint32_t)(val_len > 0 ? val_len : 0));
    /* DMSG("cc_put_state in, key_len=%d, val_len=%d", key_len, val_len); */
    if (!g_chaincode_sess)
        return -1;
    TEE_MemFill(g_chaincode_sess->key, 0, KEY_SIZE);
    TEE_MemFill(g_chaincode_sess->value, 0, VAL_SIZE);
    int klen = key_len < KEY_SIZE-1 ? key_len : KEY_SIZE-1;
    int vlen = val_len < VAL_SIZE-1 ? val_len : VAL_SIZE-1;
    if (klen > 0 && key)
        TEE_MemMove(g_chaincode_sess->key, key, (size_t)klen);
    if (vlen > 0 && val)
        TEE_MemMove(g_chaincode_sess->value, val, (size_t)vlen);

    g_chaincode_sess->pending_type = PUT_STATE_REQUEST;
    g_chaincode_sess->wasm_out_offset = 0;
    g_chaincode_sess->wasm_out_len = 0;
    /* 예외 발생 없이 요청만 표시 */
    return -1;
}

/* cc_return_response 함수를 int 반환 타입으로 구현 */
static int cc_return_response_native(wasm_exec_env_t exec_env, uint32_t msg_ptr, int msg_len)
{
    /* DMSG("[DEBUG] cc_return_response in, msg_ptr=0x%x, msg_len=%d", msg_ptr, msg_len); */
    
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) {
        EMSG("cc_return_response: module_inst is NULL");
        return 0;
    }
    
    const char *msg = (const char*)to_native(inst, msg_ptr, (uint32_t)(msg_len > 0 ? msg_len : 0));
    if (!g_chaincode_sess) {
        EMSG("cc_return_response: no session context");
        return 0;
    }
    
    int n = (msg_len < (int)RESPONSE_SIZE - 1) ? msg_len : ((int)RESPONSE_SIZE - 1);
    TEE_MemFill(g_chaincode_sess->response, 0, RESPONSE_SIZE);
    
    if (msg && n > 0) {
        TEE_MemMove(g_chaincode_sess->response, msg, (size_t)n);
        /* DMSG("[DEBUG] cc_return_response: copied response '%.*s'", n, msg); */
    }
    
    g_chaincode_sess->response[n] = '\0';
    g_chaincode_sess->has_response = 1;
    
    /* DMSG("[DEBUG] cc_return_response: set response='%s', has_response=1", g_chaincode_sess->response); */
    return n; // 복사된 바이트 수 반환
}

static int cc_log_native(wasm_exec_env_t exec_env, uint32_t msg_ptr, int msg_len)
{
    /* DMSG("[DEBUG] cc_log in, msg_ptr=0x%x, msg_len=%d", msg_ptr, msg_len); */
    
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) {
        EMSG("cc_log: module_inst is NULL");
        return 0;
    }
    
    const char *msg = (const char*)to_native(inst, msg_ptr, (uint32_t)(msg_len > 0 ? msg_len : 0));
    if (msg && msg_len > 0) {
        char buf[128];
        int n = msg_len < (int)sizeof(buf)-1 ? msg_len : (int)sizeof(buf)-1;
        TEE_MemFill(buf, 0, sizeof(buf));
        TEE_MemMove(buf, msg, (size_t)n);
        /* DMSG("cc_log: %s", buf); */
        return n;
    }
    return 0;
}

/* 디버그 로깅 함수 - int 반환으로 변경 */
static int debug_log_native(wasm_exec_env_t exec_env, int step_num)
{
    /* DMSG("WASM DEBUG: step %d reached", step_num); */
    return 0; // 성공
}

/* 표준 C 빌트인 대체 (컴파일러가 생성하는 env.mem*) */
static uint32_t env_memset_native(wasm_exec_env_t exec_env, uint32_t dst_ptr, int c, uint32_t n)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    void *dst = to_native(inst, dst_ptr, n);
    if (!dst && n)
        return 0;
    if (n)
        TEE_MemFill(dst, (uint8_t)c, (size_t)n);
    return dst_ptr; /* libc 규약: dst 반환 */
}

static uint32_t env_memcpy_native(wasm_exec_env_t exec_env, uint32_t dst_ptr, uint32_t src_ptr, uint32_t n)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    void *dst = to_native(inst, dst_ptr, n);
    void *src = to_native(inst, src_ptr, n);
    if ((!dst || !src) && n)
        return 0;
    if (n)
        TEE_MemMove(dst, src, (size_t)n);
    return dst_ptr;
}

static uint32_t env_memmove_native(wasm_exec_env_t exec_env, uint32_t dst_ptr, uint32_t src_ptr, uint32_t n)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    void *dst = to_native(inst, dst_ptr, n);
    void *src = to_native(inst, src_ptr, n);
    if ((!dst || !src) && n)
        return 0;
    if (n)
        TEE_MemMove(dst, src, (size_t)n);
    return dst_ptr;
}

/* 네이티브 심볼 테이블 - cc_return_response, cc_log 추가 */
NativeSymbol chaincode_native_symbols[] = {
    { "cc_get_function",       cc_get_function_native,       "(ii)i",   NULL },
    { "cc_get_arg",            cc_get_arg_native,            "(iii)i",  NULL },
    { "cc_get_state",          cc_get_state_native,          "(iiii)i", NULL },
    { "cc_put_state",          cc_put_state_native,          "(iiii)i", NULL },
    { "cc_return_response",    cc_return_response_native,    "(ii)i",   NULL },
    { "cc_log",                cc_log_native,                "(ii)i",   NULL },
    { "debug_log",             debug_log_native,             "(i)i",    NULL },
    { "memset",                env_memset_native,            "(iii)i",  NULL },
    { "memcpy",                env_memcpy_native,            "(iii)i",  NULL },
    { "memmove",               env_memmove_native,           "(iii)i",  NULL },
};

uint32_t chaincode_native_symbols_size = sizeof(chaincode_native_symbols);


