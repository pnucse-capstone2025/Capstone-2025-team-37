#ifndef TA_CHAINCODE_TEE_REE_COMMUNICATION_H
#define TA_CHAINCODE_TEE_REE_COMMUNICATION_H

#include <stdint.h>

/* max size for strings is inclusive null terminator */
#define FCT_SIZE 20
#define KEY_SIZE 64    // WASM과 일치하도록 증가
#define VAL_SIZE 256   // WASM과 일치하도록 증가
#define ARG_SIZE 64    // 충분한 크기로 증가
#define ARGS_NUMBER 10
#define RESPONSE_SIZE 256  // 증가된 VAL_SIZE와 일치
#define ACK_SIZE 20

#define INVOCATION_RESPONSE 0
#define GET_STATE_REQUEST 1
#define PUT_STATE_REQUEST  2
#define ERROR 100

struct key_value {
    char key[KEY_SIZE];
    char value[VAL_SIZE];
};

struct arguments {
    char arguments[ARGS_NUMBER][ARG_SIZE];
};

struct invocation_response {
    char execution_response[RESPONSE_SIZE];
};

struct acknowledgement {
    char acknowledgement[ACK_SIZE];
};

#endif /* TA_CHAINCODE_TEE_REE_COMMUNICATION_H */


