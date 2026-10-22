# FRIDGE001 calibration data findings (export of food-spoilage-7b60a, analysed 2026-10-17)

Source: full RTDB export of food-spoilage-7b60a. Data used: /baseline (baseline_logger, 14 to 15 Oct) and telemetry/FRIDGE001/runs (fridge_monitor calibration mode, 15 to 17 Oct). Times in WAT.

## Usable runs (both sensors above 0.05 V)

| Run | Start (WAT) | Minutes | MQ1 Rs, min 30 to end | MQ2 Rs, min 30 to end | Max soup % (Aug R0, no food) |
|---|---|---|---|---|---|
| baseline boot 5 | 14 Oct 08:12 | 376 | 26,855 ohm | 7,814 ohm | 6.0 |
| baseline boot 6 | 14 Oct 19:05 | 116 | 27,183 ohm | 9,282 ohm | 4.2 |
| fridge_monitor R00004 | 16 Oct 14:15 | 284 | 14,483 ohm | 3,528 ohm | 39.7 |
| fridge_monitor R00005 | 16 Oct 19:51 | 174 | 18,346 ohm | 3,806 ohm | 11.2 |
| fridge_monitor R00007 | 16 Oct 23:56 | 160 | 13,836 ohm | 1,832 ohm | 49.6 |

Aug R0: MQ1 7,802.26 ohm, MQ2 3,463.12 ohm. Soup anchor 120.68 ppm equals MQ1 Rs = 7,295 ohm with the Aug R0.

## Findings relevant to firmware

1. Heater warm-up is fast. In every run the steep rise ends 10 to 15 minutes after power-on. Rs is within a few percent of its plateau by 45 to 60 minutes unless something in the room changes.
2. A 3 h SETTLE_S is longer than the data supports; 60 minutes is defensible. (The running firmware already uses 3600 s.)
3. The clean-air level is not stable between days, so a single fixed R0 cannot work. Food runs are scored by the app against their own pre-food baseline (median Rs over the 15 minutes before the food goes in). The firmware keeps the August R0 constants and uploads raw statistics; it makes no spoilage decision.
4. The history upload backlog (`unsent`) grew past 250 records during R00004. On 19 to 20 Oct it stayed between 106 and 115 for a whole run while uploads were succeeding (see the brief, Part 3).
