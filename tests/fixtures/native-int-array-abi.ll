; Native Array<int> C ABI smoke across LLVM opaque-pointer calls.
; This is hand-authored IR for ABI verification, NOT compiled Oreslang source.
declare ptr @oreslang_int_array_region_open()
declare i64 @oreslang_int_array_new(ptr)
declare void @oreslang_int_array_reserve(ptr, i64, i64)
declare void @oreslang_int_array_add(ptr, i64, i64)
declare i64 @oreslang_int_array_length(ptr, i64)
declare i64 @oreslang_int_array_get(ptr, i64, i64)
declare void @oreslang_int_array_set(ptr, i64, i64, i64)
declare void @oreslang_int_array_drop(ptr, i64)
declare void @oreslang_int_array_region_close(ptr)

define i32 @main() {
entry:
  %region = call ptr @oreslang_int_array_region_open()
  %a = call i64 @oreslang_int_array_new(ptr %region)
  %b = call i64 @oreslang_int_array_new(ptr %region)
  call void @oreslang_int_array_reserve(ptr %region, i64 %a, i64 8)
  call void @oreslang_int_array_add(ptr %region, i64 %a, i64 -77)
  call void @oreslang_int_array_add(ptr %region, i64 %a, i64 100)
  call void @oreslang_int_array_set(ptr %region, i64 %a, i64 1, i64 123456789)
  call void @oreslang_int_array_add(ptr %region, i64 %b, i64 44)
  %a_count = call i64 @oreslang_int_array_length(ptr %region, i64 %a)
  %b_count = call i64 @oreslang_int_array_length(ptr %region, i64 %b)
  %a_first = call i64 @oreslang_int_array_get(ptr %region, i64 %a, i64 0)
  %a_last = call i64 @oreslang_int_array_get(ptr %region, i64 %a, i64 1)
  %b_first = call i64 @oreslang_int_array_get(ptr %region, i64 %b, i64 0)
  %same = icmp eq i64 %a, %b
  %c1 = icmp eq i64 %a_count, 2
  %c2 = icmp eq i64 %b_count, 1
  %c3 = icmp eq i64 %a_first, -77
  %c4 = icmp eq i64 %a_last, 123456789
  %c5 = icmp eq i64 %b_first, 44
  %d1 = and i1 %c1, %c2
  %d2 = and i1 %c3, %c4
  %d3 = and i1 %d1, %d2
  %d4 = and i1 %d3, %c5
  %okay = xor i1 %same, true
  %all = and i1 %d4, %okay
  call void @oreslang_int_array_drop(ptr %region, i64 %b)
  call void @oreslang_int_array_drop(ptr %region, i64 %a)
  call void @oreslang_int_array_region_close(ptr %region)
  %result = select i1 %all, i32 0, i32 1
  ret i32 %result
}
