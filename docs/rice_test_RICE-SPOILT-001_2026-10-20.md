# Spoilt rice test RICE-SPOILT-001 (run R00062, 19 to 20 Oct 2026, WAT)

Why this matters for the firmware: the board logged this food test in calibration mode, because the experiment ID could only be set over Serial and the board is sealed. The labels had to be added afterwards with an import file. Firmware 1.1.0 removes that problem.

- Sample: cooked rice about 0.25 kg, open, in the refrigerator with the device; spoiled by smell and some yellow grains.
- Timeline: R00062 boot 22:47; valid from 23:47; baseline 23:49 to 00:04; rice in 00:04; door open 00:09 to 00:10; MQ2 at the 3.3 V ADC limit from 01:58 (quality fault, saturation flags worked as designed); rice out about 02:09; device restarted twice while handled.
- Response: MQ1 Rs fell about 32 times (72.9 kohm to 2.30 kohm), MQ2 about 4.3 times (6.17 kohm to 1.44 kohm), within 2 to 3 minutes.
- Firmware behaviour seen: latest uploaded every 60 s; history every 10 min; no reset for 3 h 20 min; unsent stuck at 106 to 115 throughout.
