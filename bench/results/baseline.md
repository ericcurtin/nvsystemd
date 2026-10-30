
#### baseline / S0

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 605.1 (615.5, 598.9-615.6, 3.8) n=30 | 663.6 (674.9, 650.8-678.9, 5.7) n=30 |
|   of which kernel time before /init [ms] | 604.8 (614.9, 598.5-615.0, 3.7) n=30 | 615.1 (622.7, 604.1-631.3, 5.6) n=30 |
|   of which init to kata-agent [ms] | 0.3 (0.6, -0.0-0.7, 0.1) n=30 | 48.3 (58.1, 40.9-59.7, 4.0) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 957.6 (966.7, 947.4-972.1, 4.8) n=30 | 1022.6 (1032.4, 1009.6-1037.8, 6.5) n=30 |
| initramfs size (compressed) [KiB] | 258.7 (258.7, 258.7-258.7, 0.0) n=30 | 7953.8 (7953.8, 7953.8-7953.8, 0.0) n=30 |
| init RSS at kata start [KiB] | 270.0 (320.0, 232.0-320.0, 28.5) n=30 | 11802.0 (11952.0, 11596.0-11964.0, 104.6) n=30 |
| init PSS at kata start [KiB] | 270.0 (320.0, 232.0-320.0, 28.5) n=30 | 11802.0 (11952.0, 11596.0-11964.0, 104.6) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 49756.0 (52300.0, 43096.0-53892.0, 2891.6) n=30 | 83868.0 (88096.0, 74528.0-88744.0, 2762.0) n=30 |
| memory used 200 ms later [KiB] | 49816.0 (52356.0, 43128.0-53924.0, 2884.0) n=30 | 83868.0 (88096.0, 74528.0-88744.0, 2762.2) n=30 |
| user processes at kata start [] | 2.0 (2.0, 2.0-2.0, 0.0) n=30 | 2.0 (2.0, 2.0-2.0, 0.0) n=30 |
| user threads at kata start [] | 2.0 (2.0, 2.0-2.0, 0.0) n=30 | 2.0 (2.0, 2.0-2.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 200.0 (200.0, 190.0-210.0, 3.2) n=30 | 220.0 (230.0, 210.0-230.0, 5.1) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 205.3 (208.8, 199.3-210.3, 2.8) n=30 | 232.3 (237.9, 223.2-238.7, 3.3) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | 0.9 (4.4, -5.1-5.9, 2.8) n=30 | 28.4 (34.0, 19.3-34.8, 3.3) n=30 |

#### baseline / S1

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 614.0 (622.9, 605.5-627.4, 5.1) n=30 | 687.6 (706.0, 675.9-720.7, 9.9) n=30 |
|   of which kernel time before /init [ms] | 611.9 (620.3, 603.3-624.5, 5.1) n=30 | 621.3 (630.7, 606.5-645.2, 7.3) n=30 |
|   of which init to kata-agent [ms] | 2.4 (3.0, 1.5-3.0, 0.4) n=30 | 69.2 (80.0, 56.1-81.0, 7.1) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 965.8 (978.8, 954.9-979.9, 6.5) n=30 | 1043.6 (1064.3, 1034.6-1077.0, 10.0) n=30 |
| initramfs size (compressed) [KiB] | 258.7 (258.7, 258.7-258.7, 0.0) n=30 | 7953.8 (7953.8, 7953.8-7953.8, 0.0) n=30 |
| init RSS at kata start [KiB] | 262.0 (320.0, 236.0-320.0, 27.5) n=30 | 11842.0 (11928.0, 11596.0-11944.0, 108.0) n=30 |
| init PSS at kata start [KiB] | 262.0 (320.0, 236.0-320.0, 27.5) n=30 | 11842.0 (11928.0, 11596.0-11944.0, 108.0) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 48390.0 (52184.0, 42172.0-52436.0, 2532.9) n=30 | 83868.0 (88340.0, 74096.0-90760.0, 3432.2) n=30 |
| memory used 200 ms later [KiB] | 48390.0 (52272.0, 42200.0-52464.0, 2532.4) n=30 | 83868.0 (88312.0, 74096.0-90760.0, 3432.2) n=30 |
| user processes at kata start [] | 3.0 (3.0, 3.0-3.0, 0.0) n=30 | 4.0 (4.0, 4.0-4.0, 0.0) n=30 |
| user threads at kata start [] | 3.0 (3.0, 3.0-3.0, 0.0) n=30 | 4.0 (4.0, 4.0-4.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 210.0 (210.0, 190.0-210.0, 5.7) n=30 | 230.0 (240.0, 220.0-240.0, 4.9) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 210.8 (215.8, 199.4-218.3, 4.2) n=30 | 241.2 (247.9, 235.5-251.7, 3.8) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | 6.4 (11.4, -5.0-13.9, 4.2) n=30 | 37.3 (44.0, 31.6-47.8, 3.8) n=30 |

#### baseline / S2

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 616.0 (620.2, 606.6-624.5, 4.5) n=30 | 702.7 (715.6, 687.9-717.5, 7.6) n=30 |
|   of which kernel time before /init [ms] | 612.6 (617.1, 603.1-620.8, 4.5) n=30 | 621.1 (630.3, 613.1-630.6, 5.2) n=30 |
|   of which init to kata-agent [ms] | 3.2 (3.7, 2.9-3.8, 0.3) n=30 | 80.3 (92.1, 72.6-93.4, 5.7) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 969.0 (980.1, 954.7-987.6, 7.0) n=30 | 1063.3 (1082.3, 1044.2-1086.8, 10.6) n=30 |
| initramfs size (compressed) [KiB] | 258.7 (258.7, 258.7-258.7, 0.0) n=30 | 7953.8 (7953.8, 7953.8-7953.8, 0.0) n=30 |
| init RSS at kata start [KiB] | 274.0 (324.0, 232.0-328.0, 29.3) n=30 | 11708.0 (11908.0, 11608.0-11952.0, 99.1) n=30 |
| init PSS at kata start [KiB] | 274.0 (324.0, 232.0-328.0, 29.3) n=30 | 11708.0 (11908.0, 11608.0-11952.0, 99.1) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 48756.0 (52104.0, 42480.0-52356.0, 3101.3) n=30 | 83830.0 (89484.0, 79648.0-89892.0, 2193.6) n=30 |
| memory used 200 ms later [KiB] | 48838.0 (52208.0, 42512.0-52468.0, 3104.9) n=30 | 83830.0 (89484.0, 79648.0-89892.0, 2188.9) n=30 |
| user processes at kata start [] | 5.0 (5.0, 5.0-5.0, 0.0) n=30 | 6.0 (6.0, 6.0-6.0, 0.0) n=30 |
| user threads at kata start [] | 5.0 (5.0, 5.0-5.0, 0.0) n=30 | 6.0 (6.0, 6.0-6.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 210.0 (210.0, 190.0-210.0, 6.3) n=30 | 240.0 (250.0, 230.0-250.0, 6.5) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 210.4 (215.8, 199.5-218.0, 4.6) n=30 | 247.2 (256.8, 238.8-256.9, 4.5) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | 6.0 (11.4, -4.9-13.6, 4.6) n=30 | 43.3 (52.9, 34.9-53.0, 4.5) n=30 |

#### baseline / S3

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 1522.2 (1533.2, 1513.4-1549.8, 6.2) n=30 | 1326.7 (1340.2, 1312.5-1346.4, 7.4) n=30 |
|   of which kernel time before /init [ms] | 614.0 (624.5, 605.0-641.9, 6.3) n=30 | 622.9 (634.0, 612.7-641.3, 7.2) n=30 |
|   of which init to kata-agent [ms] | 908.0 (909.5, 907.3-910.9, 0.7) n=30 | 704.9 (712.4, 682.4-714.0, 6.1) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 1880.8 (1906.6, 1866.6-1919.4, 11.3) n=30 | 1692.8 (1808.3, 1673.7-1893.6, 43.1) n=30 |
| initramfs size (compressed) [KiB] | 258.6 (258.6, 258.6-258.6, 0.0) n=30 | 7954.2 (7954.2, 7954.2-7954.2, 0.0) n=30 |
| init RSS at kata start [KiB] | 266.0 (328.0, 236.0-328.0, 27.0) n=30 | 11864.0 (11940.0, 11596.0-11948.0, 98.4) n=30 |
| init PSS at kata start [KiB] | 266.0 (328.0, 236.0-328.0, 27.0) n=30 | 11864.0 (11940.0, 11596.0-11948.0, 98.4) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 48826.0 (52056.0, 43568.0-52556.0, 2257.0) n=30 | 85578.0 (89160.0, 76832.0-89368.0, 3109.4) n=30 |
| memory used 200 ms later [KiB] | 48858.0 (52088.0, 43596.0-52588.0, 2281.3) n=30 | 85632.0 (89268.0, 76832.0-89340.0, 3117.9) n=30 |
| user processes at kata start [] | 5.0 (5.0, 5.0-5.0, 0.0) n=30 | 6.0 (6.0, 6.0-6.0, 0.0) n=30 |
| user threads at kata start [] | 5.0 (5.0, 5.0-5.0, 0.0) n=30 | 6.0 (6.0, 6.0-6.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 210.0 (210.0, 200.0-210.0, 3.8) n=30 | 240.0 (250.0, 230.0-250.0, 5.1) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 213.1 (216.6, 206.0-219.9, 2.7) n=30 | 251.9 (257.4, 244.1-257.5, 3.4) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | 8.8 (12.2, 1.6-15.6, 2.7) n=30 | 48.0 (53.5, 40.2-53.6, 3.4) n=30 |

#### baseline / S4

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 628.2 (1816.1, 616.4-1878.9, 359.4) n=30 | 711.7 (2290.4, 687.2-4107.4, 729.1) n=30 |
|   of which kernel time before /init [ms] | 625.6 (1814.0, 614.7-1876.7, 359.4) n=30 | 642.4 (2226.6, 616.6-4044.9, 727.3) n=30 |
|   of which init to kata-agent [ms] | 2.0 (2.5, 1.6-2.7, 0.3) n=30 | 64.2 (83.3, 55.0-153.9, 17.8) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 1005.3 (5163.2, 982.2-7094.1, 1596.8) n=30 | 1088.7 (5029.3, 1060.0-10191.9, 2013.8) n=30 |
| initramfs size (compressed) [KiB] | 258.7 (258.7, 258.7-258.7, 0.0) n=30 | 7953.8 (7953.8, 7953.8-7953.8, 0.0) n=30 |
| init RSS at kata start [KiB] | 276.0 (316.0, 244.0-316.0, 24.6) n=30 | 11756.0 (11952.0, 11616.0-11956.0, 102.1) n=30 |
| init PSS at kata start [KiB] | 276.0 (316.0, 244.0-316.0, 24.6) n=30 | 11756.0 (11952.0, 11616.0-11956.0, 102.1) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 49802.0 (52432.0, 43232.0-52476.0, 2572.6) n=30 | 84292.0 (89156.0, 77756.0-92508.0, 3043.2) n=30 |
| memory used 200 ms later [KiB] | 49956.0 (52488.0, 43288.0-52504.0, 2578.8) n=30 | 84292.0 (89156.0, 77756.0-92508.0, 3037.4) n=30 |
| user processes at kata start [] | 3.0 (3.0, 3.0-3.0, 0.0) n=30 | 4.0 (4.0, 4.0-4.0, 0.0) n=30 |
| user threads at kata start [] | 3.0 (3.0, 3.0-3.0, 0.0) n=30 | 4.0 (4.0, 4.0-4.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 220.0 (650.0, 210.0-890.0, 149.6) n=30 | 240.0 (680.0, 230.0-1670.0, 275.6) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 220.4 (662.2, 214.4-899.6, 150.9) n=30 | 251.6 (695.8, 240.8-1678.2, 275.3) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | 16.0 (457.8, 10.0-695.2, 150.9) n=30 | 47.7 (491.9, 36.9-1474.3, 275.3) n=30 |
