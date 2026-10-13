# Thin forwarder: the Linux harness lives in src/.
all test check9 bench clean:
	$(MAKE) -C src $@

.PHONY: all test check9 bench clean
