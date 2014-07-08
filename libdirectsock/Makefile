ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, can be overriden by command line or environment
RTE_TARGET ?= x86_64-default-linuxapp-gcc

include $(RTE_SDK)/mk/rte.vars.mk

# binary name
SHARED = libdirectsock.so

# all source are stored in SRCS-y
SRCS-y := directsock.c

CFLAGS += -O3
CFLAGS += $(WERROR_FLAGS)
LDFLAGS +=-z defs
LDLIBS += -lrte_pmd_virtio_uio -lrte_pmd_vmxnet3_uio -lrte_pmd_ixgbe -lrte_pmd_e1000 -lrte_timer -lrte_hash \
	  -lrte_mbuf -lethdev \
	  -lrte_eal -lrte_mempool -lrte_ring -lrte_pmd_ring -lrte_malloc
LDLIBS+=-lm -lrt -ldl -lpthread

include $(RTE_SDK)/mk/rte.extshared.mk
