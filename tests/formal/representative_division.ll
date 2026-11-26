declare void @llvm.trap()
define i64 @quotient(i64 %a, i64 %b) {
entry:
  %sa = alloca i64, align 8
  %sb = alloca i64, align 8
  store i64 %a, ptr %sa, align 8
  store i64 %b, ptr %sb, align 8
  %x = load i64, ptr %sa, align 8
  %y = load i64, ptr %sb, align 8
  %z = icmp eq i64 %y, 0
  %m = icmp eq i64 %x, -9223372036854775808
  %n = icmp eq i64 %y, -1
  %o = and i1 %m, %n
  %g = or i1 %z, %o
  br i1 %g, label %trap, label %ok
trap:
  call void @llvm.trap()
  unreachable
ok:
  %s = select i1 %g, i64 1, i64 %y
  %q = sdiv i64 %x, %s
  ret i64 %q
}
