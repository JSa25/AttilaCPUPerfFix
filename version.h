// The mod version: shown in heapfix.log (diag.c) and in the marker the Workshop self-update compares (update.c).
// The marker number only has to grow: 4, 5, then version x 10 from 5.5 on (55, 60, 61, 62, 63), then version x 100
// from 6.3.1 on (631, 632, then 640 for 6.4, 650 for 6.5, 651 for 6.5.1, 652 for 6.5.2, 660 for 6.6, 661 for 6.6.1, 662 for 6.6.2, 663 for 6.6.3, 670 for 6.7, 680 for 6.8, 690 for 6.9, 691 for 6.9.1, 692 for 6.9.2, 700 for 6.10.0 (6 x 100 + 10 x 10; a later 7.0 needs a number above 700, e.g. 710), 701 for 6.10.1, 702 for 6.10.2, 703 for 6.10.3, ...; 6.3.2 was never released, it became 6.4).
#define MOD_VERSION_NUM 703
#define MOD_VERSION_STR "6.10.3"
#define MOD_VERSION_RC 6, 10, 3, 0   // dinput8.rc (the resource compiler cannot divide MOD_VERSION_NUM)
