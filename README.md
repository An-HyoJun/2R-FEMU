# 2R++: FEMU
2R++ Code in FEMU

# FEMU
A QEMU-based and DRAM-backed NVMe SSD Emulator

https://github.com/vtess/FEMU

# Greedy
Original FTL provided by FEMU

# 2R
Ported version of 2R FTL code to FEMU

# 2R++B
2R++ block version algorithm

# 2R++P
2R++ page version algorithm

# MODIFY
## location
'ftl.c' and 'ftl.h' located in /femu/hw/block/femu/bbssd
## size setting
Modify 'spp->pgs_per_blk' and 'spp->blks_per_pl' variable
