#include "smartptr.h"

int main() {
    // shared_ptr 생성
    SharedPtr sp = create_shared_ptr(sizeof(int), NULL);
    *(int*)sp.ptr = 42;  // 값을 설정
    printf("SharedPtr Origin Value : %d\n", *(int*)sp.ptr);

    // 참조 카운트 증가 및 감소 테스트
    retain_shared_ptr(&sp);
    printf("참조 카운트: %d\n", *(sp.ref_count));
    printf("SharedPtr 2nd Value : %d\n", *(int*)sp.ptr);

    retain_shared_ptr(&sp);
    printf("참조 카운트: %d\n", *(sp.ref_count));
    printf("SharedPtr 3th Value : %d\n", *(int*)sp.ptr);

    release_shared_ptr(&sp);
    
    // 메모리 해제 확인
    if (sp.ptr == NULL) {
        printf("SharedPtr 메모리 해제 성공\n");
    } else {
        printf("SharedPtr 메모리 해제 실패\n");
    }
    printf("sp.ptr : %p\n", sp.ptr);

    // unique_ptr 생성
    UniquePtr up = create_unique_ptr(sizeof(int), NULL);
    *(int*)up.ptr = 100;  // 값을 설정
    printf("UniquePtr Origin Value : %d\n", *(int*)up.ptr);

    // unique_ptr 소유권 이전 및 해제
    UniquePtr new_up = transfer_unique_ptr(&up);
    printf("UniquePtr 2nd Value : %d\n", *(int*)new_up.ptr);
    release_unique_ptr(&new_up);

    // 메모리 해제 확인
    if (new_up.ptr == NULL && up.ptr == NULL) {
        printf("UniquePtr 메모리 해제 성공\n");
    } else {
        printf("UniquePtr 메모리 해제 실패\n");
    }

    return 0;
}