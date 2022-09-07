================
Snapshot/restore
================

The ability to rapidly snapshot and restore guest VM state is a
crucial component of fuzzing applications with QEMU. A special virtual
device can be used by fuzzers to interface with snapshot/restores
commands in QEMU. The virtual device should have the following
commands supported that can be called by the guest:

- snapshot: save a copy of the guest VM memory, registers, and virtual
  device state
- restore: restore the saved copy of guest VM state
- shared_memory: request a page of guest memory to be "linked" to shared memory
  on the host, which can then communicate to a fuzzer on the host

Coverage data will be collected by code on the guest with source-based coverage
tracking. Another process on the host will control the fuzzing through the
shared memory.
