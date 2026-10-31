
#### baseline3 / S0

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 604.5 (615.0, 594.7-616.2, 5.1) n=30 | 664.4 (681.0, 648.1-685.6, 8.6) n=30 |
|   of which kernel time before /init [ms] | 604.2 (614.7, 594.5-616.0, 5.1) n=30 | 619.1 (630.4, 602.6-635.0, 7.4) n=30 |
|   of which init to kata-agent [ms] | 0.3 (0.5, 0.2-0.5, 0.1) n=30 | 46.8 (52.7, 42.8-53.3, 2.7) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 952.6 (960.9, 940.4-961.6, 5.9) n=30 | 1016.9 (1037.6, 1000.4-1037.8, 9.0) n=30 |
| initramfs size (compressed) [KiB] | 258.6 (258.6, 258.6-258.6, 0.0) n=30 | 7954.1 (7954.1, 7954.1-7954.1, 0.0) n=30 |
| init RSS at kata start [KiB] | 254.0 (320.0, 232.0-320.0, 30.2) n=30 | 11796.0 (11948.0, 11616.0-11948.0, 111.8) n=30 |
| init PSS at kata start [KiB] | 254.0 (320.0, 232.0-320.0, 30.2) n=30 | 11796.0 (11948.0, 11616.0-11948.0, 111.8) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 49824.0 (51920.0, 41132.0-51944.0, 2940.8) n=30 | 84238.0 (88040.0, 77716.0-88884.0, 2407.9) n=30 |
| memory used 200 ms later [KiB] | 49904.0 (51952.0, 41160.0-51976.0, 2949.7) n=30 | 84238.0 (88040.0, 77716.0-88996.0, 2418.3) n=30 |
| user processes at kata start [] | 2.0 (2.0, 2.0-2.0, 0.0) n=30 | 2.0 (2.0, 2.0-2.0, 0.0) n=30 |
| user threads at kata start [] | 2.0 (2.0, 2.0-2.0, 0.0) n=30 | 2.0 (2.0, 2.0-2.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 200.0 (200.0, 190.0-210.0, 4.3) n=30 | 220.0 (230.0, 210.0-230.0, 5.8) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 204.3 (210.0, 194.5-211.9, 4.1) n=30 | 230.8 (241.6, 222.0-242.7, 4.4) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | -1.3 (4.3, -11.1-6.3, 4.1) n=30 | 24.8 (35.6, 16.0-36.7, 4.4) n=30 |

#### baseline3 / S1

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 613.4 (619.9, 605.0-623.9, 5.0) n=30 | 688.1 (705.8, 673.7-707.1, 9.5) n=30 |
|   of which kernel time before /init [ms] | 611.3 (617.9, 603.0-621.9, 5.0) n=30 | 620.6 (638.3, 610.9-646.9, 9.0) n=30 |
|   of which init to kata-agent [ms] | 2.1 (2.5, 1.5-2.5, 0.2) n=30 | 66.1 (78.5, 60.2-86.7, 6.0) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 964.2 (972.5, 954.9-974.8, 5.1) n=30 | 1046.3 (1060.7, 1030.9-1065.0, 9.5) n=30 |
| initramfs size (compressed) [KiB] | 258.6 (258.6, 258.6-258.6, 0.0) n=30 | 7954.1 (7954.1, 7954.1-7954.1, 0.0) n=30 |
| init RSS at kata start [KiB] | 272.0 (324.0, 232.0-328.0, 27.3) n=30 | 11714.0 (11904.0, 11608.0-11908.0, 93.8) n=30 |
| init PSS at kata start [KiB] | 272.0 (324.0, 232.0-328.0, 27.3) n=30 | 11714.0 (11904.0, 11608.0-11908.0, 93.8) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 49370.0 (52136.0, 43288.0-52388.0, 2565.2) n=30 | 84408.0 (89640.0, 80972.0-89888.0, 2410.2) n=30 |
| memory used 200 ms later [KiB] | 49546.0 (52196.0, 43420.0-52420.0, 2565.3) n=30 | 84464.0 (89640.0, 80972.0-89888.0, 2401.3) n=30 |
| user processes at kata start [] | 3.0 (3.0, 3.0-3.0, 0.0) n=30 | 4.0 (4.0, 4.0-4.0, 0.0) n=30 |
| user threads at kata start [] | 3.0 (3.0, 3.0-3.0, 0.0) n=30 | 4.0 (4.0, 4.0-4.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 200.0 (210.0, 200.0-210.0, 5.0) n=30 | 230.0 (240.0, 220.0-250.0, 6.5) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 209.0 (215.8, 201.3-216.5, 4.1) n=30 | 240.1 (250.7, 231.9-254.0, 5.3) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | 3.4 (10.1, -4.3-10.9, 4.1) n=30 | 34.1 (44.7, 25.9-48.0, 5.3) n=30 |

#### baseline3 / S2

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 614.7 (625.5, 599.4-625.9, 6.7) n=30 | 704.4 (721.1, 680.4-740.6, 12.8) n=30 |
|   of which kernel time before /init [ms] | 612.0 (622.6, 596.4-622.8, 6.7) n=30 | 621.6 (638.5, 605.8-642.8, 9.7) n=30 |
|   of which init to kata-agent [ms] | 3.0 (3.4, 2.3-3.7, 0.3) n=30 | 78.9 (91.9, 71.5-102.1, 6.8) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 966.4 (977.6, 951.1-978.6, 6.8) n=30 | 1061.9 (1081.4, 1040.2-1097.4, 12.4) n=30 |
| initramfs size (compressed) [KiB] | 258.6 (258.6, 258.6-258.6, 0.0) n=30 | 7954.1 (7954.1, 7954.1-7954.1, 0.0) n=30 |
| init RSS at kata start [KiB] | 272.0 (324.0, 232.0-324.0, 27.8) n=30 | 11742.0 (11952.0, 11632.0-11952.0, 106.9) n=30 |
| init PSS at kata start [KiB] | 272.0 (324.0, 232.0-324.0, 27.8) n=30 | 11742.0 (11952.0, 11632.0-11952.0, 106.9) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 49288.0 (52468.0, 45208.0-52784.0, 2214.0) n=30 | 83672.0 (88968.0, 74492.0-89636.0, 3217.0) n=30 |
| memory used 200 ms later [KiB] | 49384.0 (52524.0, 45236.0-52872.0, 2185.4) n=30 | 83672.0 (88968.0, 74492.0-89636.0, 3217.6) n=30 |
| user processes at kata start [] | 5.0 (5.0, 5.0-5.0, 0.0) n=30 | 6.0 (6.0, 6.0-6.0, 0.0) n=30 |
| user threads at kata start [] | 5.0 (5.0, 5.0-5.0, 0.0) n=30 | 6.0 (6.0, 6.0-6.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 200.0 (210.0, 200.0-220.0, 5.7) n=30 | 240.0 (250.0, 230.0-250.0, 7.0) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 209.8 (217.8, 203.0-221.6, 4.3) n=30 | 246.4 (256.7, 241.2-259.7, 5.1) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | 4.2 (12.1, -2.7-16.0, 4.3) n=30 | 40.4 (50.7, 35.2-53.7, 5.1) n=30 |

#### baseline3 / S3

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 1520.3 (1528.2, 1505.6-1534.1, 6.7) n=30 | 1323.5 (1342.8, 1310.4-1352.0, 10.0) n=30 |
|   of which kernel time before /init [ms] | 612.6 (620.9, 597.4-627.2, 6.9) n=30 | 621.4 (637.2, 605.0-646.0, 9.2) n=30 |
|   of which init to kata-agent [ms] | 907.9 (908.7, 906.7-909.3, 0.6) n=30 | 701.5 (710.7, 693.2-714.6, 4.9) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 1873.3 (1882.8, 1857.1-1887.1, 7.1) n=30 | 1685.9 (1701.2, 1668.6-1708.3, 9.8) n=30 |
| initramfs size (compressed) [KiB] | 258.7 (258.7, 258.7-258.7, 0.0) n=30 | 7954.0 (7954.0, 7954.0-7954.0, 0.0) n=30 |
| init RSS at kata start [KiB] | 270.0 (316.0, 236.0-320.0, 30.1) n=30 | 11854.0 (11912.0, 11596.0-11956.0, 101.5) n=30 |
| init PSS at kata start [KiB] | 270.0 (316.0, 236.0-320.0, 30.1) n=30 | 11854.0 (11912.0, 11596.0-11956.0, 101.5) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 48726.0 (52116.0, 42968.0-52912.0, 2459.7) n=30 | 84994.0 (88584.0, 82172.0-88948.0, 1956.8) n=30 |
| memory used 200 ms later [KiB] | 48766.0 (52540.0, 43152.0-52988.0, 2457.1) n=30 | 84968.0 (88584.0, 82172.0-88948.0, 1952.1) n=30 |
| user processes at kata start [] | 5.0 (5.0, 5.0-5.0, 0.0) n=30 | 6.0 (6.0, 6.0-6.0, 0.0) n=30 |
| user threads at kata start [] | 5.0 (5.0, 5.0-5.0, 0.0) n=30 | 6.0 (6.0, 6.0-6.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 210.0 (210.0, 200.0-210.0, 4.3) n=30 | 240.0 (250.0, 230.0-260.0, 6.5) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 211.9 (219.5, 206.0-219.5, 3.2) n=30 | 251.1 (264.5, 244.0-264.5, 5.8) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | 6.3 (13.9, 0.4-13.9, 3.2) n=30 | 45.1 (58.5, 38.0-58.5, 5.8) n=30 |

#### baseline3 / S4

| metric | nvrc median (p95, min-max, sd) | nvsd median (p95, min-max, sd) |
|---|---|---|
| time to kata-agent start (CLOCK_BOOTTIME) [ms] | 625.1 (634.1, 615.0-636.0, 5.9) n=30 | 696.0 (711.4, 680.5-716.7, 9.1) n=30 |
|   of which kernel time before /init [ms] | 623.1 (632.5, 613.3-634.1, 5.9) n=30 | 634.3 (645.6, 619.6-653.0, 7.6) n=30 |
|   of which init to kata-agent [ms] | 1.8 (2.5, 1.5-2.6, 0.3) n=30 | 62.8 (71.3, 54.2-74.8, 4.7) n=30 |
| host wall: QEMU spawn to first stub report line [ms] | 987.6 (998.8, 978.7-998.8, 5.7) n=30 | 1063.2 (1080.7, 1047.7-1084.4, 10.0) n=30 |
| initramfs size (compressed) [KiB] | 258.6 (258.6, 258.6-258.6, 0.0) n=30 | 7954.1 (7954.1, 7954.1-7954.1, 0.0) n=30 |
| init RSS at kata start [KiB] | 282.0 (332.0, 248.0-332.0, 26.9) n=30 | 11872.0 (11952.0, 11640.0-11956.0, 112.0) n=30 |
| init PSS at kata start [KiB] | 282.0 (332.0, 248.0-332.0, 26.9) n=30 | 11872.0 (11952.0, 11640.0-11956.0, 112.0) n=30 |
| memory used (MemTotal-MemAvailable) at kata start [KiB] | 48698.0 (51780.0, 41868.0-52452.0, 2909.7) n=30 | 84386.0 (89020.0, 77716.0-90696.0, 2832.4) n=30 |
| memory used 200 ms later [KiB] | 48802.0 (51860.0, 42036.0-52480.0, 2884.3) n=30 | 84372.0 (89020.0, 77716.0-90668.0, 2821.6) n=30 |
| user processes at kata start [] | 3.0 (3.0, 3.0-3.0, 0.0) n=30 | 4.0 (4.0, 4.0-4.0, 0.0) n=30 |
| user threads at kata start [] | 3.0 (3.0, 3.0-3.0, 0.0) n=30 | 4.0 (4.0, 4.0-4.0, 0.0) n=30 |
| PID 1 utime+stime (10 ms ticks, includes kernel init thread) [ms] | 215.0 (220.0, 210.0-220.0, 5.1) n=30 | 240.0 (250.0, 230.0-250.0, 5.5) n=30 |
| PID 1 run time (schedstat, includes kernel init thread) [ms] | 220.2 (225.8, 213.2-230.5, 3.8) n=30 | 250.4 (260.7, 243.1-263.0, 5.0) n=30 |
| init CPU time (PID 1 run time minus calibration image) [ms] | 14.5 (20.2, 7.5-24.8, 3.8) n=30 | 44.4 (54.7, 37.1-57.0, 5.0) n=30 |
