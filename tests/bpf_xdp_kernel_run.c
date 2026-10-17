/*
 * Privileged Linux verifier and BPF_PROG_RUN proof for the stage-0 XDP object.
 * This test must fail, never skip, when kernel BPF admission is unavailable.
 */
#include <bpf/libbpf.h>
#include <linux/bpf.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_action(const char *text) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !end || *end != '\0' || value < 0 || value > 4)
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
        fprintf(stderr, "expected XDP action must be 0..4\n");
        return 64;
    }

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
    options.repeat = 1;

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

    printf("BPF_XDP_KERNEL_PASS verifier=accepted action=%u duration_ns=%u\n",
           options.retval, options.duration);
    bpf_object__close(object);
    return 0;
}
