CC ?= cc

POLICY_TESTS := pcie_p2p_policy_test bar1_p2p_policy_test dmabuf_gdr_policy_test \
	dmabuf_gdr_topology_policy_test
PCIE_P2P_POLICY_HEADER := ../src/nvidia/src/kernel/platform/p2p/pcie_p2p_policy.h
BAR1_POLICY_HEADER := ../src/nvidia/src/kernel/gpu/bus/arch/turing/bar1_p2p_policy.h
DMABUF_GDR_POLICY_HEADER := ../src/nvidia/arch/nvalloc/unix/include/dmabuf_gdr_policy.h
DMABUF_GDR_TOPOLOGY_POLICY_HEADER := ../kernel-open/nvidia/dmabuf-gdr-topology-policy.h
POLICY_TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror

.PHONY: all check clean

all: $(POLICY_TESTS)

pcie_p2p_policy_test: pcie_p2p_policy_test.c $(PCIE_P2P_POLICY_HEADER)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(POLICY_TEST_CFLAGS) -o $@ $<

bar1_p2p_policy_test: bar1_p2p_policy_test.c $(BAR1_POLICY_HEADER)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(POLICY_TEST_CFLAGS) -o $@ $<

dmabuf_gdr_policy_test: dmabuf_gdr_policy_test.c $(DMABUF_GDR_POLICY_HEADER)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(POLICY_TEST_CFLAGS) -o $@ $<

dmabuf_gdr_topology_policy_test: dmabuf_gdr_topology_policy_test.c $(DMABUF_GDR_TOPOLOGY_POLICY_HEADER)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(POLICY_TEST_CFLAGS) -o $@ $<

check: $(POLICY_TESTS)
	./pcie_p2p_policy_test
	./bar1_p2p_policy_test
	./dmabuf_gdr_policy_test
	./dmabuf_gdr_topology_policy_test

clean:
	$(RM) $(POLICY_TESTS)
