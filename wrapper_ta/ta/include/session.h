#ifndef TA_SESSION_H
#define TA_SESSION_H

#include <stdint.h>
#include "chaincode_tee_ree_communication.h"
#include "wasm.h"

typedef struct chaincode_session_ctx {
    struct arguments args; /* arguments[0]를 function으로 사용 */
    int pending_type; /* 0 none, 1 GET_STATE_REQUEST, 2 PUT_STATE_REQUEST */
    char key[KEY_SIZE];
    char value[VAL_SIZE];
    uint32_t wasm_out_offset; /* WASM out 버퍼의 앱 오프셋 (포인터 보관 금지) */
    int wasm_out_len;

    char response[RESPONSE_SIZE];
    int has_response;

    /* Persistent runtime (mode 2) */
    wamr_context *runtime; /* owned while invocation in progress */
    uint8_t *heap_buf_owned;
    uint8_t *trusted_wasm_owned;
} chaincode_session_ctx;

extern chaincode_session_ctx *g_chaincode_sess;

#endif /* TA_SESSION_H */


