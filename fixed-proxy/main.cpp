// Standard C library headers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <csignal>
#include <chrono>
#include <iomanip>

// GlobalPlatform Client API
#include <tee_client_api.h>

// GlobalPlatfrom TA
#include <wamr_ta.h>
#include "chaincode_tee_ree_communication.h"

// gRPC includes
#include <grpcpp/grpcpp.h>
#include "invocation.grpc.pb.h"

using namespace std;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReaderWriter;
using grpc::Status;
using invocation::ChaincodeProxyMessage;
using invocation::ChaincodeWrapperMessage;
using invocation::GetStateRequest;
using invocation::PutStateRequest;
using invocation::InvocationResponse;
using invocation::Invocation;

/* TEE resources */
typedef struct _tee_ctx {
	TEEC_Context ctx;
	TEEC_Session sess;
    uint8_t *output_buffer;
    uint64_t output_buffer_size;
    uint8_t *benchmark_buffer;
    uint64_t benchmark_buffer_size;
} tee_ctx;

/* Forward declarations */
static void prepare_tee_session(tee_ctx* ctx);
static void configure_heap_size(tee_ctx *ctx, uint32_t size);
static void allocate_buffers(tee_ctx* ctx, uint64_t buffers_size);
static void terminate_tee_session(tee_ctx* ctx);
static void free_buffers(tee_ctx* ctx);
void cleanup(int signum);
static void run_server();

/* Time measurement utility */
static std::string get_timestamp() {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return "[" + std::to_string(millis) + "ms]";
}

static void prepare_tee_session(tee_ctx* ctx)
{
	TEEC_UUID uuid = TA_WAMR_UUID;
	uint32_t origin;
	TEEC_Result res;

	/* Initialize a context connecting us to the TEE */
	printf("%s TEE context 초기화 시작\n", get_timestamp().c_str());
	res = TEEC_InitializeContext(NULL, &ctx->ctx);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_InitializeContext failed with code 0x%x\n", res);
		exit(1);
	}
	printf("%s TEE context 초기화 완료\n", get_timestamp().c_str());

	/* Open a session with the TA */
	printf("%s TEE session 오픈 시작\n", get_timestamp().c_str());
	res = TEEC_OpenSession(&ctx->ctx, &ctx->sess, &uuid,
			       TEEC_LOGIN_PUBLIC, NULL, NULL, &origin);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_OpenSession failed with code 0x%x origin 0x%x\n", res, origin);
		exit(1);
	}
	printf("%s TEE session 오픈 완료\n", get_timestamp().c_str());
}

static void configure_heap_size(tee_ctx *ctx, uint32_t size) {
    TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;

    memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE);
	op.params[0].value.a = size;

	printf("%s WaTZ heap 크기 설정 시작 (%u bytes)\n", get_timestamp().c_str(), size);
	res = TEEC_InvokeCommand(&ctx->sess, COMMAND_CONFIGURE_HEAP, &op, &origin);
    if (res != TEEC_SUCCESS) {
        printf("%s WaTZ heap 크기 설정 실패. Error: %x\n", get_timestamp().c_str(), res);
    } else {
        printf("%s WaTZ heap 크기 설정 완료\n", get_timestamp().c_str());
    }
}

static void allocate_buffers(tee_ctx* ctx, uint64_t buffers_size) {
    printf("%s 버퍼 할당 시작 (%lu bytes)\n", get_timestamp().c_str(), buffers_size);
    // The output buffer is used to capture writes to stdout from the WASM
    ctx->output_buffer = (uint8_t*)malloc(buffers_size);
    ctx->output_buffer_size = buffers_size;

    // The benchmark buffer is used to capture benchmark information from the TA
    ctx->benchmark_buffer = (uint8_t*)malloc(buffers_size);
    ctx->benchmark_buffer_size = buffers_size;
    printf("%s 버퍼 할당 완료\n", get_timestamp().c_str());
}

static void terminate_tee_session(tee_ctx* ctx)
{
	printf("%s TEE 세션 종료 시작\n", get_timestamp().c_str());
	TEEC_CloseSession(&ctx->sess);
	TEEC_FinalizeContext(&ctx->ctx);
	printf("%s TEE 세션 종료 완료\n", get_timestamp().c_str());
}

