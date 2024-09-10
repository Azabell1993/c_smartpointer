#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <fcntl.h>

// 고급 오류 처리 함수 구현
#include "ename.c.inc"

#define BUF_SIZE 100
#define NUM_THREADS 3
#define MAX_STRING_SIZE 100

typedef struct SmartPtr SmartPtr;
#define CREATE_SMART_PTR(type, ...) create_smart_ptr(sizeof(type), __VA_ARGS__)

static void retain(SmartPtr *sp);
static void release(SmartPtr *sp);
static void safe_kernel_printf(const char *format, ...);
static void kernel_errExit(const char *format, ...);
static void kernel_socket_communication(int sock_fd, const char *message, char *response, size_t response_size);
static void kernel_create_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg);
static void* thread_function(void* arg);
static void terminate(bool useExit3);
static void kernel_join_thread(pthread_t thread);
static void kernel_wait_for_process(pid_t pid);

static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @struct SmartPtr
 * @brief 스마트 포인터 구조체
 *
 * 이 구조체는 포인터와 참조 카운트, 뮤텍스를 관리합니다.
 */
typedef struct SmartPtr {
    void *ptr;                ///< 실제 메모리를 가리킴
    int *ref_count;           ///< 참조 카운트
    pthread_mutex_t *mutex;   ///< 뮤텍스 보호
} SmartPtr;

/**
 * @brief 네트워크 정보를 저장하는 구조체
 */
typedef struct {
    char ip[INET_ADDRSTRLEN];  ///< IPv4 주소
    sa_family_t family;        ///< 주소 패밀리 (AF_INET 등)
} NetworkInfo;

/**
 * @brief 로컬 네트워크 정보를 가져오는 함수
 *
 * @return NetworkInfo 로컬 네트워크 정보가 저장된 구조체
 */
