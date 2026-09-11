# VM conformance programs

`vm_program.c` contains the guest Linux workloads shared by native host test
frontends: inherited descriptors, private-page COW, shared mappings, clone
threads, group exit, and autonomous execution. Linux owns their syscall
semantics. `kobox_vm_program_syscall` is supplied by the frontend and transports
the arguments and result; it must not implement these operations itself.

The x86 stack-access instruction sequence is in `arch/x86_64/vm_program.S`.
Native OS launch, syscall interception, fault delivery, and private bootstrap
state are outside the common workload. A fork frontend must enter the child
with a stack that Linux actually inherits, not with an uncopied native
dispatcher stack. A clone frontend returns at Linux's supplied child stack
without expecting a native call's return address there.

These are GPL conformance clients, not a controller wire API or an alternative
Linux process/FD implementation. Native compiler TLS instrumentation cannot
run in a client after its FS/GS bases become guest controlled.
