#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <stdbool.h>
#include <netdb.h>
#include <arpa/inet.h>

// 고급 오류 처리 함수 구현
#include "ename.c.inc"

#define BUF_SIZE 100
#define NUM_THREADS 3
#define MAX_STRING_SIZE 100

#define RETAIN_SHARED_PTR(ptr) retain_shared_ptr(ptr);
#define RELEASE_SHARED_PTR(ptr) release_shared_ptr(ptr);

static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;
static void kernel_errExit(const char *format, ...);
static void safe_kernel_printf(const char *format, ...);
static void terminate(bool useExit3);
void default_deleter(void *ptr);

// 네트워크 정보 구조체
typedef struct {
    char ip[INET_ADDRSTRLEN];  // IPv4 주소를 저장
    sa_family_t family;        // 주소 패밀리 (AF_INET 등)
} NetworkInfo;

// 네트워크 정보를 얻는 함수
NetworkInfo get_local_network_info() {
    struct addrinfo hints, *res;
    NetworkInfo net_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;  // 로컬 호스트를 위한 IP 주소 찾기

    // 호스트 이름을 통해 네트워크 정보 얻기
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
        perror("getaddrinfo 실패");
        exit(EXIT_FAILURE);
    }

    // 네트워크 정보 저장 (IPv4 주소)
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &(ipv4->sin_addr), net_info.ip, INET_ADDRSTRLEN);
    net_info.family = res->ai_family;

    freeaddrinfo(res);  // 동적 할당된 메모리 해제
    return net_info;
}

// 기본 소멸자 함수 (free)
void default_deleter(void *ptr) {
    free(ptr);
}

// shared_ptr 구조체 정의
typedef struct {
    void *ptr;               // 실제로 가리키는 메모리
    int *ref_count;          // 참조 카운트
    pthread_mutex_t *mutex;  // 참조 카운트 보호용 뮤텍스
    void (*deleter)(void*);  // 사용자 정의 소멸자 함수
} SharedPtr;

// unique_ptr 구조체 정의
typedef struct {
    void *ptr;               // 실제로 가리키는 메모리
    void (*deleter)(void*);  // 사용자 정의 소멸자 함수
} UniquePtr;

// 공유 스마트 포인터 생성 (shared_ptr)
SharedPtr create_shared_ptr(size_t size, void (*deleter)(void*)) {
    SharedPtr sp;
    sp.ptr = malloc(size);  // 동적 메모리 할당
    sp.ref_count = (int*)malloc(sizeof(int));  // 참조 카운트 메모리 할당
    *(sp.ref_count) = 1;    // 참조 카운트 초기값 1
    sp.mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));  // 뮤텍스 할당
    sp.deleter = deleter ? deleter : default_deleter;  // 사용자 정의 소멸자 함수 설정
    pthread_mutex_init(sp.mutex, NULL);  // 뮤텍스 초기화

    return sp;
}

// 고유 스마트 포인터 생성 (unique_ptr)
UniquePtr create_unique_ptr(size_t size, void (*deleter)(void*)) {
    UniquePtr up;
    up.ptr = malloc(size);  // 동적 메모리 할당
    up.deleter = deleter ? deleter : default_deleter;  // 사용자 정의 소멸자 함수 설정
    return up;
}

// shared_ptr 참조 카운트 증가 (retain)
void retain_shared_ptr(SharedPtr *sp) {
    pthread_mutex_lock(sp->mutex);  // 참조 카운트 보호를 위한 뮤텍스 잠금
    (*(sp->ref_count))++;           // 참조 카운트 증가
    pthread_mutex_unlock(sp->mutex);  // 뮤텍스 잠금 해제
}

// shared_ptr 참조 카운트 감소 및 메모리 해제 (release)
void release_shared_ptr(SharedPtr *sp) {
    int should_free = 0;

    pthread_mutex_lock(sp->mutex);  // 참조 카운트 보호를 위한 뮤텍스 잠금
    (*(sp->ref_count))--;           // 참조 카운트 감소
    if (*(sp->ref_count) == 0) {    // 참조 카운트가 0이 되면 메모리 해제
        should_free = 1;
    }
    pthread_mutex_unlock(sp->mutex);  // 뮤텍스 잠금 해제

    if (should_free) {
        sp->deleter(sp->ptr);       // 사용자 정의 소멸자 호출을 통한 메모리 해제
        sp->ptr = NULL;             // 포인터를 NULL로 설정하여 중복 해제를 방지
        free(sp->ref_count);        // 참조 카운트 메모리 해제
        pthread_mutex_destroy(sp->mutex);  // 뮤텍스 파괴
        free(sp->mutex);            // 뮤텍스 메모리 해제
    }
}

// unique_ptr 메모리 해제 (release)
void release_unique_ptr(UniquePtr *up) {
    if (up->ptr) {
        up->deleter(up->ptr);  // 사용자 정의 소멸자 호출
        up->ptr = NULL;        // 포인터를 NULL로 설정하여 중복 해제를 방지
    }
}

// unique_ptr 소유권 이전 (transfer, move semantics)
UniquePtr transfer_unique_ptr(UniquePtr *up) {
    UniquePtr new_up = *up;  // 새로운 unique_ptr로 소유권 이동
    up->ptr = NULL;          // 기존 unique_ptr은 NULL로 설정
    return new_up;
}

// 스레드 함수 (shared_ptr 사용)
void* thread_function_shared(void* arg) {
    SharedPtr *sp = (SharedPtr*)arg;
    retain_shared_ptr(sp);  // 스레드 내에서 참조 카운트 증가
    printf("스레드에서 shared_ptr 사용 중 - ref_count: %d\n", *(sp->ref_count));

    sleep(1);  // 작업 수행...

    release_shared_ptr(sp);  // 참조 카운트 감소
    return NULL;
}

// 스레드 함수 (unique_ptr 사용)
void* thread_function_unique(void* arg) {
    UniquePtr *up = (UniquePtr*)arg;
    printf("스레드에서 unique_ptr 사용 중\n");

    sleep(1);  // 작업 수행...

    // unique_ptr은 스레드 종료 시 자동 해제됨
    return NULL;
}

// 안전한 출력 함수
static void safe_kernel_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    pthread_mutex_lock(&print_mutex);  // 출력 뮤텍스 잠금
    vprintf(format, args);  // 출력 수행
    pthread_mutex_unlock(&print_mutex);  // 출력 뮤텍스 해제

    if(errno != 0) {
        kernel_errExit("Failed to print message");
    }

    va_end(args);
}

// 오류 출력 함수
static void kernel_errExit(const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    safe_kernel_printf("ERROR: %s\n", format);
    fprintf(stderr, "errno: %d (%s)\n", errno, strerror(errno));
    
    va_end(args);
    exit(EXIT_FAILURE);  // 프로그램 종료
}

// 종료 처리 함수
static void terminate(bool useExit3) {
    if (useExit3) {
        exit(EXIT_FAILURE);
    } else {
        _exit(EXIT_FAILURE);
    }
}
