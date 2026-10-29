; Test-only hand-written LLVM IR. ABI v2 status/out-value, not .ores lowering.
%OresIsolateContext = type { i32, i32, i64, ptr, ptr, ptr }

declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)

define i32 @guest_counter_llvm(ptr %ctx, i64 %delta, ptr %out) {
entry:
  %vp = getelementptr inbounds %OresIsolateContext, ptr %ctx, i32 0, i32 0
  %version = load i32, ptr %vp, align 4
  %compatible = icmp eq i32 %version, 2
  br i1 %compatible, label %call_load, label %invalid_version
invalid_version:
  ret i32 1
call_load:
  %data_ptr = getelementptr inbounds %OresIsolateContext, ptr %ctx, i32 0, i32 3
  %data = load ptr, ptr %data_ptr, align 8
  %load_fnptr = getelementptr inbounds %OresIsolateContext, ptr %ctx, i32 0, i32 4
  %load_fn = load ptr, ptr %load_fnptr, align 8
  %old = alloca i64, align 8
  %ok = call i32 %load_fn(ptr %data, i32 0, ptr %old)
  %valid = icmp eq i32 %ok, 1
  br i1 %valid, label %call_store, label %invalid_load
invalid_load:
  ret i32 2
call_store:
  %value = load i64, ptr %old, align 8
  %checked = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %value, i64 %delta)
  %overflow = extractvalue { i64, i1 } %checked, 1
  br i1 %overflow, label %invalid_overflow, label %safe_store
invalid_overflow:
  ret i32 4
safe_store:
  %next = extractvalue { i64, i1 } %checked, 0
  %store_fnptr = getelementptr inbounds %OresIsolateContext, ptr %ctx, i32 0, i32 5
  %store_fn = load ptr, ptr %store_fnptr, align 8
  %stored = call i32 %store_fn(ptr %data, i32 0, i64 %next)
  %ok_store = icmp eq i32 %stored, 1
  br i1 %ok_store, label %success, label %invalid_store
invalid_store:
  ret i32 3
success:
  store i64 %next, ptr %out, align 8
  ret i32 0
}
