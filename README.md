Simulated block device for QEMU
===============================

This is a version of QEMU that includes a block device (disksim) that 
introduces I/O delays according to a simulator. This allows you to use
a ramdisk while obtaining performance consistent with what a real disk would
produce.

To use this, you need DiskSim 4.0 source from:

http://www.pdl.cmu.edu/DiskSim/

and the patches from:

http://scobyseo.blogspot.pt/2009/12/how-to-compile-disksim-40-ssdsimms-in.html

This post includes also detailed information on how to compile DiskSim.

After having compiled qemu with --enable-disksim, use the following to configure
a simulated disk:

	-drive file=disksim:barracuda.parv:stats.txt:image.raw

where barracuda.parv is the configuration file for DiskSim, stats.txt will be the
simulation results file, and image.raw is the supporting file, that should be
located in a ramdisk.
