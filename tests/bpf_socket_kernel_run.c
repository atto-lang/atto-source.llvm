/*
 * Privileged Linux verifier and BPF_PROG_RUN test. Never silently skip.
 * bpf_object__load invokes the actual Linux kernel BPF verifier.
 */
#include <bpf/libbpf.h>
#include <linux/bpf.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: bpf_socket_kernel_run object.o\n");
        return 64;
    }
    struct bpf_object *object = bpf_object__open_file(argv[1], NULL);
    long error = libbpf_get_error(object);
    if (error || !object) {
        fprintf(stderr, "libbpf object open failed: %ld\n", error);
        return 1;
    }
    struct bpf_program *program = bpf_object__find_program_by_name(object, "ores_bpf_socket");
    if (!program || bpf_program__type(program) != BPF_PROG_TYPE_SOCKET_FILTER) {
        fprintf(stderr, "wrong or missing BPF socket filter type\n");
        bpf_object__close(object);
        return 1;
    }
    if (bpf_object__load(object)) {
        fprintf(stderr, "kernel BPF verifier rejected program or BPF capability unavailable\n");
        bpf_object__close(object);
        return 1;
    }
    unsigned char packet[64] = {0};
    struct bpf_test_run_opts options;
    memset(&options, 0, sizeof(options));
    options.sz = sizeof(options);
    options.data_in = packet;
    options.data_size_in = sizeof(packet);
    options.repeat = 1;
    if (bpf_prog_test_run_opts(bpf_program__fd(program), &options)) {
        perror("BPF_PROG_RUN failed");
        bpf_object__close(object);
        return 1;
    }
    if (options.retval != 65535) {
        fprintf(stderr, "BPF_PROG_RUN result %u != expected 65535\n", options.retval);
        bpf_object__close(object);
        return 1;
    }
    printf("BPF_KERNEL_TEST_PASS verifier=accepted retval=%u\n", options.retval);
    bpf_object__close(object);
    return 0;
}