static void free_buffers(tee_ctx* ctx) {
    ctx->output_buffer_size = 0;
    ctx->benchmark_buffer_size = 0;
    free(ctx->output_buffer);
    free(ctx->benchmark_buffer);
}

void cleanup(int signum)
{
	exit(0);
}

/* gRPC Server Implementation */
class InvocationImpl final : public Invocation::Service
{
private:
    tee_ctx ctx;
    
    /* write the uuid received from the chaincode_wrapper to the shared memory */
	static void set_uuid(ChaincodeWrapperMessage *wrapper_msg, TEEC_UUID *uuid)
	{
		std::string chaincode_uuid = wrapper_msg->invocation_request().chaincode_uuid();
		uuid->clockSeqAndNode[0] = chaincode_uuid[8];
		uuid->clockSeqAndNode[1] = chaincode_uuid[9];
		uuid->clockSeqAndNode[2] = chaincode_uuid[10];
		uuid->clockSeqAndNode[3] = chaincode_uuid[11];
		uuid->clockSeqAndNode[4] = chaincode_uuid[12];
		uuid->clockSeqAndNode[5] = chaincode_uuid[13];
		uuid->clockSeqAndNode[6] = chaincode_uuid[14];
		uuid->clockSeqAndNode[7] = chaincode_uuid[15];
		uuid->timeLow = (chaincode_uuid[0] << 24) | (chaincode_uuid[1] << 16) | (chaincode_uuid[2] << 8) | (chaincode_uuid[3]);
		uuid->timeMid = (chaincode_uuid[4] << 8) | (chaincode_uuid[5]);
		uuid->timeHiAndVersion = (chaincode_uuid[6] << 8) | (chaincode_uuid[7]);
	}

    /* execute WASM with gRPC proxy loop */
    bool execute_wasm_with_grpc_proxy(const std::string& aot_file,
                                     const std::string& function_name, 
                                     const std::vector<std::string>& args,
                                     ServerReaderWriter<ChaincodeProxyMessage, ChaincodeWrapperMessage>* stream)
    {
        TEEC_Operation op;
        uint32_t origin;
        TEEC_Result res;
        FILE* wasm_file = NULL;
        long wasm_file_length;
        unsigned char *wasm_bytecode = NULL;
        
        // AOT 파일 로드
        std::string aot_path = "./chaincode/" + aot_file;
        printf("%s AOT 파일 로드 시작: %s\n", get_timestamp().c_str(), aot_path.c_str());
        
        wasm_file = fopen(aot_path.c_str(), "rb");
        if (!wasm_file) {
            printf("%s Error: AOT 파일 열기 실패: %s\n", get_timestamp().c_str(), aot_path.c_str());
            return false;
        }
        
        fseek(wasm_file, 0, SEEK_END);
        wasm_file_length = ftell(wasm_file);
        wasm_bytecode = (unsigned char*)malloc(wasm_file_length);
        rewind(wasm_file);
        fread(wasm_bytecode, wasm_file_length, 1, wasm_file);
        fclose(wasm_file);
        
        printf("%s AOT 파일 로드 완료: %ld Bytes\n", get_timestamp().c_str(), wasm_file_length);
        
        // 공유 버퍼 설정
        size_t structure_sizes[] = { sizeof(struct key_value), sizeof(struct acknowledgement), sizeof(struct invocation_response), sizeof(struct arguments) };
        size_t max_size = 0;
        for (size_t i = 0; i < sizeof(structure_sizes)/sizeof(structure_sizes[0]); i++)
            if (structure_sizes[i] > max_size) max_size = structure_sizes[i];
        uint8_t *shared_buf = (uint8_t*)calloc(1, max_size);
        if (!shared_buf) {
            free(wasm_bytecode);
            return false;
        }

        memset(&op, 0, sizeof(op));
        // AOT 바이트코드와 arguments 전달
        op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT, TEEC_VALUE_INOUT, TEEC_MEMREF_TEMP_INOUT, TEEC_MEMREF_TEMP_INOUT);

