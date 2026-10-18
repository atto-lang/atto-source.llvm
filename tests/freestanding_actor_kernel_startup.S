.syntax unified
.cpu cortex-m3
.thumb
.section .isr_vector,"a",%progbits
.word __stack_top
.word Reset_Handler

.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
.thumb_func
Reset_Handler:
  @ Copy initialized data from flash load address to SRAM.
  ldr r0, =__data_load_start
  ldr r1, =__data_start
  ldr r2, =__data_end
1:
  cmp r1,r2
  bhs 2f
  ldr r3,[r0],#4
  str r3,[r1],#4
  b 1b
2:
  @ Zero the BSS before touching static OresKernel storage.
  ldr r1, =__bss_start
  ldr r2, =__bss_end
  movs r3,#0
3:
  cmp r1,r2
  bhs 4f
  str r3,[r1],#4
  adds r1,#4
  b 3b
4:
  bl firmware_main
  cmp r0,#0
  bne guest_fail
guest_pass:
  ldr r1, =pass_message
  movs r0,#4
  bkpt #0xab
  ldr r1, =exit_success
  movs r0,#0x20
  bkpt #0xab
5:
  b 5b
guest_fail:
  ldr r1, =fail_message
  movs r0,#4
  bkpt #0xab
  ldr r1, =exit_failure
  movs r0,#0x20
  bkpt #0xab
6:
  b 6b

.section .rodata,"a",%progbits
.balign 4
pass_message: .asciz "ORES_CORTEXM_ACTOR_KERNEL_PASS\n"
fail_message: .asciz "ORES_CORTEXM_ACTOR_KERNEL_FAIL\n"
.balign 4
exit_success: .word 0x20026, 0
exit_failure: .word 0x20023, 1
