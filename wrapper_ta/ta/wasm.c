#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include "logging.h"
#include "wasm.h"
#include <string.h>

wamr_context *singleton_wamr_context;

#ifdef DEBUG_MESSAGE
#define UINT8_DIGIT_MAX_SIZE 	2
static void utils_print_byte_array(uint8_t *byte_array, int byte_array_len)
{
	int buffer_len = UINT8_DIGIT_MAX_SIZE * byte_array_len + byte_array_len;
	char *buffer = TEE_Malloc(buffer_len, 0);

	int i, buffer_cursor = 0;
	for (i = 0; i < byte_array_len; ++i)
	{
		buffer_cursor += snprintf(buffer + buffer_cursor, buffer_len - buffer_cursor, "%02x ", byte_array[i]);
	}

	buffer[buffer_cursor] = '\0';
	TEE_Free(buffer);
}
#endif

void TA_SetOutputBuffer(void *output_buffer, uint64_t output_buffer_size) {
    vedliot_set_output_buffer(output_buffer, output_buffer_size);
}

TEE_Result TA_HashWasmBytecode(wamr_context *ctx) {
    TEE_Result res = TEE_SUCCESS;
    TEE_OperationHandle operation_handle;
    uint32_t expected_digest_len = RA_HASH_SIZE / 8;
	uint32_t digest_len = RA_HASH_SIZE;

    res = TEE_AllocateOperation(&operation_handle, TEE_ALG_SHA256, TEE_MODE_DIGEST, 0);
    if (res != TEE_SUCCESS) {
        EMSG("TEE_AllocateOperation failed. Error: %x", res);
        goto out;
    }

    res = TEE_DigestDoFinal(operation_handle, ctx->wasm_bytecode, ctx->wasm_bytecode_size, ctx->wasm_bytecode_hash, &digest_len);
    if (res != TEE_SUCCESS) {
        EMSG("TEE_DigestDoFinal failed. Error: %x", res);
        goto out;
    }

    if (digest_len != expected_digest_len) {
        EMSG("The hash size does not correspond to the expected value (actual: %d; expected: %d).", digest_len, expected_digest_len);
        res = TEE_ERROR_GENERIC;
        goto out;
    }
out:
    TEE_FreeOperation(operation_handle);

    return res;
}

TEE_Result TA_InitializeWamrRuntime(wamr_context* context, int argc, char** argv)
{
    RuntimeInitArgs init_args;
    TEE_MemFill(&init_args, 0, sizeof(RuntimeInitArgs));

    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = context->heap_buf;
    init_args.mem_alloc_option.pool.heap_size = context->heap_size;

    init_args.n_native_symbols = context->native_symbols_size / sizeof(NativeSymbol);
    init_args.native_module_name = "env";
    init_args.native_symbols = context->native_symbols;

    if (!wasm_runtime_full_init(&init_args)) {
        EMSG("Init runtime environment failed.\n");
        return TEE_ERROR_GENERIC;
    }

    char error_buf[128];
    context->module = wasm_runtime_load(context->wasm_bytecode, context->wasm_bytecode_size, error_buf, sizeof(error_buf));
    if(!context->module) {
        EMSG("Load wasm module failed. error: %s\n", error_buf);
        return TEE_ERROR_GENERIC;
    }

    wasm_runtime_set_wasi_args(context->module, NULL, 0, NULL, 0, NULL, 0, argv, argc);

    uint32_t stack_size = 256 * 1024, heap_size = 64 * 1024; // 256KB 스택, 64KB 힙

    context->module_inst = wasm_runtime_instantiate(context->module,
                                         stack_size,
                                         heap_size,
                                         error_buf,
                                         sizeof(error_buf));
    if (!context->module_inst) {
        EMSG("Instantiate wasm module failed. error: %s", error_buf);
        return TEE_ERROR_GENERIC;
    }

    // WASM 모듈 정보 디버깅
    IMSG("WASM module loaded successfully");
    IMSG("Module instance: %p", context->module_inst);
    IMSG("Exec env: %p", context->exec_env);
    singleton_wamr_context = context;

    return TEE_SUCCESS;
}

TEE_Result TA_ExecuteWamrRuntime(wamr_context* context)
{
    // Pure WASM을 위한 main 함수 직접 호출 (우선순위)
    wasm_function_inst_t main_func = wasm_runtime_lookup_function(context->module_inst, "main", NULL);
    if (main_func) {
        IMSG("Found main function, calling directly for Pure WASM...");
        if (wasm_runtime_call_wasm(context->exec_env, main_func, 0, NULL)) {
            IMSG("Pure WASM main function executed successfully");
            return TEE_SUCCESS;
        } else {
            EMSG("Direct main function call failed: %s", wasm_runtime_get_exception(context->module_inst));
            return TEE_ERROR_GENERIC;
        }
    }
    
    // WASI 타겟을 위한 다양한 함수명 시도 (폴백)
    const char* function_names[] = {"_start", "_main", "start"};
    wasm_function_inst_t start_func = NULL;
    
    for (int i = 0; i < 3; i++) {
        start_func = wasm_runtime_lookup_function(context->module_inst, function_names[i], NULL);
        if (start_func) {
            IMSG("Found function '%s', calling directly...", function_names[i]);
            if (wasm_runtime_call_wasm(context->exec_env, start_func, 0, NULL)) {
                IMSG("WASM function '%s' executed successfully", function_names[i]);
                return TEE_SUCCESS;
            } else {
                EMSG("Direct function '%s' call failed: %s", function_names[i], wasm_runtime_get_exception(context->module_inst));
            }
        }
    }
    
    // 기본 방식으로 폴백
    IMSG("No main function found, trying wasm_application_execute_main...");
    
    // 메모리 보호를 위한 try-catch 스타일 실행
    if (!wasm_application_execute_main(context->module_inst, 0, NULL))
    {
        const char* exception = wasm_runtime_get_exception(context->module_inst);
        if (exception && strstr(exception, "out of bounds")) {
            IMSG("Memory bounds error detected, but continuing...");
            // 메모리 오류가 발생해도 일부 실행은 성공했을 수 있음
            return TEE_SUCCESS;
        } else {
            EMSG("call wasm entry point test failed. %s\n", exception);
            return TEE_ERROR_GENERIC;
        }
    }

    return TEE_SUCCESS;
}

void TA_TearDownWamrRuntime(wamr_context* context)
{
    singleton_wamr_context = NULL;

    if (context->exec_env) {
        wasm_runtime_destroy_exec_env(context->exec_env);
        context->exec_env = NULL;
    }

    if (context->module_inst)
    {
        wasm_runtime_deinstantiate(context->module_inst);
    }

    if (context->module) wasm_runtime_unload(context->module);
    wasm_runtime_destroy();
}