        op.params[0].tmpref.buffer = wasm_bytecode;
        op.params[0].tmpref.size = wasm_file_length;
        op.params[1].value.a = 0;
        op.params[2].tmpref.buffer = shared_buf;
        op.params[2].tmpref.size = max_size;
        op.params[3].tmpref.buffer = ctx.output_buffer;
        op.params[3].tmpref.size = ctx.output_buffer_size;

        // struct arguments 설정
        memset(shared_buf, 0, max_size);
        struct arguments *arguments_data = (struct arguments *)shared_buf;
        
        // function_name을 arguments[0]에 설정
        size_t fn_len = function_name.length();
        if (fn_len >= ARG_SIZE) fn_len = ARG_SIZE - 1;
        memcpy(arguments_data->arguments[0], function_name.c_str(), fn_len);
        arguments_data->arguments[0][fn_len] = '\0';
        
        printf("%s gRPC arguments 설정:\n", get_timestamp().c_str());
        printf("   Function (args[0]): '%s'\n", arguments_data->arguments[0]);
        
        // 나머지 arguments 설정
        size_t n = args.size();
        if (n > ARGS_NUMBER - 1) n = ARGS_NUMBER - 1;
        for (size_t i = 0; i < n; i++) {
            size_t alen = args[i].length();
            if (alen >= ARG_SIZE) alen = ARG_SIZE - 1;
            memcpy(arguments_data->arguments[i + 1], args[i].c_str(), alen);
            arguments_data->arguments[i + 1][alen] = '\0';
            printf("   Arg%zu (args[%zu]): '%s'\n", i, i+1, arguments_data->arguments[i + 1]);
        }

        printf("%s TEE에서 WASM 실행 시작...\n", get_timestamp().c_str());
        res = TEEC_InvokeCommand(&ctx.sess, COMMAND_RUN_WASM, &op, &origin);
        if (res != TEEC_SUCCESS) {
            printf("%s WASM 실행 실패! res=0x%x origin=0x%x\n", get_timestamp().c_str(), res, origin);
            free(shared_buf);
            free(wasm_bytecode);
            return false;
        }

