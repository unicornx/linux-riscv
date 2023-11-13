CONFIG_SOC_SOPHGO_CLOCK_DUMMY = n
ifeq ($(CONFIG_SOC_SOPHGO_CLOCK_DUMMY),y)
obj-$(CONFIG_SOC_SOPHGO)	+= clk-dummy.o
else
obj-$(CONFIG_SOC_SOPHGO)	+= clk.o
endif
obj-$(CONFIG_SOC_SOPHGO)	+= clk-mango.o
