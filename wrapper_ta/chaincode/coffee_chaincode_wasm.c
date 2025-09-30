// WASM 체인코드
#include <stdint.h>

// 네이티브 임포트 선언
#if defined(__wasm__)
__attribute__((import_module("env"))) int cc_get_function(char *out, int out_len);
__attribute__((import_module("env"))) int cc_get_arg(int idx, char *out, int out_len);
__attribute__((import_module("env"))) int cc_get_state(const char *key, int key_len, char *out, int out_len);
__attribute__((import_module("env"))) int cc_put_state(const char *key, int key_len, const char *val, int val_len);
__attribute__((import_module("env"))) void cc_return_response(const char *msg, int msg_len);
__attribute__((import_module("env"))) void cc_log(const char *msg, int msg_len);
__attribute__((import_module("env"))) int debug_log(int step_num);

__attribute__((import_module("env"))) void *memset(void *s, int c, unsigned long n);
__attribute__((import_module("env"))) void *memcpy(void *d, const void *s, unsigned long n);
__attribute__((import_module("env"))) void *memmove(void *d, const void *s, unsigned long n);
#endif

// 경량 유틸리티(표준 라이브러리 대체) - 먼저 정의
static int s_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void s_memset(void *dst, int v, int n) { unsigned char *p = (unsigned char*)dst; for (int i=0;i<n;i++) p[i] = (unsigned char)v; }
static void s_memcpy(void *dst, const void *src, int n) { unsigned char *d=(unsigned char*)dst; const unsigned char *s=(const unsigned char*)src; for (int i=0;i<n;i++) d[i]=s[i]; }
static int s_streq(const char *a, const char *b) { int i=0; for (;;i++){ char ca=a[i], cb=b[i]; if (ca!=cb) return 0; if (ca==0) return 1; } }
static unsigned long s_atoul(const char *s) { unsigned long v=0; int i=0; while (s && s[i]>='0' && s[i]<='9') { v = v*10 + (unsigned long)(s[i]-'0'); i++; } return v; }
static void s_ultoa(unsigned long v, char *out, int out_len) {
    if (!out || out_len <= 0) return;
    if (v == 0) { if (out_len>0) out[0]='0'; if (out_len>1) out[1]=0; return; }
    char buf[32]; int i=0; while (v && i < (int)sizeof(buf)) { buf[i++] = (char)('0' + (v % 10)); v/=10; }
    int k=0; while (i && k < out_len-1) out[k++] = buf[--i]; out[k]=0;
}

// 체인코드 내부 상태
enum Op { OP_NONE=0, OP_CREATE=1, OP_ADD=2, OP_QUERY=3 };
static int fsm_state = 0; // 0: idle
static enum Op current_op = OP_NONE;

// 중간 데이터 버퍼(재개 간 보존 필요)
#define KEY_MAX   64
#define ARG_MAX   64
#define VAL_MAX   256

// fsm_state 범례
// 0: idle - 초기 상태
// 11: CREATE_AFTER_GET - create 작업 중 get_state 완료 후 상태
// 12: CREATE_AFTER_PUT - create 작업 중 put_state 완료 후 상태
// 21: ADD_AFTER_GET - add 작업 중 get_state 완료 후 상태  
// 22: ADD_AFTER_PUT - add 작업 중 put_state 완료 후 상태
// 31: QUERY_AFTER_GET - query 작업 중 get_state 완료 후 상태

// 변수 선언
static char g_function[KEY_MAX];
static char g_arg0[ARG_MAX];
static char g_arg1[ARG_MAX];
static char g_person[KEY_MAX];
static char g_cur_val[VAL_MAX];
static char g_tmp[VAL_MAX];

// 조기 종료 플래그
static int g_should_exit = 0;

static void log_str(const char *s) { (void)s; }

// create: step 흐름
static void cc_do_create_init() {
    s_memset(g_person, 0, KEY_MAX);
    s_memset(g_arg1, 0, ARG_MAX);
    s_memset(g_cur_val, 0, VAL_MAX);
    int arg0_len = cc_get_arg(0, g_person, KEY_MAX);
    int arg1_len = cc_get_arg(1, g_arg1, ARG_MAX);
    int get_state_result = cc_get_state(g_person, s_strlen(g_person), g_cur_val, VAL_MAX);
    if (get_state_result > 0) {
        // cc_get_state가 성공적으로 pending_type을 설정했음
        fsm_state = 11; // CREATE_AFTER_GET
        g_should_exit = 1; // 조기 종료 플래그 설정
    } else {
    }
}