        bool ok = true;
        printf("%s gRPC proxy 루프 진입...\n", get_timestamp().c_str());
        while (true) {
            switch (op.params[1].value.a) {
                case INVOCATION_RESPONSE: {
                    struct invocation_response *resp = (struct invocation_response *)shared_buf;
                    printf("%s [INVOCATION_RESPONSE] %s\n", get_timestamp().c_str(), resp->execution_response);
                    
                    // Send final response to chaincode_wrapper
                    ChaincodeProxyMessage proxy_msg;
                    InvocationResponse* invocation_response = new InvocationResponse();
                    invocation_response->set_execution_response(resp->execution_response);
                    proxy_msg.set_allocated_invocation_response(invocation_response);
                    ok = stream->Write(proxy_msg);
                    goto out;
                }
                case GET_STATE_REQUEST: {
                    struct key_value *kv = (struct key_value *)shared_buf;
                    printf("%s [GET_STATE_REQUEST] chaincode_wrapper로 전송: key='%s'\n", get_timestamp().c_str(), kv->key);
                    
                    // Forward GET_STATE to chaincode_wrapper
                    ChaincodeProxyMessage proxy_msg;
                    GetStateRequest* get_state_request = new GetStateRequest();
                    get_state_request->set_key(kv->key);
                    proxy_msg.set_allocated_get_state_request(get_state_request);
                    if (!stream->Write(proxy_msg)) {
                        printf("Failed to send GET_STATE_REQUEST to chaincode_wrapper\n");
                        ok = false; goto out;
                    }
                    
                    // Wait for response from chaincode_wrapper
                    ChaincodeWrapperMessage wrapper_msg;
                    if (!stream->Read(&wrapper_msg)) {
                        printf("%s ❌ chaincode_wrapper로부터 GET_STATE_RESPONSE 수신 실패\n", get_timestamp().c_str());
                        ok = false; goto out;
                    }
                    std::string get_state_response = wrapper_msg.get_state_response().value();
                    printf("%s [GET_STATE_RESPONSE] chaincode_wrapper로부터 수신: value='%s' (len=%zu)\n", 
                           get_timestamp().c_str(), get_state_response.c_str(), get_state_response.length());
                    
                    // Write response back to shared memory
                    memset(shared_buf, 0, max_size);
                    kv = (struct key_value *)shared_buf;
                    if (get_state_response.length() < VAL_SIZE) {
                        strncpy(kv->value, get_state_response.c_str(), VAL_SIZE - 1);
                    }
                    printf("%s [GET_STATE_RESPONSE] TA 공유 메모리에 쓰기: value='%s'\n", get_timestamp().c_str(), kv->value);
                    
                    // Resume WASM execution
                    printf("%s WASM 실행 재개 (GET_STATE 응답 후)\n", get_timestamp().c_str());
                    op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_VALUE_INOUT, TEEC_MEMREF_TEMP_INOUT, TEEC_MEMREF_TEMP_INOUT);
                    res = TEEC_InvokeCommand(&ctx.sess, COMMAND_RESUME_WASM, &op, &origin);
                    if (res != TEEC_SUCCESS) { 
                        ok = false; goto out; 
                    }
                    break;
                }
                case PUT_STATE_REQUEST: {
                    struct key_value *kv = (struct key_value *)shared_buf;
                    printf("%s [PUT_STATE_REQUEST] chaincode_wrapper로 전송: key='%s', value='%s'\n", get_timestamp().c_str(), kv->key, kv->value);
                    
                    // Forward PUT_STATE to chaincode_wrapper
                    ChaincodeProxyMessage proxy_msg;
                    PutStateRequest* put_state_request = new PutStateRequest();
                    put_state_request->set_key(kv->key);
                    put_state_request->set_value(kv->value);
                    proxy_msg.set_allocated_put_state_request(put_state_request);
                    if (!stream->Write(proxy_msg)) {
                        printf("Failed to send PUT_STATE_REQUEST to chaincode_wrapper\n");
                        ok = false; goto out;
                    }
                    printf("%s PUT_STATE_REQUEST 전송 완료\n", get_timestamp().c_str());
                    
                    // Wait for acknowledgement from chaincode_wrapper
                    ChaincodeWrapperMessage wrapper_msg;
                    if (!stream->Read(&wrapper_msg)) {
                        printf("%s chaincode_wrapper로부터 PUT_STATE_RESPONSE 수신 실패\n", get_timestamp().c_str());
                        ok = false; goto out;
                    }
                    printf("%s [PUT_STATE_RESPONSE] chaincode_wrapper로부터 확인 수신\n", get_timestamp().c_str());
                    std::string put_state_response = wrapper_msg.put_state_response().acknowledgement();
                    printf("%s [PUT_STATE_RESPONSE] 확인 메시지: '%s' (len=%zu)\n", 
                           get_timestamp().c_str(), put_state_response.c_str(), put_state_response.length());
                    
                    // Write acknowledgement back to shared memory
                    memset(shared_buf, 0, max_size);
                    struct acknowledgement *ack = (struct acknowledgement *)shared_buf;
                    if (put_state_response.length() < ACK_SIZE) {
                        strncpy(ack->acknowledgement, put_state_response.c_str(), ACK_SIZE - 1);
                    }
                    printf("%s [PUT_STATE_RESPONSE] TA 공유 메모리에 쓰기: ack='%s'\n", get_timestamp().c_str(), ack->acknowledgement);
                    
                    // Resume WASM execution
                    printf("%s WASM 실행 재개 (PUT_STATE 응답 후)\n", get_timestamp().c_str());
                    op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_VALUE_INOUT, TEEC_MEMREF_TEMP_INOUT, TEEC_MEMREF_TEMP_INOUT);
                    res = TEEC_InvokeCommand(&ctx.sess, COMMAND_RESUME_WASM, &op, &origin);
                    if (res != TEEC_SUCCESS) { 
                        ok = false; goto out; 
                    }
                    break;
                }
                case ERROR:
                default:
                    ok = false;
                    goto out;
            }
        }

    out:
        free(shared_buf);
        free(wasm_bytecode);
        return ok;
    }