NetworkInfo get_local_network_info() {
    struct addrinfo hints, *res;
    NetworkInfo net_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

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
 * @brief 스마트 포인터를 생성하는 함수 (가변 인자 사용)
 *
 * @param size 할당할 메모리 크기
 * @param ... 가변 인자 리스트 (초기값)
 * @return SmartPtr 스마트 포인터 구조체
 */
SmartPtr create_smart_ptr(size_t size, ...) {
    SmartPtr sp;
    sp.ptr = malloc(size);
    sp.ref_count = (int *)malloc(sizeof(int));
    *(sp.ref_count) = 1;
    sp.mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(sp.mutex, NULL);

    va_list args;
    va_start(args, size);

    if (size == sizeof(int)) {
        int value = va_arg(args, int);
        *(int *)sp.ptr = value;
    } else if (size == sizeof(char) * MAX_STRING_SIZE) {
        const char *str = va_arg(args, const char *);
        strncpy((char *)sp.ptr, str, MAX_STRING_SIZE);
    }

    va_end(args);
    return sp;
}

/**
 * @brief 스마트 포인터의 참조 카운트를 증가시키는 함수
 *
 * @param sp 증가시킬 스마트 포인터
 */
void retain(SmartPtr *sp) {
    pthread_mutex_lock(sp->mutex);
    (*(sp->ref_count))++;
    pthread_mutex_unlock(sp->mutex);
}

/**
 * @brief 스마트 포인터의 참조 카운트를 감소시키고 필요시 메모리를 해제하는 함수
 *
 * @param sp 해제할 스마트 포인터
 */
void release(SmartPtr *sp) {
    int should_free = 0;

    pthread_mutex_lock(sp->mutex);
    (*(sp->ref_count))--;
    safe_kernel_printf("Smart pointer released (ref_count: %d)\n", *(sp->ref_count));

    if (*(sp->ref_count) == 0) {
        should_free = 1;
        safe_kernel_printf("Reference count is 0, freeing memory...\n");
    }

    pthread_mutex_unlock(sp->mutex);

    if (should_free) {
        free(sp->ptr);
        sp->ptr = NULL;
        free(sp->ref_count);
        sp->ref_count = NULL;

        pthread_mutex_destroy(sp->mutex);
        free(sp->mutex);
        sp->mutex = NULL;

        safe_kernel_printf("Memory has been freed\n");
    }
}

/**
 * @brief 스레드 안전한 출력 함수
 *
 * @param format 출력할 메시지 형식
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
 * @brief 오류 메시지를 출력하고 프로그램을 종료하는 함수
 *
 * @param format 출력할 오류 메시지 형식
 */
static void kernel_errExit(const char *format, ...) {
    va_list argList;
    va_start(argList, format);

    safe_kernel_printf("ERROR: %s\n", format);
    fprintf(stderr, "errno: %d (%s)\n", errno, strerror(errno));
    fflush(stdout);
    va_end(argList);

    terminate(true);
}

/**
 * @brief 프로그램 종료 처리 함수
 *
 * @param useExit3 true면 exit() 호출, false면 _exit() 호출
 */
static void terminate(bool useExit3) {
    char *s = getenv("EF_DUMPCORE");

    if (s != NULL && *s != '\0')
        abort();
    else if (useExit3)
        exit(EXIT_FAILURE);
    else
        _exit(EXIT_FAILURE);
}

/**
 * @brief 스레드에서 수행할 함수
 *
 * @param arg 스레드 인수 (스레드 번호)
 * @return NULL
 */
void* thread_function(void* arg) {
    int thread_num = *((int*)arg);

    NetworkInfo net_info = get_local_network_info();

    safe_kernel_printf("Thread %d: 시작 - 로컬 IP 주소: %s\n", thread_num, net_info.ip);

    sleep(1);

    safe_kernel_printf("Thread %d: 종료 - 주소 패밀리: %d\n", thread_num, net_info.family);
    return NULL;
}

/**
 * @brief 소켓을 사용한 메시지 전송 및 수신 함수
 *
 * @param sock_fd 소켓 파일 디스크립터
 * @param message 전송할 메시지
 * @param response 수신할 응답
 * @param response_size 응답 버퍼 크기
 */
static void kernel_socket_communication(int sock_fd, const char *message, char *response, size_t response_size) {
    if (write(sock_fd, message, strlen(message)) == -1) {
        safe_kernel_printf("Failed to send message through socket");
        kernel_errExit("Failed to send message through socket");
    }

    ssize_t bytes_read = read(sock_fd, response, response_size - 1);
    if (bytes_read == -1) {
        safe_kernel_printf("Failed to receive message from socket");
        kernel_errExit("Failed to receive message from socket");
    }

    response[bytes_read] = '\0';
}

/**
 * @brief 자식 프로세스를 대기하는 함수
 *
 * @param pid 자식 프로세스의 PID
 */
static void kernel_wait_for_process(pid_t pid) {
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        safe_kernel_printf("Failed to wait for process");
        kernel_errExit("Failed to wait for process");
    } else {
        safe_kernel_printf("Child process exited with status %d\n", status);
    }
}

/**
 * @brief 스레드를 생성하는 함수
 *
 * @param thread 생성할 스레드의 포인터
 * @param start_routine 스레드에서 실행할 함수
 * @param arg 스레드 함수에 전달할 인수
 */
static void kernel_create_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg) {
    int err = pthread_create(thread, NULL, start_routine, arg);
    if (err != 0) {
        safe_kernel_printf("Failed to create thread");
        kernel_errExit("Failed to create thread");
    } else {
        safe_kernel_printf("Thread created successfully\n");
    }
}

/**
 * @brief 스레드 종료를 대기하는 함수
 *
 * @param thread 종료 대기할 스레드
 */
static void kernel_join_thread(pthread_t thread) {
    int err = pthread_join(thread, NULL);
    if (err != 0) {
        safe_kernel_printf("Failed to join thread");
        kernel_errExit("Failed to join thread");
    } else {
        safe_kernel_printf("Thread joined successfully\n");
    }
}