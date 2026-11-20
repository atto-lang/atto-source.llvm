; Source-free, hand-authored LLVM IR ABI fixture. NOT Atto compiler output.
source_filename = "synthetic-telemetry-abi"
target triple = "x86_64-unknown-linux-gnu"
@.name = private unnamed_addr constant [6 x i8] c"hammer"
@.first = private unnamed_addr constant [14 x i8] c"hammer.started"
@.second = private unnamed_addr constant [15 x i8] c"hammer.finished"
declare i64 @ores_native_telemetry_span_begin(ptr, i64)
declare void @ores_native_telemetry_span_end(i64)
declare i32 @ores_native_telemetry_log_info(i64, i64, i64, i64, ptr, i64)
define i64 @hammer() {
entry:
  %token = call i64 @ores_native_telemetry_span_begin(ptr @.name, i64 6)
  %a = call i32 @ores_native_telemetry_log_info(i64 -3028423055512222963, i64 -6022454595349414358, i64 7290789254643732443, i64 -7822818455218873221, ptr @.first, i64 14), !atto.source.site_id.v1 !0
  %b = call i32 @ores_native_telemetry_log_info(i64 -3028423055512222963, i64 -6022454595349414358, i64 8608480567731124087, i64 8608480567731124087, ptr @.second, i64 15), !atto.source.site_id.v1 !1
  call void @ores_native_telemetry_span_end(i64 %token)
  ret i64 42
}
!0 = !{!"sid_652e1632e7464fdb936fc40fe24f1c7b"}
!1 = !{!"sid_77777777777777777777777777777777"}
