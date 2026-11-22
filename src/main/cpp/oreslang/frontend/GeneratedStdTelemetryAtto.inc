// Generated from stdlib/std/telemetry.atto; do not edit manually.
// source Git blob SHA-1: 2a339892995922e0064fc903fd264d9efd5e9b26
R"ATTO_STD_TLM(// LLVM-owned native Atto std/telemetry snapshot/admission policies.
// No wire parsing, OTLP I/O, JIT patching, clocks or host tracing in source.
// Native AOT may only toggle probes compiled into the exact code generation.
define module telemetry as
  pub fnc traceparent_v00_fields_valid(int version, int trace_high,
       int trace_low, int parent_id, int flags): bool {
    if version != 0 || (trace_high == 0 && trace_low == 0) then return false; fi
    if parent_id == 0 then return false; fi
    return flags >= 0 && flags <= 255;
  }

  pub fnc sampled_flag(int flags): bool {
    return flags >= 0 && flags <= 255 && flags % 2 == 1;
  }

  // 0 DROP, 1 RECORD_ONLY, 2 RECORD_AND_SAMPLE; 3 selects parent policy.
  pub fnc sampling_decision(int policy, bool parent_sampled): int {
    if policy == 1 then return 1; fi
    if policy == 2 then return 2; fi
    if policy == 3 && parent_sampled then return 2; fi
    return 0;
  }

  pub fnc decision_is_recording(int decision): bool {
    return decision == 1 || decision == 2;
  }

  pub fnc decision_is_sampled(int decision): bool {
    return decision == 2;
  }

  pub fnc span_window_valid(int started_ns, int ended_ns): bool {
    return started_ns >= 0 && ended_ns >= started_ns;
  }

  pub fnc dormant_probe_admissible(int compiled_ceiling,
                                    int requested, bool authorized): bool {
    if !authorized || compiled_ceiling < 1 || compiled_ceiling > 3 then return false; fi
    return requested >= 1 && requested <= compiled_ceiling;
  }

  pub fnc queue_admissible(int queued, int capacity,
                           int batch_bytes, int available_bytes): bool {
    if capacity < 1 || capacity > 65536 then return false; fi
    if queued < 0 || queued >= capacity then return false; fi
    return batch_bytes >= 0 && available_bytes >= 0 &&
           batch_bytes <= available_bytes;
  }

  pub fnc drop_increment(int count): int {
    if count < 0 then return -1; fi
    if count == 2147483647 then return count; fi
    return count + 1;
  }
end
)ATTO_STD_TLM"
