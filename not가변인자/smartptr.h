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

/**
 * @brief 커널 오류 메시지를 출력하고 프로그램을 종료하는 함수
 *
 * @param format 오류 메시지 형식
 */
static void kernel_errExit(const char *format, ...);

/**
 * @brief 스레드 안전한 출력 함수
 *
 * @param format 출력 메시지 형식
 */
static void safe_kernel_printf(const char *format, ...);

/**
 * @brief 종료 처리 함수
 *
 * @param useExit3 true면 exit(), false면 _exit() 호출
 */
static void terminate(bool useExit3);

/**
 * @brief 기본 소멸자 함수 (free 사용)
 *
 * @param ptr 해제할 포인터
 */
void default_deleter(void *ptr) {
    free(ptr);
}

/**
 * @struct NetworkInfo
 * @brief 네트워크 정보를 저장하는 구조체
 *
 * IPv4 주소와 주소 패밀리를 저장합니다.
 */
typedef struct {
    char ip[INET_ADDRSTRLEN];  ///< IPv4 주소
    sa_family_t family;        ///< 주소 패밀리 (AF_INET 등)
} NetworkInfo;

/**
 * @brief 로컬 네트워크 정보를 가져오는 함수
 *
 * @return NetworkInfo 네트워크 정보 구조체
 */
NetworkInfo get_local_network_info() {
    struct addrinfo hints, *res;
    NetworkInfo net_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;  // 로컬 IP 찾기

    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
        perror("getaddrinfo 실패");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &(ipv4->sin_addr), net_info.ip, INET_ADDRSTRLEN);
    net_info.family = res->ai_family;

    freeaddrinfo(res);
    return net_info;
}

/**
 * @struct SharedPtr
 * @brief 공유 스마트 포인터
 */
typedef struct {
    void *ptr;               ///< 실제 메모리
    int *ref_count;          ///< 참조 카운트
    pthread_mutex_t *mutex;  ///< 뮤텍스
    void (*deleter)(void*);  ///< 소멸자 함수
} SharedPtr;

/**
 * @struct UniquePtr
 * @brief 고유 스마트 포인터
 */
typedef struct {
    void *ptr;               ///< 실제 메모리
    void (*deleter)(void*);  ///< 소멸자 함수
} UniquePtr;

/**
 * @brief shared_ptr 생성 함수
 *
 * @param size 할당할 메모리 크기
 * @param deleter 소멸자 함수
 * @return 생성된 SharedPtr 구조체
 */
SharedPtr create_shared_ptr(size_t size, void (*deleter)(void*)) {
    SharedPtr sp;
    sp.ptr = malloc(size);
    sp.ref_count = (int*)malloc(sizeof(int));
    *(sp.ref_count) = 1;
    sp.mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    sp.deleter = deleter ? deleter : default_deleter;
    pthread_mutex_init(sp.mutex, NULL);

    return sp;
}

/**
 * @brief unique_ptr 생성 함수
 *
 * @param size 할당할 메모리 크기
 * @param deleter 소멸자 함수
 * @return 생성된 UniquePtr 구조체
 */
UniquePtr create_unique_ptr(size_t size, void (*deleter)(void*)) {
    UniquePtr up;
    up.ptr = malloc(size);
    up.deleter = deleter ? deleter : default_deleter;
    return up;
}

/**
 * @brief shared_ptr 참조 카운트 증가
 *
 * @param sp 참조할 SharedPtr
 */
void retain_shared_ptr(SharedPtr *sp) {
    pthread_mutex_lock(sp->mutex);
    (*(sp->ref_count))++;
    pthread_mutex_unlock(sp->mutex);
}

/**
 * @brief shared_ptr 참조 카운트 감소 및 메모리 해제
 *
 * @param sp 해제할 SharedPtr
 */
void release_shared_ptr(SharedPtr *sp) {
    if (sp->ptr == NULL) {
        safe_kernel_printf("SharedPtr is already released\n");
        return;
    }

    sp->deleter(sp->ptr);
    sp->ptr = NULL;

    free(sp->ref_count);
    sp->ref_count = NULL;
    pthread_mutex_destroy(sp->mutex);
    free(sp->mutex);
    sp->mutex = NULL;
}

/**
 * @brief unique_ptr 메모리 해제
 *
 * @param up 해제할 UniquePtr
 */
void release_unique_ptr(UniquePtr *up) {
    if (up->ptr) {
        up->deleter(up->ptr);
        up->ptr = NULL;
    }
}

/**
 * @brief unique_ptr 소유권 이전
 *
 * @param up 소유권을 이전할 UniquePtr
 * @return 새로 생성된 UniquePtr
 */
UniquePtr transfer_unique_ptr(UniquePtr *up) {
    UniquePtr new_up = *up;
    up->ptr = NULL;
    return new_up;
}

/**
 * @brief 스레드 함수 (shared_ptr 사용)
 *
 * @param arg SharedPtr 인수
 * @return NULL
 */
void* thread_function_shared(void* arg) {
    SharedPtr *sp = (SharedPtr*)arg;
    retain_shared_ptr(sp);
    printf("스레드에서 shared_ptr 사용 중 - ref_count: %d\n", *(sp->ref_count));

    sleep(1);

    release_shared_ptr(sp);
    return NULL;
}

/**
 * @brief 스레드 함수 (unique_ptr 사용)
 *
 * @param arg UniquePtr 인수
 * @return NULL
 */
void* thread_function_unique(void* arg) {
    UniquePtr *up = (UniquePtr*)arg;
    printf("스레드에서 unique_ptr 사용 중\n");

    sleep(1);
    return NULL;
}

/**
 * @brief 스레드 안전한 출력 함수
 *
 * @param format 출력 메시지 형식
 */
static void safe_kernel_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    pthread_mutex_lock(&print_mutex);
    vprintf(format, args);
    pthread_mutex_unlock(&print_mutex);

    if(errno != 0) {
        kernel_errExit("Failed to print message");
    }

    va_end(args);
}

/**
 * @brief 커널 오류 메시지 출력 후 종료
 *
 * @param format 오류 메시지 형식
 */
static void kernel_errExit(const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    safe_kernel_printf("ERROR: %s\n", format);
    fprintf(stderr, "errno: %d (%s)\n", errno, strerror(errno));
    
    va_end(args);
    exit(EXIT_FAILURE);
}

/**
 * @brief 종료 처리 함수
 *
 * @param useExit3 true면 exit(), false면 _exit() 호출
 */
static void terminate(bool useExit3) {
    if (useExit3) {
        exit(EXIT_FAILURE);
    } else {
        _exit(EXIT_FAILURE);
    }
}