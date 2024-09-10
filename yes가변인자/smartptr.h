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

// 스마트 포인터 관련 정의
typedef struct SmartPtr {
    void *ptr;
    int *ref_count;
    pthread_mutex_t *mutex;
} SmartPtr;

#define CREATE_SMART_PTR(type, ...) create_smart_ptr(sizeof(type), __VA_ARGS__)

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

// 스마트 포인터 생성 함수 (가변 인자 사용)
SmartPtr create_smart_ptr(size_t size, ...) {
    SmartPtr sp;
    sp.ptr = malloc(size);
    sp.ref_count = (int *)malloc(sizeof(int));
    *(sp.ref_count) = 1;
    sp.mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(sp.mutex, NULL);

    // 가변 인자를 통해 초기화
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

// 참조 카운트 증가
void retain(SmartPtr *sp) {
    pthread_mutex_lock(sp->mutex);
    (*(sp->ref_count))++;
    pthread_mutex_unlock(sp->mutex);
}

// 참조 카운트 감소 및 메모리 해제
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

// 안전한 출력 함수
static void safe_kernel_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    pthread_mutex_lock(&print_mutex);  // 출력 뮤텍스 잠금
    vprintf(format, args);  // 커널 printf 함수 호출
    pthread_mutex_unlock(&print_mutex);  // 출력 뮤텍스 해제

    if(errno != 0) {
        kernel_errExit("Failed to print message");
    }

    va_end(args);
}

// 오류 출력 함수
static void kernel_errExit(const char *format, ...) {
    va_list argList;

    va_start(argList, format);
    stdout->_IO_write_ptr = stdout->_IO_write_end;  // 출력 버퍼 비우기
    safe_kernel_printf("ERROR: %s\n", format);
    fprintf(stderr, "errno: %d (%s)\n", errno, strerror(errno));
    fflush(stdout);
    va_end(argList);

    terminate(true);  // 프로그램 종료
}

// 종료 처리
static void terminate(bool useExit3) {
    char *s = getenv("EF_DUMPCORE");

    if (s != NULL && *s != '\0')
        abort();
    else if (useExit3)
        exit(EXIT_FAILURE);
    else
        _exit(EXIT_FAILURE);
}

// 스레드에서 사용할 함수 예시
void* thread_function(void* arg) {
    int thread_num = *((int*)arg);

    // 네트워크 정보 가져오기
    NetworkInfo net_info = get_local_network_info();

    // 스레드 시작 메시지
    safe_kernel_printf("Thread %d: 시작 - 로컬 IP 주소: %s\n", thread_num, net_info.ip);

    sleep(1); // 작업을 모방하기 위한 대기 시간

    // 스레드 종료 메시지
    safe_kernel_printf("Thread %d: 종료 - 주소 패밀리: %d\n", thread_num, net_info.family);
    return NULL;
}

// 소켓 통신 함수
static void kernel_socket_communication(int sock_fd, const char *message, char *response, size_t response_size) {
    if (write(sock_fd, message, strlen(message)) == -1) {
        safe_kernel_printf("Failed to send message through socket");
    } else {
        safe_kernel_printf("Failed to send message through socket");
        kernel_errExit("Failed to send message through socket");
    }

    ssize_t bytes_read = read(sock_fd, response, response_size - 1);
    if (bytes_read == -1) {
        safe_kernel_printf("Failed to receive message from socket");
        kernel_errExit("Failed to receive message from socket");
    } else {
        kernel_errExit("Failed to receive message from socket");
    }
    response[bytes_read] = '\0';
}

// 자식 프로세스 대기 함수
static void kernel_wait_for_process(pid_t pid) {
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        safe_kernel_printf("Failed to wait for process");
        kernel_errExit("Failed to wait for process");
    } else {
        safe_kernel_printf("Child process exited with status %d\n", status);
    }
}

// 스레드 생성 함수
static void kernel_create_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg) {
    int err = pthread_create(thread, NULL, start_routine, arg);
    if (err != 0) {
        safe_kernel_printf("Failed to create thread");
        kernel_errExit("Failed to create thread");
    } else {
        safe_kernel_printf("Thread created successfully\n");
    }
}

// 스레드 조인 함수
static void kernel_join_thread(pthread_t thread) {
    int err = pthread_join(thread, NULL);
    if (err != 0) {
        safe_kernel_printf("Failed to join thread");\
        kernel_errExit("Failed to join thread");
    } else {
        safe_kernel_printf("Thread joined successfully\n");
    }
}