private:
    void initialize_session() {
        printf("%s InvocationImpl 세션 초기화 시작\n", get_timestamp().c_str());
        allocate_buffers(&ctx, 5 * 1024);
        prepare_tee_session(&ctx);
        configure_heap_size(&ctx, 10 * 1024 * 1024);  // 10MB heap
        printf("%s InvocationImpl 세션 초기화 완료\n", get_timestamp().c_str());
    }
    
    void cleanup_session() {
        terminate_tee_session(&ctx);
        free_buffers(&ctx);
    }
    
    void restart_session() {
        printf("%s TEE 세션 재시작 (메모리 정리)\n", get_timestamp().c_str());
        cleanup_session();
        initialize_session();
        printf("%s TEE 세션 재시작 완료\n", get_timestamp().c_str());
    }

public:
    InvocationImpl() {
        initialize_session();
    }
    
    ~InvocationImpl() {
        cleanup_session();
    }

    Status TransactionInvocation(ServerContext *context, 
                                ServerReaderWriter<ChaincodeProxyMessage, ChaincodeWrapperMessage> *stream) override
    {
        printf("%s 새로운 gRPC 트랜잭션 요청 수신\n", get_timestamp().c_str());
        
        // Read invocation request from chaincode_wrapper
        ChaincodeWrapperMessage wrapper_msg;
        if (!stream->Read(&wrapper_msg)) {
            return Status(grpc::StatusCode::UNKNOWN, "Failed to read invocation request");  
        }

        // Extract AOT file, function name and arguments
        std::string aot_file = wrapper_msg.invocation_request().aot_file();
        std::string function_name = wrapper_msg.invocation_request().function_name();
        std::vector<std::string> args;
        for (int i = 0; i < wrapper_msg.invocation_request().arguments_size(); i++) {
            args.push_back(wrapper_msg.invocation_request().arguments(i));
        }
        
        printf("%s AOT File: %s, Function: %s, Args count: %zu\n", 
               get_timestamp().c_str(), aot_file.c_str(), function_name.c_str(), args.size());

        // Execute WASM with gRPC proxy loop
        printf("%s WASM 실행 시작\n", get_timestamp().c_str());
        bool success = execute_wasm_with_grpc_proxy(aot_file, function_name, args, stream);
        printf("%s WASM 실행 완료 (성공: %s)\n", get_timestamp().c_str(), success ? "true" : "false");
        
        // 실행 완료 후 TEE 세션 재시작으로 메모리 정리
        restart_session();
        
        if (!success) {
            return Status(grpc::StatusCode::UNKNOWN, "WASM execution failed");
        }
        
        return Status::OK;
    }
};

static void run_server()
{
	printf("%s gRPC 서버 설정 시작\n", get_timestamp().c_str());
	/* create server, add listening port and register service */
	std::string server_address("0.0.0.0:50051");
	InvocationImpl service;
	ServerBuilder builder;
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
	builder.RegisterService(&service);

	/* start the server */
	printf("%s gRPC 서버 시작 중...\n", get_timestamp().c_str());
	std::unique_ptr<Server> server(builder.BuildAndStart());
	printf("%s fixed-proxy gRPC 서버가 %s에서 대기 중\n", get_timestamp().c_str(), server_address.c_str());

	/* 
	* let gRPC server stream run and handle incoming transaction invocation
	* until it gets shutted down or killed 
	*/
	server->Wait();
}

int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("🔧 Fixed Chaincode Proxy with gRPC and WASM Support\n");
        printf("\n");
        printf("기본 동작: gRPC 서버 모드로 실행\n");
        printf("  - 포트 50051에서 chaincode_wrapper 요청 대기\n");
        printf("  - WASM/AOT 파일을 OP-TEE에서 실행\n");
        printf("  - GET_STATE/PUT_STATE 요청을 chaincode_wrapper로 전달\n");
        printf("\n");
        return 0;
    }

    printf("%s Chaincode Proxy 시작 (gRPC + WASM)\n", get_timestamp().c_str());
    printf("%s    gRPC 서버 모드\n", get_timestamp().c_str());
    printf("%s    OP-TEE WASM 실행 준비\n", get_timestamp().c_str());
    printf("\n");

    /* catch Ctrl+C keyboard event to cleanup (kill gRPC server) */
	signal(SIGINT, cleanup);
	
	/* start the gRPC server stream */
	run_server();

    return 0;
}