# Experimental Linux eBPF socket-filter milestone

This is a **restricted compiler bootstrap** and is not general eBPF support.
It supports the following source form only:

    pub fnc socket_filter(): int {
        return 65535;
    }

One public zero-argument function with an integer literal return value from
0 to 65535. The emitted kernel ABI is i32(ptr ctx), symbol
ores_bpf_socket, ELF section socket and license section GPL. The ctx
pointer is not read. Zero drops the packet, nonzero keeps up to that
many bytes. No host main, guest runtime, heap, actors, dynamic calls,
imports, maps, loops, packet inspection, recursion, helpers, arithmetic,
borrows or exceptions are permitted. Input is capped at 4096 bytes.
Unsupported source must fail before any IR is printed.

## Evidence gates

1. The Oreslang LLVM compiler verifies and prints BPF-specific IR.
2. Clang's BPF backend emits a real little-endian ELF64 with EM_BPF (247),
   socket and license sections. The negative-source matrix rejects
   unsafe and unsupported code.
3. On a Linux host with sufficient BPF privileges, libbpf loads the
   generated ELF, invoking the real kernel verifier; BPF_PROG_RUN then
   executes the program and checks retval == 65535.

The tests/bpf_socket_source_object.py test implements gates 1 and 2.
The tests/bpf_socket_kernel_run.c harness implements gate 3.
The workflow .github/workflows/ebpf.yml reports them separately.
Neither compiler IR verification nor an EM_BPF object alone is proof of
kernel verifier acceptance. An unavailable kernel privilege is a blocker,
not a passing or skipped test.

Next: bounded packet parsing with checked data/data_end provenance, helper
allowlists, typed BPF maps and ring buffers, BTF/CO-RE compatibility, and
XDP hooks. Keep ordinary Oreslang actors in user space.

Do not export private source to other GitHub organizations to obtain
CI runner minutes without explicit approval for that source transfer.
