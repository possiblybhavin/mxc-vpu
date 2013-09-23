imx-vpu
========

This is a port of the i.MX VPU kernel drivers.


Compiling
----------

Compile with:

    make -C [kernel-source-path] CONFIG_MXC_VPU=m CONFIG_MXC_IRAM=m M=$PWD
    make -C [kernel-source-path] CONFIG_MXC_VPU=m CONFIG_MXC_IRAM=m M=$PWD modules_install

Configuration
-------------

Add the following entry to your platform's device tree file:

    soc {
        vpu {
            compatible = "fsl,imx6q-vpu";
            clocks = <&clks 168>;
            clock-names = "vpu_clk";
            interrupts = <0 3 0x04>, <0 12 0x04>;
            interrupt-names = "vpu_ipi_irq", "vpu_jpu_irq";
            reg = <0x02040000 0x4000>;
            reg-names = "vpu_regs";
            iram-size = <0x21000>;
        };
    };

