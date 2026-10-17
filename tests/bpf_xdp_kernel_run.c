/*
 * Privileged Linux verifier and BPF_PROG_RUN proof for the stage-0 XDP object.
 * This test must fail, never skip, when kernel BPF admission is unavailable.
 */
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/bpf.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int reject_invalid_program(const char *label,
                                  const struct bpf_insn *insns,
                                  size_t insn_count) {
    char log[65536];
    memset(log, 0, sizeof(log));
    LIBBPF_OPTS(bpf_prog_load_opts, opts,
        .log_level = 1,
        .log_buf = log,
        .log_size = sizeof(log));

    int fd = bpf_prog_load(BPF_PROG_TYPE_XDP, label, "Proprietary",
                           insns, insn_count, &opts);
    if (fd >= 0) {
        close(fd);
        fprintf(stderr, "invalid XDP fixture %s unexpectedly passed kernel verifier\n",
                label);
        return 1;
    }
    if (!log[0]) {
        fprintf(stderr, "invalid XDP fixture %s failed without verifier log: %s\n",
                label, strerror(errno));
        return 1;
    }
    printf("BPF_NEGATIVE_VERIFIER_PASS fixture=%s log_bytes=%zu\n",
           label, strnlen(log, sizeof(log)));
    return 0;
}

static int prove_negative_verifier(void) {
    // 1. EXIT reads R0 before initialization.
    const struct bpf_insn uninitialized_r0[] = {
        { .code = BPF_JMP | BPF_EXIT }
    };
    // 2. XDP context is a typed pointer; reading thousands of bytes past the
    // xdp_md layout must be rejected rather than treated as arbitrary memory.
    const struct bpf_insn invalid_context_read[] = {
        { .code = BPF_LDX | BPF_MEM | BPF_W,
          .dst_reg = BPF_REG_0, .src_reg = BPF_REG_1, .off = 4096 },
        { .code = BPF_JMP | BPF_EXIT }
    };
    // 3. Stack slots are unreadable until initialized.
    const struct bpf_insn uninitialized_stack[] = {
        { .code = BPF_LDX | BPF_MEM | BPF_DW,
          .dst_reg = BPF_REG_0, .src_reg = BPF_REG_10, .off = -8 },
        { .code = BPF_JMP | BPF_EXIT }
    };
    // 4. Unknown helper IDs must never be callable merely because a BPF CALL
    // instruction can be represented.
    const struct bpf_insn unknown_helper[] = {
        { .code = BPF_JMP | BPF_CALL, .imm = 0x7fffffff },
        { .code = BPF_JMP | BPF_EXIT }
    };

    if (reject_invalid_program("bad_r0", uninitialized_r0,
                               sizeof(uninitialized_r0) / sizeof(uninitialized_r0[0])) ||
        reject_invalid_program("bad_ctx", invalid_context_read,
                               sizeof(invalid_context_read) / sizeof(invalid_context_read[0])) ||
        reject_invalid_program("bad_stack", uninitialized_stack,
                               sizeof(uninitialized_stack) / sizeof(uninitialized_stack[0])) ||
        reject_invalid_program("bad_helper", unknown_helper,
                               sizeof(unknown_helper) / sizeof(unknown_helper[0])))
        return 1;
    return 0;
}

static int parse_action(const char *text) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !end || *end != '\0' || value < 0 || value > 3)
        return -1;
    return (int)value;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: bpf_xdp_kernel_run object.o expected_action\n");
        return 64;
    }
    const int expected = parse_action(argv[2]);
    if (expected < 0) {
        fprintf(stderr, "expected XDP action must be 0..3\n");
        return 64;
    }

    if (prove_negative_verifier() != 0)
        return 1;

    struct bpf_object *object = bpf_object__open_file(argv[1], NULL);
    long open_error = libbpf_get_error(object);
    if (open_error || !object) {
        fprintf(stderr, "libbpf object open failed: %ld\n", open_error);
        return 1;
    }

    struct bpf_program *program = NULL;
    struct bpf_program *only = NULL;
    int program_count = 0;
    bpf_object__for_each_program(program, object) {
        ++program_count;
        only = program;
    }
    if (program_count != 1 || !only ||
        strcmp(bpf_program__name(only), "ores_xdp") != 0 ||
        bpf_program__type(only) != BPF_PROG_TYPE_XDP) {
        fprintf(stderr, "object must contain exactly one XDP program named ores_xdp\n");
        bpf_object__close(object);
        return 1;
    }

    char verifier_log[65536];
    memset(verifier_log, 0, sizeof(verifier_log));
    int log_error = bpf_program__set_log_buf(only, verifier_log, sizeof(verifier_log));
    if (log_error) {
        fprintf(stderr, "cannot configure verifier log buffer: %d\n", log_error);
        bpf_object__close(object);
        return 1;
    }

    if (bpf_object__load(object)) {
        fprintf(stderr, "kernel verifier/load rejected program or BPF capability unavailable\n%s\n",
                verifier_log[0] ? verifier_log : "(no verifier log returned)");
        bpf_object__close(object);
        return 1;
    }

    unsigned char packet[64] = {0};
    unsigned char packet_out[128] = {0};
    struct bpf_test_run_opts options;
    memset(&options, 0, sizeof(options));
    options.sz = sizeof(options);
    options.data_in = packet;
    options.data_size_in = sizeof(packet);
    options.data_out = packet_out;
    options.data_size_out = sizeof(packet_out);
    options.repeat = 8;

    if (bpf_prog_test_run_opts(bpf_program__fd(only), &options)) {
        fprintf(stderr, "BPF_PROG_RUN failed: %s\n", strerror(errno));
        bpf_object__close(object);
        return 1;
    }
    if ((int)options.retval != expected) {
        fprintf(stderr, "XDP return %u != expected %d\n", options.retval, expected);
        bpf_object__close(object);
        return 1;
    }
    if (options.data_size_out > sizeof(packet_out)) {
        fprintf(stderr, "BPF_PROG_RUN reported impossible output size %u\n",
                options.data_size_out);
        bpf_object__close(object);
        return 1;
    }

    printf("BPF_XDP_KERNEL_PASS verifier=accepted action=%u duration_ns=%u\n",
           options.retval, options.duration);
    bpf_object__close(object);
    return 0;
}
