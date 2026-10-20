# Explicit cross-VM demo; intentionally excluded from the normal make test.
DEMO_CONFIG ?= $(HOME)/.config/91kvstore/two-vm-demo.json
DEMO_MODE ?= both

.PHONY: demo demo-show

demo:
	python3 -B tests/demo_two_vm.py --config "$(DEMO_CONFIG)" --mode "$(DEMO_MODE)"

demo-show:
	python3 -B tests/demo_two_vm.py --config "$(DEMO_CONFIG)" --mode "$(DEMO_MODE)" --show
