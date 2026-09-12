# Device-backed module launch

The optional `device` input to `kobox_linux_modules_run()` attaches one
host-authorized virtio GPU to the booted Linux core. This is a process-local
GPL interface, not a controller wire format or a host-kernel ABI. Adding the
input and device report changes the unnumbered development structure sizes;
the launcher and core must be rebuilt together. Old sizes are rejected.

The existing one-shot module owner covers the entire transaction, including
PCI preparation. Preparation scans the authorized function, assigns its
resources, attaches DMA and IRQ ports, and publishes the PCI devices before
loading modules. Module admission and reverse unload still use the existing
native module loader, including its pre-init name check.

After all modules load, Linux waits for device probing, verifies the actual
`virtio-pci` and child `virtio_gpu` drivers, and finds both primary and render
DRM nodes under that PCI function. It opens the verified render node on a
private tmpfs mount, then executes VERSION and GET_CAP through a real internal
FD and the upstream ioctl syscall. Only then does it call the original
lifecycle wait/READY path. No Linux application launcher, assumed vector
count, or synthetic DRM implementation participates in readiness.

Readiness establishes driver binding and real render-file queries, **not** publication
of an external DRM service. A service still needs request admission, dispatch,
client/FD ownership and a shutdown barrier before module unload. Native
conformance launches exercise render SESSION_OPEN/CLOSE and read-only commands
over a host-authenticated channel, not a public LPR endpoint or arbitrary DRM operations.

The device launcher creates a `drm_service` ownership ledger after readiness.
Its required `render_file_limit` is trusted launch policy, not a peer input.
Each admitted open creates a fresh upstream file, internal FD and `drm_file`;
it never duplicates the readiness file or another session file. Simultaneously
owned files are checked for distinct identities. Only opaque, non-reused private
cookies reach the native dispatch layer. Wire session IDs, authenticated client
bindings, generation, quota reservations and dispatch-drain admission remain
host policy. No cookie is accepted directly from a peer.

Cleanup first stops ledger admission and closes all session files, including
opens whose response was never delivered, then closes the readiness render
file synchronously in its opening Linux task. Failed close attempts are not
repeated; the first error remains sticky even after a consumed file disappears.
The private FD has a guard reference: `close_fd()` removes the FD, followed by
`__fput_sync()` for the last release, then private mount release. No raw FD,
file pointer or nested VERSION pointer crosses the service boundary. Calls
from another task or after a change to the FD identity are rejected; the
internal FD must not be duplicated or shared. A close error blocks unload,
including a flush error after the FD has already been consumed.

Cleanup then unloads every loaded module, flushes module work and RCU, then detaches
IRQ, DMA and PCI in that order. It runs only when all loaded modules were
unloaded and no unload error remains. A live driver also blocks detach.
Partial preparation is represented by an allocated session. Failed unload or
detach preserves the remaining session/ports/backing for process termination
and host-controlled device recovery; it is not permission to reuse them.

Lifecycle callback lifetime is unchanged: host context survives process exit,
and callbacks do not allocate, block or reenter Linux. Device inspection takes
place before lifecycle publication, in Linux process context, without wrapping
the interrupt callback around a temporary device session. An optional dispatch
callback runs only in the original Linux task, borrowing the service ledger until
quiesce. It resolves a private cookie to a borrowed file for that dispatch only.
Its native receiver publishes a private decoded plan; IRQ callbacks
only observe pending state and notify a completion. The task loop consumes
work exactly once, returns canonical query completions, and drains accepted
work before honoring STOP. Host FP/SSE state is protected at callback boundaries.

`drm_query.c` is native-side GPL glue, not a core-stack command decoder. It
decodes canonical GPU commands on the native receiver stack and marshals only
VERSION/GET_CAP into private typed Linux arguments on the owner task. Other
well-formed operations receive an unsupported completion without DRM effects.
The host integration supplies transport, generation/session authority and
private output staging. No host service names or native ABI enter this code.

Native integration evidence and exact inputs belong to the host project's
completion record. Graceful cleanup alone does not establish forced-process-death
recovery, device reset or generation revoke.

Gate-enabled cores additionally check full, one-byte and zero-capacity VERSION
queries, an invalid capability, excess capacity without output mutation, and
invalid API inputs. These checks do not test external command admission,
another task's ownership failure, FD identity corruption, or close failure.
The typed helper admits only VERSION and GET_CAP; it has no raw ioctl entry.
