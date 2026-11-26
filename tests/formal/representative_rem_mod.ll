declare void @llvm.trap()
define i64 @modulo(i64 %a, i64 %b) {
entry:
  %a.slot = alloca i64, align 8
  store i64 %a, ptr %a.slot, align 4
  %b.slot = alloca i64, align 8
  store i64 %b, ptr %b.slot, align 4
  %x = load i64, ptr %a.slot, align 4
  %y = load i64, ptr %b.slot, align 4
  %zero = icmp eq i64 %y, 0
  br i1 %zero, label %trap, label %ok
trap:
  call void @llvm.trap()
  unreachable
ok:
  %minus = icmp eq i64 %y, -1
  %safe = select i1 %minus, i64 1, i64 %y
  %rem = srem i64 %x, %safe
  %neg = icmp slt i64 %rem, 0
  %pos = icmp sgt i64 %y, 0
  %add = add i64 %rem, %y
  %sub = sub i64 %rem, %y
  %adjust = select i1 %pos, i64 %add, i64 %sub
  %result = select i1 %neg, i64 %adjust, i64 %rem
  ret i64 %result
}
