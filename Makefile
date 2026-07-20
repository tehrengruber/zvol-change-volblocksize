.PHONY: test test-qemu test-local

# Run the suite in a Podman-wrapped QEMU/KVM guest (needs /dev/kvm).
test:
	./test-harness/run.sh

# Same, but drive QEMU directly without Podman.
test-qemu:
	./test-harness/run-qemu.sh

# Run the suite directly on a host that already has a working ZFS (needs root).
test-local:
	sudo python3 -m pytest -v tests/
