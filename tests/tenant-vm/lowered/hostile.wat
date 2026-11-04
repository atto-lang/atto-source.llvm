;; A guest with attacker-chosen behaviour, limited only by what Wasm can express. It has no
;; way to write a host pointer: every address it can form is an i32 into its actor's memory.
(module $guest
  (import "env" "memory" (memory 2 2))
  (import "env" "oreslang_vm_spawn" (func $vm_spawn (param i64) (result i64)))
  (import "env" "oreslang_vm_send" (func $vm_send (param i64 i64)))
  (import "env" "oreslang_vm_stop" (func $vm_stop (param i64)))
  (table 1 1 funcref)
  (type $unused (func (param i64) (result i64)))

  (func $recurse (param $n i64) (result i64)
    local.get $n
    i64.const 1
    i64.add
    call $recurse
    i64.const 1
    i64.add)

  ;; Input is op, a, b as i64. State holds one i64.
  (func (export "turn") (param $self i64) (param $state i32) (param $input i32)
        (param $input_length i32) (param $output i32) (param $capacity i32) (result i64)
    (local $op i64) (local $a i64) (local $b i64) (local $value i64)
    (local.set $op (i64.load (local.get $input)))
    (local.set $a (i64.load offset=8 (local.get $input)))
    (local.set $b (i64.load offset=16 (local.get $input)))
    (block $done
      (block $case_stop (block $case_send (block $case_spawn
      (block $case_indirect (block $case_recurse (block $case_spin (block $case_trap
      (block $case_poke (block $case_peek_far (block $case_peek (block $case_read (block $case_add
        (br_table $case_add $case_read $case_peek $case_peek_far $case_poke $case_trap
                  $case_spin $case_recurse $case_indirect $case_spawn $case_send $case_stop $done
                  (i32.wrap_i64 (local.get $op))))
      ;; add: state += a
      (i64.store (local.get $state) (i64.add (i64.load (local.get $state)) (local.get $a)))
      (local.set $value (i64.load (local.get $state)))
      (br $done))
      ;; report memory.size and memory.grow, which must stay fixed
      (local.set $value (i64.extend_i32_s (memory.grow (i32.const 1))))
      (br $done))
      ;; peek: any address it likes
      (local.set $value (i64.load (i32.wrap_i64 (local.get $a))))
      (br $done))
      ;; peek with the largest static displacement Wasm allows
      (local.set $value (i64.load offset=4294967295 (i32.wrap_i64 (local.get $a))))
      (br $done))
      ;; poke, with the largest displacement as well if b says so
      (if (i64.eq (local.get $b) (i64.const -1))
        (then (i64.store offset=4294967295 (i32.wrap_i64 (local.get $a)) (local.get $b)))
        (else (i64.store (i32.wrap_i64 (local.get $a)) (local.get $b))))
      (br $done))
      unreachable)
      (loop $forever (br $forever)))
      (local.set $value (call $recurse (i64.const 0)))
      (br $done))
      ;; indirect call through an empty table slot, at a chosen index
      (local.set $value (call_indirect (type $unused) (i64.const 0) (i32.wrap_i64 (local.get $a))))
      (br $done))
      ;; host calls, with whatever references it likes
      (local.set $value (call $vm_spawn (local.get $a)))
      (br $done))
      (call $vm_send (local.get $a) (local.get $b))
      (br $done))
      (call $vm_stop (local.get $a))
      (br $done))
    (i64.store (local.get $output) (local.get $value))
    (i64.const 8)))
