#include "smartptr.h"

int main() {
    // shared_ptr 생성
    SharedPtr sp = create_shared_ptr(sizeof(int), NULL);
    *(int*)sp.ptr = 42;  // 값을 설정
    printf("SharedPtr: %d\n", *(int*)sp.ptr);

    // 참조 카운트 증가 및 감소 테스트
    retain_shared_ptr(&sp);
    release_shared_ptr(&sp);
    
    // 메모리 해제 확인
    if (sp.ptr == NULL) {
        printf("SharedPtr 메모리 해제 성공\n");
    } else {
        printf("SharedPtr 메모리 해제 실패\n");
    }

    // unique_ptr 생성
    UniquePtr up = create_unique_ptr(sizeof(int), NULL);
    *(int*)up.ptr = 100;  // 값을 설정
    printf("UniquePtr: %d\n", *(int*)up.ptr);

    // unique_ptr 소유권 이전 및 해제
    UniquePtr new_up = transfer_unique_ptr(&up);
    release_unique_ptr(&new_up);

    // 메모리 해제 확인
    if (new_up.ptr == NULL) {
        printf("UniquePtr 메모리 해제 성공\n");
    } else {
        printf("UniquePtr 메모리 해제 실패\n");
    }

    return 0;
}