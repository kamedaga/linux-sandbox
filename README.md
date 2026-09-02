# linux-sandbox

`linux-sandbox` is the GPL-2.0-only Linux runtime used by kobox2. It executes as
a separate process from the Apache-2.0 controller and owns all Linux-specific
code: the self-loader, Linux core primitives, subsystem implementations,
module symbols, structures, and loaded `.ko` state.

It is based on the upstream stable Linux tree rather than a collection of
custom compatibility files. The supported Linux version is `v6.18.48`.
