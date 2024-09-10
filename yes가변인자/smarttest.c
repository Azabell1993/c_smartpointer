#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <error.h>
#include "smartptr.h"

#define MAX_STRING_SIZE 100

// 메인 함수 - 스마트 포인터 사용 예시
int main() {
    int num_threads;
    int sum = 0;
    char strsum[1024] = "";

    // 사용자로부터 생성할 스레드 개수를 입력받습니다.
    safe_kernel_printf("생성할 스레드의 수를 입력하세요: ");
    scanf("%d", &num_threads);

    // 스레드 포인터와 스레드 번호 동적 할당
    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * num_threads);
    int *thread_ids = (int *)malloc(sizeof(int) * num_threads);

    // 각 스레드에서 사용할 int와 char 값을 저장할 배열 동적 할당
    int *int_num_values = (int *)malloc(sizeof(int) * num_threads);
    char (*char_ptr1_values)[MAX_STRING_SIZE] = (char (*)[MAX_STRING_SIZE])malloc(sizeof(char[MAX_STRING_SIZE]) * num_threads);

    if (threads == NULL || thread_ids == NULL || int_num_values == NULL || char_ptr1_values == NULL) {
        perror("Failed to allocate memory");
        exit(EXIT_FAILURE);
    }

    // 사용자 정의 스레드 수 만큼 int 값 입력을 받습니다.
    for (int i = 0; i < num_threads; i++) {
        thread_ids[i] = i + 1;

        // 사용자로부터 int_num 입력받기
        safe_kernel_printf("스레드 %d에서 사용할 int_num 값을 입력하세요: ", thread_ids[i]);
        scanf("%d", &int_num_values[i]);
    }

    // 사용자 정의 스레드 수 만큼 char 값을 입력받습니다.
    for (int i = 0; i < num_threads; i++) {
        thread_ids[i] = i + 1;

        // 사용자로부터 char_ptr1 입력받기
        safe_kernel_printf("스레드 %d에서 사용할 문자열을 입력하세요: ", thread_ids[i]);
        scanf(" %[^\n]%*c", char_ptr1_values[i]);
    }

    // 사용자 정의 스레드 수 만큼 스레드를 생성
    for (int i = 0; i < num_threads; i++) {
        // 스레드 생성
        pthread_create(&threads[i], NULL, thread_function, &thread_ids[i]);

        // 각 스레드에서 사용할 int 스마트 포인터 생성
        SmartPtr int_num = CREATE_SMART_PTR(int, int_num_values[i]);  // 사용자 입력값으로 초기화

        // 참조 카운트 증가
        retain(&int_num);

        // 스레드별로 할당된 값을 출력
        printf("스레드 %d: int_num = %d\n", thread_ids[i], *(int *)int_num.ptr);
        sum += *(int *)int_num.ptr;
        
        // 스마트 포인터 해제 (참조 카운트 감소)
        release(&int_num);
    }
    safe_kernel_printf("결과 : %d\n", sum);

    // 모든 스레드가 종료될 때까지 대기
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);

        // 각 스레드가 종료된 후 사용할 char 스마트 포인터 생성
        SmartPtr char_ptr1 = CREATE_SMART_PTR(char[MAX_STRING_SIZE], char_ptr1_values[i]);

        // 참조 카운트 증가
        retain(&char_ptr1);

        // 스레드 종료 메시지 출력
        safe_kernel_printf("스레드 %d: %s\n", thread_ids[i], (char *)char_ptr1.ptr);
        strcat(strsum, (char *)char_ptr1.ptr);

        // 스마트 포인터 해제 (참조 카운트 감소)
        release(&char_ptr1);
    }
    safe_kernel_printf("결과 : %s\n", strsum);

    // 동적으로 할당된 스레드 포인터와 스레드 ID 배열 해제
    if (threads != NULL) {
        free(threads);
        threads = NULL;
        safe_kernel_printf("스레드 포인터 메모리 해제 완료\n");
    }

    if (thread_ids != NULL) {
        free(thread_ids);
        thread_ids = NULL;
        safe_kernel_printf("스레드 ID 배열 메모리 해제 완료\n");
    }

    if (int_num_values != NULL) {
        free(int_num_values);
        int_num_values = NULL;
        safe_kernel_printf("int_num_values 배열 메모리 해제 완료\n");
    }

    if (char_ptr1_values != NULL) {
        free(char_ptr1_values);
        char_ptr1_values = NULL;
        safe_kernel_printf("char_ptr1_values 배열 메모리 해제 완료\n");
    }


    return 0;
}