static void cc_do_create_resume() {
    if (fsm_state == 11) {
        if (g_cur_val[0] != 0) { 
            cc_return_response("EXIST", 5);
            fsm_state = 0; current_op = OP_NONE; return; 
        }
        (void)cc_put_state(g_person, s_strlen(g_person), g_arg1, s_strlen(g_arg1));
        fsm_state = 12; return;
    }
    if (fsm_state == 12) { 
        cc_return_response("OK", 2);
        fsm_state = 0; current_op = OP_NONE; return; 
    }
}

// add: step 흐름
static void cc_do_add_init() {
    s_memset(g_person, 0, KEY_MAX);
    s_memset(g_arg1, 0, ARG_MAX);
    s_memset(g_cur_val, 0, VAL_MAX);
    (void)cc_get_arg(0, g_person, KEY_MAX);
    (void)cc_get_arg(1, g_arg1, ARG_MAX);
    (void)cc_get_state(g_person, s_strlen(g_person), g_cur_val, VAL_MAX);
    fsm_state = 21; // ADD_AFTER_GET
}

static void cc_do_add_resume() {
    
    if (fsm_state == 21) {
        if (g_cur_val[0] == 0) { 
            cc_return_response("EMPTY", 5);
            fsm_state = 0; current_op = OP_NONE; return; 
        }
        unsigned long cur = s_atoul(g_cur_val);
        unsigned long add = s_atoul(g_arg1);
        unsigned long nv = cur + add;
        s_memset(g_tmp, 0, VAL_MAX);
        s_ultoa(nv, g_tmp, VAL_MAX);
        (void)cc_put_state(g_person, s_strlen(g_person), g_tmp, s_strlen(g_tmp));
        fsm_state = 22; return;
    }
    if (fsm_state == 22) {
        cc_return_response("OK", 2);
        fsm_state = 0; current_op = OP_NONE;
        return; 
    }
}

// query: step 흐름
static void cc_do_query_init() {
    s_memset(g_person, 0, KEY_MAX);
    s_memset(g_cur_val, 0, VAL_MAX);
    (void)cc_get_arg(0, g_person, KEY_MAX);
    (void)cc_get_state(g_person, s_strlen(g_person), g_cur_val, VAL_MAX);
    // 디버그 로그: query cc_get_state 완료
    fsm_state = 31; // QUERY_AFTER_GET
}

static void cc_do_query_resume() {
    if (fsm_state == 31) {
        if (g_cur_val[0] == 0) { 
            cc_return_response("NOTFOUND", 8);
        }
        else { 
            cc_return_response(g_cur_val, s_strlen(g_cur_val));
        }
        fsm_state = 0; current_op = OP_NONE; return;
    }
}

// 외부 진입점
void step_init(void) {
}

void step_resume(void) {
    log_str("WASM: step_resume called");
    
    // 조기 종료 플래그 리셋
    g_should_exit = 0;
    
    // 첫 번째 resume: GET_FUNCTION 결과 반영 및 분기
    if (current_op == OP_NONE) {
        if (!g_function[0]) {
            // 재시도: 아직 채워지지 않았다면 다시 시도
            (void)cc_get_function(g_function, KEY_MAX);
        }
        if (g_function[0]) {
            if (s_streq(g_function, "create")) { 
                current_op = OP_CREATE; 
                cc_do_create_init(); // 실제 create 로직 사용
                goto end_function;
            }
            if (s_streq(g_function, "add"))    { current_op = OP_ADD;    cc_do_add_init();    goto end_function; }
            if (s_streq(g_function, "query"))  { current_op = OP_QUERY;  cc_do_query_init();  goto end_function; }
            cc_return_response("ERROR", 5);
            goto end_function;
        }
    }
    
    // 이후 resume: 각 operation의 resume 처리
    if (current_op == OP_CREATE) { cc_do_create_resume(); goto end_function; }
    if (current_op == OP_ADD)    { cc_do_add_resume();    goto end_function; }
    if (current_op == OP_QUERY)  { cc_do_query_resume();  goto end_function; }
    
    cc_return_response("ERROR", 5);

end_function:
    return; // void 반환으로 변경
}

int main(void) { 
    step_init(); 
    return 0; 
}


